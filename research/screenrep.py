#!/usr/bin/env python3
"""How much repetition a screenshot offers a backward-reference coder (D01 T07 section 2).

For each image this reports the share of pixels equal to their left, above and above-left neighbours, the number
of distinct colours, and a greedy parse into literals, colour-cache hits and copies, with an entropy estimate of
what a coder of that kind would spend. The parse and the costs are defined in `todo/01-compression/TODO-07-
screen-content.md` section 2 and follow RFC 9649 (WebP lossless) where it applies:

* raster order over whole RGB pixels, greedy; candidates are distance 1, distance `width`, and the most recent
  64 earlier positions whose next four pixels hash equal; the longest exact match of at least 4 pixels (at most
  4096) wins, the shorter distance on a tie; a copy may overlap the pixels it produces;
* otherwise a 256-entry colour cache, index `(0x1e35a7bd * argb) >> 24` on the 32-bit ARGB value with alpha 255,
  every coded pixel inserted, including each pixel of a copy; a hit codes the index;
* otherwise a literal, its channels coded after subtracting green (`G`, `R - G`, `B - G` modulo 256);
* costs: the empirical Shannon entropy of each stream counted separately (token kind, the three literal
  channels, the cache index, the length and distance prefix symbols of RFC 9649 section 5.2.2) plus the exact
  extra bits of each length and distance. Excluded: entropy-code tables, transforms other than subtract-green,
  the 2D distance mapping and container overhead, so the figure is a lower bound for a coder of this kind.

Only the development screenshots are read, after `research/corpus.py`'s `verify_screens` passes; no held-out
image is opened. Standard library and Pillow only.

    python3 research/screenrep.py              # all ten development screenshots, a table and a total
    python3 research/screenrep.py dv_s03       # one image
"""
from __future__ import annotations

import math
import sys
from collections import Counter, deque
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

MIN_MATCH = 4
MAX_MATCH = 4096
CHAIN = 64
CACHE_BITS = 8


def prefix_code(value: int) -> tuple[int, int]:
    """RFC 9649 section 5.2.2: the prefix symbol and the number of extra bits for a length or distance >= 1."""
    if value < 1:
        raise ValueError(f"prefix-coded values start at 1, got {value}")
    d = value - 1
    if d < 4:
        return d, 0
    high = d.bit_length() - 1
    second = (d >> (high - 1)) & 1
    return 2 * high + second, high - 1


def cache_index(rgb: int) -> int:
    """RFC 9649 section 5.2.3 with 8 cache bits; `rgb` is 0xRRGGBB and alpha is 255."""
    argb = 0xFF000000 | rgb
    return ((0x1E35A7BD * argb) & 0xFFFFFFFF) >> (32 - CACHE_BITS)


def entropy_bits(counts: Counter) -> float:
    total = sum(counts.values())
    if total == 0:
        return 0.0
    return -sum(c * math.log2(c / total) for c in counts.values() if c)


def match_length(data: bytes, a: int, b: int, limit: int) -> int:
    """Pixels equal between positions a and b (a > b), up to `limit`, by binary search on slice equality.
    Comparing against the original pixels gives the overlapping-copy semantics of LZ77."""
    lo, hi = 0, limit
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if data[3 * a:3 * (a + mid)] == data[3 * b:3 * (b + mid)]:
            lo = mid
        else:
            hi = mid - 1
    return lo


def parse(width: int, height: int, data: bytes) -> dict:
    """The greedy parse and its costs; `data` is packed RGB, row-major."""
    n = width * height
    if len(data) != 3 * n:
        raise ValueError("pixel data does not match the dimensions")
    pixels = [int.from_bytes(data[3 * i:3 * i + 3], "big") for i in range(n)]
    chains: dict[bytes, deque] = {}
    cache: list[int | None] = [None] * (1 << CACHE_BITS)
    kinds = Counter()
    lit = [Counter(), Counter(), Counter()]
    cache_idx = Counter()
    len_sym, dist_sym = Counter(), Counter()
    extra = 0
    copy_lengths = []
    dist_class = Counter()

    def insert(p: int) -> None:
        if p + MIN_MATCH <= n:
            key = data[3 * p:3 * (p + MIN_MATCH)]
            chain = chains.get(key)
            if chain is None:
                chains[key] = chain = deque(maxlen=CHAIN)
            chain.append(p)

    def cache_put(p: int) -> None:
        cache[cache_index(pixels[p])] = pixels[p]

    p = 0
    while p < n:
        limit = min(MAX_MATCH, n - p)
        best_len, best_dist = 0, 0
        if limit >= MIN_MATCH:
            candidates = []
            if p >= 1:
                candidates.append(p - 1)
            if p >= width:
                candidates.append(p - width)
            chain = chains.get(data[3 * p:3 * (p + MIN_MATCH)])
            if chain:
                candidates.extend(reversed(chain))
            for c in candidates:
                length = match_length(data, p, c, limit)
                dist = p - c
                if length > best_len or (length == best_len and length and dist < best_dist):
                    best_len, best_dist = length, dist
        if best_len >= MIN_MATCH:
            kinds["copy"] += 1
            ls, le = prefix_code(best_len)
            ds, de = prefix_code(best_dist)
            len_sym[ls] += 1
            dist_sym[ds] += 1
            extra += le + de
            copy_lengths.append(best_len)
            if best_dist == 1:
                dist_class["left"] += 1
            elif best_dist == width:
                dist_class["above"] += 1
            elif best_dist < width and (p % width) >= best_dist:
                dist_class["same row"] += 1
            elif best_dist <= 64 * width:
                dist_class["within 64 rows"] += 1
            else:
                dist_class["further"] += 1
            for q in range(p, p + best_len):
                insert(q)
                cache_put(q)
            p += best_len
            continue
        px = pixels[p]
        idx = cache_index(px)
        if cache[idx] == px:
            kinds["cache"] += 1
            cache_idx[idx] += 1
        else:
            kinds["literal"] += 1
            r, g, b = px >> 16, (px >> 8) & 0xFF, px & 0xFF
            lit[0][g] += 1
            lit[1][(r - g) & 0xFF] += 1
            lit[2][(b - g) & 0xFF] += 1
        insert(p)
        cache_put(p)
        p += 1

    bits = {
        "kind": entropy_bits(kinds),
        "literal G": entropy_bits(lit[0]),
        "literal R-G": entropy_bits(lit[1]),
        "literal B-G": entropy_bits(lit[2]),
        "cache index": entropy_bits(cache_idx),
        "length symbol": entropy_bits(len_sym),
        "distance symbol": entropy_bits(dist_sym),
        "extra bits": float(extra),
    }
    covered = sum(copy_lengths)
    return {
        "pixels": n,
        "tokens": dict(kinds),
        "copy_pixels": covered,
        "copy_pixels_at_least": {k: sum(x for x in copy_lengths if x >= k) for k in (4, 8, 32)},
        "distance_classes": dict(dist_class),
        "bits": bits,
        "bytes": sum(bits.values()) / 8,
    }


def neighbour_shares(width: int, height: int, data: bytes) -> dict:
    left = above = above_left = 0
    for y in range(height):
        row = 3 * y * width
        for x in range(width):
            i = row + 3 * x
            px = data[i:i + 3]
            if x and data[i - 3:i] == px:
                left += 1
            if y and data[i - 3 * width:i - 3 * width + 3] == px:
                above += 1
            if x and y and data[i - 3 * width - 3:i - 3 * width] == px:
                above_left += 1
    n = width * height
    colours = len({data[3 * i:3 * i + 3] for i in range(n)})
    return {"left": left / n, "above": above / n, "above_left": above_left / n, "colours": colours}


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    import corpus
    return corpus._ppm_pixels(path)


def main(argv: list[str]) -> int:
    import ab
    import corpus
    names = argv or ab.SETS["screens"]
    unknown = [n for n in names if n not in ab.SETS["screens"]]
    if unknown:
        raise SystemExit(f"screenrep reads only the development screenshots (set screens); refused: {', '.join(unknown)}")
    corpus.verify_screens(corpus.PPM)
    total = Counter()
    print(f"{'image':8} {'left':>6} {'above':>6} {'colours':>8} {'literal':>8} {'cache':>7} {'copy':>6} {'copied':>7} "
          f"{'>=32':>6} {'est_bytes':>10}")
    for name in names:
        w, h, data = read_ppm(corpus.PPM / f"{name}.ppm")
        shares = neighbour_shares(w, h, data)
        result = parse(w, h, data)
        t = result["tokens"]
        print(f"{name:8} {shares['left']:6.3f} {shares['above']:6.3f} {shares['colours']:8d} {t.get('literal', 0):8d} "
              f"{t.get('cache', 0):7d} {t.get('copy', 0):6d} {result['copy_pixels'] / result['pixels']:7.3f} "
              f"{result['copy_pixels_at_least'][32] / result['pixels']:6.3f} {result['bytes']:10.0f}", flush=True)
        print(f"         bits: " + ", ".join(f"{k} {v / 8:.0f} B" for k, v in result["bits"].items())
              + " | copy distances: " + ", ".join(f"{k} {v}" for k, v in sorted(result["distance_classes"].items())), flush=True)
        total["est"] += result["bytes"]
    print(f"total est_bytes={total['est']:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
