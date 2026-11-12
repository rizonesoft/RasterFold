#!/usr/bin/env python3
"""Round 1 prototype of a backward-reference coding method for screen content (D01 T07 section 3).

An exact implementation of the frozen design in `todo/01-compression/TODO-07-screen-content.md` section 3:
literals (subtract-green), a 256-entry colour cache and LZ77 copies, coded with method 4's 16-bit range coder and
counters, every symbol and context as that section defines them. `encode` writes the payload (the range-coded
stream alone) and `decode` restores the pixels from those bytes alone, refusing every malformed stream the
section lists. The sizes it reports are the bytes a decoder needs, not an estimate.

Only the development screenshots are measured (after `research/corpus.py`'s `verify_screens`). Standard
library and Pillow only.

    python3 research/screencode.py              # all ten development screenshots: sizes, round trip, split
    python3 research/screencode.py dv_s03
"""
from __future__ import annotations

import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

MIN_MATCH = 4
MAX_MATCH = 4096
CHAIN = 64
WINDOW = (1 << 20) - 120
HASH_BITS = 16
CACHE_BITS = 8

# RFC 9649 section 3.6.2.2.1, Figure 20: distance code v (1..120) names DISTANCE_MAP[v - 1] = (xi, yi).
DISTANCE_MAP = (
    (0, 1), (1, 0), (1, 1), (-1, 1), (0, 2), (2, 0), (1, 2),
    (-1, 2), (2, 1), (-2, 1), (2, 2), (-2, 2), (0, 3), (3, 0),
    (1, 3), (-1, 3), (3, 1), (-3, 1), (2, 3), (-2, 3), (3, 2),
    (-3, 2), (0, 4), (4, 0), (1, 4), (-1, 4), (4, 1), (-4, 1),
    (3, 3), (-3, 3), (2, 4), (-2, 4), (4, 2), (-4, 2), (0, 5),
    (3, 4), (-3, 4), (4, 3), (-4, 3), (5, 0), (1, 5), (-1, 5),
    (5, 1), (-5, 1), (2, 5), (-2, 5), (5, 2), (-5, 2), (4, 4),
    (-4, 4), (3, 5), (-3, 5), (5, 3), (-5, 3), (0, 6), (6, 0),
    (1, 6), (-1, 6), (6, 1), (-6, 1), (2, 6), (-2, 6), (6, 2),
    (-6, 2), (4, 5), (-4, 5), (5, 4), (-5, 4), (3, 6), (-3, 6),
    (6, 3), (-6, 3), (0, 7), (7, 0), (1, 7), (-1, 7), (5, 5),
    (-5, 5), (7, 1), (-7, 1), (4, 6), (-4, 6), (6, 4), (-6, 4),
    (2, 7), (-2, 7), (7, 2), (-7, 2), (3, 7), (-3, 7), (7, 3),
    (-7, 3), (5, 6), (-5, 6), (6, 5), (-6, 5), (8, 0), (4, 7),
    (-4, 7), (7, 4), (-7, 4), (8, 1), (8, 2), (6, 6), (-6, 6),
    (8, 3), (5, 7), (-5, 7), (7, 5), (-7, 5), (8, 4), (6, 7),
    (-6, 7), (7, 6), (-7, 6), (8, 5), (7, 7), (-7, 7), (8, 6),
    (8, 7),
)
assert len(DISTANCE_MAP) == 120

LITERAL, CACHE, COPY = 0, 1, 2


class Refused(Exception):
    """A malformed method 5 stream; the message names the rule."""


# ------------------------------------------------------------------ the range coder, as src/rc.h and method 4

RATE = [(65536 * 2) // (2 * n + 3) for n in range(61)]


class Counter:
    """Method 4's adaptive probability: c is the probability of a 1 in 1/65536, n the adaptation count."""
    __slots__ = ("c", "n")

    def __init__(self) -> None:
        self.c = 32768
        self.n = 0

    def take(self, bit: int) -> int:
        """The probability to code with, then the update `pal_update_m3` applies (method 4)."""
        p = self.c or 1
        r = RATE[self.n]
        if self.n < 60:
            self.n += 1
        cur = self.c
        if bit:
            if cur < 65535:
                step = ((65535 - cur) * r) >> 16
                self.c = cur + (step or 1)
        elif cur > 0:
            step = (cur * r) >> 16
            self.c = cur - (step or 1)
        return p


class Encoder:
    def __init__(self) -> None:
        self.low = 0
        self.range = 0xFFFFFFFF
        self.cache = 0
        self.pending = 1
        self.out = bytearray()
        self.bits = {}

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

    def bit16(self, bit: int, p1: int) -> None:
        bound = (self.range * p1) >> 16
        if bit:
            self.range = bound
        else:
            self.low += bound
            self.range -= bound
        while self.range < (1 << 24):
            self.range = (self.range << 8) & 0xFFFFFFFF
            self._shift()

    def coded(self, counter: Counter, bit: int) -> None:
        self.bit16(bit, counter.take(bit))

    def half(self, bit: int) -> None:
        self.bit16(bit, 32768)

    def finish(self) -> bytes:
        for _ in range(5):
            self._shift()
        return bytes(self.out)


class Decoder:
    def __init__(self, data: bytes) -> None:
        if len(data) < 5:
            raise Refused("payload shorter than 5 bytes")
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

    def bit16(self, p1: int) -> int:
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

    def coded(self, counter: Counter) -> int:
        # The probability depends only on the counter's state before the bit; update after decoding.
        p = counter.c or 1
        bit = self.bit16(p)
        took = counter.take(bit)
        assert took == p
        return bit

    def half(self) -> int:
        return self.bit16(32768)


# ------------------------------------------------------------------ shared definitions

def prefix_code(value: int) -> tuple[int, int, int]:
    """RFC 9649 section 3.6.2.2: (symbol, extra bit count, extra bits value) for a value >= 1."""
    d = value - 1
    if d < 4:
        return d, 0, 0
    high = d.bit_length() - 1
    second = (d >> (high - 1)) & 1
    nbits = high - 1
    return 2 * high + second, nbits, d & ((1 << nbits) - 1)


def prefix_value(symbol: int, extra: int) -> int:
    """RFC 9649 section 3.6.2.2's pseudocode."""
    if symbol < 4:
        return symbol + 1
    nbits = (symbol - 2) >> 1
    offset = (2 + (symbol & 1)) << nbits
    return offset + extra + 1


def cache_index(rgb: int) -> int:
    return ((0x1E35A7BD * (0xFF000000 | rgb)) & 0xFFFFFFFF) >> (32 - CACHE_BITS)


def fnv_bucket(key: bytes) -> int:
    h = 2166136261
    for b in key:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h >> (32 - HASH_BITS)


def map_distance(v: int, width: int) -> int:
    xi, yi = DISTANCE_MAP[v - 1]
    return max(1, xi + yi * width)


def distance_code(distance: int, width: int) -> int:
    for i in range(120):
        if map_distance(i + 1, width) == distance:
            return i + 1
    return distance + 120


class Model:
    """Every adaptive counter of the method, all starting at one half."""

    def __init__(self) -> None:
        self.is_copy = [Counter() for _ in range(3)]
        self.is_cache = [Counter() for _ in range(3)]
        self.lit_g = [Counter() for _ in range(256)]
        self.lit_rg = [[Counter() for _ in range(256)] for _ in range(4)]
        self.lit_bg = [[Counter() for _ in range(256)] for _ in range(4)]
        self.cache_tree = [Counter() for _ in range(256)]
        self.len_tree = [Counter() for _ in range(32)]
        self.len_extra = [Counter() for _ in range(32)]
        self.dist_tree = [[Counter() for _ in range(64)] for _ in range(2)]
        self.dist_extra = [Counter() for _ in range(64)]


def put_tree(enc: Encoder, tree: list, value: int, levels: int) -> None:
    node = 1
    for i in range(levels - 1, -1, -1):
        bit = (value >> i) & 1
        enc.coded(tree[node], bit)
        node = 2 * node + bit


def get_tree(dec: Decoder, tree: list, levels: int) -> int:
    node = 1
    for _ in range(levels):
        node = 2 * node + dec.coded(tree[node])
    return node - (1 << levels)


def put_extra(enc: Encoder, first: Counter, nbits: int, value: int) -> None:
    for i in range(nbits - 1, -1, -1):
        bit = (value >> i) & 1
        if i == nbits - 1:
            enc.coded(first, bit)
        else:
            enc.half(bit)


def get_extra(dec: Decoder, first: Counter, nbits: int) -> int:
    value = 0
    for i in range(nbits):
        value = (value << 1) | (dec.coded(first) if i == 0 else dec.half())
    return value


# ------------------------------------------------------------------ the match search and the encoder

def tokens(width: int, height: int, data: bytes):
    """The frozen parse: yields (LITERAL, rgb), (CACHE, index) or (COPY, length, distance) in order."""
    n = width * height
    pixels = [int.from_bytes(data[3 * i:3 * i + 3], "big") for i in range(n)]
    head = [-1] * (1 << HASH_BITS)
    prev = [-1] * n
    cache: list[int | None] = [None] * (1 << CACHE_BITS)

    def match(a: int, b: int, limit: int) -> int:
        lo, hi = 0, limit
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if data[3 * a:3 * (a + mid)] == data[3 * b:3 * (b + mid)]:
                lo = mid
            else:
                hi = mid - 1
        return lo

    def insert(q: int) -> None:
        if q + MIN_MATCH <= n:
            bucket = fnv_bucket(data[3 * q:3 * (q + MIN_MATCH)])
            prev[q] = head[bucket]
            head[bucket] = q

    p = 0
    while p < n:
        limit = min(MAX_MATCH, n - p)
        best_len = best_dist = 0
        if limit >= MIN_MATCH:
            candidates = []
            if p >= 1:
                candidates.append(p - 1)
            if p >= width:
                candidates.append(p - width)
            q = head[fnv_bucket(data[3 * p:3 * (p + MIN_MATCH)])]
            visited = 0
            while q >= 0 and visited < CHAIN and p - q <= WINDOW:
                candidates.append(q)
                visited += 1
                q = prev[q]
            for c in candidates:
                if p - c > WINDOW:
                    continue
                length = match(p, c, limit)
                dist = p - c
                if length > best_len or (length == best_len and length and dist < best_dist):
                    best_len, best_dist = length, dist
        if best_len >= MIN_MATCH:
            yield (COPY, best_len, best_dist)
            for q in range(p, p + best_len):
                insert(q)
                cache[cache_index(pixels[q])] = pixels[q]
            p += best_len
            continue
        px = pixels[p]
        idx = cache_index(px)
        if cache[idx] == px:
            yield (CACHE, idx)
        else:
            yield (LITERAL, px)
        insert(p)
        cache[idx] = px
        p += 1


def encode(width: int, height: int, data: bytes, split: dict | None = None) -> bytes:
    """The method 5 payload for packed RGB `data`. With `split`, the coded bytes per token kind are added to it
    (measured from the encoder's output length before and after each token, so they sum to the payload)."""
    enc = Encoder()
    m = Model()
    prev_kind, prev_short = LITERAL, 0
    for tok in tokens(width, height, data):
        before = len(enc.out) + enc.pending
        kind = tok[0]
        enc.coded(m.is_copy[prev_kind], 1 if kind == COPY else 0)
        if kind == COPY:
            length, dist = tok[1], tok[2]  # type: ignore[misc]
            sym, nbits, extra = prefix_code(length - 3)
            put_tree(enc, m.len_tree, sym, 5)
            put_extra(enc, m.len_extra[sym], nbits, extra)
            v = distance_code(dist, width)
            sym, nbits, extra = prefix_code(v)
            put_tree(enc, m.dist_tree[prev_short], sym, 6)
            put_extra(enc, m.dist_extra[sym], nbits, extra)
            prev_short = 1 if v <= 120 else 0
        else:
            enc.coded(m.is_cache[prev_kind], 1 if kind == CACHE else 0)
            if kind == CACHE:
                put_tree(enc, m.cache_tree, tok[1], 8)
            else:
                rgb = tok[1]
                r, g, b = rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF
                put_tree(enc, m.lit_g, g, 8)
                put_tree(enc, m.lit_rg[g >> 6], (r - g) & 0xFF, 8)
                put_tree(enc, m.lit_bg[g >> 6], (b - g) & 0xFF, 8)
        prev_kind = kind
        if split is not None:
            name = ("literal", "cache", "copy")[kind]
            split[name] = split.get(name, 0) + (len(enc.out) + enc.pending - before)
    out = enc.finish()
    if split is not None:
        split["flush"] = split.get("flush", 0) + len(out) - sum(v for k, v in split.items() if k != "flush")
    return out


def decode(width: int, height: int, payload: bytes) -> bytes:
    """Restore packed RGB from a method 5 payload, or raise Refused naming the rule broken."""
    n = width * height
    dec = Decoder(payload)
    m = Model()
    out = bytearray(3 * n)
    cache: list[int | None] = [None] * (1 << CACHE_BITS)
    prev_kind, prev_short = LITERAL, 0
    p = 0
    while p < n:
        if dec.coded(m.is_copy[prev_kind]):
            sym = get_tree(dec, m.len_tree, 5)
            if sym > 23:
                raise Refused(f"length symbol {sym} above 23")
            nbits = 0 if sym < 4 else (sym - 2) >> 1
            length = prefix_value(sym, get_extra(dec, m.len_extra[sym], nbits)) + 3
            if length > MAX_MATCH or p + length > n:
                raise Refused(f"copy of {length} pixels past the image or above 4096")
            sym = get_tree(dec, m.dist_tree[prev_short], 6)
            if sym > 39:
                raise Refused(f"distance symbol {sym} above 39")
            nbits = 0 if sym < 4 else (sym - 2) >> 1
            v = prefix_value(sym, get_extra(dec, m.dist_extra[sym], nbits))
            dist = map_distance(v, width) if v <= 120 else v - 120
            if dist > p:
                raise Refused(f"copy distance {dist} before the first pixel (at pixel {p})")
            for q in range(p, p + length):
                out[3 * q:3 * q + 3] = out[3 * (q - dist):3 * (q - dist) + 3]
                rgb = int.from_bytes(out[3 * q:3 * q + 3], "big")
                cache[cache_index(rgb)] = rgb
            p += length
            prev_kind, prev_short = COPY, (1 if v <= 120 else 0)
            continue
        if dec.coded(m.is_cache[prev_kind]):
            idx = get_tree(dec, m.cache_tree, 8)
            rgb = cache[idx]
            if rgb is None:
                raise Refused(f"cache hit on entry {idx}, never set")
            prev_kind = CACHE
        else:
            g = get_tree(dec, m.lit_g, 8)
            r = (get_tree(dec, m.lit_rg[g >> 6], 8) + g) & 0xFF
            b = (get_tree(dec, m.lit_bg[g >> 6], 8) + g) & 0xFF
            rgb = (r << 16) | (g << 8) | b
            prev_kind = LITERAL
        out[3 * p:3 * p + 3] = rgb.to_bytes(3, "big")
        cache[cache_index(rgb)] = rgb
        p += 1
    if dec.pos != len(payload):
        raise Refused(f"stream ends at byte {dec.pos}, payload has {len(payload)}")
    return bytes(out)


def main(argv: list[str]) -> int:
    import ab
    import corpus
    names = argv or ab.SETS["screens"]
    unknown = [n for n in names if n not in ab.SETS["screens"]]
    if unknown:
        raise SystemExit(f"screencode measures only the development screenshots (set screens); refused: {', '.join(unknown)}")
    corpus.verify_screens(corpus.PPM)
    total = {"size": 0, "literal": 0, "cache": 0, "copy": 0, "flush": 0}
    print(f"{'image':8} {'bytes':>9} {'literal':>9} {'cache':>8} {'copy':>9} {'round_trip':>10}")
    for name in names:
        w, h, data = corpus._ppm_pixels(corpus.PPM / f"{name}.ppm")
        split: dict = {}
        payload = encode(w, h, data, split)
        ok = decode(w, h, payload) == data
        if not ok:
            raise SystemExit(f"{name}: the decoder did not restore the pixels")
        print(f"{name:8} {len(payload):9d} {split.get('literal', 0):9d} {split.get('cache', 0):8d} "
              f"{split.get('copy', 0):9d} {'exact':>10}", flush=True)
        total["size"] += len(payload)
        for k in ("literal", "cache", "copy", "flush"):
            total[k] += split.get(k, 0)
    print(f"total bytes={total['size']} literal={total['literal']} cache={total['cache']} copy={total['copy']} "
          f"flush={total['flush']} round_trip=exact")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
