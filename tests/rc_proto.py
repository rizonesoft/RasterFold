"""Adaptive rANS coder over long-match LZ77 tokens. Size prototype."""

from __future__ import annotations

from pathlib import Path

from PIL import Image

import tests.lz_proto as lz

HEADER = 24
LSTATE = 1 << 23
SCALE = 4096


class Model:
    def __init__(self, n: int, inc: int = 32) -> None:
        self.n = n
        self.f = [1] * n
        self.inc = inc
        self.scaled = [SCALE // n] * n
        self._fix()

    def _fix(self) -> None:
        scaled = [max(1, freq * SCALE // sum(self.f)) for freq in self.f]
        while sum(scaled) > SCALE:
            i = max(range(self.n), key=lambda k: scaled[k])
            if scaled[i] == 1:
                break
            scaled[i] -= 1
        while sum(scaled) < SCALE:
            i = max(range(self.n), key=lambda k: self.f[k])
            scaled[i] += 1
        self.scaled = scaled
        self.cum = [0]
        for freq in scaled:
            self.cum.append(self.cum[-1] + freq)

    def slot(self, sym: int) -> tuple[int, int]:
        return self.cum[sym], self.scaled[sym]

    def find(self, value: int) -> tuple[int, int, int]:
        # value in 0..SCALE-1
        lo, hi = 0, self.n
        while lo + 1 < hi:
            mid = (lo + hi) // 2
            if self.cum[mid] <= value:
                lo = mid
            else:
                hi = mid
        return lo, self.cum[lo], self.scaled[lo]

    def update(self, sym: int) -> None:
        self.f[sym] += self.inc
        if sum(self.f) > 8192:
            self.f = [max(1, freq >> 1) for freq in self.f]
        self._fix()


class Rans:
    def __init__(self) -> None:
        self.x = LSTATE
        self.out = bytearray()

    def put(self, start: int, freq: int) -> None:
        x = self.x
        x_max = ((LSTATE >> 12) << 8) * freq
        while x >= x_max:
            self.out.append(x & 255)
            x >>= 8
        self.x = ((x // freq) << 12) + (x % freq) + start

    def finish(self) -> bytes:
        tail = bytearray()
        x = self.x
        for _ in range(4):
            tail.append(x & 255)
            x >>= 8
        return bytes(tail) + bytes(reversed(self.out))


def enc_bits(rs: Rans, value: int, bits: int) -> None:
    half = SCALE // 2
    for shift in range(bits - 1, -1, -1):
        bit = (value >> shift) & 1
        rs.put(bit * half, half)


def dec_bits(rd: RansDec, bits: int) -> int:
    value = 0
    half = SCALE // 2
    for _ in range(bits):
        bit = 1 if rd.slot() >= half else 0
        rd.advance(bit * half, half)
        value = (value << 1) | bit
    return value


class RansDec:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 4
        self.x = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24)
        if len(data) < 4:
            raise ValueError("short")

    def slot(self) -> int:
        return self.x & (SCALE - 1)

    def advance(self, start: int, freq: int) -> None:
        x = freq * (self.x >> 12) + (self.x & (SCALE - 1)) - start
        while x < LSTATE:
            if self.pos >= len(self.data):
                raise EOFError
            x = (x << 8) | self.data[self.pos]
            self.pos += 1
        self.x = x


def enc_sym(rs: Rans, model: Model, sym: int) -> None:
    start, freq = model.slot(sym)
    rs.put(start, freq)
    model.update(sym)


def dec_sym(rd: RansDec, model: Model) -> int:
    sym, start, freq = model.find(rd.slot())
    rd.advance(start, freq)
    model.update(sym)
    return sym


def enc_bits(rs: Rans, value: int, bits: int) -> None:
    for shift in range(bits - 1, -1, -1):
        bit = (value >> shift) & 1
        rs.put(bit * (SCALE // 2), SCALE // 2)


def dec_bits(rd: RansDec, bits: int) -> int:
    value = 0
    half = SCALE // 2
    for _ in range(bits):
        bit = 1 if rd.slot() >= half else 0
        rd.advance(bit * half, half)
        value = (value << 1) | bit
    return value


def enc_int(rs: Rans, mag_model: Model, value: int) -> None:
    mag = value.bit_length()
    enc_sym(rs, mag_model, mag)
    if mag > 1:
        enc_bits(rs, value ^ (1 << (mag - 1)), mag - 1)


def dec_int(rd: RansDec, mag_model: Model) -> int:
    mag = dec_sym(rd, mag_model)
    if mag == 0:
        return 0
    if mag == 1:
        return 1
    return (1 << (mag - 1)) + dec_bits(rd, mag - 1)


def enc_dist(rs: Rans, cat: Model, mag: Model, dist: int, stride: int, bpp: int, last: int) -> None:
    if dist == 1:
        which = 0
    elif bpp > 1 and dist == bpp:
        which = 1
    elif stride and dist == stride:
        which = 2
    elif last and dist == last:
        which = 3
    elif stride and dist == stride * 2:
        which = 4
    else:
        which = 5
    enc_sym(rs, cat, which)
    if which == 5:
        enc_int(rs, mag, dist)


def dec_dist(rd: RansDec, cat: Model, mag: Model, stride: int, bpp: int, last: int) -> int:
    which = dec_sym(rd, cat)
    if which == 0:
        return 1
    if which == 1:
        return bpp
    if which == 2:
        return stride
    if which == 3:
        return last if last else 1
    if which == 4:
        return stride * 2 if stride else 1
    return dec_int(rd, mag)


def encode_tokens(tokens: list, stride: int, bpp: int) -> bytes:
    # rANS decodes in reverse, so encode from the end marker backward.
    events = [("end",)]
    last = 0
    prepared = []
    for tok in tokens:
        if isinstance(tok, int):
            prepared.append(("lit", tok))
        else:
            prepared.append(("match", tok[0], tok[1], last))
            last = tok[1]
    rs = Rans()
    kind = Model(3, 64)
    lits = Model(256, 16)
    len_mag = Model(32, 16)
    dist_cat = Model(6, 64)
    dist_mag = Model(32, 8)
    # Walk events in reverse. Models must be updated in reverse too, which means
    # the decoder (forward) and encoder (backward) see different adaptation order
    # unless we use a static model. Use a two-pass static model from full counts
    # so both sides share the exact frequencies. That also removes adaptation order.
    return encode_static(tokens, stride, bpp)


def _dist_which(dist: int, stride: int, bpp: int, last: int) -> int:
    if dist == 1:
        return 0
    if bpp > 1 and dist == bpp:
        return 1
    if stride and dist == stride:
        return 2
    if last and dist == last:
        return 3
    if stride and dist == stride * 2:
        return 4
    return 5


def encode_static(tokens: list, stride: int, bpp: int) -> bytes:
    kind_f = [1, 1, 1]
    lit_f = [1] * 256
    len_f = [1] * 32
    cat_f = [1] * 6
    dmag_f = [1] * 32
    last = 0
    ints = []  # (mag_model_name, value) recorded in forward order for the raw bits
    for tok in tokens:
        if isinstance(tok, int):
            kind_f[0] += 1
            lit_f[tok] += 1
        else:
            length, dist = tok
            kind_f[1] += 1
            mag = (length - 3).bit_length()
            len_f[mag] += 1
            which = _dist_which(dist, stride, bpp, last)
            cat_f[which] += 1
            if which == 5:
                dmag_f[dist.bit_length()] += 1
            last = dist
    kind_f[2] += 1
    models = {
        "kind": Model(3),
        "lit": Model(256),
        "len": Model(32),
        "cat": Model(6),
        "dmag": Model(32),
    }
    models["kind"].f = kind_f
    models["lit"].f = lit_f
    models["len"].f = len_f
    models["cat"].f = cat_f
    models["dmag"].f = dmag_f
    for model in models.values():
        model._fix()
    # Encode reverse with frozen models (no update). Decoder uses the same frozen models,
    # which means the tables must be transmitted. Store the raw counts compactly.
    rs = Rans()

    def put_sym(model: Model, sym: int) -> None:
        start, freq = model.slot(sym)
        rs.put(start, freq)

    def put_int(model: Model, value: int) -> None:
        mag = value.bit_length()
        put_sym(model, mag)
        if mag > 1:
            enc_bits(rs, value ^ (1 << (mag - 1)), mag - 1)

    put_sym(models["kind"], 2)
    last = 0
    seq = []
    for tok in tokens:
        if isinstance(tok, int):
            seq.append(("lit", tok, 0))
        else:
            seq.append(("match", tok[0], tok[1]))
    for item in reversed(seq):
        if item[0] == "lit":
            put_sym(models["kind"], 0)
            put_sym(models["lit"], item[1])
        else:
            # Distance category depends on the previous distance in forward order.
            # Recompute last by a forward scan stored on the item.
            pass
    # Rebuild with last-dist stored on each match.
    annotated = []
    last = 0
    for tok in tokens:
        if isinstance(tok, int):
            annotated.append(("lit", tok, 0))
        else:
            annotated.append(("match", tok[0], tok[1], last))
            last = tok[1]
    rs = Rans()
    put_sym(models["kind"], 2)
    for item in reversed(annotated):
        if item[0] == "lit":
            put_sym(models["kind"], 0)
            put_sym(models["lit"], item[1])
        else:
            _, length, dist, prev = item
            put_sym(models["kind"], 1)
            put_int(models["len"], length - 3)
            which = _dist_which(dist, stride, bpp, prev)
            put_sym(models["cat"], which)
            if which == 5:
                put_int(models["dmag"], dist)
    payload = rs.finish()
    table = _pack_counts(kind_f, lit_f, len_f, cat_f, dmag_f)
    return table + payload


def _pack_counts(kind_f, lit_f, len_f, cat_f, dmag_f) -> bytes:
    # Store only the added counts (freq-1) as bytes when they fit, else 16-bit.
    # Keep it simple and small: varints of every count. Counts are modest.
    out = bytearray()

    def put_var(n: int) -> None:
        while n >= 128:
            out.append((n & 127) | 128)
            n >>= 7
        out.append(n)

    for freq in kind_f + lit_f + len_f + cat_f + dmag_f:
        put_var(freq)
    return bytes(out)


def _read_counts(blob: bytes) -> tuple[dict, int]:
    pos = 0

    def get_var() -> int:
        nonlocal pos
        n = 0
        shift = 0
        while True:
            if pos >= len(blob):
                raise ValueError("counts")
            b = blob[pos]
            pos += 1
            n |= (b & 127) << shift
            if b < 128:
                return n
            shift += 7

    def take(n: int) -> list[int]:
        return [get_var() for _ in range(n)]

    counts = {
        "kind": take(3),
        "lit": take(256),
        "len": take(32),
        "cat": take(6),
        "dmag": take(32),
    }
    models = {}
    for name, freq in counts.items():
        model = Model(len(freq))
        model.f = freq
        model._fix()
        models[name] = model
    return models, pos


def decode_tokens(blob: bytes, out_len: int, stride: int, bpp: int) -> bytes:
    models, pos = _read_counts(blob)
    rd = RansDec(blob[pos:])
    out = bytearray()
    last = 0

    def get_sym(model: Model) -> int:
        sym, start, freq = model.find(rd.slot())
        rd.advance(start, freq)
        return sym

    def get_int(model: Model) -> int:
        mag = get_sym(model)
        if mag == 0:
            return 0
        if mag == 1:
            return 1
        return (1 << (mag - 1)) + dec_bits(rd, mag - 1)

    while len(out) < out_len:
        k = get_sym(models["kind"])
        if k == 2:
            break
        if k == 0:
            out.append(get_sym(models["lit"]))
        else:
            length = get_int(models["len"]) + 3
            which = get_sym(models["cat"])
            if which == 0:
                dist = 1
            elif which == 1:
                dist = bpp
            elif which == 2:
                dist = stride
            elif which == 3:
                dist = last
            elif which == 4:
                dist = stride * 2
            else:
                dist = get_int(models["dmag"])
            last = dist
            if dist <= 0 or dist > len(out) or len(out) + length > out_len:
                raise ValueError(f"bad match dist={dist} len={length} at {len(out)}")
            for _ in range(length):
                out.append(out[-dist])
    if len(out) != out_len:
        raise ValueError(f"len {len(out)} != {out_len}")
    return bytes(out)


def roundtrip(data: bytes, stride: int, bpp: int) -> int:
    tokens = lz.lz77(data, stride)
    blob = encode_static(tokens, stride, bpp)
    back = decode_tokens(blob, len(data), stride, bpp)
    if back != data:
        raise RuntimeError("mismatch")
    return len(blob)


def subgreen(rgb: bytes) -> bytes:
    out = bytearray(len(rgb))
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        out[i] = (r - g) & 255
        out[i + 1] = g
        out[i + 2] = (b - g) & 255
    return bytes(out)


def file_size(rgb: bytes, w: int, h: int) -> tuple[int, str]:
    best = HEADER + len(rgb)
    name = "store"
    options = [
        ("raw", rgb, w * 3, 3, 0),
        ("sub", lz.sub_filter(rgb, 3), w * 3, 3, 0),
        ("up", lz.up_filter(rgb, w * 3), w * 3, 3, 0),
        ("sg", subgreen(rgb), w * 3, 3, 0),
    ]
    pal = lz.palette(rgb)
    if pal:
        head, idxs = pal
        options.append(("pal", idxs, w, 1, len(head)))
        options.append(("pal-sub", lz.sub_filter(idxs, 1), w, 1, len(head)))
        options.append(("pal-up", lz.up_filter(idxs, w), w, 1, len(head)))
    for mode, data, stride, bpp, extra in options:
        n = HEADER + extra + roundtrip(data, stride, bpp)
        if n < best:
            best = n
            name = mode
    return best, name


def _self_test() -> None:
    sample = bytes([7, 7, 7, 8, 9] + [7] * 300 + [8, 9, 7] * 40)
    n = roundtrip(sample, 1, 1)
    if n <= 0:
        raise RuntimeError("empty")
    image = bytes([1, 2, 3, 9, 9, 9] * 80)
    roundtrip(image, 3, 3)


if __name__ == "__main__":
    _self_test()
    root = Path("benchmarks/work")
    for group in ("dev", "holdout"):
        print("==", group, flush=True)
        for bmp in sorted((root / group).glob("*.bmp")):
            if bmp.name.count(".") > 1:
                continue
            image = Image.open(bmp).convert("RGB")
            rgb = image.tobytes()
            w, h = image.size
            webp = bmp.with_suffix(".webp").stat().st_size
            size, mode = file_size(rgb, w, h)
            flag = "WIN" if size < webp else "LOSE"
            print(f"{group}/{bmp.stem:10} webp={webp:7} rc={size:7} {flag} {mode}", flush=True)
