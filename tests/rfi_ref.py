"""Independent RasterFold version-1 codec and 24-bit BMP helpers.

This module is a test oracle. It is not the codec implementation.
"""

from __future__ import annotations

import struct
import zlib

MAGIC = b"RFI\x01"
BLOCK = 64
MAX_DIM = 32768
MAX_PIXELS = 32 * 1024 * 1024
HEADER = 32


class FormatError(Exception):
    pass


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def packbits_encode(src: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        run = 1
        while run < 128 and i + run < n and src[i + run] == src[i]:
            run += 1
        if run >= 2:
            out.append((257 - run) & 0xFF)
            out.append(src[i])
            i += run
            continue
        start = i
        lit = 0
        while i < n and lit < 128:
            if i + 2 < n and src[i] == src[i + 1] == src[i + 2]:
                break
            i += 1
            lit += 1
        if lit == 0:
            raise RuntimeError("packbits literal gather produced an empty run")
        out.append(lit - 1)
        out.extend(src[start : start + lit])
    return bytes(out)


def packbits_decode(src: bytes, expected: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(src):
        control = src[i]
        i += 1
        if control == 0x80:
            raise FormatError("packbits nop")
        if control <= 127:
            count = control + 1
            if i + count > len(src) or len(out) + count > expected:
                raise FormatError("packbits literal overrun")
            out.extend(src[i : i + count])
            i += count
        else:
            count = 257 - control
            if count < 2 or count > 128 or i >= len(src) or len(out) + count > expected:
                raise FormatError("packbits repeat overrun")
            out.extend(bytes([src[i]]) * count)
            i += 1
    if i != len(src) or len(out) != expected:
        raise FormatError("packbits length")
    return bytes(out)


def predict(rgb: bytes, bw: int, bh: int) -> bytes:
    npix = bw * bh
    planes = [bytearray(npix), bytearray(npix), bytearray(npix)]
    i = 0
    for _y in range(bh):
        left = [0, 0, 0]
        for _x in range(bw):
            r, g, b = rgb[i * 3 : i * 3 + 3]
            rp = (r - g) & 255
            gp = g & 255
            bp = (b - g) & 255
            planes[0][i] = (rp - left[0]) & 255
            planes[1][i] = (gp - left[1]) & 255
            planes[2][i] = (bp - left[2]) & 255
            left = [rp, gp, bp]
            i += 1
    return bytes(planes[0] + planes[1] + planes[2])


def unpredict(residual: bytes, bw: int, bh: int) -> bytes:
    npix = bw * bh
    if len(residual) != npix * 3:
        raise FormatError("residual length")
    pr = residual[0:npix]
    pg = residual[npix : npix * 2]
    pb = residual[npix * 2 :]
    out = bytearray()
    i = 0
    for _y in range(bh):
        left = [0, 0, 0]
        for _x in range(bw):
            rp = (pr[i] + left[0]) & 255
            gp = (pg[i] + left[1]) & 255
            bp = (pb[i] + left[2]) & 255
            out.append((rp + gp) & 255)
            out.append(gp)
            out.append((bp + gp) & 255)
            left = [rp, gp, bp]
            i += 1
    return bytes(out)


def try_palette(rgb: bytes) -> bytes | None:
    order: list[bytes] = []
    index: dict[bytes, int] = {}
    idxs = bytearray()
    for i in range(0, len(rgb), 3):
        pix = rgb[i : i + 3]
        found = index.get(pix)
        if found is None:
            if len(order) == 256:
                return None
            index[pix] = len(order)
            order.append(pix)
            found = index[pix]
        idxs.append(found)
    body = bytes([len(order) - 1]) + b"".join(order) + packbits_encode(bytes(idxs))
    return body


def encode_block(rgb: bytes, bw: int, bh: int) -> bytes:
    raw = rgb
    best_mode = 0
    best = raw
    packed = packbits_encode(raw)
    if len(packed) < len(best):
        best_mode, best = 1, packed
    residual = packbits_encode(predict(raw, bw, bh))
    if len(residual) < len(best):
        best_mode, best = 2, residual
    palette = try_palette(raw)
    if palette is not None and len(palette) < len(best):
        best_mode, best = 3, palette
    return bytes([best_mode, 0]) + struct.pack("<I", len(best)) + best


def iter_blocks(width: int, height: int):
    blocks_x = (width + BLOCK - 1) // BLOCK
    blocks_y = (height + BLOCK - 1) // BLOCK
    for by in range(blocks_y):
        y0 = by * BLOCK
        bh = min(BLOCK, height - y0)
        for bx in range(blocks_x):
            x0 = bx * BLOCK
            bw = min(BLOCK, width - x0)
            yield x0, y0, bw, bh


def crop_block(rgb: bytes, width: int, x0: int, y0: int, bw: int, bh: int) -> bytes:
    out = bytearray()
    stride = width * 3
    for y in range(y0, y0 + bh):
        start = y * stride + x0 * 3
        out += rgb[start : start + bw * 3]
    return bytes(out)


def paste_block(image: bytearray, width: int, x0: int, y0: int, bw: int, bh: int, block: bytes) -> None:
    stride = width * 3
    pos = 0
    row = bw * 3
    for y in range(y0, y0 + bh):
        start = y * stride + x0 * 3
        image[start : start + row] = block[pos : pos + row]
        pos += row


def build_file(width: int, height: int, payload: bytes) -> bytes:
    header = bytearray(HEADER)
    header[0:4] = MAGIC
    struct.pack_into("<H", header, 4, 1)
    struct.pack_into("<H", header, 6, HEADER)
    struct.pack_into("<I", header, 8, width)
    struct.pack_into("<I", header, 12, height)
    struct.pack_into("<H", header, 16, BLOCK)
    struct.pack_into("<H", header, 18, 1)
    struct.pack_into("<I", header, 20, len(payload))
    struct.pack_into("<I", header, 24, crc32(payload))
    struct.pack_into("<I", header, 28, crc32(header[:28]))
    return bytes(header) + payload


def encode_image(width: int, height: int, rgb: bytes) -> bytes:
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    payload = bytearray()
    for x0, y0, bw, bh in iter_blocks(width, height):
        payload += encode_block(crop_block(rgb, width, x0, y0, bw, bh), bw, bh)
    return build_file(width, height, bytes(payload))


def decode_block(data: bytes, bw: int, bh: int) -> bytes:
    if len(data) < 6:
        raise FormatError("truncated block")
    mode = data[0]
    flags = data[1]
    size = struct.unpack_from("<I", data, 2)[0]
    if flags != 0:
        raise FormatError("block flags")
    if size != len(data) - 6:
        raise FormatError("block length")
    body = data[6:]
    raw_len = bw * bh * 3
    if mode == 0:
        if len(body) != raw_len:
            raise FormatError("raw length")
        return body
    if mode == 1:
        return packbits_decode(body, raw_len)
    if mode == 2:
        return unpredict(packbits_decode(body, raw_len), bw, bh)
    if mode == 3:
        if not body:
            raise FormatError("palette")
        ncolors = body[0] + 1
        pal_bytes = ncolors * 3
        if len(body) < 1 + pal_bytes:
            raise FormatError("palette")
        indexes = packbits_decode(body[1 + pal_bytes :], bw * bh)
        out = bytearray()
        palette = body[1 : 1 + pal_bytes]
        for idx in indexes:
            if idx >= ncolors:
                raise FormatError("palette index")
            out += palette[idx * 3 : idx * 3 + 3]
        return bytes(out)
    raise FormatError("block mode")


def decode_image(blob: bytes) -> tuple[int, int, bytes]:
    if len(blob) < HEADER or blob[0:4] != MAGIC:
        raise FormatError("magic")
    version, header_size = struct.unpack_from("<HH", blob, 4)
    if version != 1:
        raise FormatError("version")
    if header_size != HEADER:
        raise FormatError("header size")
    width, height = struct.unpack_from("<II", blob, 8)
    block_size, pixel_format = struct.unpack_from("<HH", blob, 16)
    payload_size, payload_crc, header_crc = struct.unpack_from("<III", blob, 20)
    if block_size != BLOCK or pixel_format != 1:
        raise FormatError("format")
    if crc32(blob[:28]) != header_crc:
        raise FormatError("header checksum")
    if not (1 <= width <= MAX_DIM and 1 <= height <= MAX_DIM):
        raise FormatError("dimensions")
    if width * height > MAX_PIXELS:
        raise FormatError("limit")
    if payload_size != len(blob) - HEADER:
        raise FormatError("payload length")
    payload = blob[HEADER:]
    if crc32(payload) != payload_crc:
        raise FormatError("payload checksum")
    image = bytearray(width * height * 3)
    offset = 0
    for x0, y0, bw, bh in iter_blocks(width, height):
        if offset + 6 > len(payload):
            raise FormatError("truncated block")
        size = struct.unpack_from("<I", payload, offset + 2)[0]
        take = 6 + size
        if offset + take > len(payload):
            raise FormatError("block length")
        block = decode_block(payload[offset : offset + take], bw, bh)
        paste_block(image, width, x0, y0, bw, bh, block)
        offset += take
    if offset != len(payload):
        raise FormatError("trailing data")
    return width, height, bytes(image)


def row_stride(width: int) -> int:
    return (width * 3 + 3) & ~3


def write_bmp24(
    width: int,
    height: int,
    rgb: bytes,
    *,
    topdown: bool = False,
    gap: int = 0,
    sizeimage_zero: bool = False,
    reserved: int = 0,
    bf_size_zero: bool = False,
) -> bytes:
    stride = row_stride(width)
    pixels = stride * height
    off = 54 + gap
    file_size = off + pixels
    out = bytearray(file_size)
    struct.pack_into("<2sIHHI", out, 0, b"BM", 0 if bf_size_zero else file_size, reserved & 0xFFFF, (reserved >> 16) & 0xFFFF, off)
    image_size = 0 if sizeimage_zero else pixels
    stored_h = -height if topdown else height
    struct.pack_into("<IiiHHIIiiII", out, 14, 40, width, stored_h, 1, 24, 0, image_size, 3780, 3780, 0, 0)
    if gap:
        out[54 : 54 + gap] = b"\xA5" * gap
    for y in range(height):
        src_y = y if topdown else height - 1 - y
        src = rgb[src_y * width * 3 : (src_y + 1) * width * 3]
        dst = off + y * stride
        for x in range(width):
            r, g, b = src[x * 3 : x * 3 + 3]
            out[dst + x * 3 : dst + x * 3 + 3] = bytes((b, g, r))
    return bytes(out)


def read_bmp24(blob: bytes) -> tuple[int, int, bytes]:
    if len(blob) < 54 or blob[0:2] != b"BM":
        raise FormatError("bmp signature")
    _bf_size, _r1, _r2, off = struct.unpack_from("<IHHI", blob, 2)
    header = struct.unpack_from("<IiiHHIIiiII", blob, 14)
    bi_size, width, height, planes, bits, comp, sizeimage, _x, _y, _used, _imp = header
    if bi_size != 40 or planes != 1 or bits != 24 or comp != 0:
        raise FormatError("bmp subset")
    if width <= 0 or height == 0 or height == -2147483648:
        raise FormatError("bmp dimensions")
    topdown = height < 0
    height = abs(height)
    stride = row_stride(width)
    need = off + stride * height
    if off < 54 or need > len(blob):
        raise FormatError("bmp truncated")
    if sizeimage not in (0, ) and sizeimage < stride * height:
        raise FormatError("bmp image size")
    rgb = bytearray(width * height * 3)
    for file_y in range(height):
        img_y = file_y if topdown else height - 1 - file_y
        src = off + file_y * stride
        for x in range(width):
            b, g, r = blob[src + x * 3 : src + x * 3 + 3]
            base = (img_y * width + x) * 3
            rgb[base : base + 3] = bytes((r, g, b))
        pad = blob[src + width * 3 : src + stride]
        if any(pad):
            raise FormatError("bmp padding")
    return width, height, bytes(rgb)
