#!/usr/bin/env python3
"""Independent pure-Python TIFF and GIF reference readers for D10 T02 s6.

Written from the TIFF Revision 6.0 specification (Adobe, 1992; sections 2, 5,
8, 9, 13, 14, 15 and 17, plus the Adobe Deflate technical note) and the
GIF89a specification (CompuServe, 1990); they never call libtiff, giflib,
Pillow or the converter C.

decode_tiff() returns (width, height, channels, bits, alpha, native sample
bytes) with 16-bit samples little-endian, matching the converter's frame
layout, plus the ICC, orientation and XMP it found.  JPEG- and WebP-coded
TIFF raise UnsupportedTiff: callers report shared-provider coverage.

decode_gif() returns (width, height, channels, pixels) for a single-image
GIF: rgb8, or rgba8 when a transparent index is declared.
"""
from __future__ import annotations

import struct
import zlib


class RefError(ValueError):
    """Malformed input."""


class UnsupportedTiff(RefError):
    """Valid TIFF this oracle does not model."""


_TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8, 16: 8}


def _ifd_entries(data: bytes, off: int, e: str) -> tuple[dict[int, list[int]], dict[int, bytes], int]:
    """Return (numeric tags, byte/ASCII/UNDEFINED tags, next IFD offset)."""
    n = struct.unpack_from(e + "H", data, off)[0]
    nums: dict[int, list[int]] = {}
    blobs: dict[int, bytes] = {}
    for i in range(n):
        tag, typ, cnt = struct.unpack_from(e + "HHI", data, off + 2 + 12 * i)
        size = _TYPE_SIZE.get(typ)
        if size is None:
            continue
        total = size * cnt
        val_off = off + 2 + 12 * i + 8
        if total > 4:
            val_off = struct.unpack_from(e + "I", data, val_off)[0]
        raw = data[val_off : val_off + total]
        if len(raw) != total:
            raise RefError("tag value out of file")
        if typ in (2, 7):
            blobs[tag] = raw
        elif typ in (1, 6):
            nums[tag] = list(raw)
            blobs[tag] = raw
        elif typ in (3, 8):
            nums[tag] = list(struct.unpack(e + f"{cnt}H", raw))
        elif typ in (4, 9):
            nums[tag] = list(struct.unpack(e + f"{cnt}I", raw))
        elif typ == 16:
            nums[tag] = list(struct.unpack(e + f"{cnt}Q", raw))
    nxt = struct.unpack_from(e + "I", data, off + 2 + 12 * n)[0]
    return nums, blobs, nxt


def _lzw_tiff(src: bytes, expected: int) -> bytes:
    """TIFF LZW (section 13): MSB-first codes, 9..12 bits, early change."""
    out = bytearray()
    table: list[bytes] = []

    def reset() -> None:
        table.clear()
        table.extend(bytes([i]) for i in range(256))
        table.extend((b"", b""))  # 256 Clear, 257 EOI

    reset()
    width = 9
    bitpos = 0
    nbits = len(src) * 8
    prev: bytes | None = None
    while bitpos + width <= nbits and len(out) < expected:
        code = 0
        for _ in range(width):
            byte = src[bitpos >> 3]
            code = (code << 1) | ((byte >> (7 - (bitpos & 7))) & 1)
            bitpos += 1
        if code == 256:
            reset()
            width = 9
            prev = None
            continue
        if code == 257:
            break
        if prev is None:
            if code >= len(table):
                raise RefError("LZW code out of table")
            entry = table[code]
        elif code < len(table):
            entry = table[code]
            table.append(prev + entry[:1])
        elif code == len(table):
            entry = prev + prev[:1]
            table.append(entry)
        else:
            raise RefError("LZW code out of table")
        out += entry
        prev = entry
        # Early change: widen one code before the table fills the width.
        if len(table) + 1 >= (1 << width) and width < 12:
            width += 1
    return bytes(out[:expected])


def _packbits(src: bytes, expected: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < expected:
        n = src[i]
        i += 1
        if n < 128:
            out += src[i : i + n + 1]
            i += n + 1
        elif n > 128:
            out += src[i : i + 1] * (257 - n)
            i += 1
    return bytes(out[:expected])


def decode_tiff(data: bytes) -> dict[str, object]:
    if data[:2] == b"II":
        e = "<"
    elif data[:2] == b"MM":
        e = ">"
    else:
        raise RefError("not TIFF")
    if struct.unpack_from(e + "H", data, 2)[0] != 42:
        raise UnsupportedTiff("BigTIFF or bad magic")
    ifd0 = struct.unpack_from(e + "I", data, 4)[0]
    t, blobs, nxt = _ifd_entries(data, ifd0, e)
    if nxt:
        raise UnsupportedTiff("more than one IFD")

    def one(tag: int, default: int | None = None) -> int:
        v = t.get(tag)
        if v is None:
            if default is None:
                raise RefError(f"missing tag {tag}")
            return default
        return v[0]

    w, h = one(256), one(257)
    spp = one(277, 1)
    bps_l = t.get(258, [1])
    bps = bps_l[0]
    comp = one(259, 1)
    photo = one(262)
    planar = one(284, 1)
    pred = one(317, 1)
    extra = t.get(338, [])
    if comp not in (1, 5, 8, 32946, 32773):
        raise UnsupportedTiff(f"compression {comp}")
    if bps not in (8, 16) or any(b != bps for b in bps_l):
        raise UnsupportedTiff("bit depth")
    alpha = 0
    if photo == 1 and spp == 1:
        ch = 1
    elif photo == 2 and spp == 3:
        ch = 3
    elif photo == 2 and spp == 4 and list(extra) == [2]:
        ch, alpha = 4, 1
    else:
        raise UnsupportedTiff("photometric/samples")
    bpsb = bps // 8
    tiled = 322 in t
    if tiled:
        tw, th = one(322), one(323)
        offs, cnts = t[324], t[325]
    else:
        tw, th = w, one(278, h)
        offs, cnts = t[273], t[279]
    planes = ch if planar == 2 else 1
    seg_ch = 1 if planar == 2 else ch
    across = -(-w // tw) if tiled else 1
    down = -(-h // th)
    per_plane = across * down
    img = [bytearray(w * h * bpsb) for _ in range(ch)] if planar == 2 else [bytearray(w * h * ch * bpsb)]
    for p in range(planes):
        for s in range(per_plane):
            idx = p * per_plane + s
            raw = data[offs[idx] : offs[idx] + cnts[idx]]
            ty, tx = divmod(s, across)
            rows = th if tiled else min(th, h - ty * th)
            cols = tw
            expected = rows * cols * seg_ch * bpsb
            if comp == 1:
                buf = raw[:expected]
            elif comp == 5:
                buf = _lzw_tiff(raw, expected)
            elif comp in (8, 32946):
                buf = zlib.decompress(raw)[:expected]
            else:
                buf = _packbits(raw, expected)
            if len(buf) < expected:
                raise RefError("short segment")
            row_b = cols * seg_ch * bpsb
            rws = []
            for r in range(rows):
                row = bytearray(buf[r * row_b : (r + 1) * row_b])
                if bpsb == 2:
                    vals = list(struct.unpack(e + f"{cols * seg_ch}H", row))
                    if pred == 2:
                        for i in range(seg_ch, len(vals)):
                            vals[i] = (vals[i] + vals[i - seg_ch]) & 0xFFFF
                    row = bytearray(struct.pack(f"<{len(vals)}H", *vals))
                elif pred == 2:
                    for i in range(seg_ch, len(row)):
                        row[i] = (row[i] + row[i - seg_ch]) & 0xFF
                rws.append(row)
            dst = img[p]
            out_cols = min(cols, w - tx * tw)
            for r in range(rows):
                y = ty * th + r
                if y >= h:
                    break
                start = (y * w + tx * tw) * seg_ch * bpsb
                dst[start : start + out_cols * seg_ch * bpsb] = rws[r][: out_cols * seg_ch * bpsb]
    if planar == 2:
        out = bytearray(w * h * ch * bpsb)
        for i in range(w * h):
            for c in range(ch):
                out[(i * ch + c) * bpsb : (i * ch + c + 1) * bpsb] = img[c][i * bpsb : (i + 1) * bpsb]
    else:
        out = img[0]
    return {
        "width": w, "height": h, "channels": ch, "bits": bps, "alpha": alpha,
        "pixels": bytes(out),
        "icc": blobs.get(34675, b""),
        "xmp": blobs.get(700, b""),
        "orientation": one(274, 1),
        "compression": comp, "predictor": pred, "tiled": tiled, "planar": planar,
    }


# ---------------------------------------------------------------------------
# GIF
# ---------------------------------------------------------------------------

def _sub_blocks(data: bytes, pos: int) -> tuple[bytes, int]:
    out = bytearray()
    while True:
        if pos >= len(data):
            raise RefError("truncated sub-blocks")
        n = data[pos]
        pos += 1
        if n == 0:
            return bytes(out), pos
        out += data[pos : pos + n]
        pos += n


def _lzw_gif(src: bytes, min_size: int, expected: int) -> bytes:
    """GIF LZW (appendix F): LSB-first codes, deferred-clear tolerant."""
    clear, eoi = 1 << min_size, (1 << min_size) + 1
    out = bytearray()
    table: list[bytes] = []
    width = min_size + 1
    prev: bytes | None = None

    def reset() -> None:
        table.clear()
        table.extend(bytes([i]) for i in range(clear))
        table.extend((b"", b""))

    reset()
    acc = 0
    nacc = 0
    pos = 0
    while len(out) < expected:
        while nacc < width:
            if pos >= len(src):
                return bytes(out)
            acc |= src[pos] << nacc
            pos += 1
            nacc += 8
        code = acc & ((1 << width) - 1)
        acc >>= width
        nacc -= width
        if code == clear:
            reset()
            width = min_size + 1
            prev = None
            continue
        if code == eoi:
            break
        if prev is None:
            if code >= len(table):
                raise RefError("GIF LZW code out of table")
            entry = table[code]
        elif code < len(table):
            entry = table[code]
            if len(table) < 4096:
                table.append(prev + entry[:1])
        elif code == len(table) and len(table) < 4096:
            entry = prev + prev[:1]
            table.append(entry)
        else:
            raise RefError("GIF LZW code out of table")
        out += entry
        prev = entry
        if len(table) == (1 << width) and width < 12:
            width += 1
    return bytes(out[:expected])


def _colour_table(data: bytes, pos: int, n: int) -> list[tuple[int, int, int]]:
    if pos + 3 * n > len(data):
        raise RefError("truncated colour table")
    return [(data[pos + 3 * i], data[pos + 3 * i + 1], data[pos + 3 * i + 2]) for i in range(n)]


def decode_gif(data: bytes) -> tuple[int, int, int, bytes]:
    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise RefError("not GIF")
    sw, sh, flags = struct.unpack_from("<HHB", data, 6)
    pos = 13
    gct: list[tuple[int, int, int]] = []
    if flags & 0x80:
        n = 2 << (flags & 7)
        gct = _colour_table(data, pos, n)
        pos += 3 * n
    transparent = None
    image = None
    while pos < len(data):
        b = data[pos]
        pos += 1
        if b == 0x3B:
            break
        if b == 0x21:
            label = data[pos]
            pos += 1
            body, pos = _sub_blocks(data, pos)
            if label == 0xF9 and len(body) >= 4 and body[0] & 1:
                transparent = body[3]
            continue
        if b != 0x2C:
            raise RefError("bad block")
        if image is not None:
            raise RefError("more than one image")
        x, y, w, h, iflags = struct.unpack_from("<HHHHB", data, pos)
        pos += 9
        table = gct
        if iflags & 0x80:
            n = 2 << (iflags & 7)
            table = _colour_table(data, pos, n)
            pos += 3 * n
        min_size = data[pos]
        pos += 1
        lzw, pos = _sub_blocks(data, pos)
        idx = _lzw_gif(lzw, min_size, w * h)
        if len(idx) < w * h:
            raise RefError("short raster")
        if iflags & 0x40:
            order = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
            rows = [b""] * h
            for i, r in enumerate(order):
                rows[r] = idx[i * w : (i + 1) * w]
            idx = b"".join(rows)
        image = (x, y, w, h, table, idx)
    if image is None:
        raise RefError("no image")
    x, y, w, h, table, idx = image
    if (x, y, w, h) != (0, 0, sw, sh):
        raise RefError("partial frame")
    ch = 4 if transparent is not None else 3
    out = bytearray()
    for i in idx:
        r, g, bl = table[i]
        out += bytes((r, g, bl))
        if ch == 4:
            out.append(0 if i == transparent else 255)
    return w, h, ch, bytes(out)
