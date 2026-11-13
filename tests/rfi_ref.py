"""Independent RasterFold .fold (versions 2 and 3) codec and 24-bit BMP helpers.

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
METHOD_PAL3 = 4      # compact palette, version 3 only
METHOD_SC = 5        # screen content, version 3 only
METHOD_SC2 = 6       # screen content v2, version 3 only


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
RATE_V3 = [(65536 * 2) // (2 * n + 7) for n in range(73)]
COLD_CTR_V3 = (0, 32768, 32768, 30000, 28000, 24000, 18000, 12000, 6000, 29000, 29000, 29000, 29000, 30000, 30000, 30000)
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

    def bit16(self, bit: int, p1: int) -> None:
        # Method 4: probability p1 / 65536 with an exact split.
        bound = (self.range * p1) >> 16
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

    def bit16(self, p1: int) -> int:
        # Method 4: probability p1 / 65536 with an exact split.
        bound = (self.range * p1) >> 16
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
NODES = 16          # nodes per row in version 3
NODES_V2 = 32       # nodes per row in a version 2 file
LMSN = 12

# Gated gradient (version 3) boundary counts, accumulated by every coded sample;
# tests reset and read it to show which parts of the selector a file exercised.
GG_STATS: dict[str, int] = {}


def _gg_count(key: str) -> None:
    GG_STATS[key] = GG_STATS.get(key, 0) + 1


def _gg_dist(v: int, p: int) -> int:
    m = (v - p) % 256
    return min(m, 256 - m, 32)


class Channel:
    """Model state of one channel (G, R-G or B-G) and its coder."""

    def __init__(self, w: int, nmodel: int = NMODEL, c: int = 0) -> None:
        self.w = w
        self.nmodel = nmodel
        self.nodes = NODES if nmodel == NMODEL else NODES_V2
        if nmodel == NMODEL:
            self.ctr = [list(COLD_CTR_V3) for _ in range(nmodel * NROWS)]
            self.lms = [20000 if (c == 2 and k == 6) else 0 for k in range(LMSN)]
        else:
            self.ctr = [[0 if node == 0 else 32768 for node in range(self.nodes)] for _ in range(nmodel * NROWS)]
            self.lms = [0] * LMSN
        self.wt = [[19661] * nmodel + [0] for _ in range(128 * self.nodes)]
        # weighted predictor state, two rows of w + 2 positions (JPEG XL layout)
        self.perr = [[0, 0, 0, 0] for _ in range(2 * (w + 2))]
        self.err = [0] * (2 * (w + 2))
        self.er = [0] * (2 * (w + 2))
        self.fe = [0] * (2 * (w + 2))
        self.plane: list[list[int]] = []       # plane rows (values minus the G base)
        self.vals: list[list[int]] = []        # version 3: channel value rows V
        self.exs: list[list[int]] = []         # version 3: ex rows
        self.eg = 0                            # version 3: Eg and Et
        self.et = 0
        self.enc: RangeEncoder | None = None
        self.dec: RangeDecoder | None = None
        self.bad = False

    def copy_warm_from(self, src: Channel) -> None:
        for r, src_row in enumerate(src.ctr):
            row = src_row.copy()
            if row[0] > 24:
                row[0] = 24
            self.ctr[r] = row
        self.wt = [w.copy() for w in src.wt]
        self.lms = src.lms.copy()
        self.eg = src.eg
        self.et = src.et

    def begin_row(self) -> None:
        self.plane.append([0] * self.w)
        self.vals.append([0] * self.w)
        self.exs.append([0] * self.w)

    def drop_row(self, y: int) -> None:
        """Rows below y + 1 are no longer read."""
        self.plane[y] = []
        self.vals[y] = []
        self.exs[y] = []

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
        v3 = self.nodes == NODES
        if not self.code_bit(rows, rates, wrow, 1, 1 if r != 0 else 0):
            return 0
        neg = self.code_bit(rows, rates, wrow, 2, 1 if r < 0 else 0)
        e = got = 0
        if enc:
            a = abs(r)
            e = _ilog2(a)
            for k in range(e):
                unode = (3 + k if k < 5 else 8) if v3 else (3 + k)
                self.code_bit(rows, rates, wrow, unode, 1)
            if e < 7:
                unode = (3 + e if e < 5 else 8) if v3 else (3 + e)
                self.code_bit(rows, rates, wrow, unode, 0)
            rem = a - (1 << e)
            for b in range(e - 1, -1, -1):
                bit = (rem >> b) & 1
                if b == e - 1:
                    mnode = (8 + min(e, 4)) if v3 else (10 + e)
                    self.code_bit(rows, rates, wrow, mnode, bit)
                elif b == e - 2:
                    mnode = (13 + (got & 1) if e == 2 else 15) if v3 else (14 + 2 * e + (got & 1))
                    self.code_bit(rows, rates, wrow, mnode, bit)
                else:
                    self.code_raw(bit)
                got = (got << 1) | bit
            return r
        while e < 7:
            unode = (3 + e if e < 5 else 8) if v3 else (3 + e)
            if not self.code_bit(rows, rates, wrow, unode, 0):
                break
            e += 1
        for b in range(e - 1, -1, -1):
            if b == e - 1:
                mnode = (8 + min(e, 4)) if v3 else (10 + e)
                bit = self.code_bit(rows, rates, wrow, mnode, 0)
            elif b == e - 2:
                mnode = (13 + (got & 1) if e == 2 else 15) if v3 else (14 + 2 * e + (got & 1))
                bit = self.code_bit(rows, rates, wrow, mnode, 0)
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
    if ch.nmodel == NMODEL:
        pr = [w8 + (((ne8 - n8) * 3) >> 3),
              n8 - (((sumWN + teNE) * 12) >> 5),
              w8 - (((sumWN + teNW) * 8) >> 5),
              n8 - (((teNW + teN + teNE) * 5) >> 5)]
    else:
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

    # gated gradient (version 3)
    pt = predv
    gp = None
    gate = use = False
    form = ""
    if ch.nmodel == NMODEL:
        V, EX = ch.vals, ch.exs
        if x > 0 and y > 0:
            form = "interior"
            gp = (V[y][x - 1] + V[y - 1][x] - V[y - 1][x - 1]) % 256
            gate = bool(EX[y][x - 1] and EX[y - 1][x] and EX[y - 1][x - 1] and (x + 1 == w or EX[y - 1][x + 1]))
        elif x == 0 and y >= 2:
            form = "col0"
            gp = (2 * V[y - 1][0] - V[y - 2][0]) % 256
            gate = bool(EX[y - 1][0] and (w == 1 or EX[y - 1][1]) and EX[y - 2][0])
        elif y == 0 and x >= 2:
            form = "row0"
            gp = (2 * V[0][x - 1] - V[0][x - 2]) % 256
            gate = bool(EX[0][x - 1] and EX[0][x - 2])
        use = gate and 2 * ch.eg < ch.et
        if gp is not None:
            if not gate:
                _gg_count("closed")
            else:
                _gg_count(f"open-{form}")
                if ch.eg == 0 and ch.et == 0:
                    _gg_count("empty-start")
                elif 2 * ch.eg == ch.et:
                    _gg_count("tie-keeps-pt")
                elif ch.eg < ch.et <= 2 * ch.eg:
                    _gg_count("margin-keeps-pt")
                if use:
                    _gg_count("gp-chosen")
                    _gg_count(f"gp-chosen-{form}")
    pc = gp if use else pt

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
    band = 0 if qE < 4 else 1 if qE < 8 else 2
    q2d = _qj(NE - N) * 81 + _qj(N - NW) * 9 + _qj(NW - W)
    if ch.nmodel == NMODEL:
        scl = (E >> 2) + 3
        cq = rq if c == 2 else gq
        qe_c = ((qE >> 1) * 2) + (1 if cq != 4 else 0) if c >= 1 else qE
        k0 = (_qn7(W - pred, scl) * 195 + _qn7(N - pred, scl) * 31 + _qn7(NE - pred, scl) * 5 + _qn7(NW - pred, scl)) * 3 + band
        k1 = qE if c == 0 else qE * 9 + gq if c == 1 else qE * 81 + gq * 9 + rq
        k2 = q2d if c == 0 else q2d * 3 + (0 if cq < 3 else 2 if cq > 5 else 1)
        k3 = _q7(_med3(W, N, NW) - pred) * 289 + _q7(W + N - NW - pred) * 43 + _q7(((W + NE + 1) >> 1) - pred) * 7 + _q7(N + NE - NNE - pred)
        k4 = (_q9(W - pred) * 9 + _q9(N - pred)) * 16 + qe_c
        WW = r0[x - 2] if x > 1 else W
        k5 = (_q9(NE - pred) * 9 + _q9(NW - pred)) * 16 + qe_c
        k6 = ((_qn7(WW - pred, scl) * 7 + _qn7(NN - pred, scl)) * 7 + _qn7(NNE - pred, scl)) * 6 + ((qE * 3) >> 3)
        ks = (k0, k1, k2, k3, k4, k5, k6)
        rows = [k * NROWS + kk % NROWS for k, kk in enumerate(ks)]
        rates = []
        g_use = pc != g
        for r in rows:
            n = ch.ctr[r][0]
            rn = n - 2 if (g_use and n >= 2) else n
            rates.append(RATE_V3[rn])
            ch.ctr[r][0] = n + (1 if n < 72 else 0)
        if c == 0:
            wsel = qE * 2 + (1 if use else 0)
        elif c == 1:
            wsel = (qE * 3 + (0 if gq == 4 else 1 + (1 if abs(eg) > 3 else 0))) * 2 + (1 if use else 0)
        else:
            wsel = (qE * 3 + (0 if (rq == 4 and gq == 4) else 1 + (1 if abs(er_r) > 3 else 0))) * 2 + (1 if use else 0)
    else:
        scl = (E >> 2) + 2
        k0 = (((_qn7(W - pred, scl) * 7 + _qn7(N - pred, scl)) * 7 + _qn7(NE - pred, scl)) * 7 + _qn7(NW - pred, scl)) * 3 + band
        k1 = qE * 81 + gq * 9 + rq
        k2 = q2d * 5 + (gq + 1) // 2
        k3 = ((_q7(_med3(W, N, NW) - pred) * 7 + _q7(W + N - NW - pred)) * 7 + _q7(((W + NE + 1) >> 1) - pred)) * 7 + _q7(N + NE - NNE - pred)
        k4 = (_q9(W - pred) * 9 + _q9(N - pred)) * 16 + qE
        ks = (k0, k1, k2, k3, k4)
        rows = [k * NROWS + kk % NROWS for k, kk in enumerate(ks)]
        rates = []
        for r in rows:
            n = ch.ctr[r][0]
            rates.append(RATE[n])
            ch.ctr[r][0] = n + (1 if n < 60 else 0)
        wsel = qE * 8 + ((0 if gq == 4 else 1 + (1 if abs(eg) > 3 else 0)) if c else 3)
    wrow = wsel * ch.nodes

    # code
    if ch.enc is not None:
        r = (value - pc) & 255
        if r >= 128:
            r -= 256
        ch.code_residual(rows, rates, wrow, r)
    else:
        r = ch.code_residual(rows, rates, wrow, 0)
        if r == 128:
            ch.bad = True
        value = (pc + r) & 255

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
    if ch.nmodel == NMODEL:
        ch.vals[y][x] = value
        ch.exs[y][x] = 1 if gp is not None and value == gp else 0
        if gate:
            for p_ in (gp, pt):
                m = (value - p_) % 256
                if 256 - m < 32:
                    _gg_count("fold")
                if min(m, 256 - m) > 32:
                    _gg_count("cap")
            ch.eg += _gg_dist(value, gp)
            ch.et += _gg_dist(value, pt)
            if ch.eg >= 4096:
                _gg_count("halve-by-eg")
            if ch.et >= 4096:
                _gg_count("halve-by-et")
            if ch.eg >= 4096 or ch.et >= 4096:
                ch.eg >>= 1
                ch.et >>= 1
    cross["r"] = ((r + 128) & 255) - 128                 # stored as a signed byte
    cross["fe"] = _clamp(ferr, -2047, 2047)
    return value


def strip_bounds(height: int, nstrips: int, s: int) -> tuple[int, int]:
    w_rows = 68 if nstrips == 2 else max(64, height // 10)
    total_extra = (nstrips * (nstrips - 1) // 2) * w_rows
    if nstrips == 1 or height <= total_extra + 32 * nstrips:
        return (s * height) // nstrips, ((s + 1) * height) // nstrips
    rem = height - total_extra
    q, r_rem = divmod(rem, nstrips)
    y0 = 0
    for k in range(s):
        y0 += q + (1 if k < r_rem else 0) + (nstrips - 1 - k) * w_rows
    sh = q + (1 if s < r_rem else 0) + (nstrips - 1 - s) * w_rows
    return y0, y0 + sh


def _run_strips(width: int, height: int, nstrips: int, strips: list[list[Channel]], rgb: bytearray) -> None:
    for s in range(nstrips):
        chans = strips[s]
        y0, y1 = strip_bounds(height, nstrips, s)
        sh = y1 - y0
        warm_rows = min(sh, 68 if nstrips == 2 else max(96, height // 6))
        for y in range(sh):
            for ch in chans:
                ch.begin_row()
            for x in range(width):
                i = ((y0 + y) * width + x) * 3
                gs: dict = {}
                g = _code_sample(chans[0], 0, x, y, rgb[i + 1], gs)
                rgb[i + 1] = g
                cross = {"g": g, "eg": gs["r"], "feg": gs["fe"]}
                rs = dict(cross)
                rgb[i] = _code_sample(chans[1], 1, x, y, rgb[i], rs)
                cross["er"], cross["fer"] = rs["r"], rs["fe"]
                rgb[i + 2] = _code_sample(chans[2], 2, x, y, rgb[i + 2], dict(cross))
            if s + 1 < nstrips and y == warm_rows - 1:
                for c in range(3):
                    strips[s + 1][c].copy_warm_from(chans[c])
            if y >= 2:
                for ch in chans:
                    ch.drop_row(y - 2)


def cm_encode(width: int, height: int, rgb: bytes, nstrips: int | None = None) -> bytes:
    if nstrips is None:
        nstrips = 3 if height >= 768 else 1
    if nstrips < 1 or nstrips > 4 or nstrips > height:
        raise ValueError("cm strips")
    strips = [[Channel(width, NMODEL, c) for c in range(3)] for _ in range(nstrips)]
    for chans in strips:
        for ch in chans:
            ch.enc = RangeEncoder()
    _run_strips(width, height, nstrips, strips, bytearray(rgb))
    streams = [ch.enc.finish() for chans in strips for ch in chans]
    return bytes([nstrips]) + b"".join(struct.pack("<I", len(st)) for st in streams) + b"".join(streams)


def cm_decode(payload: bytes, width: int, height: int, version: int = VERSION) -> bytes:
    if version == 2:
        if len(payload) < 12:
            raise FormatError("cm streams")
        sizes = struct.unpack_from("<III", payload, 0)
        if any(s < 5 for s in sizes) or 12 + sum(sizes) != len(payload):
            raise FormatError("cm streams")
        nstrips = 1
        pos = 12
    else:
        if len(payload) < 1:
            raise FormatError("cm streams")
        nstrips = payload[0]
        if nstrips < 1 or nstrips > 4 or nstrips > height:
            raise FormatError("cm streams")
        hdr = 1 + 12 * nstrips
        if len(payload) < hdr:
            raise FormatError("cm streams")
        sizes = struct.unpack_from(f"<{3 * nstrips}I", payload, 1)
        if any(s < 5 for s in sizes) or hdr + sum(sizes) != len(payload):
            raise FormatError("cm streams")
        pos = hdr
    nmodel = NMODEL if version == 3 else NMODEL_V2
    strips = [[Channel(width, nmodel, c) for c in range(3)] for _ in range(nstrips)]
    for s in range(nstrips):
        for c in range(3):
            size = sizes[3 * s + c]
            strips[s][c].dec = RangeDecoder(payload[pos:pos + size])
            pos += size
    rgb = bytearray(width * height * 3)
    _run_strips(width, height, nstrips, strips, rgb)
    if any(ch.bad or not ch.dec.exhausted_exactly() for chans in strips for ch in chans):
        raise FormatError("cm stream")
    return bytes(rgb)


# ---------------------------------------------------------------- palette (methods 2 and 3, decode-only in the encoder policy)

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


# ---------------------------------------------------------------- compact palette (method 4)

class _Pal4Counter:
    """A method 4 counter: method 3's update rule, coded on a 16-bit probability."""

    def __init__(self) -> None:
        self.c = 32768
        self.n = 0

    def code(self, enc: RangeEncoder | None, dec: RangeDecoder | None, bit: int = 0) -> int:
        p = max(1, self.c)
        rate = RATE[self.n]
        if self.n < 60:
            self.n += 1
        if enc is not None:
            enc.bit16(bit, p)
        else:
            assert dec is not None
            bit = dec.bit16(p)
        if bit == 1 and self.c < 65535:
            self.c += max(1, ((65535 - self.c) * rate) >> 16)
        elif bit == 0 and self.c > 0:
            self.c -= max(1, (self.c * rate) >> 16)
        return bit


def _pal4_class(x: int, y: int) -> int:
    if y == 0:
        return 1
    return 2 if x == 0 else 0


def _zigzag(v: int) -> int:
    return 2 * v if v >= 0 else -2 * v - 1


def _unzigzag(u: int) -> int:
    return u >> 1 if u % 2 == 0 else -((u + 1) >> 1)


def _rice_bits(v: int, k: int) -> str:
    low = format(v & ((1 << k) - 1), f"0{k}b") if k else ""
    return "1" * (v >> k) + "0" + low


def pal4_field(colours: list[tuple[int, int, int]], ks: tuple[int, int, int] | None = None) -> bytes:
    """The method 4 palette field for `colours`, which must already be in
    strictly increasing order. `ks` forces the three Rice parameters (any
    legal values decode); None applies the reference encoder's choice."""
    inputs: tuple[list[int], list[int], list[int]] = ([], [], [])
    prev_r = 0
    for r, g, b in colours:
        inputs[0].append(r - prev_r)
        inputs[1].append(_zigzag(g - r))
        inputs[2].append(_zigzag(b - g))
        prev_r = r
    if ks is None:
        chosen = []
        for vals in inputs:
            costs = [sum((v >> k) + 1 + k for v in vals) for k in range(9)]
            chosen.append(costs.index(min(costs)))        # the smallest k among the cheapest
        ks = (chosen[0], chosen[1], chosen[2])
    bits = "".join(format(k, "04b") for k in ks)
    for i in range(len(colours)):
        for ch in range(3):
            bits += _rice_bits(inputs[ch][i], ks[ch])
    bits += "0" * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, "big")


def _pal4_read_field(payload: bytes, count: int) -> tuple[list[bytes], int]:
    """Parses the palette field that starts at payload[1]; returns the palette
    and the field's size in bytes."""
    total_bits = (len(payload) - 1) * 8
    pos = 0

    def take(n: int) -> int:
        nonlocal pos
        if pos + n > total_bits:
            raise FormatError("palette")
        v = 0
        for _ in range(n):
            v = (v << 1) | ((payload[1 + (pos >> 3)] >> (7 - (pos & 7))) & 1)
            pos += 1
        return v

    def rice(k: int) -> int:
        q = 0
        while take(1) == 1:
            q += 1
            if q > 511:
                raise FormatError("palette")
        return (q << k) + take(k)

    ks = [take(4) for _ in range(3)]
    if any(k > 8 for k in ks):
        raise FormatError("palette")
    palette: list[bytes] = []
    prev_r = 0
    prev_key = -1
    for _ in range(count):
        r = prev_r + rice(ks[0])
        g = r + _unzigzag(rice(ks[1]))
        b = g + _unzigzag(rice(ks[2]))
        if not (0 <= r <= 255 and 0 <= g <= 255 and 0 <= b <= 255):
            raise FormatError("palette")
        key = r * 65536 + g * 256 + b
        if key <= prev_key:
            raise FormatError("palette")
        prev_key = key
        prev_r = r
        palette.append(bytes((r, g, b)))
    while pos % 8:
        if take(1) != 0:
            raise FormatError("palette")
    return palette, pos // 8


def pal4_encode(width: int, height: int, rgb: bytes, *, ks: tuple[int, int, int] | None = None) -> bytes | None:
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    distinct = {rgb[pos : pos + 3] for pos in range(0, len(rgb), 3)}
    if len(distinct) > 256:
        return None
    palette = sorted(distinct)                         # bytes order is R, then G, then B
    index_of = {col: i for i, col in enumerate(palette)}
    count = len(palette)
    field = pal4_field([(c[0], c[1], c[2]) for c in palette], ks)
    d1 = [_Pal4Counter() for _ in range(768)]
    d2 = [_Pal4Counter() for _ in range(128)]
    d3 = [_Pal4Counter() for _ in range(128)]
    tree = [_Pal4Counter() for _ in range(256)]
    enc = RangeEncoder()
    rows: list[list[int]] = []
    hit = 0
    for y in range(height):
        rows.append([0] * width)
        for x in range(width):
            pos = (y * width + x) * 3
            idx = index_of[rgb[pos : pos + 3]]
            rows[y][x] = idx
            a, b, e, pattern = _pal_neighbours_m3(rows, width, x, y)
            if d1[pattern * 2 + hit + 256 * _pal4_class(x, y)].code(enc, None, 1 if idx == a else 0):
                hit = 1
                continue
            hit = 0
            if count == 2:
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
    return bytes((count - 1,)) + field + enc.finish()


def pal4_decode(payload: bytes, width: int, height: int) -> bytes:
    if len(payload) < 8:
        raise FormatError("palette")
    count = payload[0] + 1
    palette, field_size = _pal4_read_field(payload, count)
    stream = payload[1 + field_size :]
    if len(stream) < 5:
        raise FormatError("palette")
    dec = RangeDecoder(stream)
    d1 = [_Pal4Counter() for _ in range(768)]
    d2 = [_Pal4Counter() for _ in range(128)]
    d3 = [_Pal4Counter() for _ in range(128)]
    tree = [_Pal4Counter() for _ in range(256)]
    rows: list[list[int]] = []
    rgb = bytearray(width * height * 3)
    hit = 0
    for y in range(height):
        rows.append([0] * width)
        for x in range(width):
            a, b, e, pattern = _pal_neighbours_m3(rows, width, x, y)
            if d1[pattern * 2 + hit + 256 * _pal4_class(x, y)].code(None, dec) == 1:
                idx = a
                hit = 1
            elif count == 2:
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
                        node = node * 2 + tree[node].code(None, dec)
                    idx = node - 256
                    if idx >= count or idx in (a, b, e):
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


# ---------------------------------------------------------------- method 5 (screen content, version 3)

_SC_MAP: tuple[tuple[int, int], ...] = (
    (0, 1), (1, 0), (1, 1), (-1, 1), (0, 2), (2, 0), (1, 2), (-1, 2), (2, 1), (-2, 1), (2, 2), (-2, 2),
    (0, 3), (3, 0), (1, 3), (-1, 3), (3, 1), (-3, 1), (2, 3), (-2, 3), (3, 2), (-3, 2), (0, 4), (4, 0),
    (1, 4), (-1, 4), (4, 1), (-4, 1), (3, 3), (-3, 3), (2, 4), (-2, 4), (4, 2), (-4, 2), (0, 5), (3, 4),
    (-3, 4), (4, 3), (-4, 3), (5, 0), (1, 5), (-1, 5), (5, 1), (-5, 1), (2, 5), (-2, 5), (5, 2), (-5, 2),
    (4, 4), (-4, 4), (3, 5), (-3, 5), (5, 3), (-5, 3), (0, 6), (6, 0), (1, 6), (-1, 6), (6, 1), (-6, 1),
    (2, 6), (-2, 6), (6, 2), (-6, 2), (4, 5), (-4, 5), (5, 4), (-5, 4), (3, 6), (-3, 6), (6, 3), (-6, 3),
    (0, 7), (7, 0), (1, 7), (-1, 7), (5, 5), (-5, 5), (7, 1), (-7, 1), (4, 6), (-4, 6), (6, 4), (-6, 4),
    (2, 7), (-2, 7), (7, 2), (-7, 2), (3, 7), (-3, 7), (7, 3), (-7, 3), (5, 6), (-5, 6), (6, 5), (-6, 5),
    (8, 0), (4, 7), (-4, 7), (7, 4), (-7, 4), (8, 1), (8, 2), (6, 6), (-6, 6), (8, 3), (5, 7), (-5, 7),
    (7, 5), (-7, 5), (8, 4), (6, 7), (-6, 7), (7, 6), (-7, 6), (8, 5), (7, 7), (-7, 7), (8, 6), (8, 7),
)
_SC_WINDOW = (1 << 20) - 120


def _sc_slot(r: int, g: int, b: int) -> int:
    rgb24 = (r << 16) | (g << 8) | b
    return ((0x1E35A7BD * (0xFF000000 | rgb24)) & 0xFFFFFFFF) >> 24


def _sc_tree_put(enc: RangeEncoder, tree: list[_Pal4Counter], levels: int, value: int) -> None:
    node = 1
    for bit_pos in range(levels - 1, -1, -1):
        bit = (value >> bit_pos) & 1
        tree[node].code(enc, None, bit)
        node = node * 2 + bit


def _sc_tree_get(dec: RangeDecoder, tree: list[_Pal4Counter], levels: int) -> int:
    node = 1
    for _ in range(levels):
        node = node * 2 + tree[node].code(None, dec)
    return node - (1 << levels)


def _sc_prefix_encode(value: int) -> tuple[int, int, int]:
    d = value - 1
    if d < 4:
        return d, 0, 0
    h = d.bit_length() - 1
    nbits = h - 1
    symbol = 2 * h + ((d >> (h - 1)) & 1)
    extra = d & ((1 << nbits) - 1)
    return symbol, nbits, extra


def _sc_prefix_decode(symbol: int, extra: int) -> int:
    if symbol < 4:
        return symbol + 1
    nbits = (symbol - 2) >> 1
    return ((2 + (symbol & 1)) << nbits) + extra + 1


def _sc_extra_put(enc: RangeEncoder, first: _Pal4Counter, nbits: int, extra: int) -> None:
    for bit_pos in range(nbits - 1, -1, -1):
        bit = (extra >> bit_pos) & 1
        if bit_pos == nbits - 1:
            first.code(enc, None, bit)
        else:
            enc.bit16(bit, 32768)


def _sc_extra_get(dec: RangeDecoder, first: _Pal4Counter, nbits: int) -> int:
    val = 0
    for i in range(nbits):
        bit = first.code(None, dec) if i == 0 else dec.bit16(32768)
        val = (val << 1) | bit
    return val


def _sc_map_dist(v: int, width: int) -> int:
    xi, yi = _SC_MAP[v - 1]
    return max(1, xi + yi * width)


def _sc_dist_code(dist: int, width: int) -> int:
    for v in range(1, 121):
        if _sc_map_dist(v, width) == dist:
            return v
    return dist + 120


def _sc_fnv1a_4px(rgb: bytes, byte_off: int) -> int:
    h = 2166136261
    for b in rgb[byte_off : byte_off + 12]:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h >> 16


def sc_encode(width: int, height: int, rgb: bytes, *, force: bool = False) -> bytes | None:
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    total = width * height
    head = [-1] * 65536
    prev = [-1] * total

    def insert(q: int) -> None:
        if q + 4 <= total:
            b = _sc_fnv1a_4px(rgb, 3 * q)
            prev[q] = head[b]
            head[b] = q

    tokens: list[tuple[int, int] | None] = []
    covered = 0
    p = 0
    while p < total:
        if not force and (p - covered) * 2 > total:
            return None
        limit = min(4096, total - p)
        best_len = 0
        best_dist = 0
        if limit >= 4:
            cands: list[int] = []
            if p >= 1:
                cands.append(p - 1)
            if p >= width:
                cands.append(p - width)
            q = head[_sc_fnv1a_4px(rgb, 3 * p)]
            visited = 0
            while q >= 0 and visited < 64 and p - q <= _SC_WINDOW:
                cands.append(q)
                q = prev[q]
                visited += 1
            cur_off = 3 * p
            for c in cands:
                d = p - c
                if d > _SC_WINDOW:
                    continue
                cand_off = 3 * c
                m = 0
                while m < limit and rgb[cur_off + 3 * m : cur_off + 3 * m + 3] == rgb[cand_off + 3 * m : cand_off + 3 * m + 3]:
                    m += 1
                if m > best_len or (m == best_len and m > 0 and d < best_dist):
                    best_len = m
                    best_dist = d
        if best_len >= 4:
            tokens.append((best_len, best_dist))
            for q in range(p, p + best_len):
                insert(q)
            covered += best_len
            p += best_len
        else:
            tokens.append(None)
            insert(p)
            p += 1
    if not force and covered * 2 < total:
        return None

    is_copy = [_Pal4Counter() for _ in range(3)]
    is_cache = [_Pal4Counter() for _ in range(3)]
    lit_g = [_Pal4Counter() for _ in range(256)]
    lit_rg = [[_Pal4Counter() for _ in range(256)] for _ in range(4)]
    lit_bg = [[_Pal4Counter() for _ in range(256)] for _ in range(4)]
    cache_tree = [_Pal4Counter() for _ in range(256)]
    len_tree = [_Pal4Counter() for _ in range(32)]
    len_extra = [_Pal4Counter() for _ in range(32)]
    dist_tree = [[_Pal4Counter() for _ in range(64)] for _ in range(2)]
    dist_extra = [_Pal4Counter() for _ in range(64)]
    cache: list[tuple[int, int, int] | None] = [None] * 256
    enc = RangeEncoder()
    prev_kind = 0
    prev_short = 0
    p = 0
    for tok in tokens:
        if tok is not None:
            length, dist = tok
            is_copy[prev_kind].code(enc, None, 1)
            lsym, lnb, lex = _sc_prefix_encode(length - 3)
            _sc_tree_put(enc, len_tree, 5, lsym)
            _sc_extra_put(enc, len_extra[lsym], lnb, lex)
            v = _sc_dist_code(dist, width)
            dsym, dnb, dex = _sc_prefix_encode(v)
            _sc_tree_put(enc, dist_tree[prev_short], 6, dsym)
            _sc_extra_put(enc, dist_extra[dsym], dnb, dex)
            for q in range(p, p + length):
                px = (rgb[3 * q], rgb[3 * q + 1], rgb[3 * q + 2])
                cache[_sc_slot(*px)] = px
            p += length
            prev_kind = 2
            prev_short = 1 if v <= 120 else 0
        else:
            px = (rgb[3 * p], rgb[3 * p + 1], rgb[3 * p + 2])
            idx = _sc_slot(*px)
            hit = 1 if cache[idx] == px else 0
            is_copy[prev_kind].code(enc, None, 0)
            is_cache[prev_kind].code(enc, None, hit)
            if hit:
                _sc_tree_put(enc, cache_tree, 8, idx)
                prev_kind = 1
            else:
                r, g, b = px
                _sc_tree_put(enc, lit_g, 8, g)
                _sc_tree_put(enc, lit_rg[g >> 6], 8, (r - g) & 255)
                _sc_tree_put(enc, lit_bg[g >> 6], 8, (b - g) & 255)
                prev_kind = 0
            cache[idx] = px
            p += 1
    return enc.finish()


def sc_decode(payload: bytes, width: int, height: int) -> bytes:
    if len(payload) < 5:
        raise FormatError("screen content")
    dec = RangeDecoder(payload)
    is_copy = [_Pal4Counter() for _ in range(3)]
    is_cache = [_Pal4Counter() for _ in range(3)]
    lit_g = [_Pal4Counter() for _ in range(256)]
    lit_rg = [[_Pal4Counter() for _ in range(256)] for _ in range(4)]
    lit_bg = [[_Pal4Counter() for _ in range(256)] for _ in range(4)]
    cache_tree = [_Pal4Counter() for _ in range(256)]
    len_tree = [_Pal4Counter() for _ in range(32)]
    len_extra = [_Pal4Counter() for _ in range(32)]
    dist_tree = [[_Pal4Counter() for _ in range(64)] for _ in range(2)]
    dist_extra = [_Pal4Counter() for _ in range(64)]
    cache: list[tuple[int, int, int] | None] = [None] * 256
    total = width * height
    rgb = bytearray(total * 3)
    prev_kind = 0
    prev_short = 0
    p = 0
    while p < total:
        if is_copy[prev_kind].code(None, dec) == 1:
            lsym = _sc_tree_get(dec, len_tree, 5)
            if lsym > 23:
                raise FormatError("screen content stream")
            lnb = 0 if lsym < 4 else (lsym - 2) >> 1
            length = _sc_prefix_decode(lsym, _sc_extra_get(dec, len_extra[lsym], lnb)) + 3
            if length > 4096 or p + length > total:
                raise FormatError("screen content stream")
            dsym = _sc_tree_get(dec, dist_tree[prev_short], 6)
            if dsym > 39:
                raise FormatError("screen content stream")
            dnb = 0 if dsym < 4 else (dsym - 2) >> 1
            v = _sc_prefix_decode(dsym, _sc_extra_get(dec, dist_extra[dsym], dnb))
            dist = _sc_map_dist(v, width) if v <= 120 else v - 120
            if dist > p:
                raise FormatError("screen content stream")
            for q in range(p, p + length):
                src = 3 * (q - dist)
                dst = 3 * q
                r, g, b = rgb[src], rgb[src + 1], rgb[src + 2]
                rgb[dst], rgb[dst + 1], rgb[dst + 2] = r, g, b
                cache[_sc_slot(r, g, b)] = (r, g, b)
            p += length
            prev_kind = 2
            prev_short = 1 if v <= 120 else 0
        else:
            if is_cache[prev_kind].code(None, dec) == 1:
                idx = _sc_tree_get(dec, cache_tree, 8)
                cached = cache[idx]
                if cached is None:
                    raise FormatError("screen content stream")
                r, g, b = cached
                prev_kind = 1
            else:
                g = _sc_tree_get(dec, lit_g, 8)
                rg = _sc_tree_get(dec, lit_rg[g >> 6], 8)
                bg = _sc_tree_get(dec, lit_bg[g >> 6], 8)
                r = (rg + g) & 255
                b = (bg + g) & 255
                prev_kind = 0
            dst = 3 * p
            rgb[dst], rgb[dst + 1], rgb[dst + 2] = r, g, b
            cache[_sc_slot(r, g, b)] = (r, g, b)
            p += 1
        if dec.pos > len(dec.data) + 2:
            raise FormatError("screen content stream")
    if not dec.exhausted_exactly():
        raise FormatError("screen content stream")
    return bytes(rgb)


# ---------------------------------------------------------------- method 6 (screen content v2, version 3)

_SC2_WINDOW = (1 << 20) - 1


def _sc2_slot11(r: int, g: int, b: int) -> int:
    rgb24 = (r << 16) | (g << 8) | b
    return ((0x1E35A7BD * (0xFF000000 | rgb24)) & 0xFFFFFFFF) >> 21


def _sc2_mtf_shift4(q: list[int], v: int) -> None:
    pos = 3
    for i in range(4):
        if q[i] == v:
            pos = i
            break
    for i in range(pos, 0, -1):
        q[i] = q[i - 1]
    q[0] = v


def _sc2_cands(p: int, width: int, orig_ring: list[int], rep: list[int]) -> list[int]:
    ring_cap = width + 3
    maxd = min(p, _SC2_WINDOW)
    y, x = divmod(p, width)
    out: list[int] = []

    def add(d: int) -> None:
        if 1 <= d <= maxd and d not in out and len(out) < 4:
            out.append(d)

    if y >= 1:
        add(orig_ring[(p - width) % ring_cap])
        if x + 1 < width:
            add(orig_ring[(p - width + 1) % ring_cap])
        if x >= 1:
            add(orig_ring[(p - width - 1) % ring_cap])
        if x + 2 < width:
            add(orig_ring[(p - width + 2) % ring_cap])
        if x >= 2:
            add(orig_ring[(p - width - 2) % ring_cap])
    if x >= 1:
        add(orig_ring[(p - 1) % ring_cap])
    for d in rep:
        add(d)
    for d in (1, width, width + 1, width - 1):
        add(d)
    return out


class _SC2Model:
    def __init__(self) -> None:
        self.is_copy = [_Pal4Counter() for _ in range(6)]
        self.is_inherit = [_Pal4Counter() for _ in range(6)]
        self.inh_b0 = [_Pal4Counter() for _ in range(6)]
        self.inh_b1 = _Pal4Counter()
        self.inh_b2 = _Pal4Counter()
        self.len_tree = [[_Pal4Counter() for _ in range(32)] for _ in range(2)]
        self.len_extra = [[_Pal4Counter() for _ in range(24)] for _ in range(2)]
        self.is_same_row = [_Pal4Counter() for _ in range(2)]
        self.is_rep_dy = _Pal4Counter()
        self.rep_dy_b0 = _Pal4Counter()
        self.rep_dy_b1 = [_Pal4Counter() for _ in range(2)]
        self.dy_tree = [_Pal4Counter() for _ in range(64)]
        self.dy_extra = [_Pal4Counter() for _ in range(40)]
        self.dx0_tree = [_Pal4Counter() for _ in range(64)]
        self.dx0_extra = [_Pal4Counter() for _ in range(40)]
        self.dx_zero = [_Pal4Counter() for _ in range(2)]
        self.dx_sign = _Pal4Counter()
        self.dx_tree = [_Pal4Counter() for _ in range(64)]
        self.dx_extra = [_Pal4Counter() for _ in range(50)]
        self.is_eq_w = [_Pal4Counter() for _ in range(12)]
        self.is_eq_n = [_Pal4Counter() for _ in range(6)]
        self.is_mtf = [_Pal4Counter() for _ in range(6)]
        self.mtf_tree = [_Pal4Counter() for _ in range(16)]
        self.is_cache = [_Pal4Counter() for _ in range(6)]
        self.cache_tree = [_Pal4Counter() for _ in range(2048)]
        self.lit_g = [[_Pal4Counter() for _ in range(256)] for _ in range(8)]
        self.lit_rg = [[_Pal4Counter() for _ in range(256)] for _ in range(4)]
        self.lit_bg = [[_Pal4Counter() for _ in range(256)] for _ in range(4)]


def sc2_encode(width: int, height: int, rgb: bytes, *, force: bool = False) -> bytes | None:
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    total = width * height
    if not force and total < 4:
        return None
    ring_cap = width + 3
    orig_ring = [0] * ring_cap
    rep = [1, width, width + 1, width - 1]
    rep_dy = [1, 2, 4, 8]
    mtf: list[tuple[int, int, int]] = []
    cache: list[tuple[int, int, int] | None] = [None] * 2048
    head = [-1] * 65536
    prev = [-1] * total

    def insert(q: int) -> None:
        if q + 4 <= total:
            b = _sc_fnv1a_4px(rgb, 3 * q)
            prev[q] = head[b]
            head[b] = q

    def update_color(px: tuple[int, int, int]) -> None:
        if px in mtf:
            mtf.remove(px)
        mtf.insert(0, px)
        if len(mtf) > 16:
            mtf.pop()
        cache[_sc2_slot11(*px)] = px

    def match_len(a: int, b: int, limit: int) -> int:
        ao, bo = 3 * a, 3 * b
        if rgb[ao : ao + 3] != rgb[bo : bo + 3]:
            return 0
        lo, hi = 1, limit
        while lo < hi:
            mid = (lo + hi + 1) >> 1
            if rgb[ao : ao + 3 * mid] == rgb[bo : bo + 3 * mid]:
                lo = mid
            else:
                hi = mid - 1
        return lo

    m = _SC2Model()
    enc = RangeEncoder()
    px0 = (rgb[0], rgb[1], rgb[2])
    r0, g0, b0 = px0
    _sc_tree_put(enc, m.lit_g[4], 8, g0)
    _sc_tree_put(enc, m.lit_rg[g0 >> 6], 8, (r0 - g0) & 255)
    _sc_tree_put(enc, m.lit_bg[g0 >> 6], 8, (b0 - g0) & 255)
    orig_ring[0] = 0
    insert(0)
    update_color(px0)

    prev_kind = 5
    prev_same_row = 0
    covered4 = 0
    p = 1
    while p < total:
        if not force and (p - covered4) * 2 > total:
            return None
        limit = min(4096, total - p)
        cands = _sc2_cands(p, width, orig_ring, rep)

        best_inh_len, best_inh_idx, best_inh_dist = 0, -1, 0
        if limit >= 2:
            for idx, d in enumerate(cands):
                ml = match_len(p, p - d, limit)
                if ml >= 2 and ml > best_inh_len:
                    best_inh_len, best_inh_idx, best_inh_dist = ml, idx, d

        best_gen_len, best_gen_dist = 0, 0
        if limit >= 4:
            hcands = [p - 1]
            if p >= width and width <= _SC2_WINDOW:
                hcands.append(p - width)
            q = head[_sc_fnv1a_4px(rgb, 3 * p)]
            visited = 0
            while q >= 0 and visited < 64 and p - q <= _SC2_WINDOW:
                hcands.append(q)
                q = prev[q]
                visited += 1
            for c in hcands:
                d = p - c
                if d > _SC2_WINDOW:
                    continue
                ml = match_len(p, c, limit)
                if ml >= 4 and (ml > best_gen_len or (ml == best_gen_len and d < best_gen_dist)):
                    best_gen_len, best_gen_dist = ml, d

        use_copy = False
        use_inh = False
        chosen_len, chosen_dist, chosen_idx = 0, 0, -1
        if best_inh_len >= 2 and (best_gen_len < 4 or best_inh_len + 1 >= best_gen_len):
            use_copy, use_inh = True, True
            chosen_len, chosen_dist, chosen_idx = best_inh_len, best_inh_dist, best_inh_idx
        elif best_gen_len >= 4:
            use_copy = True
            chosen_len, chosen_dist = best_gen_len, best_gen_dist
            if best_gen_dist in cands:
                use_inh = True
                chosen_idx = cands.index(best_gen_dist)

        if use_copy:
            m.is_copy[prev_kind].code(enc, None, 1)
            m.is_inherit[prev_kind].code(enc, None, 1 if use_inh else 0)
            if use_inh:
                k = len(cands)
                if k > 1:
                    m.inh_b0[prev_kind].code(enc, None, 1 if chosen_idx == 0 else 0)
                    if chosen_idx > 0 and k > 2:
                        m.inh_b1.code(enc, None, 1 if chosen_idx == 1 else 0)
                        if chosen_idx > 1 and k > 3:
                            m.inh_b2.code(enc, None, 1 if chosen_idx == 2 else 0)
                lsym, lnb, lex = _sc_prefix_encode(chosen_len - 1)
                _sc_tree_put(enc, m.len_tree[1], 5, lsym)
                _sc_extra_put(enc, m.len_extra[1][lsym], lnb, lex)
            else:
                lsym, lnb, lex = _sc_prefix_encode(chosen_len - 3)
                _sc_tree_put(enc, m.len_tree[0], 5, lsym)
                _sc_extra_put(enc, m.len_extra[0][lsym], lnb, lex)
                y, x = divmod(p, width)
                qy, qx = divmod(p - chosen_dist, width)
                dy = y - qy
                if y == 0:
                    same_row = 1
                elif x == 0:
                    same_row = 0
                else:
                    same_row = 1 if dy == 0 else 0
                    m.is_same_row[prev_same_row].code(enc, None, same_row)
                if same_row == 1:
                    dx = x - qx
                    dsym, dnb, dex = _sc_prefix_encode(dx)
                    _sc_tree_put(enc, m.dx0_tree, 6, dsym)
                    _sc_extra_put(enc, m.dx0_extra[dsym], dnb, dex)
                    prev_same_row = 1
                else:
                    in_rdy = 1 if dy in rep_dy else 0
                    m.is_rep_dy.code(enc, None, in_rdy)
                    if in_rdy == 1:
                        ridx = rep_dy.index(dy)
                        m.rep_dy_b0.code(enc, None, 1 if ridx == 0 else 0)
                        if ridx > 0:
                            m.rep_dy_b1[0].code(enc, None, 1 if ridx == 1 else 0)
                            if ridx > 1:
                                m.rep_dy_b1[1].code(enc, None, 1 if ridx == 2 else 0)
                    else:
                        dysym, dynb, dyex = _sc_prefix_encode(dy)
                        _sc_tree_put(enc, m.dy_tree, 6, dysym)
                        _sc_extra_put(enc, m.dy_extra[dysym], dynb, dyex)
                    _sc2_mtf_shift4(rep_dy, dy)
                    dx = x - qx
                    m.dx_zero[in_rdy].code(enc, None, 1 if dx == 0 else 0)
                    if dx != 0:
                        m.dx_sign.code(enc, None, 1 if dx < 0 else 0)
                        dxsym, dxnb, dxex = _sc_prefix_encode(abs(dx))
                        _sc_tree_put(enc, m.dx_tree, 6, dxsym)
                        _sc_extra_put(enc, m.dx_extra[dxsym], dxnb, dxex)
                    prev_same_row = 0

            for q in range(p, p + chosen_len):
                orig_ring[q % ring_cap] = chosen_dist
                insert(q)
                update_color((rgb[3 * q], rgb[3 * q + 1], rgb[3 * q + 2]))
            _sc2_mtf_shift4(rep, chosen_dist)
            if chosen_len >= 4:
                covered4 += chosen_len
            p += chosen_len
            prev_kind = 0
            continue

        m.is_copy[prev_kind].code(enc, None, 0)
        y, x = divmod(p, width)
        has_w, has_n = x >= 1, y >= 1
        px = (rgb[3 * p], rgb[3 * p + 1], rgb[3 * p + 2])
        w_px = (rgb[3 * (p - 1)], rgb[3 * (p - 1) + 1], rgb[3 * (p - 1) + 2]) if has_w else None
        n_px = (rgb[3 * (p - width)], rgb[3 * (p - width) + 1], rgb[3 * (p - width) + 2]) if has_n else None
        done = False
        tk = 5
        if has_w:
            wn_eq = 1 if (has_n and w_px == n_px) else 0
            bw = 1 if px == w_px else 0
            m.is_eq_w[prev_kind * 2 + wn_eq].code(enc, None, bw)
            if bw == 1:
                tk, done = 1, True
        if not done and has_n and (not has_w or n_px != w_px):
            bn = 1 if px == n_px else 0
            m.is_eq_n[prev_kind].code(enc, None, bn)
            if bn == 1:
                tk, done = 2, True
        if not done:
            if px in mtf:
                m.is_mtf[prev_kind].code(enc, None, 1)
                _sc_tree_put(enc, m.mtf_tree, 4, mtf.index(px))
                tk = 3
            else:
                m.is_mtf[prev_kind].code(enc, None, 0)
                cidx = _sc2_slot11(*px)
                if cache[cidx] == px:
                    m.is_cache[prev_kind].code(enc, None, 1)
                    _sc_tree_put(enc, m.cache_tree, 11, cidx)
                    tk = 4
                else:
                    m.is_cache[prev_kind].code(enc, None, 0)
                    r, g, b = px
                    wg_ctx = (w_px[1] >> 5) if has_w and w_px is not None else 4
                    _sc_tree_put(enc, m.lit_g[wg_ctx], 8, g)
                    _sc_tree_put(enc, m.lit_rg[g >> 6], 8, (r - g) & 255)
                    _sc_tree_put(enc, m.lit_bg[g >> 6], 8, (b - g) & 255)
                    tk = 5
        orig_ring[p % ring_cap] = 0
        insert(p)
        update_color(px)
        p += 1
        prev_kind = tk

    if not force and covered4 * 2 < total:
        return None
    return enc.finish()


def sc2_decode(payload: bytes, width: int, height: int) -> bytes:
    if len(payload) < 5:
        raise FormatError("screen content")
    dec = RangeDecoder(payload)
    m = _SC2Model()
    total = width * height
    ring_cap = width + 3
    orig_ring = [0] * ring_cap
    rep = [1, width, width + 1, width - 1]
    rep_dy = [1, 2, 4, 8]
    mtf: list[tuple[int, int, int]] = []
    cache: list[tuple[int, int, int] | None] = [None] * 2048
    rgb = bytearray(total * 3)

    def update_color(px: tuple[int, int, int]) -> None:
        if px in mtf:
            mtf.remove(px)
        mtf.insert(0, px)
        if len(mtf) > 16:
            mtf.pop()
        cache[_sc2_slot11(*px)] = px

    g0 = _sc_tree_get(dec, m.lit_g[4], 8)
    r0 = (_sc_tree_get(dec, m.lit_rg[g0 >> 6], 8) + g0) & 255
    b0 = (_sc_tree_get(dec, m.lit_bg[g0 >> 6], 8) + g0) & 255
    rgb[0], rgb[1], rgb[2] = r0, g0, b0
    orig_ring[0] = 0
    update_color((r0, g0, b0))
    if dec.pos > len(dec.data) + 2:
        raise FormatError("screen content stream")

    prev_kind = 5
    prev_same_row = 0
    p = 1
    while p < total:
        cands = _sc2_cands(p, width, orig_ring, rep)
        if m.is_copy[prev_kind].code(None, dec) == 1:
            use_inh = m.is_inherit[prev_kind].code(None, dec)
            if use_inh == 1:
                k = len(cands)
                if k == 1:
                    idx = 0
                elif m.inh_b0[prev_kind].code(None, dec) == 1:
                    idx = 0
                elif k == 2 or m.inh_b1.code(None, dec) == 1:
                    idx = 1
                elif k == 3 or m.inh_b2.code(None, dec) == 1:
                    idx = 2
                else:
                    idx = 3
                dist = cands[idx]
                lsym = _sc_tree_get(dec, m.len_tree[1], 5)
                if lsym > 23:
                    raise FormatError("screen content stream")
                lnb = 0 if lsym < 4 else (lsym - 2) >> 1
                length = _sc_prefix_decode(lsym, _sc_extra_get(dec, m.len_extra[1][lsym], lnb)) + 1
                if length > 4096 or p + length > total:
                    raise FormatError("screen content stream")
            else:
                lsym = _sc_tree_get(dec, m.len_tree[0], 5)
                if lsym > 23:
                    raise FormatError("screen content stream")
                lnb = 0 if lsym < 4 else (lsym - 2) >> 1
                length = _sc_prefix_decode(lsym, _sc_extra_get(dec, m.len_extra[0][lsym], lnb)) + 3
                if length > 4096 or p + length > total:
                    raise FormatError("screen content stream")
                y, x = divmod(p, width)
                if y == 0:
                    same_row = 1
                elif x == 0:
                    same_row = 0
                else:
                    same_row = m.is_same_row[prev_same_row].code(None, dec)
                if same_row == 1:
                    dsym = _sc_tree_get(dec, m.dx0_tree, 6)
                    if dsym > 39:
                        raise FormatError("screen content stream")
                    dnb = 0 if dsym < 4 else (dsym - 2) >> 1
                    dx = _sc_prefix_decode(dsym, _sc_extra_get(dec, m.dx0_extra[dsym], dnb))
                    if dx > x or dx > _SC2_WINDOW:
                        raise FormatError("screen content stream")
                    dist = dx
                    prev_same_row = 1
                else:
                    in_rdy = m.is_rep_dy.code(None, dec)
                    if in_rdy == 1:
                        if m.rep_dy_b0.code(None, dec) == 1:
                            ridx = 0
                        elif m.rep_dy_b1[0].code(None, dec) == 1:
                            ridx = 1
                        elif m.rep_dy_b1[1].code(None, dec) == 1:
                            ridx = 2
                        else:
                            ridx = 3
                        dy = rep_dy[ridx]
                    else:
                        dysym = _sc_tree_get(dec, m.dy_tree, 6)
                        if dysym > 39:
                            raise FormatError("screen content stream")
                        dynb = 0 if dysym < 4 else (dysym - 2) >> 1
                        dy = _sc_prefix_decode(dysym, _sc_extra_get(dec, m.dy_extra[dysym], dynb))
                    if dy > y or dy > _SC2_WINDOW:
                        raise FormatError("screen content stream")
                    _sc2_mtf_shift4(rep_dy, dy)
                    if m.dx_zero[in_rdy].code(None, dec) == 1:
                        dx = 0
                    else:
                        neg = m.dx_sign.code(None, dec)
                        dxsym = _sc_tree_get(dec, m.dx_tree, 6)
                        if dxsym > 49:
                            raise FormatError("screen content stream")
                        dxnb = 0 if dxsym < 4 else (dxsym - 2) >> 1
                        adx = _sc_prefix_decode(dxsym, _sc_extra_get(dec, m.dx_extra[dxsym], dxnb))
                        dx = -adx if neg == 1 else adx
                    qx = x - dx
                    if qx < 0 or qx >= width:
                        raise FormatError("screen content stream")
                    dist = dy * width + dx
                    if dist > _SC2_WINDOW:
                        raise FormatError("screen content stream")
                    prev_same_row = 0

            for q in range(p, p + length):
                src = 3 * (q - dist)
                dst = 3 * q
                r, g, b = rgb[src], rgb[src + 1], rgb[src + 2]
                rgb[dst], rgb[dst + 1], rgb[dst + 2] = r, g, b
                orig_ring[q % ring_cap] = dist
                update_color((r, g, b))
            _sc2_mtf_shift4(rep, dist)
            p += length
            prev_kind = 0
        else:
            y, x = divmod(p, width)
            has_w, has_n = x >= 1, y >= 1
            w_px = (rgb[3 * (p - 1)], rgb[3 * (p - 1) + 1], rgb[3 * (p - 1) + 2]) if has_w else None
            n_px = (rgb[3 * (p - width)], rgb[3 * (p - width) + 1], rgb[3 * (p - width) + 2]) if has_n else None
            done = False
            tk = 5
            r = g = b = 0
            if has_w and w_px is not None:
                wn_eq = 1 if (has_n and w_px == n_px) else 0
                if m.is_eq_w[prev_kind * 2 + wn_eq].code(None, dec) == 1:
                    r, g, b = w_px
                    tk, done = 1, True
            if not done and has_n and n_px is not None and (not has_w or n_px != w_px):
                if m.is_eq_n[prev_kind].code(None, dec) == 1:
                    r, g, b = n_px
                    tk, done = 2, True
            if not done:
                if m.is_mtf[prev_kind].code(None, dec) == 1:
                    midx = _sc_tree_get(dec, m.mtf_tree, 4)
                    if midx >= len(mtf):
                        raise FormatError("screen content stream")
                    r, g, b = mtf[midx]
                    tk = 3
                elif m.is_cache[prev_kind].code(None, dec) == 1:
                    cidx = _sc_tree_get(dec, m.cache_tree, 11)
                    cached = cache[cidx]
                    if cached is None:
                        raise FormatError("screen content stream")
                    r, g, b = cached
                    tk = 4
                else:
                    wg_ctx = (w_px[1] >> 5) if has_w and w_px is not None else 4
                    g = _sc_tree_get(dec, m.lit_g[wg_ctx], 8)
                    rg = _sc_tree_get(dec, m.lit_rg[g >> 6], 8)
                    bg = _sc_tree_get(dec, m.lit_bg[g >> 6], 8)
                    r = (rg + g) & 255
                    b = (bg + g) & 255
                    tk = 5
            dst = 3 * p
            rgb[dst], rgb[dst + 1], rgb[dst + 2] = r, g, b
            orig_ring[p % ring_cap] = 0
            update_color((r, g, b))
            p += 1
            prev_kind = tk
        if dec.pos > len(dec.data) + 2:
            raise FormatError("screen content stream")

    if not dec.exhausted_exactly():
        raise FormatError("screen content stream")
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
    nstrips = cm_payload[0]
    cm_sizes = struct.unpack_from(f"<{3 * nstrips}I", cm_payload, 1)
    strip_ok = True
    for s in range(nstrips):
        y0, y1 = strip_bounds(height, nstrips, s)
        cap = width * (y1 - y0) + 64 // nstrips
        if any(cm_sizes[3 * s + c] > cap for c in range(3)):
            strip_ok = False
            break
    if strip_ok and len(cm_payload) < len(best_payload):
        best_method = METHOD_CM
        best_payload = cm_payload
    pal_payload = pal4_encode(width, height, rgb)
    if pal_payload is not None and len(pal_payload) < len(best_payload):
        best_method = METHOD_PAL3
        best_payload = pal_payload
    if pal_payload is None or pal_payload[0] >= 16:
        sc_payload = sc2_encode(width, height, rgb)
        if sc_payload is not None and len(sc_payload) < len(best_payload):
            best_method = METHOD_SC2
            best_payload = sc_payload
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
    if method > METHOD_SC2 or (method in (METHOD_PAL3, METHOD_SC, METHOD_SC2) and version != 3):
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
    if method == METHOD_PAL3:
        return width, height, pal4_decode(payload, width, height)
    if method == METHOD_SC:
        return width, height, sc_decode(payload, width, height)
    if method == METHOD_SC2:
        return width, height, sc2_decode(payload, width, height)
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
