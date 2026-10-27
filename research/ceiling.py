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


CHAN = re.compile(r"^(\S+)\.ppm\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)")


def parse_chan(path):
    rows = {}
    for ln in open(path):
        m = CHAN.match(ln)
        if not m:
            continue
        name = m.group(1)
        if name in rows:
            raise SystemExit(f"ceiling: duplicate row {name} in {path}")
        rows[name] = tuple(int(m.group(i)) for i in (2, 3, 4, 5))
        g, r, b, written = rows[name]
        if min(g, r, b) < 5 or written != 12 + g + r + b:
            raise SystemExit(
                f"ceiling: impossible stream sizes for {name} in {path} "
                f"({g}, {r}, {b}, written {written})")
    return rows


def compute_channels(tables, labels, expected, overhead):
    """Per-channel ceiling: overhead[i] plus 12 plus, per channel, the
    smallest stream any candidate produced. Tables map name -> (g, r, b,
    written); overhead maps name -> fold bytes outside the cm payload."""
    for label, rows in zip(labels, tables):
        if set(rows) != set(expected):
            raise SystemExit(
                f"ceiling: {label} does not hold exactly the photo set")
        for img in expected:
            g, r, b, written = rows[img]
            if min(g, r, b) < 5 or written != 12 + g + r + b:
                raise SystemExit(
                    f"ceiling: impossible stream sizes for {img} in "
                    f"{label} ({g}, {r}, {b}, written {written})")
    winners = Counter()
    ceiling_total = 0
    base_total = 0
    for img in expected:
        base_total += overhead[img] + tables[0][img][3]
        total = overhead[img] + 12
        for c in range(3):
            best = min(range(len(tables)), key=lambda i: tables[i][img][c])
            winners[labels[best]] += 1
            total += tables[best][img][c]
        ceiling_total += total
    return ceiling_total, base_total, winners


def report_channels(files, labels, expected):
    vrows = parse(files[0])
    tables = [parse_chan(f) for f in files[1:]]
    for label, rows in zip(labels[1:], tables):
        if set(rows) != set(vrows):
            raise SystemExit(
                f"ceiling: {label} does not hold exactly the photo set")
    overhead = {}
    base_chan = tables[0]
    for img in expected:
        overhead[img] = vrows[img][0] - base_chan[img][3]
        if overhead[img] < 0:
            raise SystemExit(f"ceiling: negative overhead for {img}")
    ceiling_total, base_total, winners = compute_channels(
        tables, labels[1:], expected, overhead)
    gain = (base_total - ceiling_total) / base_total * 100
    print(f"baseline total: {base_total}")
    print(f"per-channel ceiling total: {ceiling_total} "
          f"({gain:+.3f}% vs baseline)")
    print("channel winners: " + ", ".join(
        f"{label}={winners.get(label, 0)}" for label in labels[1:]))
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
    # Per-channel: independent selection beats whole-image selection.
    ca = {"x": (100, 200, 100, 412), "y": (500, 500, 500, 1512)}
    cb = {"x": (200, 100, 100, 412), "y": (500, 400, 500, 1412)}
    total, base_total, winners = compute_channels(
        [ca, cb], ["base", "var"], ["x", "y"], {"x": 20, "y": 30})
    assert (total, base_total) == (1774, 1974), (total, base_total)
    assert winners == Counter({"var": 2, "base": 4}), winners
    # Impossible stream sizes are refused (spec: streams at least 5 bytes,
    # written == 12 + G + R + B).
    bad = {"x": (0, 200, 100, 412), "y": (500, 500, 500, 1512)}
    try:
        compute_channels([bad, cb], ["bad", "var"], ["x", "y"],
                         {"x": 20, "y": 30})
    except SystemExit:
        pass
    else:
        raise AssertionError("zero stream not refused")
    bad2 = {"x": (100, 200, 100, 999), "y": (500, 500, 500, 1512)}
    try:
        compute_channels([bad2, cb], ["bad", "var"], ["x", "y"],
                         {"x": 20, "y": 30})
    except SystemExit:
        pass
    else:
        raise AssertionError("written mismatch not refused")
    print("ceiling self-test: PASS (minima, percentages, ties, "
          "winner counts, 0.3% boundary, truncated input refused, "
          "per-channel minima)")
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if argv[:1] == ["channels"]:
        # ceiling.py channels <baseline -v> <chanbench base> <chanbench var>...
        if len(argv) < 4:
            raise SystemExit(__doc__)
        labels = [Path(a).stem for a in argv[1:]]
        return report_channels(argv[1:], labels, ab.SETS["photo"])
    if len(argv) < 2:
        raise SystemExit(__doc__)
    labels = [Path(a).stem for a in argv]
    tables = [parse(a) for a in argv]
    return report(tables, labels, ab.SETS["photo"])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
