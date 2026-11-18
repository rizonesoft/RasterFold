#!/usr/bin/env python3
"""Independent pure-Python JPEG reference decoder for D10 T02 s5.

Written from ITU-T T.81 (ISO/IEC 10918-1) Annexes B, F and G and the IJG
algorithm descriptions; it never calls libjpeg, Pillow or the converter C.

Two products:
  - decode_coefficients(): quantized DCT coefficients (natural order) per
    component for baseline, extended sequential and progressive Huffman
    JPEG, with quantization tables, sampling factors and coding flags.  This
    is the independent oracle for coefficient-exact entropy optimization.
  - decode_pixels(): the pixels libjpeg produces under the converter's
    pinned settings (JDCT_ISLOW integer IDCT, fancy upsampling, no block
    smoothing, JFIF YCbCr to RGB with the libjpeg integer tables).

Arithmetic-coded JPEG (SOF9/SOF10) is outside this oracle: decode raises
UnsupportedJpeg, and callers report shared-provider coverage instead.
"""
from __future__ import annotations

from dataclasses import dataclass, field

ZIGZAG = (
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
)


class JpegError(ValueError):
    """Malformed stream."""


class UnsupportedJpeg(JpegError):
    """Valid JPEG this oracle does not model (arithmetic, lossless, 12-bit)."""


@dataclass
class Component:
    cid: int
    h: int
    v: int
    tq: int
    blocks_w: int = 0
    blocks_h: int = 0
    alloc_w: int = 0
    alloc_h: int = 0
    coefs: list[list[int]] = field(default_factory=list)  # per block, natural order
    dc_tbl: int = 0
    ac_tbl: int = 0


@dataclass
class JpegCoefficients:
    width: int
    height: int
    precision: int
    sof: int
    progressive: bool
    arithmetic: bool
    components: list[Component]
    qtables: dict[int, list[int]]  # natural order
    saw_jfif: bool
    adobe_transform: int  # -1 when no Adobe APP14
    scans: int

    def block(self, ci: int, by: int, bx: int) -> list[int]:
        c = self.components[ci]
        return c.coefs[by * c.alloc_w + bx]


def _u16(b: bytes, o: int) -> int:
    return (b[o] << 8) | b[o + 1]


class _Huff:
    __slots__ = ("lookup", "maxlen")

    def __init__(self, counts: list[int], symbols: bytes) -> None:
        self.lookup: dict[tuple[int, int], int] = {}
        code = 0
        k = 0
        for length in range(1, 17):
            for _ in range(counts[length - 1]):
                if k >= len(symbols):
                    raise JpegError("DHT symbol count mismatch")
                self.lookup[(length, code)] = symbols[k]
                k += 1
                code += 1
            if code > (1 << length):
                raise JpegError("DHT over-subscribed")
            code <<= 1
        self.maxlen = 16


class _Bits:
    """Entropy-coded segment reader with byte unstuffing and marker stop."""

    def __init__(self, data: bytes, pos: int) -> None:
        self.data = data
        self.pos = pos
        self.acc = 0
        self.nbits = 0
        self.marker: int | None = None

    def _fill(self) -> None:
        while self.nbits <= 24:
            if self.marker is not None or self.pos >= len(self.data):
                self.acc = (self.acc << 8)  # libjpeg pads with zeros past the end
                self.nbits += 8
                continue
            b = self.data[self.pos]
            if b == 0xFF:
                nxt = self.data[self.pos + 1] if self.pos + 1 < len(self.data) else 0xD9
                if nxt == 0x00:
                    self.pos += 2
                elif nxt == 0xFF:
                    self.pos += 1
                    continue
                else:
                    self.marker = nxt
                    continue
            else:
                self.pos += 1
            self.acc = (self.acc << 8) | b
            self.nbits += 8

    def bit(self) -> int:
        if self.nbits == 0:
            self._fill()
        self.nbits -= 1
        return (self.acc >> self.nbits) & 1

    def bits(self, n: int) -> int:
        if n == 0:
            return 0
        if self.nbits < n:
            self._fill()
        self.nbits -= n
        return (self.acc >> self.nbits) & ((1 << n) - 1)

    def huff(self, t: _Huff) -> int:
        code = 0
        for length in range(1, 17):
            code = (code << 1) | self.bit()
            sym = t.lookup.get((length, code))
            if sym is not None:
                return sym
        raise JpegError("invalid Huffman code")

    def restart(self) -> None:
        """Discard buffered bits and consume the expected RSTn marker."""
        self.acc = 0
        self.nbits = 0
        if self.marker is None:
            # Skip to the marker (fill bytes are allowed before it).
            while self.pos < len(self.data):
                if self.data[self.pos] == 0xFF and self.pos + 1 < len(self.data) and self.data[self.pos + 1] not in (0x00, 0xFF):
                    self.marker = self.data[self.pos + 1]
                    break
                self.pos += 1
        if self.marker is None or not (0xD0 <= self.marker <= 0xD7):
            raise JpegError("missing restart marker")
        self.pos += 2
        self.marker = None


def _extend(v: int, s: int) -> int:
    return v - (1 << s) + 1 if s and v < (1 << (s - 1)) else v


def decode_coefficients(data: bytes) -> JpegCoefficients:
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xD8:
        raise JpegError("missing SOI")
    pos = 2
    qt: dict[int, list[int]] = {}
    dc_tabs: dict[int, _Huff] = {}
    ac_tabs: dict[int, _Huff] = {}
    restart_interval = 0
    frame: JpegCoefficients | None = None
    saw_jfif = False
    adobe = -1
    eobrun_state = {"eobrun": 0}
    scans = 0
    while True:
        if pos + 2 > len(data):
            raise JpegError("truncated before EOI")
        if data[pos] != 0xFF:
            raise JpegError(f"expected marker at {pos}")
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        marker = data[pos]
        pos += 1
        if marker == 0xD9:
            break
        if 0xD0 <= marker <= 0xD7 or marker == 0x01:
            continue
        seg_len = _u16(data, pos)
        seg = data[pos + 2 : pos + seg_len]
        if len(seg) != seg_len - 2:
            raise JpegError("truncated segment")
        pos += seg_len
        if marker == 0xDB:
            o = 0
            while o < len(seg):
                pq, tq = seg[o] >> 4, seg[o] & 15
                o += 1
                vals = [0] * 64
                for k in range(64):
                    if pq:
                        vals[ZIGZAG[k]] = _u16(seg, o)
                        o += 2
                    else:
                        vals[ZIGZAG[k]] = seg[o]
                        o += 1
                qt[tq] = vals
        elif marker == 0xC4:
            o = 0
            while o < len(seg):
                tc, th = seg[o] >> 4, seg[o] & 15
                counts = list(seg[o + 1 : o + 17])
                n = sum(counts)
                syms = seg[o + 17 : o + 17 + n]
                (dc_tabs if tc == 0 else ac_tabs)[th] = _Huff(counts, syms)
                o += 17 + n
        elif marker == 0xDD:
            restart_interval = _u16(seg, 0)
        elif marker == 0xE0 and seg[:5] == b"JFIF\x00":
            saw_jfif = True
        elif marker == 0xEE and seg[:5] == b"Adobe" and len(seg) >= 12:
            adobe = seg[11]
        elif marker in (0xC0, 0xC1, 0xC2):
            if frame is not None:
                raise JpegError("multiple SOF")
            prec = seg[0]
            if prec != 8:
                raise UnsupportedJpeg(f"precision {prec}")
            height, width, nc = _u16(seg, 1), _u16(seg, 3), seg[5]
            comps = [Component(seg[6 + 3 * i], seg[7 + 3 * i] >> 4, seg[7 + 3 * i] & 15, seg[8 + 3 * i]) for i in range(nc)]
            hmax = max(c.h for c in comps)
            vmax = max(c.v for c in comps)
            mcux = -(-width // (8 * hmax))
            mcuy = -(-height // (8 * vmax))
            for c in comps:
                c.blocks_w = -(-(-(-width * c.h // hmax)) // 8)
                c.blocks_h = -(-(-(-height * c.v // vmax)) // 8)
                c.alloc_w = mcux * c.h
                c.alloc_h = mcuy * c.v
                c.coefs = [[0] * 64 for _ in range(c.alloc_w * c.alloc_h)]
            frame = JpegCoefficients(width, height, prec, marker, marker == 0xC2, False, comps, {}, saw_jfif, adobe, 0)
        elif marker in (0xC9, 0xCA, 0xC3, 0xC5, 0xC6, 0xC7, 0xCB, 0xCD, 0xCE, 0xCF):
            raise UnsupportedJpeg(f"SOF marker 0x{marker:02X}")
        elif marker == 0xDA:
            if frame is None:
                raise JpegError("SOS before SOF")
            scans += 1
            ns = seg[0]
            o = 1 + 2 * ns
            ss, se, ah, al = seg[o], seg[o + 1], seg[o + 2] >> 4, seg[o + 2] & 15
            scomps = []
            for i in range(ns):
                cid, tbl = seg[1 + 2 * i], seg[2 + 2 * i]
                ci = next(j for j, c in enumerate(frame.components) if c.cid == cid)
                # A selector counts only in a scan that uses it (T.81 G.1.1.1:
                # a DC refinement scan or an AC scan carries an unused one).
                if ss == 0 and ah == 0:
                    frame.components[ci].dc_tbl = tbl >> 4
                if se > 0:
                    frame.components[ci].ac_tbl = tbl & 15
                scomps.append(ci)
            scan_tables = {ci: (seg[2 + 2 * i] >> 4, seg[2 + 2 * i] & 15) for i, ci in enumerate(scomps)}
            pos = _decode_scan(data, pos, frame, scomps, ss, se, ah, al, dc_tabs, ac_tabs, restart_interval, eobrun_state, scan_tables)
        # Every other marker (APPn, COM, DNL absent) carries no coefficient data.
    if frame is None:
        raise JpegError("no SOF")
    frame.qtables = {c.tq: list(qt[c.tq]) for c in frame.components}
    frame.saw_jfif = saw_jfif
    frame.adobe_transform = adobe
    frame.scans = scans
    return frame


def _decode_scan(data, pos, frame, scomps, ss, se, ah, al, dc_tabs, ac_tabs, ri, eob_state, scan_tables):
    br = _Bits(data, pos)
    comps = frame.components
    hmax = max(c.h for c in comps)
    vmax = max(c.v for c in comps)
    progressive = frame.progressive
    preds = [0] * len(comps)
    eob_state["eobrun"] = 0

    def decode_block(ci: int, blk: list[int]) -> None:
        c = comps[ci]
        dc_id, ac_id = scan_tables[ci]
        if not progressive:
            t = br.huff(dc_tabs[dc_id])
            diff = _extend(br.bits(t), t)
            preds[ci] += diff
            blk[0] = preds[ci]
            k = 1
            act = ac_tabs[ac_id]
            while k < 64:
                rs = br.huff(act)
                r, s = rs >> 4, rs & 15
                if s == 0:
                    if r != 15:
                        break
                    k += 16
                    continue
                k += r
                if k > 63:
                    raise JpegError("AC index overflow")
                blk[ZIGZAG[k]] = _extend(br.bits(s), s)
                k += 1
            return
        if ss == 0:
            if ah == 0:
                t = br.huff(dc_tabs[dc_id])
                diff = _extend(br.bits(t), t)
                preds[ci] += diff
                blk[0] = preds[ci] * (1 << al)
            else:
                if br.bit():
                    blk[0] |= 1 << al
            return
        act = ac_tabs[ac_id]
        if ah == 0:
            if eob_state["eobrun"] > 0:
                eob_state["eobrun"] -= 1
                return
            k = ss
            while k <= se:
                rs = br.huff(act)
                r, s = rs >> 4, rs & 15
                if s == 0:
                    if r < 15:
                        eob_state["eobrun"] = (1 << r) - 1
                        if r:
                            eob_state["eobrun"] += br.bits(r)
                        break
                    k += 16
                    continue
                k += r
                if k > 63:
                    raise JpegError("AC index overflow")
                blk[ZIGZAG[k]] = _extend(br.bits(s), s) * (1 << al)
                k += 1
            return
        # AC successive-approximation refinement (G.1.2.3)
        p1 = 1 << al
        m1 = -1 << al
        k = ss
        if eob_state["eobrun"] == 0:
            while k <= se:
                rs = br.huff(act)
                r, s = rs >> 4, rs & 15
                val = 0
                if s:
                    if s != 1:
                        raise JpegError("bad refinement magnitude")
                    val = p1 if br.bit() else m1
                elif r != 15:
                    eob_state["eobrun"] = 1 << r
                    if r:
                        eob_state["eobrun"] += br.bits(r)
                    break
                while k <= se:
                    z = ZIGZAG[k]
                    if blk[z] != 0:
                        if br.bit():
                            if (blk[z] & p1) == 0:
                                blk[z] += p1 if blk[z] >= 0 else m1
                    else:
                        if r == 0:
                            if val:
                                blk[z] = val
                            k += 1
                            break
                        r -= 1
                    k += 1
        if eob_state["eobrun"] > 0:
            while k <= se:
                z = ZIGZAG[k]
                if blk[z] != 0 and br.bit():
                    if (blk[z] & p1) == 0:
                        blk[z] += p1 if blk[z] >= 0 else m1
                k += 1
            eob_state["eobrun"] -= 1

    if len(scomps) == 1:
        ci = scomps[0]
        c = comps[ci]
        units = [(by, bx) for by in range(c.blocks_h) for bx in range(c.blocks_w)]
        for n, (by, bx) in enumerate(units):
            if ri and n and n % ri == 0:
                br.restart()
                preds = [0] * len(comps)
                eob_state["eobrun"] = 0
            decode_block(ci, c.coefs[by * c.alloc_w + bx])
    else:
        mcux = -(-frame.width // (8 * hmax))
        mcuy = -(-frame.height // (8 * vmax))
        n = 0
        for my in range(mcuy):
            for mx in range(mcux):
                if ri and n and n % ri == 0:
                    br.restart()
                    preds = [0] * len(comps)
                    eob_state["eobrun"] = 0
                for ci in scomps:
                    c = comps[ci]
                    for v in range(c.v):
                        for h in range(c.h):
                            by, bx = my * c.v + v, mx * c.h + h
                            decode_block(ci, c.coefs[by * c.alloc_w + bx])
                n += 1
    # Resume marker parsing at the first marker after the entropy data.
    p = br.pos
    while p < len(data):
        if data[p] == 0xFF and p + 1 < len(data) and data[p + 1] not in (0x00,) and not (0xD0 <= data[p + 1] <= 0xD7):
            if data[p + 1] == 0xFF:
                p += 1
                continue
            return p
        p += 1
    raise JpegError("entropy data runs past end")


# ---------------------------------------------------------------------------
# Pixel reconstruction under the pinned libjpeg settings
# ---------------------------------------------------------------------------

_CONST_BITS = 13
_PASS1_BITS = 2
_F0298, _F0390, _F0541, _F0765 = 2446, 3196, 4433, 6270
_F0899, _F1175, _F1501, _F1847 = 7373, 9633, 12299, 15137
_F1961, _F2053, _F2562, _F3072 = 16069, 16819, 20995, 25172


def _descale(x: int, n: int) -> int:
    return (x + (1 << (n - 1))) >> n


def _idct_1d(d: list[int], shift: int) -> tuple[int, ...]:
    i0, i1, i2, i3, i4, i5, i6, i7 = d
    z1 = (i2 + i6) * _F0541
    tmp2 = z1 + i6 * (-_F1847)
    tmp3 = z1 + i2 * _F0765
    tmp0 = (i0 + i4) << _CONST_BITS
    tmp1 = (i0 - i4) << _CONST_BITS
    t10, t13 = tmp0 + tmp3, tmp0 - tmp3
    t11, t12 = tmp1 + tmp2, tmp1 - tmp2
    a0, a1, a2, a3 = i7, i5, i3, i1
    z1 = a0 + a3
    z2 = a1 + a2
    z3 = a0 + a2
    z4 = a1 + a3
    z5 = (z3 + z4) * _F1175
    a0 *= _F0298
    a1 *= _F2053
    a2 *= _F3072
    a3 *= _F1501
    z1 *= -_F0899
    z2 *= -_F2562
    z3 = z3 * -_F1961 + z5
    z4 = z4 * -_F0390 + z5
    a0 += z1 + z3
    a1 += z2 + z4
    a2 += z2 + z3
    a3 += z1 + z4
    return (
        _descale(t10 + a3, shift), _descale(t11 + a2, shift),
        _descale(t12 + a1, shift), _descale(t13 + a0, shift),
        _descale(t13 - a0, shift), _descale(t12 - a1, shift),
        _descale(t11 - a2, shift), _descale(t10 - a3, shift),
    )


def _idct_limit(v: int) -> int:
    # libjpeg post-IDCT range-limit table indexed by (v & 1023) from CENTERJSAMPLE.
    j = v & 1023
    if j < 128:
        return j + 128
    if j < 512:
        return 255
    if j < 896:
        return 0
    return j - 896


def idct_islow(coefs: list[int], q: list[int]) -> list[int]:
    ws = [0] * 64
    for col in range(8):
        d = [coefs[r * 8 + col] * q[r * 8 + col] for r in range(8)]
        out = _idct_1d(d, _CONST_BITS - _PASS1_BITS)
        for r in range(8):
            ws[r * 8 + col] = out[r]
    px = [0] * 64
    shift = _CONST_BITS + _PASS1_BITS + 3
    for r in range(8):
        out = _idct_1d(ws[r * 8 : r * 8 + 8], shift)
        for col in range(8):
            px[r * 8 + col] = _idct_limit(out[col])
    return px


def _component_plane(img: JpegCoefficients, ci: int) -> tuple[list[list[int]], int, int]:
    c = img.components[ci]
    q = img.qtables[c.tq]
    hmax = max(x.h for x in img.components)
    vmax = max(x.v for x in img.components)
    dw = -(-img.width * c.h // hmax)
    dh = -(-img.height * c.v // vmax)
    rows = [[0] * (c.blocks_w * 8) for _ in range(c.blocks_h * 8)]
    for by in range(c.blocks_h):
        for bx in range(c.blocks_w):
            px = idct_islow(c.coefs[by * c.alloc_w + bx], q)
            for r in range(8):
                rows[by * 8 + r][bx * 8 : bx * 8 + 8] = px[r * 8 : r * 8 + 8]
    return rows, dw, dh


def _upsample(rows, dw, dh, hexp, vexp, out_w, out_h):
    def row_at(r):
        return rows[0 if r < 0 else (dh - 1 if r >= dh else r)]

    if hexp == 1 and vexp == 1:
        return [rows[y][:out_w] for y in range(out_h)]
    if hexp == 2 and vexp == 1 and dw > 2:
        out = []
        for y in range(out_h):
            src = rows[y]
            o = [0] * (2 * dw)
            o[0] = src[0]
            o[1] = (src[0] * 3 + src[1] + 2) >> 2
            for x in range(1, dw - 1):
                t = src[x] * 3
                o[2 * x] = (t + src[x - 1] + 1) >> 2
                o[2 * x + 1] = (t + src[x + 1] + 2) >> 2
            o[2 * dw - 2] = (src[dw - 1] * 3 + src[dw - 2] + 1) >> 2
            o[2 * dw - 1] = src[dw - 1]
            out.append(o[:out_w])
        return out
    if hexp == 1 and vexp == 2:
        out = []
        for y in range(out_h):
            r, v = y >> 1, y & 1
            near = row_at(r)
            far = row_at(r - 1 if v == 0 else r + 1)
            bias = 1 if v == 0 else 2
            out.append([(near[x] * 3 + far[x] + bias) >> 2 for x in range(out_w)])
        return out
    if hexp == 2 and vexp == 2 and dw > 2:
        out = []
        for y in range(out_h):
            r, v = y >> 1, y & 1
            near = row_at(r)
            far = row_at(r - 1 if v == 0 else r + 1)
            cs = [near[x] * 3 + far[x] for x in range(dw)]
            o = [0] * (2 * dw)
            o[0] = (cs[0] * 4 + 8) >> 4
            o[1] = (cs[0] * 3 + cs[1] + 7) >> 4
            for x in range(1, dw - 1):
                o[2 * x] = (cs[x] * 3 + cs[x - 1] + 8) >> 4
                o[2 * x + 1] = (cs[x] * 3 + cs[x + 1] + 7) >> 4
            o[2 * dw - 2] = (cs[dw - 1] * 3 + cs[dw - 2] + 8) >> 4
            o[2 * dw - 1] = (cs[dw - 1] * 4 + 7) >> 4
            out.append(o[:out_w])
        return out
    # Box (integral-factor) replication: int_upsample, h2v1/h2v2 for narrow images.
    return [[rows[y // vexp][x // hexp] for x in range(out_w)] for y in range(out_h)]


def _clamp(v: int) -> int:
    return 0 if v < 0 else (255 if v > 255 else v)


def _fix(x: float) -> int:
    return int(x * 65536 + 0.5)


_CR_R = [(_fix(1.40200) * (i - 128) + 32768) >> 16 for i in range(256)]
_CB_B = [(_fix(1.77200) * (i - 128) + 32768) >> 16 for i in range(256)]
_CR_G = [(-_fix(0.71414)) * (i - 128) for i in range(256)]
_CB_G = [(-_fix(0.34414)) * (i - 128) + 32768 for i in range(256)]


def color_space(img: JpegCoefficients) -> str:
    n = len(img.components)
    if n == 1:
        return "gray"
    if n != 3:
        raise UnsupportedJpeg(f"{n} components")
    if img.saw_jfif:
        return "ycbcr"
    if img.adobe_transform >= 0:
        return "rgb" if img.adobe_transform == 0 else "ycbcr"
    ids = tuple(c.cid for c in img.components)
    if ids == (82, 71, 66):
        return "rgb"
    return "ycbcr"


def decode_pixels(data: bytes) -> tuple[int, int, int, bytes]:
    """Return (width, height, channels, interleaved 8-bit samples)."""
    img = decode_coefficients(data)
    hmax = max(c.h for c in img.components)
    vmax = max(c.v for c in img.components)
    planes = []
    for ci, c in enumerate(img.components):
        rows, dw, dh = _component_plane(img, ci)
        planes.append(_upsample(rows, dw, dh, hmax // c.h, vmax // c.v, img.width, img.height))
    w, h = img.width, img.height
    cs = color_space(img)
    if cs == "gray":
        return w, h, 1, bytes(v for row in planes[0] for v in row)
    out = bytearray(w * h * 3)
    o = 0
    for y in range(h):
        p0, p1, p2 = planes[0][y], planes[1][y], planes[2][y]
        for x in range(w):
            if cs == "rgb":
                out[o], out[o + 1], out[o + 2] = p0[x], p1[x], p2[x]
            else:
                yy, cb, cr = p0[x], p1[x], p2[x]
                out[o] = _clamp(yy + _CR_R[cr])
                out[o + 1] = _clamp(yy + ((_CB_G[cb] + _CR_G[cr]) >> 16))
                out[o + 2] = _clamp(yy + _CB_B[cb])
            o += 3
    return w, h, 3, bytes(out)
