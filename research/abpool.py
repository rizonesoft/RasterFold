#!/usr/bin/env python3
"""Pooled alternating A/B timing on research/ab.py's build and run helpers.

Usage:
  python3 research/abpool.py run <jsonl> <base_cm> <cand_cm> <tN> <rounds> <set...> [-D...]
  python3 research/abpool.py report <jsonl> [lo:hi] [--tot-bar F] [--img-bar F] [--small-ms F]
  python3 research/abpool.py --self-test
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import math
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent


def _load_ab():
    spec = importlib.util.spec_from_file_location("ab", TOOLS / "ab.py")
    if spec is None or spec.loader is None:
        raise SystemExit(f"unable to load {TOOLS / 'ab.py'}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _rel_or_str(path: Path, repo: Path = REPO) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(repo.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def file_sha256(path: Path) -> str:
    if not path.is_file():
        raise SystemExit(f"missing source file: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_run_args(argv: list[str]) -> tuple[Path, Path, Path, str, int, list[str], list[str]]:
    if len(argv) < 5:
        raise SystemExit(
            "usage: abpool.py run <jsonl> <base_cm> <cand_cm> <tN> <rounds> <set...> [-D...]"
        )
    log_path = Path(argv[0])
    idx = 1
    base_cm: Path | None = None
    cand_cm: Path | None = None
    while idx < len(argv) and argv[idx] in ("--base", "--cand"):
        opt = argv[idx]
        if idx + 1 >= len(argv):
            raise SystemExit(f"missing path after {opt}")
        if opt == "--base":
            base_cm = Path(argv[idx + 1])
        else:
            cand_cm = Path(argv[idx + 1])
        idx += 2
    if base_cm is None or cand_cm is None:
        if idx + 2 > len(argv):
            raise SystemExit("run requires <base_cm> and <cand_cm> paths")
        if base_cm is None:
            base_cm = Path(argv[idx])
            idx += 1
        if cand_cm is None:
            cand_cm = Path(argv[idx])
            idx += 1
    if idx + 2 > len(argv):
        raise SystemExit("run requires <tN> and <rounds> after <base_cm> <cand_cm>")
    t_flag = argv[idx]
    if not t_flag.startswith("-t") or not t_flag[2:].isdigit() or int(t_flag[2:]) <= 0:
        raise SystemExit(f"invalid thread flag {t_flag!r}: expected -t<positive int>")
    try:
        rounds = int(argv[idx + 1])
    except ValueError as exc:
        raise SystemExit(f"invalid rounds count {argv[idx + 1]!r}: {exc}") from exc
    if rounds <= 0:
        raise SystemExit(f"rounds must be positive, got {rounds}")
    rest = argv[idx + 2 :]
    if not rest and "ABPOOL_ARGS" in os.environ:
        rest = json.loads(os.environ["ABPOOL_ARGS"])
    flags = [a for a in rest if a.startswith("-D")]
    raw_sets = [a for a in rest if not a.startswith("-D")]
    if not raw_sets:
        raise SystemExit("run requires at least one image or set name")
    return log_path, base_cm, cand_cm, t_flag, rounds, raw_sets, flags


def verify_existing_log_provenance(
    log_path: Path, a_sha: str, b_sha: str, t_flag: str
) -> None:
    if not log_path.is_file():
        return
    for lineno, line in enumerate(log_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        rec = json.loads(line)
        rec_a = rec.get("a_sha256")
        rec_b = rec.get("b_sha256")
        if rec_a != a_sha or rec_b != b_sha:
            raise SystemExit(
                f"{log_path}:{lineno}: provenance mismatch in existing log "
                f"(existing A={rec_a} B={rec_b}, new A={a_sha} B={b_sha})"
            )


def run_pool(argv: list[str], ab_mod=None, repo: Path = REPO) -> dict[str, object]:
    log_path, base_cm, cand_cm, t_flag, rounds, raw_sets, flags = parse_run_args(argv)
    a_sha = file_sha256(base_cm)
    b_sha = file_sha256(cand_cm)
    a_label = _rel_or_str(base_cm, repo)
    b_label = _rel_or_str(cand_cm, repo)
    verify_existing_log_provenance(log_path, a_sha, b_sha, t_flag)

    orig_argv = list(sys.argv)
    try:
        sys.argv = ["ab.py", t_flag]
        ab = ab_mod if ab_mod is not None else _load_ab()
        ab.T = t_flag
        imgs = ab.names(raw_sets)
        bench_a = ab.RES / "benchA.exe"
        bench_b = ab.RES / "benchB.exe"
        ab.build(base_cm, bench_a, flags)
        ab.build(cand_cm, bench_b, flags)
        print(
            f"abpool build: A={a_label} (sha256={a_sha}) B={b_label} (sha256={b_sha}) "
            f"t={t_flag} rounds={rounds} images={len(imgs)}"
        )
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("a", encoding="utf-8", newline="\n") as fh:
            for i in range(rounds):
                order = ("A", "B") if i % 2 == 0 else ("B", "A")
                for k in order:
                    exe = bench_a if k == "A" else bench_b
                    out = ab.run(exe, imgs)
                    rows = ab.parse_bench_rows(out, imgs)
                    rec = {
                        "t": t_flag,
                        "k": k,
                        "a_path": a_label,
                        "a_sha256": a_sha,
                        "b_path": b_label,
                        "b_sha256": b_sha,
                        "rows": rows,
                    }
                    fh.write(json.dumps(rec) + "\n")
                    fh.flush()
    finally:
        sys.argv = orig_argv
    return {
        "log": str(log_path),
        "a_path": a_label,
        "a_sha256": a_sha,
        "b_path": b_label,
        "b_sha256": b_sha,
        "t": t_flag,
        "rounds": rounds,
    }


def parse_report_args(argv: list[str]) -> tuple[Path, int, int, float | None, float, float]:
    if not argv:
        raise SystemExit(
            "usage: abpool.py report <jsonl> [lo:hi] [--tot-bar F] [--img-bar F] [--small-ms F]"
        )
    log_path = Path(argv[0])
    lo, hi = 0, 999
    tot_bar: float | None = None
    img_bar = 1.50
    small_ms = 1.0
    idx = 1
    while idx < len(argv):
        arg = argv[idx]
        if arg == "--tot-bar":
            tot_bar = float(argv[idx + 1])
            idx += 2
        elif arg == "--img-bar":
            img_bar = float(argv[idx + 1])
            idx += 2
        elif arg == "--small-ms":
            small_ms = float(argv[idx + 1])
            idx += 2
        elif ":" in arg and not arg.startswith("-"):
            parts = arg.split(":", 1)
            lo, hi = int(parts[0]), int(parts[1])
            idx += 1
        else:
            raise SystemExit(f"unknown report argument: {arg}")
    if lo < 0 or hi <= lo:
        raise SystemExit(f"invalid slice [{lo}:{hi}]")
    return log_path, lo, hi, tot_bar, img_bar, small_ms


def report_pool(argv: list[str], ab_mod=None) -> list[dict[str, object]]:
    log_path, lo, hi, explicit_tot_bar, img_bar, small_ms = parse_report_args(argv)
    if not log_path.is_file():
        raise SystemExit(f"missing pool log: {log_path}")
    ab = ab_mod if ab_mod is not None else _load_ab()
    recs = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    if not recs:
        raise SystemExit(f"empty pool log: {log_path}")
    tot_bar = explicit_tot_bar if explicit_tot_bar is not None else (1.20 if "synth" in log_path.name else 1.10)
    summaries: list[dict[str, object]] = []
    for t in sorted({r["t"] for r in recs}):
        rr = [r for r in recs if r["t"] == t][2 * lo : 2 * hi]
        if not rr:
            continue
        a_shas = {r.get("a_sha256") for r in rr}
        b_shas = {r.get("b_sha256") for r in rr}
        if None in a_shas or None in b_shas or len(a_shas) != 1 or len(b_shas) != 1:
            raise SystemExit(
                f"{log_path.name} {t} [{lo}:{hi}]: missing or inconsistent source SHA-256 provenance "
                f"(A={sorted(str(x) for x in a_shas)}, B={sorted(str(x) for x in b_shas)})"
            )
        a_sha = next(iter(a_shas))
        b_sha = next(iter(b_shas))
        a_path = rr[0].get("a_path", "A")
        b_path = rr[0].get("b_path", "B")
        imgs = list(rr[0]["rows"])
        n = {k: sum(1 for r in rr if r["k"] == k) for k in ("A", "B")}
        if n["A"] == 0 or n["B"] == 0 or n["A"] != n["B"]:
            raise SystemExit(
                f"{log_path.name} {t} [{lo}:{hi}]: unbalanced pool runs A={n['A']} B={n['B']}"
            )
        mins = {
            k: {i: [min(r["rows"][i][j] for r in rr if r["k"] == k) for j in (1, 2)] for i in imgs}
            for k in ("A", "B")
        }
        tot = {k: [sum(mins[k][i][j] for i in imgs) for j in (0, 1)] for k in ("A", "B")}
        enc_tot_ratio = tot["B"][0] / tot["A"][0]
        dec_tot_ratio = tot["B"][1] / tot["A"][1]
        print(
            f"{log_path.name} {t} [{lo}:{hi}] runs A={n['A']} B={n['B']} images={len(imgs)} "
            f"A={a_path}({a_sha[:12]}) B={b_path}({b_sha[:12]})"
        )
        print(
            f"  totals enc {tot['B'][0]:.1f}/{tot['A'][0]:.1f}={enc_tot_ratio:.3f} "
            f"dec {tot['B'][1]:.1f}/{tot['A'][1]:.1f}={dec_tot_ratio:.3f}"
        )
        fails: list[str] = []
        if enc_tot_ratio > tot_bar:
            fails.append(f"total enc {tot['B'][0]:.1f}/{tot['A'][0]:.1f}={enc_tot_ratio:.3f}>{tot_bar:.2f}")
        if dec_tot_ratio > tot_bar:
            fails.append(f"total dec {tot['B'][1]:.1f}/{tot['A'][1]:.1f}={dec_tot_ratio:.3f}>{tot_bar:.2f}")
        worst = {0: (0.0, ""), 1: (0.0, "")}
        small_deltas: list[str] = []
        for i in imgs:
            w, h = ab.ppm_dimensions(ab.RES / "ppm" / f"{i}.ppm")
            big = w * h >= 16384
            for j, nm in ((0, "enc"), (1, "dec")):
                a, b = mins["A"][i][j], mins["B"][i][j]
                if big:
                    worst[j] = max(worst[j], (b / a, i))
                    if b / a > img_bar:
                        fails.append(f"{i} {nm} {b:.1f}/{a:.1f}={b / a:.3f}")
                else:
                    small_deltas.append(f"{i} {nm} {b:.1f}-{a:.1f}={b - a:+.2f}ms")
                    if b - a > small_ms:
                        fails.append(f"{i}(small) {nm} +{b - a:.2f}ms")
        geomeans: dict[str, float] = {}
        for j, nm in ((0, "enc"), (1, "dec")):
            g = math.exp(sum(math.log(mins["B"][i][j] / mins["A"][i][j]) for i in imgs) / len(imgs))
            geomeans[nm] = g
            print(f"  geomean {nm} {g:.3f}")
        print(f"  worst>=16384px enc {worst[0][1]}({worst[0][0]:.3f}) dec {worst[1][1]}({worst[1][0]:.3f})")
        if small_deltas:
            print(f"  small<16384px {', '.join(small_deltas)}")
        print(f"  over bar: {len(fails)} {fails}")
        summaries.append(
            {
                "log": log_path.name,
                "t": t,
                "lo": lo,
                "hi": hi,
                "runs_a": n["A"],
                "runs_b": n["B"],
                "a_path": a_path,
                "a_sha256": a_sha,
                "b_path": b_path,
                "b_sha256": b_sha,
                "enc_tot_ratio": enc_tot_ratio,
                "dec_tot_ratio": dec_tot_ratio,
                "geomean_enc": geomeans["enc"],
                "geomean_dec": geomeans["dec"],
                "worst_enc": worst[0],
                "worst_dec": worst[1],
                "fails": fails,
            }
        )
    return summaries


class _AbpoolSelfTests(unittest.TestCase):
    def test_run_and_report_verify_provenance_and_bars(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            res = root / "res"
            ppm = res / "ppm"
            ppm.mkdir(parents=True)
            (ppm / "big_img.ppm").write_bytes(b"P6\n128 128\n255\n" + b"\x00" * (128 * 128 * 3))
            (ppm / "tiny_img.ppm").write_bytes(b"P6\n4 4\n255\n" + b"\x00" * (4 * 4 * 3))

            base_c = root / "cm_m5.c"
            cand_c = root / "cm_cur.c"
            other_c = root / "cm_other.c"
            base_c.write_text("/* base m5 */\n", encoding="utf-8")
            cand_c.write_text("/* cand v3 */\n", encoding="utf-8")
            other_c.write_text("/* different */\n", encoding="utf-8")

            built_calls: list[tuple[Path, Path, list[str]]] = []
            run_orders: list[str] = []

            class FakeAb:
                RES = res
                T = "-t1"

                @staticmethod
                def names(args: list[str]) -> list[str]:
                    return list(args)

                @staticmethod
                def build(src: Path, exe: Path, flags: list[str]) -> None:
                    built_calls.append((src, exe, list(flags)))

                @staticmethod
                def run(exe: Path, imgs: list[str]) -> str:
                    tag = "A" if exe.name == "benchA.exe" else "B"
                    run_orders.append(tag)
                    return tag

                @staticmethod
                def parse_bench_rows(out: str, imgs: list[str]) -> dict[str, list[float]]:
                    if out == "A":
                        return {"big_img": [1000, 100.0, 80.0], "tiny_img": [50, 1.0, 1.0]}
                    return {"big_img": [1002, 104.0, 82.0], "tiny_img": [50, 1.4, 1.3]}

                @staticmethod
                def ppm_dimensions(p: Path) -> tuple[int, int]:
                    return (128, 128) if "big" in p.name else (4, 4)

            log_file = root / "photo_t1.jsonl"
            buf = io.StringIO()
            with redirect_stdout(buf):
                meta = run_pool(
                    [str(log_file), str(base_c), str(cand_c), "-t1", "2", "big_img", "tiny_img"],
                    ab_mod=FakeAb,
                    repo=root,
                )
            self.assertEqual(meta["a_sha256"], file_sha256(base_c))
            self.assertEqual(meta["b_sha256"], file_sha256(cand_c))
            self.assertEqual(run_orders, ["A", "B", "B", "A"])
            self.assertEqual(len(built_calls), 2)
            self.assertEqual(built_calls[0][0], base_c)
            self.assertEqual(built_calls[1][0], cand_c)

            # Appending with a different baseline or candidate into the same log is refused.
            with self.assertRaises(SystemExit) as ctx:
                run_pool(
                    [str(log_file), str(other_c), str(cand_c), "-t1", "1", "big_img", "tiny_img"],
                    ab_mod=FakeAb,
                    repo=root,
                )
            self.assertIn("provenance mismatch", str(ctx.exception))

            rep_buf = io.StringIO()
            with redirect_stdout(rep_buf):
                summaries = report_pool([str(log_file), "0:2"], ab_mod=FakeAb)
            self.assertEqual(len(summaries), 1)
            self.assertEqual(summaries[0]["fails"], [])
            self.assertIn("over bar: 0 []", rep_buf.getvalue())
            self.assertIn(meta["a_sha256"][:12], rep_buf.getvalue())
            self.assertIn(meta["b_sha256"][:12], rep_buf.getvalue())

            # Legacy/untagged log lines without a_sha256/b_sha256 are rejected by report_pool.
            untagged = root / "untagged.jsonl"
            untagged.write_text(
                json.dumps({"t": "-t1", "k": "A", "rows": {"big_img": [1000, 100.0, 80.0]}}) + "\n"
                + json.dumps({"t": "-t1", "k": "B", "rows": {"big_img": [1000, 100.0, 80.0]}}) + "\n",
                encoding="utf-8",
            )
            with self.assertRaises(SystemExit) as ctx2:
                report_pool([str(untagged), "0:1"], ab_mod=FakeAb)
            self.assertIn("missing or inconsistent source SHA-256 provenance", str(ctx2.exception))


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    if args[0] == "--self-test":
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(_AbpoolSelfTests)
        res = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if res.wasSuccessful() else 1
    mode = args[0]
    if mode == "run":
        run_pool(args[1:])
        return 0
    if mode == "report":
        report_pool(args[1:])
        return 0
    raise SystemExit(f"unknown mode {mode!r}: expected 'run', 'report', or '--self-test'")


if __name__ == "__main__":
    raise SystemExit(main())
