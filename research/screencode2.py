#!/usr/bin/env python3
"""Round 2 prototype of screen-content coding method 6 (D01 T07 section 4).

An exact implementation of the frozen design in `todo/01-compression/TODO-07-screen-content.md` section 4:
spatial + recent copy-distance inheritance (`L >= 2`) from a `(width + 3)`-entry causal distance ring and
4-entry `rep` queue, decomposed 2D `(dy, dx)` offsets with `rep_dy` and `dx == 0` on non-inherited copies
(`L >= 4`), and `EQ_W` / `EQ_N` neighbour flags, a 16-entry colour MTF list, a 2048-entry (11-bit) direct
colour cache, and `W_g >> 5`-conditioned subtract-green literals, all coded with method 4's 16-bit binary
range coder and counters (`src/rc.h`).

Only the development screenshots (`screens`) are measured by the CLI (after `research/corpus.py`'s
`verify_screens`); `heldout` is never read.

    python3 research/screencode2.py             # all ten development screenshots: sizes, ratios, split
    python3 research/screencode2.py dv_s03
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from screencode import Counter, Decoder, Encoder, Refused, fnv_bucket  # noqa: E402

MIN_HASH_MATCH = 4
MIN_INHERIT_MATCH = 2
MAX_MATCH = 4096
CHAIN = 64
WINDOW = (1 << 20) - 1  # 1,048,575
HASH_BITS = 16
CACHE_BITS = 11  # 2048-entry direct colour cache

TK_COPY = 0
TK_EQ_W = 1
TK_EQ_N = 2
TK_MTF = 3
TK_CACHE = 4
TK_LIT = 5


def prefix_code(value: int) -> tuple[int, int, int]:
    """RFC 9649 section 3.6.2.2 prefix code extended uniformly to 1 <= value <= 2^25 (symbols 0..49)."""
    if value < 1 or value > (1 << 25):
        raise ValueError(f"value out of prefix range: {value}")
    d = value - 1
    if d < 4:
        return d, 0, 0
    high = d.bit_length() - 1
    nbits = high - 1
    sym = 2 * high + ((d >> nbits) & 1)
    extra = d & ((1 << nbits) - 1)
    return sym, nbits, extra


def prefix_value(symbol: int, extra: int) -> int:
    """Inverse of `prefix_code`."""
    if symbol < 4:
        return symbol + 1
    nbits = (symbol - 2) >> 1
    return ((2 + (symbol & 1)) << nbits) + extra + 1


def cache_index(rgb: int) -> int:
    return ((0x1E35A7BD * (0xFF000000 | rgb)) & 0xFFFFFFFF) >> (32 - CACHE_BITS)


def put_tree(enc: Encoder, tree: list[Counter], value: int, levels: int) -> None:
    node = 1
    for i in range(levels - 1, -1, -1):
        bit = (value >> i) & 1
        enc.coded(tree[node], bit)
        node = 2 * node + bit


def get_tree(dec: Decoder, tree: list[Counter], levels: int) -> int:
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


def mtf_shift4(q: list[int], val: int) -> None:
    pos = 3
    for i in range(4):
        if q[i] == val:
            pos = i
            break
    while pos > 0:
        q[pos] = q[pos - 1]
        pos -= 1
    q[0] = val


def gather_inherit_cands(p: int, width: int, orig_ring: list[int], ring_cap: int, rep: list[int]) -> list[int]:
    """Up to 4 distinct causal copy distances in 1..min(p, WINDOW) at pixel p >= 1."""
    maxd = min(p, WINDOW)
    if maxd < 1:
        return []
    x = p % width
    y = p // width
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


class Model6:
    """Every adaptive 16-bit counter of method 6, all starting at 32768 (n = 0)."""

    def __init__(self) -> None:
        self.is_copy = [Counter() for _ in range(6)]
        self.is_inherit = [Counter() for _ in range(6)]
        self.inh_b0 = [Counter() for _ in range(6)]
        self.inh_b1 = Counter()
        self.inh_b2 = Counter()

        self.len_tree = [[Counter() for _ in range(32)] for _ in range(2)]
        self.len_extra = [[Counter() for _ in range(24)] for _ in range(2)]

        self.is_same_row = [Counter() for _ in range(2)]
        self.is_rep_dy = Counter()
        self.rep_dy_b0 = Counter()
        self.rep_dy_b1 = [Counter() for _ in range(2)]
        self.dy_tree = [Counter() for _ in range(64)]
        self.dy_extra = [Counter() for _ in range(40)]

        self.dx0_tree = [Counter() for _ in range(64)]
        self.dx0_extra = [Counter() for _ in range(40)]

        self.dx_zero = [Counter() for _ in range(2)]
        self.dx_sign = Counter()
        self.dx_tree = [Counter() for _ in range(64)]
        self.dx_extra = [Counter() for _ in range(50)]

        self.is_eq_w = [Counter() for _ in range(12)]
        self.is_eq_n = [Counter() for _ in range(6)]
        self.is_mtf = [Counter() for _ in range(6)]
        self.mtf_tree = [Counter() for _ in range(16)]
        self.is_cache = [Counter() for _ in range(6)]
        self.cache_tree = [Counter() for _ in range(1 << CACHE_BITS)]
        self.lit_g = [[Counter() for _ in range(256)] for _ in range(8)]
        self.lit_rg = [[Counter() for _ in range(256)] for _ in range(4)]
        self.lit_bg = [[Counter() for _ in range(256)] for _ in range(4)]


def encode_info(width: int, height: int, data: bytes, split: dict[str, int] | None = None) -> tuple[bytes, int]:
    """Encode packed RGB `data` with method 6; returns `(payload, covered4_pixels)`."""
    if width < 1 or height < 1 or len(data) != 3 * width * height:
        raise ValueError("invalid dimensions or RGB length")
    n = width * height
    pixels = [int.from_bytes(data[3 * i : 3 * i + 3], "big") for i in range(n)]
    head = [-1] * (1 << HASH_BITS)
    prev = [-1] * n
    ring_cap = width + 3
    orig_ring = [0] * ring_cap
    rep = [1, width, width + 1, width - 1]
    rep_dy = [1, 2, 4, 8]
    mtf: list[int] = []
    cache: list[int] = [-1] * (1 << CACHE_BITS)
    covered4 = 0

    def match_len(a: int, b: int, limit: int) -> int:
        if pixels[a] != pixels[b]:
            return 0
        lo, hi = 1, limit
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if data[3 * a : 3 * (a + mid)] == data[3 * b : 3 * (b + mid)]:
                lo = mid
            else:
                hi = mid - 1
        return lo

    def insert_pos(q: int) -> None:
        if q + MIN_HASH_MATCH <= n:
            b = fnv_bucket(data[3 * q : 3 * (q + MIN_HASH_MATCH)])
            prev[q] = head[b]
            head[b] = q

    def update_color(rgb: int) -> None:
        if rgb in mtf:
            mtf.remove(rgb)
        mtf.insert(0, rgb)
        if len(mtf) > 16:
            mtf.pop()
        cache[cache_index(rgb)] = rgb

    enc = Encoder()
    m = Model6()
    prev_kind = TK_LIT
    prev_same_row = 0

    # Pixel 0 is coded unconditionally as a literal (no earlier pixels exist).
    before = len(enc.out) + enc.pending
    rgb0 = pixels[0]
    r0, g0, b0 = rgb0 >> 16, (rgb0 >> 8) & 0xFF, rgb0 & 0xFF
    put_tree(enc, m.lit_g[4], g0, 8)
    put_tree(enc, m.lit_rg[g0 >> 6], (r0 - g0) & 0xFF, 8)
    put_tree(enc, m.lit_bg[g0 >> 6], (b0 - g0) & 0xFF, 8)
    orig_ring[0] = 0
    insert_pos(0)
    update_color(rgb0)
    if split is not None:
        split["literal"] = split.get("literal", 0) + (len(enc.out) + enc.pending - before)

    p = 1
    while p < n:
        before = len(enc.out) + enc.pending
        limit = min(MAX_MATCH, n - p)
        cands = gather_inherit_cands(p, width, orig_ring, ring_cap, rep)

        best_inh_len, best_inh_idx, best_inh_dist = 0, -1, 0
        if limit >= MIN_INHERIT_MATCH:
            for idx, d in enumerate(cands):
                ml = match_len(p, p - d, limit)
                if ml >= MIN_INHERIT_MATCH and ml > best_inh_len:
                    best_inh_len, best_inh_idx, best_inh_dist = ml, idx, d

        best_gen_len, best_gen_dist = 0, 0
        if limit >= MIN_HASH_MATCH:
            hcands = [p - 1]
            if p >= width and width <= WINDOW:
                hcands.append(p - width)
            q = head[fnv_bucket(data[3 * p : 3 * (p + MIN_HASH_MATCH)])]
            visited = 0
            while q >= 0 and visited < CHAIN and p - q <= WINDOW:
                hcands.append(q)
                visited += 1
                q = prev[q]
            for c in hcands:
                d = p - c
                if d > WINDOW:
                    continue
                ml = match_len(p, c, limit)
                if ml >= MIN_HASH_MATCH and (ml > best_gen_len or (ml == best_gen_len and d < best_gen_dist)):
                    best_gen_len, best_gen_dist = ml, d

        use_copy = False
        use_inh = False
        chosen_len, chosen_dist, chosen_inh_idx = 0, 0, -1
        if best_inh_len >= MIN_INHERIT_MATCH and (best_gen_len < MIN_HASH_MATCH or best_inh_len + 1 >= best_gen_len):
            use_copy, use_inh = True, True
            chosen_len, chosen_dist, chosen_inh_idx = best_inh_len, best_inh_dist, best_inh_idx
        elif best_gen_len >= MIN_HASH_MATCH:
            use_copy = True
            if best_gen_dist in cands:
                use_inh = True
                chosen_len, chosen_dist, chosen_inh_idx = best_gen_len, best_gen_dist, cands.index(best_gen_dist)
            else:
                use_inh = False
                chosen_len, chosen_dist = best_gen_len, best_gen_dist

        if use_copy:
            enc.coded(m.is_copy[prev_kind], 1)
            enc.coded(m.is_inherit[prev_kind], 1 if use_inh else 0)
            if use_inh:
                if len(cands) > 1:
                    enc.coded(m.inh_b0[prev_kind], 1 if chosen_inh_idx == 0 else 0)
                    if chosen_inh_idx > 0 and len(cands) > 2:
                        enc.coded(m.inh_b1, 1 if chosen_inh_idx == 1 else 0)
                        if chosen_inh_idx > 1 and len(cands) > 3:
                            enc.coded(m.inh_b2, 1 if chosen_inh_idx == 2 else 0)
                sym, nbits, extra = prefix_code(chosen_len - 1)
                put_tree(enc, m.len_tree[1], sym, 5)
                put_extra(enc, m.len_extra[1][sym], nbits, extra)
            else:
                sym, nbits, extra = prefix_code(chosen_len - 3)
                put_tree(enc, m.len_tree[0], sym, 5)
                put_extra(enc, m.len_extra[0][sym], nbits, extra)
                q = p - chosen_dist
                py, px_col = p // width, p % width
                qy, qx = q // width, q % width
                dy = py - qy
                if py == 0:
                    same_row = 1
                elif px_col == 0:
                    same_row = 0
                else:
                    same_row = 1 if dy == 0 else 0
                    enc.coded(m.is_same_row[prev_same_row], same_row)
                if same_row:
                    dx = px_col - qx
                    sym, nbits, extra = prefix_code(dx)
                    put_tree(enc, m.dx0_tree, sym, 6)
                    put_extra(enc, m.dx0_extra[sym], nbits, extra)
                    prev_same_row = 1
                else:
                    dx = px_col - qx
                    in_rdy = dy in rep_dy
                    enc.coded(m.is_rep_dy, 1 if in_rdy else 0)
                    if in_rdy:
                        ridx = rep_dy.index(dy)
                        enc.coded(m.rep_dy_b0, 1 if ridx == 0 else 0)
                        if ridx > 0:
                            enc.coded(m.rep_dy_b1[0], 1 if ridx == 1 else 0)
                            if ridx > 1:
                                enc.coded(m.rep_dy_b1[1], 1 if ridx == 2 else 0)
                    else:
                        sym, nbits, extra = prefix_code(dy)
                        put_tree(enc, m.dy_tree, sym, 6)
                        put_extra(enc, m.dy_extra[sym], nbits, extra)
                    mtf_shift4(rep_dy, dy)

                    enc.coded(m.dx_zero[1 if in_rdy else 0], 1 if dx == 0 else 0)
                    if dx != 0:
                        enc.coded(m.dx_sign, 1 if dx < 0 else 0)
                        sym, nbits, extra = prefix_code(abs(dx))
                        put_tree(enc, m.dx_tree, sym, 6)
                        put_extra(enc, m.dx_extra[sym], nbits, extra)
                    prev_same_row = 0

            mtf_shift4(rep, chosen_dist)
            for q in range(p, p + chosen_len):
                orig_ring[q % ring_cap] = chosen_dist
                insert_pos(q)
                update_color(pixels[q])
            if chosen_len >= MIN_HASH_MATCH:
                covered4 += chosen_len
            p += chosen_len
            prev_kind = TK_COPY
            if split is not None:
                key = "inh_copy" if use_inh else "new_copy"
                split[key] = split.get(key, 0) + (len(enc.out) + enc.pending - before)
            continue

        enc.coded(m.is_copy[prev_kind], 0)
        rgb = pixels[p]
        x, y = p % width, p // width
        has_w = x >= 1
        has_n = y >= 1
        w_rgb = pixels[p - 1] if has_w else -1
        n_rgb = pixels[p - width] if has_n else -2

        if has_w:
            wn_eq = 1 if (has_n and w_rgb == n_rgb) else 0
            if rgb == w_rgb:
                enc.coded(m.is_eq_w[prev_kind * 2 + wn_eq], 1)
                orig_ring[p % ring_cap] = 0
                insert_pos(p)
                update_color(rgb)
                p += 1
                prev_kind = TK_EQ_W
                if split is not None:
                    split["eq_w"] = split.get("eq_w", 0) + (len(enc.out) + enc.pending - before)
                continue
            enc.coded(m.is_eq_w[prev_kind * 2 + wn_eq], 0)

        if has_n and (not has_w or n_rgb != w_rgb):
            if rgb == n_rgb:
                enc.coded(m.is_eq_n[prev_kind], 1)
                orig_ring[p % ring_cap] = 0
                insert_pos(p)
                update_color(rgb)
                p += 1
                prev_kind = TK_EQ_N
                if split is not None:
                    split["eq_n"] = split.get("eq_n", 0) + (len(enc.out) + enc.pending - before)
                continue
            enc.coded(m.is_eq_n[prev_kind], 0)

        if rgb in mtf:
            enc.coded(m.is_mtf[prev_kind], 1)
            put_tree(enc, m.mtf_tree, mtf.index(rgb), 4)
            tk = TK_MTF
            sname = "mtf"
        else:
            enc.coded(m.is_mtf[prev_kind], 0)
            cidx = cache_index(rgb)
            if cache[cidx] == rgb:
                enc.coded(m.is_cache[prev_kind], 1)
                put_tree(enc, m.cache_tree, cidx, CACHE_BITS)
                tk = TK_CACHE
                sname = "cache"
            else:
                enc.coded(m.is_cache[prev_kind], 0)
                r, g, b = rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF
                wg_ctx = ((w_rgb >> 8) & 0xFF) >> 5 if has_w else 4
                put_tree(enc, m.lit_g[wg_ctx], g, 8)
                put_tree(enc, m.lit_rg[g >> 6], (r - g) & 0xFF, 8)
                put_tree(enc, m.lit_bg[g >> 6], (b - g) & 0xFF, 8)
                tk = TK_LIT
                sname = "literal"

        orig_ring[p % ring_cap] = 0
        insert_pos(p)
        update_color(rgb)
        p += 1
        prev_kind = tk
        if split is not None:
            split[sname] = split.get(sname, 0) + (len(enc.out) + enc.pending - before)

    out = enc.finish()
    if split is not None:
        split["flush"] = split.get("flush", 0) + len(out) - sum(v for k, v in split.items() if k != "flush")
    return out, covered4


def encode(width: int, height: int, data: bytes, split: dict[str, int] | None = None) -> bytes:
    """Return the method 6 payload for packed RGB `data`."""
    payload, _ = encode_info(width, height, data, split=split)
    return payload


def decode(width: int, height: int, payload: bytes) -> bytes:
    """Restore packed RGB from a method 6 payload, or raise `Refused` naming the rule broken."""
    if width < 1 or height < 1:
        raise Refused("invalid dimensions")
    if len(payload) < 5:
        raise Refused("payload shorter than 5 bytes")
    n = width * height
    dec = Decoder(payload)
    m = Model6()
    out = [0] * n
    ring_cap = width + 3
    orig_ring = [0] * ring_cap
    rep = [1, width, width + 1, width - 1]
    rep_dy = [1, 2, 4, 8]
    mtf: list[int] = []
    cache: list[int] = [-1] * (1 << CACHE_BITS)

    def update_color(rgb: int) -> None:
        if rgb in mtf:
            mtf.remove(rgb)
        mtf.insert(0, rgb)
        if len(mtf) > 16:
            mtf.pop()
        cache[cache_index(rgb)] = rgb

    g0 = get_tree(dec, m.lit_g[4], 8)
    r0 = (get_tree(dec, m.lit_rg[g0 >> 6], 8) + g0) & 0xFF
    b0 = (get_tree(dec, m.lit_bg[g0 >> 6], 8) + g0) & 0xFF
    rgb0 = (r0 << 16) | (g0 << 8) | b0
    out[0] = rgb0
    orig_ring[0] = 0
    update_color(rgb0)

    prev_kind = TK_LIT
    prev_same_row = 0
    p = 1
    while p < n:
        cands = gather_inherit_cands(p, width, orig_ring, ring_cap, rep)
        if dec.coded(m.is_copy[prev_kind]):
            use_inh = bool(dec.coded(m.is_inherit[prev_kind]))
            if use_inh:
                if len(cands) == 1:
                    idx = 0
                elif dec.coded(m.inh_b0[prev_kind]):
                    idx = 0
                elif len(cands) == 2 or dec.coded(m.inh_b1):
                    idx = 1
                elif len(cands) == 3 or dec.coded(m.inh_b2):
                    idx = 2
                else:
                    idx = 3
                dist = cands[idx]
                sym = get_tree(dec, m.len_tree[1], 5)
                if sym > 23:
                    raise Refused(f"inherited length symbol {sym} above 23")
                nbits = 0 if sym < 4 else (sym - 2) >> 1
                length = prefix_value(sym, get_extra(dec, m.len_extra[1][sym], nbits)) + 1
                if length > MAX_MATCH or p + length > n:
                    raise Refused(f"inherited copy of {length} pixels past the image or above 4096")
            else:
                sym = get_tree(dec, m.len_tree[0], 5)
                if sym > 23:
                    raise Refused(f"length symbol {sym} above 23")
                nbits = 0 if sym < 4 else (sym - 2) >> 1
                length = prefix_value(sym, get_extra(dec, m.len_extra[0][sym], nbits)) + 3
                if length > MAX_MATCH or p + length > n:
                    raise Refused(f"copy of {length} pixels past the image or above 4096")
                py, px_col = p // width, p % width
                if py == 0:
                    same_row = 1
                elif px_col == 0:
                    same_row = 0
                else:
                    same_row = dec.coded(m.is_same_row[prev_same_row])
                if same_row:
                    sym = get_tree(dec, m.dx0_tree, 6)
                    if sym > 39:
                        raise Refused(f"same-row distance symbol {sym} above 39")
                    nbits = 0 if sym < 4 else (sym - 2) >> 1
                    dx = prefix_value(sym, get_extra(dec, m.dx0_extra[sym], nbits))
                    if dx > px_col or dx > WINDOW:
                        raise Refused(f"same-row distance {dx} out of bounds")
                    dist = dx
                    prev_same_row = 1
                else:
                    in_rdy = bool(dec.coded(m.is_rep_dy))
                    if in_rdy:
                        if dec.coded(m.rep_dy_b0):
                            ridx = 0
                        elif dec.coded(m.rep_dy_b1[0]):
                            ridx = 1
                        elif dec.coded(m.rep_dy_b1[1]):
                            ridx = 2
                        else:
                            ridx = 3
                        dy = rep_dy[ridx]
                    else:
                        sym = get_tree(dec, m.dy_tree, 6)
                        if sym > 39:
                            raise Refused(f"dy symbol {sym} above 39")
                        nbits = 0 if sym < 4 else (sym - 2) >> 1
                        dy = prefix_value(sym, get_extra(dec, m.dy_extra[sym], nbits))
                    if dy > py or dy > WINDOW:
                        raise Refused(f"dy {dy} out of bounds")
                    mtf_shift4(rep_dy, dy)

                    if dec.coded(m.dx_zero[1 if in_rdy else 0]):
                        dx = 0
                    else:
                        neg = dec.coded(m.dx_sign)
                        sym = get_tree(dec, m.dx_tree, 6)
                        if sym > 49:
                            raise Refused(f"dx symbol {sym} above 49")
                        nbits = 0 if sym < 4 else (sym - 2) >> 1
                        adx = prefix_value(sym, get_extra(dec, m.dx_extra[sym], nbits))
                        dx = -adx if neg else adx
                    qx = px_col - dx
                    if qx < 0 or qx >= width:
                        raise Refused(f"qx {qx} outside 0..{width - 1}")
                    dist = dy * width + dx
                    if dist > WINDOW:
                        raise Refused(f"2D copy distance {dist} above WINDOW")
                    prev_same_row = 0

            for q in range(p, p + length):
                rgb = out[q - dist]
                out[q] = rgb
                orig_ring[q % ring_cap] = dist
                update_color(rgb)
            mtf_shift4(rep, dist)
            p += length
            prev_kind = TK_COPY
            continue

        x, y = p % width, p // width
        has_w = x >= 1
        has_n = y >= 1
        w_rgb = out[p - 1] if has_w else -1
        n_rgb = out[p - width] if has_n else -2

        if has_w:
            wn_eq = 1 if (has_n and w_rgb == n_rgb) else 0
            if dec.coded(m.is_eq_w[prev_kind * 2 + wn_eq]):
                out[p] = w_rgb
                orig_ring[p % ring_cap] = 0
                update_color(w_rgb)
                p += 1
                prev_kind = TK_EQ_W
                continue

        if has_n and (not has_w or n_rgb != w_rgb):
            if dec.coded(m.is_eq_n[prev_kind]):
                out[p] = n_rgb
                orig_ring[p % ring_cap] = 0
                update_color(n_rgb)
                p += 1
                prev_kind = TK_EQ_N
                continue

        if dec.coded(m.is_mtf[prev_kind]):
            midx = get_tree(dec, m.mtf_tree, 4)
            if midx >= len(mtf):
                raise Refused(f"mtf index {midx} >= mtf_count {len(mtf)}")
            rgb = mtf[midx]
            tk = TK_MTF
        elif dec.coded(m.is_cache[prev_kind]):
            cidx = get_tree(dec, m.cache_tree, CACHE_BITS)
            if cache[cidx] < 0:
                raise Refused(f"cache hit on entry {cidx}, never set")
            rgb = cache[cidx]
            tk = TK_CACHE
        else:
            wg_ctx = ((w_rgb >> 8) & 0xFF) >> 5 if has_w else 4
            g = get_tree(dec, m.lit_g[wg_ctx], 8)
            r = (get_tree(dec, m.lit_rg[g >> 6], 8) + g) & 0xFF
            b = (get_tree(dec, m.lit_bg[g >> 6], 8) + g) & 0xFF
            rgb = (r << 16) | (g << 8) | b
            tk = TK_LIT

        out[p] = rgb
        orig_ring[p % ring_cap] = 0
        update_color(rgb)
        p += 1
        prev_kind = tk

    if dec.pos != len(payload):
        raise Refused(f"stream ends at byte {dec.pos}, payload has {len(payload)}")
    res = bytearray(3 * n)
    for i, rgb in enumerate(out):
        res[3 * i : 3 * i + 3] = rgb.to_bytes(3, "big")
    return bytes(res)


def main(argv: list[str]) -> int:
    import ab
    import corpus
    import screencode

    names = argv or ab.SETS["screens"]
    unknown = [n for n in names if n not in ab.SETS["screens"]]
    if unknown:
        raise SystemExit(
            f"screencode2 measures only the development screenshots (set screens); refused: {', '.join(unknown)}"
        )
    corpus.verify_screens(corpus.PPM)
    refs = json.loads((ab.RES / "jxl_refs.json").read_text(encoding="utf-8"))
    keys = ("inh_copy", "new_copy", "eq_w", "eq_n", "mtf", "cache", "literal", "flush")
    total_split = {k: 0 for k in keys}
    tot_pay = 0
    tot_fold = 0
    tot_sc5 = 0
    tot_webp = 0
    webp_ratios: list[float] = []
    sc5_ratios: list[float] = []
    print(
        f"{'image':7} {'payload':>8} {'fold':>8} {'sc5':>8} {'webp':>8} {'vs_sc5':>7} {'vs_webp':>8} "
        f"{'cov4':>6} {'inh_cp':>7} {'new_cp':>7} {'eq_w':>6} {'eq_n':>6} {'mtf':>6} {'cache':>6} {'lit':>7}"
    )
    for name in names:
        w, h, data = corpus._ppm_pixels(corpus.PPM / f"{name}.ppm")
        split: dict[str, int] = {}
        payload, cov4 = encode_info(w, h, data, split=split)
        if decode(w, h, payload) != data:
            raise SystemExit(f"{name}: decoder did not restore the pixels")
        sc5_fold = len(screencode.encode(w, h, data)) + 32
        fold_sz = len(payload) + 32
        webp_sz = ab.ref_size(refs, name, "webp")
        r_sc5 = fold_sz / sc5_fold
        r_webp = fold_sz / webp_sz
        sc5_ratios.append(r_sc5)
        webp_ratios.append(r_webp)
        tot_pay += len(payload)
        tot_fold += fold_sz
        tot_sc5 += sc5_fold
        tot_webp += webp_sz
        for k in keys:
            total_split[k] += split.get(k, 0)
        print(
            f"{name:7} {len(payload):8d} {fold_sz:8d} {sc5_fold:8d} {webp_sz:8d} {r_sc5:7.4f} {r_webp:8.4f} "
            f"{cov4 / (w * h):6.4f} {split.get('inh_copy', 0):7d} {split.get('new_copy', 0):7d} "
            f"{split.get('eq_w', 0):6d} {split.get('eq_n', 0):6d} {split.get('mtf', 0):6d} "
            f"{split.get('cache', 0):6d} {split.get('literal', 0):7d}",
            flush=True,
        )
    gm_sc5 = math.exp(sum(math.log(x) for x in sc5_ratios) / len(sc5_ratios))
    gm_webp = math.exp(sum(math.log(x) for x in webp_ratios) / len(webp_ratios))
    print(
        f"total payload={tot_pay} fold={tot_fold} sc5={tot_sc5} webp={tot_webp} "
        f"sc5_ratio={tot_fold / tot_sc5:.4f} sc5_geomean={gm_sc5:.4f} "
        f"webp_ratio={tot_fold / tot_webp:.4f} webp_geomean={gm_webp:.4f} "
        f"inh_copy={total_split['inh_copy']} new_copy={total_split['new_copy']} "
        f"eq_w={total_split['eq_w']} eq_n={total_split['eq_n']} mtf={total_split['mtf']} "
        f"cache={total_split['cache']} literal={total_split['literal']} flush={total_split['flush']} "
        f"round_trip=exact"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
