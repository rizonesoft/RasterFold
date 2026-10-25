#!/usr/bin/env python3
"""Unit tests for research/corpus.py and research/ab.py reference handling (D00 T02 §1)."""

from __future__ import annotations

import contextlib
import io
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "research"))
sys.path.insert(0, str(ROOT / "tests"))

import ab  # noqa: E402
import benchmark  # noqa: E402
import corpus  # noqa: E402


class ResearchRefsTests(unittest.TestCase):
    def test_summarize_sizes_matches_independent_arithmetic_for_size_and_rsize(self) -> None:
        got = [("img_a", 150), ("measure", 90), ("img_b", 300)]
        refs = {
            "img_a:e7": 100,
            "img_a:e9": 120,
            "img_a:webp": 75,
            "measure:e7": 100,
            "measure:e9": 90,
            "measure:webp": 100,
            "img_b:e7": 250,
            "img_b:e9": 300,
            "img_b:webp": 200,
        }
        tot = 150 + 90 + 300
        tj7 = 100 + 100 + 250
        tj9 = 120 + 90 + 300
        twb = 75 + 100 + 200
        gm7 = math.exp((math.log(150 / 100) + math.log(90 / 100) + math.log(300 / 250)) / 3)
        gm9 = math.exp((math.log(150 / 120) + math.log(90 / 90) + math.log(300 / 300)) / 3)
        gmw = math.exp((math.log(150 / 75) + math.log(90 / 100) + math.log(300 / 200)) / 3)

        out_size = ab.summarize_sizes(got, refs, verbose=True)
        lines = out_size.splitlines()
        self.assertEqual(len(lines), 4)
        self.assertIn("img_a", lines[0])
        self.assertIn("jxl=      100 1.5000 e9=      120 1.2500 webp=       75 2.0000", lines[0])
        expected_summary = (
            f"total={tot}"
            f" jxl={tj7} ratio={tot / tj7:.4f} geomean={gm7:.4f} worst=1.5000 (img_a)"
            f" e9={tj9} e9_ratio={tot / tj9:.4f} e9_geomean={gm9:.4f} e9_worst=1.2500 (img_a)"
            f" webp={twb} webp_ratio={tot / twb:.4f} webp_geomean={gmw:.4f} webp_worst=2.0000 (img_a)"
            f" measure=90 (0.9000 e9=1.0000 webp=0.9000)"
        )
        self.assertEqual(lines[-1], expected_summary)

        out_rsize = ab.summarize_sizes(got, refs, verbose=False, prefix="r10.exe cfg1 ")
        self.assertEqual(out_rsize, "r10.exe cfg1 " + expected_summary)

    def test_size_and_rsize_modes_invoke_shared_aggregation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            res_dir = Path(tmp)
            ppm_dir = res_dir / "ppm"
            ppm_dir.mkdir(parents=True, exist_ok=True)
            refs = {
                "alpha:e7": 200,
                "alpha:e9": 180,
                "alpha:webp": 160,
                "beta:e7": 400,
                "beta:e9": 350,
                "beta:webp": 300,
            }
            (res_dir / "jxl_refs.json").write_text(json.dumps(refs), encoding="utf-8")

            def fake_run(exe, imgs, reps=1):
                name = imgs[0]
                size = 190 if name == "alpha" else 380
                return f"ppm/{name}.ppm  {size}  bpp=1.0  enc=1.0ms dec=1.0ms (1/1 cyc/sample) OK\n"

            with (
                mock.patch.object(ab, "RES", res_dir),
                mock.patch.object(ab, "PPM", ppm_dir),
                mock.patch.object(ab, "build", lambda *a, **kw: None),
                mock.patch.object(ab, "run", side_effect=fake_run),
                mock.patch.object(sys, "argv", ["ab.py", "size", "alpha", "beta", "-v"]),
            ):
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    ab.main()
                size_out = buf.getvalue()
            self.assertIn("alpha", size_out)
            self.assertIn("beta", size_out)
            self.assertIn("total=570 jxl=600 ratio=0.9500", size_out)
            self.assertIn("e9=530 e9_ratio=1.0755", size_out)
            self.assertIn("webp=460 webp_ratio=1.2391", size_out)

            def fake_subproc(cmd, **kwargs):
                arg = cmd[-1]
                size = 190 if "alpha" in arg else 380
                return subprocess.CompletedProcess(cmd, 0, stdout=f"{arg} {size}\n", stderr="")

            with (
                mock.patch.object(ab, "RES", res_dir),
                mock.patch.object(ab, "PPM", ppm_dir),
                mock.patch.object(subprocess, "run", side_effect=fake_subproc),
                mock.patch.object(sys, "argv", ["ab.py", "rsize", "r10.exe", "cfg", "alpha", "beta"]),
            ):
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    ab.main()
                rsize_out = buf.getvalue().strip()
            self.assertTrue(rsize_out.startswith("r10.exe cfg total=570 jxl=600 ratio=0.9500"))
            self.assertIn("e9=530 e9_ratio=1.0755", rsize_out)
            self.assertIn("webp=460 webp_ratio=1.2391", rsize_out)

    def test_missing_or_invalid_reference_refused_without_partial_ratios(self) -> None:
        got = [("img_a", 100), ("img_b", 200)]
        base = {
            "img_a:e7": 100,
            "img_a:e9": 90,
            "img_a:webp": 80,
            "img_b:e7": 200,
            "img_b:e9": 180,
            "img_b:webp": 160,
        }
        for kind in ("e7", "e9", "webp"):
            missing = dict(base)
            del missing[f"img_b:{kind}"]
            with self.assertRaises(SystemExit) as ctx:
                ab.summarize_sizes(got, missing, verbose=True)
            self.assertIn(f"missing reference in jxl_refs.json: img_b:{kind}", str(ctx.exception))

        for bad_val in (0, -10, True, False, 12.5, "180", None):
            bad = dict(base)
            bad["img_b:e9"] = bad_val
            with self.assertRaises(SystemExit) as ctx:
                ab.summarize_sizes(got, bad, verbose=True)
            self.assertIn("invalid reference in jxl_refs.json: img_b:e9=", str(ctx.exception))

    def test_corpus_update_refs_writes_and_reloads_all_three_references_and_refuses_ffmpeg_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ppm_dir = root / "ppm"
            jxl_dir = root / "jxl"
            refs_path = root / "jxl_refs.json"
            ppm_dir.mkdir(parents=True, exist_ok=True)
            (ppm_dir / "sample.ppm").write_bytes(b"P6\n1 1\n255\n\x00\x00\x00")

            sizes = {".e7.jxl": 111, ".e9.jxl": 99, ".webp": 77}

            def ok_runner(cmd, capture_output=True):
                dest = Path(cmd[-1])
                for ext, sz in sizes.items():
                    if dest.name.endswith(ext):
                        dest.write_bytes(b"x" * sz)
                        break
                return subprocess.CompletedProcess(cmd, 0, stdout=b"", stderr=b"")

            reloaded = corpus.update_refs(ppm_dir, jxl_dir, refs_path, runner=ok_runner)
            self.assertEqual(
                reloaded,
                {"sample:e7": 111, "sample:e9": 99, "sample:webp": 77},
            )
            disk = json.loads(refs_path.read_text(encoding="utf-8"))
            self.assertEqual(disk, reloaded)

            # Delete sample:e9 and simulate an ffmpeg failure: refs_path on disk must remain untouched.
            partial = {"sample:e7": 111, "sample:webp": 77}
            refs_path.write_text(json.dumps(partial), encoding="utf-8")

            def fail_runner(cmd, capture_output=True):
                return subprocess.CompletedProcess(cmd, 1, stdout=b"", stderr=b"encode error")

            with self.assertRaises(SystemExit) as ctx:
                corpus.update_refs(ppm_dir, jxl_dir, refs_path, runner=fail_runner)
            self.assertIn("ffmpeg failed for sample:e9", str(ctx.exception))
            self.assertEqual(json.loads(refs_path.read_text(encoding="utf-8")), partial)

    def test_benchmark_jxl_e9_subset_merge_and_provenance(self) -> None:
        self.assertIn("jxl-e9", benchmark.CODECS)
        self.assertEqual(
            benchmark.CODECS["jxl-e9"],
            {"ext": "jxl", "args": ["-c:v", "libjxl", "-distance", "0", "-effort", "9", "-threads", "1"]},
        )
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp)
            csv_path = out_dir / "results.csv"
            six_codecs = ["rasterfold", "rasterfold-1t", "jxl", "jxl-1t", "webp", "png"]
            seed_rows = []
            for set_name, img in (("measure", "image"), ("photos", "kodim01"), ("synthetic", "checker")):
                for idx, c in enumerate(six_codecs):
                    seed_rows.append(
                        {
                            "set": set_name,
                            "image": img,
                            "width": "64",
                            "height": "64",
                            "raw": "12288",
                            "codec": c,
                            "bytes": "" if (set_name == "synthetic" and c == "png") else str(1000 + idx * 10),
                            "encode_us": str(500 + idx * 5),
                            "decode_us": str(400 + idx * 5),
                            "encode_peak": str(8192 + idx * 64),
                            "decode_peak": str(4096 + idx * 64),
                            "pixels_match": "False" if (set_name == "synthetic" and c == "png") else "True",
                        }
                    )
            import csv as csv_mod

            with csv_path.open("w", newline="", encoding="utf-8") as fh:
                writer = csv_mod.DictWriter(fh, fieldnames=list(benchmark.CSV_FIELDS))
                writer.writeheader()
                writer.writerows(seed_rows)

            existing = benchmark.load_existing_rows(csv_path)
            self.assertEqual(len(existing), 18)
            synth_png = next(r for r in existing if r["set"] == "synthetic" and r["codec"] == "png")
            self.assertFalse(synth_png["pixels_match"])
            self.assertIsNone(synth_png["bytes"])

            measured = []
            for idx, c in enumerate(benchmark.CODECS):
                measured.append(
                    {
                        "set": "measure",
                        "image": "image",
                        "width": 1672,
                        "height": 941,
                        "raw": 4720056,
                        "codec": c,
                        "bytes": 1440564 if c == "jxl-e9" else 1409028 + idx,
                        "encode_us": 200000 + idx * 1000,
                        "decode_us": 150000 + idx * 1000,
                        "encode_peak": 16000000,
                        "decode_peak": 15000000,
                        "pixels_match": True,
                    }
                )
            combined, retained_sets = benchmark.merge_rows(existing, measured, ["measure"])
            self.assertEqual(retained_sets, ["photos", "synthetic"])
            # 7 rows for measure + 6 for photos + 6 for synthetic = 19 rows (no duplicates).
            self.assertEqual(len(combined), 19)
            self.assertEqual(
                len({(r["set"], r["image"], r["codec"]) for r in combined}),
                19,
            )
            # Unrequested rows (photos, synthetic) must match existing exactly.
            self.assertEqual(
                [r for r in combined if r["set"] in ("photos", "synthetic")],
                [r for r in existing if r["set"] in ("photos", "synthetic")],
            )
            # Requested set (measure) must be replaced by measured rows without duplicates.
            self.assertEqual(
                [r for r in combined if r["set"] == "measure"],
                measured,
            )
            failures = [f"{r['codec']} {r['set']}/{r['image']}" for r in combined if not r["pixels_match"]]
            self.assertEqual(failures, ["png synthetic/checker"])

            benchmark.write_csv_rows(csv_path, combined)
            reloaded = benchmark.load_existing_rows(csv_path)
            self.assertEqual(reloaded, combined)

            with self.assertRaises(ValueError):
                benchmark.parse_csv_row({**seed_rows[0], "pixels_match": "yes"})
            self.assertEqual(benchmark.normalize_sets(["measure", "measure", "photos", "measure"]), ["measure", "photos"])
            with self.assertRaises(ValueError):
                benchmark.normalize_sets(["measure", "unknown_set"])
            with self.assertRaises(ValueError):
                benchmark.merge_rows(existing, [*measured, {**measured[0], "pixels_match": False}], ["measure"])
            with self.assertRaises(ValueError):
                benchmark.merge_rows([*existing, existing[-1]], measured, ["measure"])

            benchmark.report(reloaded, failures, ["measure"], 1, retained_sets=retained_sets, out_dir=out_dir)
            md = (out_dir / "results.md").read_text(encoding="utf-8")
            self.assertNotIn("One machine, one run;", md)
            self.assertIn("retained sets (photos, synthetic) keep their previously recorded measurements", md)
            self.assertIn("Retained from existing `benchmarks/results.csv` (not re-measured in this run).", md)
            self.assertIn("| jxl-e9 | 1440564 |", md)
            self.assertIn("| jxl-e9 | -- | -- | -- | -- | -- | -- |", md)
            for r in reloaded:
                if r["pixels_match"]:
                    self.assertIn(str(r["bytes"]), md)
                else:
                    self.assertIn("FAIL", md)


if __name__ == "__main__":
    unittest.main()
