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

import ab  # noqa: E402
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


if __name__ == "__main__":
    unittest.main()
