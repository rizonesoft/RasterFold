"""Independent RasterFold .fold (version 2) codec and 24-bit BMP helpers.

This module is the test oracle, written from spec/FOLD.md. It is not the
codec implementation. It is slow: a few hundred thousand samples per minute.
"""

from __future__ import annotations

import struct
import zlib

MAGIC = b"RFI\x03"
VERSION = 3
SUPPORTED_VERSIONS = (2, 3)
HEADER = 32
MAX_DIM = 32768
MAX_PIXELS = 32 * 1024 * 1024
MAX_FILE = 256 * 1024 * 1024
METHOD_RAW = 0
METHOD_CM = 1
METHOD_PAL = 2
METHOD_PAL2 = 3


class FormatError(Exception):
    pass


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------- tables

def _squash(d: int) -> int:
    t = (1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047, 2549, 2994, 3348,
         3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094)
    if d > 2047:
        return 4095
    if d < -2047:
        return 1
    w = d & 127
    i = (d >> 7) + 16
    return (t[i] * (128 - w) + t[i + 1] * w + 64) >> 7


SQUASH = [_squash(x) for x in range(-2047, 2048)]          # index x + 2047
STRETCH = [0] * 4096
_pi = 0
for _x in range(-2047, 2048):
    _v = _squash(_x)
    for _j in range(_pi, _v + 1):
        STRETCH[_j] = _x
    _pi = _v + 1
for _j in range(_pi, 4096):
    STRETCH[_j] = 2047
RATE = [(65536 * 2) // (2 * n + 3) for n in range(61)]
DIVL = [(1 << 24) // (i + 1) for i in range(64)]


def _ilog2(v: int) -> int:
    return max(v, 1).bit_length() - 1


def _errw(e: int, maxw: int) -> int:
    sh = _ilog2(e + 1) - 5
    if sh < 0:
        sh = 0
    return 4 + ((maxw * DIVL[e >> sh]) >> sh)


def _qlog(v: int) -> int:
    if v <= 0:
        return 0
    if v >= 16384:
        return 15
    return _ilog2(v) + 1


def _count(a: int, thresholds) -> int:
    return sum(1 for t in thresholds if a >= t)


def _signed(d: int, q: int, mid: int) -> int:
    return mid + q if d > 0 else mid - q if d < 0 else mid


def _q9(v: int) -> int:
    return _signed(v, _count(abs(v), (1, 2, 4, 8)), 4)


def _q7(d: int) -> int:
    return _signed(d, _count(abs(d), (1, 3, 7)), 3)


def _qj(d: int) -> int:
    return _signed(d, _count(abs(d), (1, 3, 7, 21)), 4)


def _qn7(d: int, scl: int) -> int:
    a = 6 * abs(d)
    return _signed(d, _count(a, (scl, 3 * scl, 7 * scl)), 3)


def _med3(w: int, n: int, nw: int) -> int:
    mx, mn = max(w, n), min(w, n)
    if nw >= mx:
        return mn
    if nw <= mn:
        return mx
    return w + n - nw


def _clamp(v: int, lo: int, hi: int) -> int:
    return lo if v < lo else hi if v > hi else v


def _wrap32(v: int) -> int:
    return ((v + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)


# ---------------------------------------------------------------- range coder

class RangeEncoder:
    def __init__(self) -> None:
        self.low = 0
        self.range = 0xFFFFFFFF
        self.cache = 0
        self.pending = 1
        self.out = bytearray()

    def _shift(self) -> None:
        if (self.low & 0xFFFFFFFF) < 0xFF000000 or (self.low >> 32) != 0:
            carry = self.low >> 32
            b = self.cache
            while True:
                self.out.append((b + carry) & 0xFF)
                b = 0xFF
                self.pending -= 1
                if self.pending == 0:
                    break
            self.cache = (self.low >> 24) & 0xFF
        self.pending += 1
        self.low = (self.low & 0x00FFFFFF) << 8

    def bit(self, bit: int, p1: int) -> None:
        bound = (self.range >> 12) * p1
        if bit:
            self.range = bound
        else:
            self.low += bound
            self.range -= bound
        while self.range < (1 << 24):
            self.range = (self.range << 8) & 0xFFFFFFFF
            self._shift()

    def finish(self) -> bytes:
        for _ in range(5):
            self._shift()
        return bytes(self.out)


class RangeDecoder:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0
        self.range = 0xFFFFFFFF
        self.code = 0
        for _ in range(5):
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF

    def _byte(self) -> int:
        b = self.data[self.pos] if self.pos < len(self.data) else 0
        self.pos += 1
        return b

    def bit(self, p1: int) -> int:
        bound = (self.range >> 12) * p1
        if self.code < bound:
            self.range = bound
            bit = 1
        else:
            self.code -= bound
            self.range -= bound
            bit = 0
        while self.range < (1 << 24):
            self.range = (self.range << 8) & 0xFFFFFFFF
            self.code = ((self.code << 8) | self._byte()) & 0xFFFFFFFF
        return bit

    def exhausted_exactly(self) -> bool:
        return self.pos == len(self.data)


# ---------------------------------------------------------------- CM model

NMODEL = 7          # models written by this implementation (version 3)
NMODEL_V2 = 5       # models in a version 2 file
NROWS = 2048
NODES = 32
LMSN = 12


class Channel:
    """Model state of one channel (G, R-G or B-G) and its coder."""

    def __init__(self, w: int, nmodel: int = NMODEL) -> None:
        self.w = w
        self.nmodel = nmodel
        self.ctr = [[0 if node == 0 else 32768 for node in range(NODES)] for _ in range(nmodel * NROWS)]
        self.wt = [[19661] * nmodel + [0] for _ in range(128 * NODES)]
        self.lms = [0] * LMSN
        # weighted predictor state, two rows of w + 2 positions (JPEG XL layout)
        self.perr = [[0, 0, 0, 0] for _ in range(2 * (w + 2))]
        self.err = [0] * (2 * (w + 2))
        self.er = [0] * (2 * (w + 2))
        self.fe = [0] * (2 * (w + 2))
        self.plane: list[list[int]] = []       # plane rows (values minus the G base)
        self.enc: RangeEncoder | None = None
        self.dec: RangeDecoder | None = None
        self.bad = False

    # one decision
    def code_bit(self, rows, rates, wrow, node: int, bit: int) -> int:
        nm = self.nmodel
        w = self.wt[wrow + node]
        cs = [self.ctr[r] for r in rows]
        st = [STRETCH[c[node] >> 4] for c in cs]
        dot = sum(w[k] * st[k] for k in range(nm)) + w[nm] * 64
        x = _clamp(dot >> 16, -2047, 2047)
        p = SQUASH[x + 2047]
        if self.enc is not None:
            self.enc.bit(bit, p)
        else:
            bit = self.dec.bit(p)
        err = (bit << 12) - p
        for k in range(nm):
            w[k] = _wrap32(w[k] + ((st[k] * err + 4096) >> 13))
        w[nm] = _wrap32(w[nm] + ((64 * err + 4096) >> 13))
        target = 65535 if bit else 0
        for k in range(nm):
            c = cs[k]
            c[node] += ((target - c[node]) * rates[k]) >> 16
        return bit

    def code_raw(self, bit: int) -> int:
        if self.enc is not None:
            self.enc.bit(bit, 2048)
            return bit
        return self.dec.bit(2048)

    def code_residual(self, rows, rates, wrow, r: int) -> int:
        enc = self.enc is not None
        if not self.code_bit(rows, rates, wrow, 1, 1 if r != 0 else 0):
            return 0
        neg = self.code_bit(rows, rates, wrow, 2, 1 if r < 0 else 0)
        e = got = 0
        if enc:
            a = abs(r)
            e = _ilog2(a)
            for k in range(e):
                self.code_bit(rows, rates, wrow, 3 + k, 1)
            if e < 7:
                self.code_bit(rows, rates, wrow, 3 + e, 0)
            rem = a - (1 << e)
            for b in range(e - 1, -1, -1):
                bit = (rem >> b) & 1
                if b == e - 1:
                    self.code_bit(rows, rates, wrow, 10 + e, bit)
                elif b == e - 2:
                    self.code_bit(rows, rates, wrow, 14 + 2 * e + (got & 1), bit)
                else:
                    self.code_raw(bit)
                got = (got << 1) | bit
            return r
        while e < 7 and self.code_bit(rows, rates, wrow, 3 + e, 0):
            e += 1
        for b in range(e - 1, -1, -1):
            if b == e - 1:
                bit = self.code_bit(rows, rates, wrow, 10 + e, 0)
            elif b == e - 2:
                bit = self.code_bit(rows, rates, wrow, 14 + 2 * e + (got & 1), 0)
            else:
                bit = self.code_raw(0)
            got = (got << 1) | bit
        a = (1 << e) + got
        return -a if neg else a


def _code_sample(ch: Channel, c: int, x: int, y: int, value: int, cross: dict) -> int:
    """Model and code sample (x, y) of channel c. cross holds the G (and R)
    value, residual and clamped final error at (x, y). Returns the channel value."""
    w = ch.w
    cur = 0 if y & 1 else w + 2
    prv = w + 2 if y & 1 else 0
    r0 = ch.plane[y]
    g = eg = feg = er_r = fer = 0
    if c >= 1:
        g, eg, feg = cross["g"], cross["eg"], cross["feg"]
    if c == 2:
        er_r, fer = cross["er"], cross["fer"]

    # neighbours
    if y == 0:
        W = r0[x - 1] if x > 0 else 0
        N = NW = NE = NN = NNE = W
    else:
        r1 = ch.plane[y - 1]
        N = r1[x]
        W = r0[x - 1] if x > 0 else N
        NW = r1[x - 1] if x > 0 else N
        NE = r1[x + 1] if x + 1 < w else N
        NN = ch.plane[y - 2][x] if y > 1 else N
        NNE = ch.plane[y - 2][x + 1] if (y > 1 and x + 1 < w) else NE
    pN = prv + x
    pNE = pN + 1 if x + 1 < w else pN
    pNW = pN - 1 if x > 0 else pN

    # weighted predictor
    wt = [_errw(ch.perr[pN][k] + ch.perr[pNE][k] + ch.perr[pNW][k], 13 if k == 0 else 12) for k in range(4)]
    n8, w8, ne8 = N * 8, W * 8, NE * 8
    teW = ch.err[cur + x - 1] if x > 0 else 0
    teN, teNW, teNE = ch.err[pN], ch.err[pNW], ch.err[pNE]
    sumWN = teN + teW
    pr = [w8 + ne8 - n8,
          n8 - (((sumWN + teNE) * 16) >> 5),
          w8 - (((sumWN + teNW) * 10) >> 5),
          n8 - (((teNW + teN + teNE) * 7) >> 5)]
    ws = sum(wt)
    lw = _ilog2(ws) - 4
    wt = [v >> lw for v in wt]
    ws = sum(wt)
    total = (ws >> 1) - 1 + sum(pr[k] * wt[k] for k in range(4))
    wpred = (total * DIVL[ws - 1]) >> 24
    if ((teN ^ teW) | (teN ^ teNW)) <= 0:
        wpred = _clamp(wpred, min(w8, ne8, n8), max(w8, ne8, n8))

    # LMS correction
    lin = [ch.fe[cur + x - 1] if x > 0 else 0,
           ch.fe[prv + x] if y > 0 else 0,
           ch.fe[prv + x - 1] if (x > 0 and y > 0) else 0,
           ch.fe[prv + x + 1] if (y > 0 and x + 1 < w) else 0,
           ch.fe[cur + x - 2] if x > 1 else 0,
           feg, fer,
           (N - W) * 8, (NE - N) * 8, (NW - W) * 8, (NN - N) * 8, (NNE - NE) * 8]
    lin = [_clamp(v, -2047, 2047) for v in lin]
    corr = _clamp(sum(ch.lms[k] * lin[k] for k in range(LMSN)) >> 16, -256, 256)
    pred8 = wpred + corr
    pred = (pred8 + 4) >> 3
    predv = _clamp(pred + g, 0, 255)

    # contexts
    if x > 0:
        eW = ch.er[cur + x - 1]
    else:
        eW = ch.er[prv + x] if y > 0 else 0
    eN = ch.er[prv + x] if y > 0 else eW
    eNW = ch.er[prv + x - 1] if (x > 0 and y > 0) else eN
    eNE = ch.er[prv + x + 1] if (y > 0 and x + 1 < w) else eN
    E = 2 * abs(eW) + 2 * abs(eN) + abs(eNW) + abs(eNE)
    qE = _qlog(E)
    gq = _q9(eg) if c else 4
    rq = _q9(er_r) if c == 2 else 4
    scl = (E >> 2) + 2
    band = 0 if qE < 4 else 1 if qE < 8 else 2
    k0 = (((_qn7(W - pred, scl) * 7 + _qn7(N - pred, scl)) * 7 + _qn7(NE - pred, scl)) * 7 + _qn7(NW - pred, scl)) * 3 + band
    k1 = qE * 81 + gq * 9 + rq
    k2 = (_qj(NE - N) * 81 + _qj(N - NW) * 9 + _qj(NW - W)) * 5 + (gq + 1) // 2
    k3 = ((_q7(_med3(W, N, NW) - pred) * 7 + _q7(W + N - NW - pred)) * 7 + _q7(((W + NE + 1) >> 1) - pred)) * 7 + _q7(N + NE - NNE - pred)
    k4 = (_q9(W - pred) * 9 + _q9(N - pred)) * 16 + qE
    if ch.nmodel == NMODEL:
        WW = r0[x - 2] if x > 1 else W
        k5 = (_q9(NE - pred) * 9 + _q9(NW - pred)) * 16 + qE
        k6 = ((_qn7(WW - pred, scl) * 7 + _qn7(NN - pred, scl)) * 7 + _qn7(NNE - pred, scl)) * 16 + qE
        ks = (k0, k1, k2, k3, k4, k5, k6)
    else:
        ks = (k0, k1, k2, k3, k4)
    rows = [k * NROWS + kk % NROWS for k, kk in enumerate(ks)]
    rates = []
    for r in rows:
        n = ch.ctr[r][0]
        rates.append(RATE[n])
        ch.ctr[r][0] = n + (1 if n < 60 else 0)
    wsel = qE * 8 + ((0 if gq == 4 else 1 + (1 if abs(eg) > 3 else 0)) if c else 3)
    wrow = wsel * NODES

    # code
    if ch.enc is not None:
        r = (value - predv) & 255
        if r >= 128:
            r -= 256
        ch.code_residual(rows, rates, wrow, r)
    else:
        r = ch.code_residual(rows, rates, wrow, 0)
        if r == 128:
            ch.bad = True
        value = (predv + r) & 255

    # update
    pv = value - g
    r0[x] = pv
    ferr = pv * 8 - pred8
    sg = 10 if ferr > 0 else -10 if ferr < 0 else 0
    for k in range(LMSN):
        s = sg if lin[k] > 0 else -sg if lin[k] < 0 else 0
        ch.lms[k] = _clamp(ch.lms[k] + s, -65536, 65536)
    v8 = pv * 8
    ch.err[cur + x] = wpred - v8
    for k in range(4):
        e = (abs(pr[k] - v8) + 3) >> 3
        ch.perr[cur + x][k] = e
        ch.perr[prv + x + 1][k] += e
    ch.er[cur + x] = r
    ch.fe[cur + x] = ferr
    cross["r"] = ((r + 128) & 255) - 128                 # stored as a signed byte
    cross["fe"] = _clamp(ferr, -2047, 2047)
    return value


def _run(width: int, height: int, chans: list[Channel], rgb: bytearray) -> None:
    for y in range(height):
        for ch in chans:
            ch.plane.append([0] * width)
        for x in range(width):
            i = (y * width + x) * 3
            gs: dict = {}
            g = _code_sample(chans[0], 0, x, y, rgb[i + 1], gs)
            rgb[i + 1] = g
            cross = {"g": g, "eg": gs["r"], "feg": gs["fe"]}
            rs = dict(cross)
            rgb[i] = _code_sample(chans[1], 1, x, y, rgb[i], rs)
            cross["er"], cross["fer"] = rs["r"], rs["fe"]
            rgb[i + 2] = _code_sample(chans[2], 2, x, y, rgb[i + 2], dict(cross))
        if y >= 2:
            for ch in chans:
                ch.plane[y - 2] = []                        # rows below y - 1 are no longer read


def cm_encode(width: int, height: int, rgb: bytes) -> bytes:
    chans = [Channel(width, NMODEL) for _ in range(3)]
    for ch in chans:
        ch.enc = RangeEncoder()
    _run(width, height, chans, bytearray(rgb))
    streams = [ch.enc.finish() for ch in chans]
    return b"".join(struct.pack("<I", len(s)) for s in streams) + b"".join(streams)


def cm_decode(payload: bytes, width: int, height: int, version: int = VERSION) -> bytes:
    if len(payload) < 12:
        raise FormatError("cm streams")
    sizes = struct.unpack_from("<III", payload, 0)
    if any(s < 5 for s in sizes) or 12 + sum(sizes) != len(payload):
        raise FormatError("cm streams")
    chans = [Channel(width, NMODEL if version == 3 else NMODEL_V2) for _ in range(3)]
    pos = 12
    for ch, size in zip(chans, sizes):
        ch.dec = RangeDecoder(payload[pos:pos + size])
        pos += size
    rgb = bytearray(width * height * 3)
    _run(width, height, chans, rgb)
    if any(ch.bad or not ch.dec.exhausted_exactly() for ch in chans):
        raise FormatError("cm stream")
    return bytes(rgb)


# ---------------------------------------------------------------- palette (methods 2 and 3)

class _PalCounter:
    def __init__(self, m3: bool = True) -> None:
        self.c = 32768
        self.n = 0
        self.m3 = m3

    def code(self, enc: RangeEncoder | None, dec: RangeDecoder | None, bit: int = 0) -> int:
        p = max(1, self.c >> 4)
        rate = RATE[self.n]
        if self.n < 60:
            self.n += 1
        if enc is not None:
            enc.bit(bit, p)
        else:
            assert dec is not None
            bit = dec.bit(p)
        if self.m3:
            if bit == 1 and self.c < 65535:
                self.c += max(1, ((65535 - self.c) * rate) >> 16)
            elif bit == 0 and self.c > 0:
                self.c -= max(1, (self.c * rate) >> 16)
        else:
            target = 65535 if bit else 0
            self.c += ((target - self.c) * rate) >> 16
        return bit


def _pal_neighbours_m2(rows: list[list[int]], width: int, x: int, y: int) -> tuple[int, int, int, int, int]:
    if y == 0:
        a = rows[0][x - 1] if x > 0 else 0
        b = c = d = a
    else:
        prv = rows[y - 1]
        b = prv[x]
        a = rows[y][x - 1] if x > 0 else b
        c = prv[x - 1] if x > 0 else b
        d = prv[x + 1] if x + 1 < width else b
    pattern = (
        (1 if a == b else 0)
        | ((1 if a == c else 0) << 1)
        | ((1 if a == d else 0) << 2)
        | ((1 if b == c else 0) << 3)
        | ((1 if b == d else 0) << 4)
    )
    return a, b, c, d, pattern


def _pal_neighbours_m3(rows: list[list[int]], width: int, x: int, y: int) -> tuple[int, int, int, int]:
    if y == 0:
        a = rows[0][x - 1] if x > 0 else 0
        b = c = d = a
        nn = a
    else:
        prv = rows[y - 1]
        b = prv[x]
        a = rows[y][x - 1] if x > 0 else b
        c = prv[x - 1] if x > 0 else b
        d = prv[x + 1] if x + 1 < width else b
        nn = b if y == 1 else rows[y - 2][x]
    ww = rows[y][x - 2] if x >= 2 else a
    if d != a and d != b:
        e = d
    elif c != a and c != b:
        e = c
    elif ww != a and ww != b:
        e = ww
    else:
        e = a
    pattern = (
        (1 if a == b else 0)
        | ((1 if a == c else 0) << 1)
        | ((1 if a == d else 0) << 2)
        | ((1 if b == c else 0) << 3)
        | ((1 if b == d else 0) << 4)
        | ((1 if ww == a else 0) << 5)
        | ((1 if nn == b else 0) << 6)
    )
    return a, b, e, pattern


def pal_encode(width: int, height: int, rgb: bytes, *, method: int = METHOD_PAL2) -> bytes | None:
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    if method not in (METHOD_PAL, METHOD_PAL2):
        raise ValueError("palette method")
    palette: list[bytes] = []
    index_of: dict[bytes, int] = {}
    for pos in range(0, len(rgb), 3):
        col = rgb[pos : pos + 3]
        if col not in index_of:
            if len(palette) == 256:
                return None
            index_of[col] = len(palette)
            palette.append(col)
    count = len(palette)
    m3 = method == METHOD_PAL2
    d1 = [_PalCounter(m3) for _ in range(256 if m3 else 64)]
    d2 = [_PalCounter(m3) for _ in range(128 if m3 else 32)]
    d3 = [_PalCounter(m3) for _ in range(128 if m3 else 32)]
    tree = [_PalCounter(m3) for _ in range(256)]
    enc = RangeEncoder()
    rows: list[list[int]] = []
    hit = 0
    for y in range(height):
        rows.append([0] * width)
        for x in range(width):
            pos = (y * width + x) * 3
            idx = index_of[rgb[pos : pos + 3]]
            rows[y][x] = idx
            if m3:
                a, b, e, pattern = _pal_neighbours_m3(rows, width, x, y)
            else:
                a, b, _c, e, pattern = _pal_neighbours_m2(rows, width, x, y)
            if d1[pattern * 2 + hit].code(enc, None, 1 if idx == a else 0):
                hit = 1
                continue
            hit = 0
            if m3 and count == 2:
                continue
            if b != a and d2[pattern].code(enc, None, 1 if idx == b else 0):
                continue
            if e != a and e != b and d3[pattern].code(enc, None, 1 if idx == e else 0):
                continue
            node = 1
            for bit_pos in range(7, -1, -1):
                bit = (idx >> bit_pos) & 1
                tree[node].code(enc, None, bit)
                node = node * 2 + bit
        if y >= 3:
            rows[y - 3] = []
    stream = enc.finish()
    return struct.pack("<H", count - 1) + b"".join(palette) + struct.pack("<I", len(stream)) + stream


def pal_decode(payload: bytes, width: int, height: int, *, method: int = METHOD_PAL2) -> bytes:
    if method not in (METHOD_PAL, METHOD_PAL2):
        raise ValueError("palette method")
    if len(payload) < 6:
        raise FormatError("palette")
    count_m1 = struct.unpack_from("<H", payload, 0)[0]
    if count_m1 > 255:
        raise FormatError("palette")
    count = count_m1 + 1
    pal_end = 2 + 3 * count
    if len(payload) < pal_end + 4:
        raise FormatError("palette")
    palette: list[bytes] = []
    seen: set[bytes] = set()
    for i in range(count):
        col = payload[2 + 3 * i : 2 + 3 * (i + 1)]
        if col in seen:
            raise FormatError("palette")
        seen.add(col)
        palette.append(col)
    stream_size = struct.unpack_from("<I", payload, pal_end)[0]
    if stream_size < 5 or pal_end + 4 + stream_size != len(payload):
        raise FormatError("palette")
    dec = RangeDecoder(payload[pal_end + 4 :])
    m3 = method == METHOD_PAL2
    d1 = [_PalCounter(m3) for _ in range(256 if m3 else 64)]
    d2 = [_PalCounter(m3) for _ in range(128 if m3 else 32)]
    d3 = [_PalCounter(m3) for _ in range(128 if m3 else 32)]
    tree = [_PalCounter(m3) for _ in range(256)]
    rows: list[list[int]] = []
    rgb = bytearray(width * height * 3)
    hit = 0
    for y in range(height):
        rows.append([0] * width)
        for x in range(width):
            if m3:
                a, b, e, pattern = _pal_neighbours_m3(rows, width, x, y)
            else:
                a, b, _c, e, pattern = _pal_neighbours_m2(rows, width, x, y)
            if d1[pattern * 2 + hit].code(None, dec) == 1:
                idx = a
                hit = 1
            elif m3 and count == 2:
                idx = 1 - a
                hit = 0
            else:
                hit = 0
                if b != a and d2[pattern].code(None, dec) == 1:
                    idx = b
                elif e != a and e != b and d3[pattern].code(None, dec) == 1:
                    idx = e
                else:
                    node = 1
                    for _ in range(8):
                        bit = tree[node].code(None, dec)
                        node = node * 2 + bit
                    idx = node - 256
                    if idx >= count or idx == a or idx == b or idx == e:
                        raise FormatError("palette stream")
            rows[y][x] = idx
            pos = (y * width + x) * 3
            rgb[pos : pos + 3] = palette[idx]
        if dec.pos > len(dec.data) + 2:
            raise FormatError("palette stream")
        if y >= 3:
            rows[y - 3] = []
    if not dec.exhausted_exactly():
        raise FormatError("palette stream")
    return bytes(rgb)


# ---------------------------------------------------------------- container

def build_file(width: int, height: int, method: int, payload: bytes, reserved: int = 0) -> bytes:
    header = bytearray(HEADER)
    header[0:4] = MAGIC
    struct.pack_into("<HH", header, 4, VERSION, HEADER)
    struct.pack_into("<II", header, 8, width, height)
    header[16] = method
    header[17] = reserved
    struct.pack_into("<H", header, 18, 1)
    struct.pack_into("<II", header, 20, len(payload), crc32(payload))
    struct.pack_into("<I", header, 28, crc32(bytes(header[:28])))
    return bytes(header) + payload


def encode_image(width: int, height: int, rgb: bytes) -> bytes:
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    best_method = METHOD_RAW
    best_payload = rgb
    cm_payload = cm_encode(width, height, rgb)
    cm_sizes = struct.unpack_from("<III", cm_payload, 0)
    if all(s <= width * height + 64 for s in cm_sizes) and len(cm_payload) < len(best_payload):
        best_method = METHOD_CM
        best_payload = cm_payload
    pal_payload = pal_encode(width, height, rgb, method=METHOD_PAL2)
    if pal_payload is not None and len(pal_payload) < len(best_payload):
        best_method = METHOD_PAL2
        best_payload = pal_payload
    return build_file(width, height, best_method, best_payload)


def decode_image(blob: bytes) -> tuple[int, int, bytes]:
    if len(blob) < HEADER:
        raise FormatError("truncated")
    if len(blob) > MAX_FILE:
        raise FormatError("file size")
    if blob[0:3] != b"RFI":
        raise FormatError("magic")
    version, header_size = struct.unpack_from("<HH", blob, 4)
    if blob[3] != version or version not in SUPPORTED_VERSIONS:
        raise FormatError("version")
    if header_size != HEADER:
        raise FormatError("header size")
    if struct.unpack_from("<H", blob, 18)[0] != 1:
        raise FormatError("pixel format")
    if crc32(blob[:28]) != struct.unpack_from("<I", blob, 28)[0]:
        raise FormatError("header checksum")
    method, reserved = blob[16], blob[17]
    if method > METHOD_PAL2:
        raise FormatError("method")
    if reserved != 0:
        raise FormatError("reserved byte")
    width, height = struct.unpack_from("<II", blob, 8)
    if not (1 <= width <= MAX_DIM and 1 <= height <= MAX_DIM) or width * height > MAX_PIXELS:
        raise FormatError("dimensions")
    size, pcrc = struct.unpack_from("<II", blob, 20)
    if size != len(blob) - HEADER:
        raise FormatError("payload length")
    payload = blob[HEADER:]
    if crc32(payload) != pcrc:
        raise FormatError("payload checksum")
    if method == METHOD_RAW:
        if size != width * height * 3:
            raise FormatError("raw length")
        return width, height, bytes(payload)
    if method in (METHOD_PAL, METHOD_PAL2):
        return width, height, pal_decode(payload, width, height, method=method)
    return width, height, cm_decode(payload, width, height, version)


# ---------------------------------------------------------------- BMP

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
    bf_size, _r1, _r2, off = struct.unpack_from("<IHHI", blob, 2)
    header = struct.unpack_from("<IiiHHIIiiII", blob, 14)
    bi_size, width, height, planes, bits, comp, sizeimage, _x, _y, _used, _imp = header
    if bi_size != 40 or planes != 1 or bits != 24 or comp != 0:
        raise FormatError("bmp subset")
    if width <= 0 or height == 0 or height == -2147483648:
        raise FormatError("bmp dimensions")
    topdown = height < 0
    height = abs(height)
    if width > MAX_DIM or height > MAX_DIM or width * height > MAX_PIXELS:
        raise FormatError("bmp size limit")
    stride = row_stride(width)
    need = off + stride * height
    if off < 54 or need > len(blob):
        raise FormatError("bmp truncated")
    if bf_size not in (0,) and (bf_size < need or bf_size > len(blob)):
        raise FormatError("bmp file size")
    if sizeimage not in (0,) and sizeimage < stride * height:
        raise FormatError("bmp image size")
    rgb = bytearray(width * height * 3)
    for file_y in range(height):
        img_y = file_y if topdown else height - 1 - file_y
        src = off + file_y * stride
        row = blob[src : src + width * 3]
        dst = img_y * width * 3
        rgb[dst : dst + width * 3] = row
        rgb[dst : dst + width * 3 : 3], rgb[dst + 2 : dst + width * 3 : 3] = row[2::3], row[0::3]
    return width, height, bytes(rgb)
