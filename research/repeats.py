#!/usr/bin/env python3
"""Where exact repeats are: per-image repeat shares for the corpus.

Reports, on raw RGB channel planes of the PPM corpus in build/research/ppm:

A) shifted-row share: for each channel and shift -8..8, the share of samples
   in rows 1..H-1 equal to the sample one row above at that shift; the best
   shift per channel and the pooled mean of the three best shares.
B) earlier-row match coverage (retrospective): scanning each channel row
   left to right, the longest match of the current segment against any
   segment lying entirely in earlier rows of that channel (at most 16
   candidate positions per 8-sample key); a match of 8+ samples covers
   those samples and skips past them. Covered over samples in rows 1..H-1.

Usage: python3 research/repeats.py <set-or-image> [...] | --self-test
Sets and image names come from research/ab.py.
"""
from __future__ import annotations

import sys
from pathlib import Path

RES = Path(__file__).resolve().parent
sys.path.insert(0, str(RES))
from ab import SETS, PPM  # noqa: E402  (ab.py only defines data under import)

MIN_MATCH = 8
MAX_CANDIDATES = 16


def read_ppm(path: Path) -> tuple[int, int, list[bytes]]:
    data = path.read_bytes()
    idx, toks = 0, []
    while len(toks) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        toks.append(data[start:idx])
    idx += 1  # single whitespace byte after maxval
    if toks[0] != b"P6" or int(toks[3]) != 255:
        raise ValueError(f"{path}: not an 8-bit P6 PPM")
    w, h = int(toks[1]), int(toks[2])
    pix = data[idx:]
    if len(pix) != w * h * 3:
        raise ValueError(f"{path}: payload {len(pix)} bytes, expected {w * h * 3}")
    return w, h, [pix[c::3] for c in range(3)]


def _eq_count(a: bytes, b: bytes) -> int:
    x = int.from_bytes(a, "little") ^ int.from_bytes(b, "little")
    return x.to_bytes(len(a), "little").count(0)


def shifted_shares(plane: bytes, w: int, h: int) -> tuple[int, float]:
    """Best (shift, share) of samples equal to the sample one row above."""
    best_shift, best_share = 0, -1.0
    for s in range(-8, 9):
        eq = tot = 0
        for y in range(1, h):
            row = plane[y * w:(y + 1) * w]
            up = plane[(y - 1) * w:y * w]
            a, b = (row[:w - s], up[s:]) if s >= 0 else (row[-s:], up[:w + s])
            eq += _eq_count(a, b)
            tot += len(a)
        share = eq / tot if tot else 0.0
        if share > best_share:
            best_shift, best_share = s, share
    return best_shift, best_share


def match_coverage(plane: bytes, w: int, h: int) -> tuple[int, int]:
    """(covered, total) samples in rows 1..H-1 under greedy >=8 matches."""
    covered = total = 0
    index: dict[bytes, list[int]] = {}

    def add_row(y: int) -> None:
        base = y * w
        for x in range(0, w - MIN_MATCH + 1):
            index.setdefault(plane[base + x:base + x + MIN_MATCH], []).append(base + x)

    if h > 1:
        add_row(0)
    for y in range(1, h):
        limit = y * w  # sources must lie entirely before this row
        row = y * w
        x = 0
        while x < w:
            hit = 0
            if x <= w - MIN_MATCH:
                candidates = index.get(plane[row + x:row + x + MIN_MATCH])
                if candidates:
                    for pos in candidates[:MAX_CANDIDATES]:
                        ln = MIN_MATCH
                        while x + ln < w and pos + ln < limit and plane[row + x + ln] == plane[pos + ln]:
                            ln += 1
                        if ln > hit:
                            hit = ln
                        if hit >= w - x:
                            break
            if hit >= MIN_MATCH:
                covered += hit
                total += hit
                x += hit
            else:
                total += 1
                x += 1
        add_row(y)
    return covered, total


def report(name: str) -> str:
    w, h, planes = read_ppm(PPM / f"{name}.ppm")
    parts = []
    bests = []
    for plane in planes:
        shift, share = shifted_shares(plane, w, h)
        bests.append(share)
        parts.append(f"{shift:+d}:{share * 100:.2f}%")
    pooled = sum(bests) / len(bests)
    cov = tot = 0
    for plane in planes:
        c, t = match_coverage(plane, w, h)
        cov += c
        tot += t
    match = cov / tot if tot else 0.0
    return (f"{name} {w}x{h} shifted-N R/G/B {' '.join(parts)} "
            f"pooled {pooled * 100:.2f}% match>=8 {match * 100:.2f}%")


def self_test() -> int:
    def cov(rows: list[list[int]]) -> tuple[int, int]:
        plane = bytes(v for row in rows for v in row)
        return match_coverage(plane, len(rows[0]), len(rows))

    cases = [
        # an exact 8-sample earlier-row match covers 8 of row 1's 16 samples
        ("8-match covers", cov([list(range(1, 17)), list(range(1, 9)) + [99] * 8]), (8, 16)),
        # a 7-sample match qualifies nowhere: no 8-window of row 1 occurs earlier
        ("7-match rejected", cov([list(range(1, 17)), list(range(1, 8)) + [42] + [99] * 8]), (0, 16)),
        # a match may not cross into the current row: row 0 ends ...1..7, row 1 is 1..8
        ("no crossing", cov([[9, 1, 2, 3, 4, 5, 6, 7], [1, 2, 3, 4, 5, 6, 7, 8]]), (0, 8)),
        # full-row repeat covers every sample exactly once (no double count)
        ("overlap once", cov([[7] * 16, [7] * 16]), (16, 16)),
    ]
    failed = 0
    for label, got, want in cases:
        ok = got == want
        failed += not ok
        print(f"self-test {label}: got {got}, want {want} {'ok' if ok else 'FAIL'}")
    print(f"self-test: {len(cases) - failed}/{len(cases)} passed")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return self_test()
    if not argv:
        print(__doc__)
        return 2
    names: list[str] = []
    for arg in argv:
        names.extend(SETS[arg] if arg in SETS else [arg])
    for name in names:
        print(report(name), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
