#!/usr/bin/env python3
"""Ceiling of encoder-chosen parameters (D01 T02 section 6).

Reads `ab.py size ... photo -v` captures (the first is the baseline
candidate, the rest are variants) and prints the oracle ceiling: the
per-image minimum over all candidates, summed over the photo set. The
ceiling is what a per-image encoder search could reach at best; the
section bar is a ceiling at least 0.3% below the baseline total.

Usage:
    python3 research/ceiling.py <baseline -v> <variant -v> [...]
    python3 research/ceiling.py --self-test
"""

import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ab  # noqa: E402  (research/ab.py, for SETS)

LINE = re.compile(r"^\s*(\S+)\s+(\d+) jxl=\s*(\d+)")


def parse(path):
    rows = {}
    for ln in open(path):
        m = LINE.match(ln)
        if not m:
            continue
        name = m.group(1)
        if name in rows:
            raise SystemExit(f"ceiling: duplicate row {name} in {path}")
        rows[name] = (int(m.group(2)), int(m.group(3)))
    return rows


def compute(tables, labels, expected):
    """Ceiling over per-image minima. Tables map name -> (size, jxl)."""
    for label, rows in zip(labels, tables):
        names = set(rows)
        if names != set(expected):
            missing = sorted(set(expected) - names)
            extra = sorted(names - set(expected))
            raise SystemExit(
                f"ceiling: {label} does not hold exactly the photo set "
                f"(missing={missing} extra={extra})")
    base = tables[0]
    for label, rows in zip(labels[1:], tables[1:]):
        for img in expected:
            if rows[img][1] != base[img][1]:
                raise SystemExit(
                    f"ceiling: JPEG XL size for {img} differs in {label} "
                    f"({rows[img][1]} != {base[img][1]})")
    winners = Counter()
    ceiling_total = 0
    for img in expected:
        best = min(range(len(tables)), key=lambda i: tables[i][img][0])
        winners[labels[best]] += 1
        ceiling_total += tables[best][img][0]
    base_total = sum(base[img][0] for img in expected)
    jxl_total = sum(base[img][1] for img in expected)
    return ceiling_total, base_total, jxl_total, winners


def report(tables, labels, expected):
    ceiling_total, base_total, jxl_total, winners = compute(
        tables, labels, expected)
    gain = (base_total - ceiling_total) / base_total * 100
    print(f"baseline total: {base_total}")
    print(f"ceiling total: {ceiling_total} "
          f"({gain:+.3f}% vs baseline, "
          f"{ceiling_total / jxl_total:.4f} of JPEG XL e7)")
    print("winners: " + ", ".join(
        f"{label}={winners.get(label, 0)}" for label in labels))
    print(f"bar (ceiling at least 0.3% below baseline): "
          f"{'cleared' if gain >= 0.3 else 'not cleared'}")
    return 0


def self_test():
    expected = ["a", "b", "c"]
    base = {"a": (1000, 900), "b": (2000, 1900), "c": (4000, 3900)}
    var = {"a": (990, 900), "b": (2000, 1900), "c": (3000, 3900)}
    total, base_total, jxl_total, winners = compute(
        [base, var], ["base", "var"], expected)
    assert (total, base_total, jxl_total) == (5990, 7000, 6700), (
        total, base_total, jxl_total)
    # b ties at 2000: the baseline (first candidate) wins the tie.
    assert winners == Counter({"var": 2, "base": 1}), winners
    # Boundary: exactly 0.3% below the baseline clears the bar.
    edge = {"a": (979, 900), "b": (2000, 1900), "c": (4000, 3900)}
    total, base_total, _, _ = compute([base, edge], ["base", "edge"], expected)
    gain = (base_total - total) / base_total * 100
    assert abs(gain - 0.3) < 1e-9 and gain >= 0.3, gain
    # A truncated input is refused.
    try:
        compute([base, {"a": (1, 900)}], ["base", "short"], expected)
    except SystemExit:
        pass
    else:
        raise AssertionError("missing rows not refused")
    print("ceiling self-test: PASS (minima, percentages, ties, "
          "winner counts, 0.3% boundary, truncated input refused)")
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) < 2:
        raise SystemExit(__doc__)
    labels = [Path(a).stem for a in argv]
    tables = [parse(a) for a in argv]
    return report(tables, labels, ab.SETS["photo"])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
