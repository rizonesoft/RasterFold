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
import vsjxl  # noqa: E402


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

            # Verify multi-step partial reruns preserve per-set provenance without misattributing retained sets.
            rev_sep = "0159404532cf944c4265ecb8ea46c54f637bdfc2"
            rev_oct = "225ba888a41e09a89d65f82829098fd8c7d41c3c"
            rev_nov = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            kat_sha = benchmark.kat_fold_sha256()
            meta_step1 = benchmark.resolve_set_provenance(
                {
                    "codec_revision": rev_sep,
                    "kat_fold_sha256": kat_sha,
                    "date": "2026-09-29",
                    "measured_sets": "measure,photos,synthetic",
                    "retained_sets": "none",
                },
                ["measure"],
                ["photos", "synthetic"],
                rev_oct,
                "2026-10-25",
                kat_sha,
            )
            benchmark.write_csv_rows(csv_path, combined, meta=meta_step1)
            loaded_meta1, _ = benchmark.read_results_csv(csv_path)
            self.assertEqual(
                benchmark.parse_set_map(loaded_meta1["set_revisions"]),
                {"measure": rev_oct, "photos": rev_sep, "synthetic": rev_sep},
            )
            self.assertEqual(
                benchmark.parse_set_map(loaded_meta1["set_dates"]),
                {"measure": "2026-10-25", "photos": "2026-09-29", "synthetic": "2026-09-29"},
            )
            # Step 2: rerun only photos on 2026-11-01; retained measure must stay 2026-10-25 / rev_oct while synthetic stays 2026-09-29 / rev_sep.
            meta_step2 = benchmark.resolve_set_provenance(
                loaded_meta1,
                ["photos"],
                ["measure", "synthetic"],
                rev_nov,
                "2026-11-01",
                kat_sha,
            )
            benchmark.write_csv_rows(csv_path, combined, meta=meta_step2)
            loaded_meta2, _ = benchmark.read_results_csv(csv_path)
            self.assertEqual(loaded_meta2["measured_sets"], "photos")
            self.assertEqual(loaded_meta2["retained_sets"], "measure,synthetic")
            self.assertEqual(
                benchmark.parse_set_map(loaded_meta2["set_revisions"]),
                {"measure": rev_oct, "photos": rev_nov, "synthetic": rev_sep},
            )
            self.assertEqual(
                benchmark.parse_set_map(loaded_meta2["set_dates"]),
                {"measure": "2026-10-25", "photos": "2026-11-01", "synthetic": "2026-09-29"},
            )
            self.assertEqual(loaded_meta2["retained_revision"], f"measure:{rev_oct},synthetic:{rev_sep}")
            self.assertEqual(loaded_meta2["retained_date"], "measure:2026-10-25,synthetic:2026-09-29")

            # Stripping per-set or retained KAT provenance must refuse partial reruns rather than falling back to file-level kat_fold_sha256.
            stripped_meta = dict(loaded_meta1)
            stripped_meta.pop("set_kat_hashes", None)
            stripped_meta.pop("retained_kat_fold_sha256", None)
            with self.assertRaisesRegex(ValueError, "missing or unverified provenance for retained set 'photos'"):
                benchmark.resolve_set_provenance(
                    stripped_meta,
                    ["measure"],
                    ["photos", "synthetic"],
                    rev_nov,
                    "2026-11-01",
                    "f" * 64,
                )
            with self.assertRaisesRegex(ValueError, "missing or unverified provenance for retained set 'photos'"):
                benchmark.resolve_set_provenance(
                    {"codec_revision": rev_sep, "date": "2026-09-29", "measured_sets": "measure,photos,synthetic", "retained_sets": "none"},
                    ["measure"],
                    ["photos", "synthetic"],
                    rev_oct,
                    "2026-10-25",
                    kat_sha,
                )
            # Malformed existing metadata is rejected when retaining sets, but full regeneration (retained_sets=[]) recovers cleanly.
            corrupt_meta = {**loaded_meta1, "set_kat_hashes": f"measure:{kat_sha},measure:{'0' * 64},photos:{kat_sha},synthetic:{kat_sha}"}
            with self.assertRaisesRegex(ValueError, "duplicate set key 'measure'"):
                benchmark.resolve_set_provenance(
                    corrupt_meta,
                    ["measure"],
                    ["photos", "synthetic"],
                    rev_nov,
                    "2026-11-01",
                    kat_sha,
                )
            recovered_meta = benchmark.resolve_set_provenance(
                corrupt_meta,
                ["measure", "photos", "synthetic"],
                [],
                rev_nov,
                "2026-11-01",
                kat_sha,
            )
            self.assertEqual(recovered_meta["retained_sets"], "none")
            self.assertEqual(
                benchmark.parse_set_map(recovered_meta["set_kat_hashes"]),
                {"measure": kat_sha, "photos": kat_sha, "synthetic": kat_sha},
            )

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

    def test_corpus_csv_reconstructs_all_five_research_summaries(self) -> None:
        csv_path = ROOT / "benchmarks" / "corpus.csv"
        meta, rows = ab.read_corpus_csv(csv_path)
        self.assertRegex(meta["codec_revision"], r"^[0-9a-f]{40}$")
        self.assertEqual(meta["kat_fold_sha256"], ab.kat_fold_sha256())
        self.assertEqual(len(rows), 55)
        self.assertEqual([r["image"] for r in rows], ab.SETS["all"])
        self.assertEqual(sum(1 for r in rows if r["set"] == "photo"), 39)
        self.assertEqual(sum(1 for r in rows if r["set"] == "tp"), 5)
        self.assertEqual(sum(1 for r in rows if r["set"] == "synth"), 11)

        photo = ab.reconstruct_set_summary(csv_path, "photo")
        self.assertEqual(photo["images"], ab.SETS["photo"])
        self.assertEqual(photo["count"], 39)
        self.assertEqual(photo["rasterfold"], 30865211)
        self.assertEqual(photo["jxl_e7"], 31782058)
        self.assertEqual(photo["jxl_e9"], 31400714)
        self.assertEqual(photo["webp"], 34138176)
        self.assertEqual(f"{photo['e7_ratio']:.4f}", "0.9712")
        self.assertEqual(f"{photo['e7_geomean']:.4f}", "0.9728")
        self.assertEqual(photo["worst_image"], "kodim17")
        self.assertEqual(f"{photo['worst_e7_ratio']:.4f}", "1.0037")
        self.assertEqual(f"{photo['e9_ratio']:.4f}", "0.9829")
        self.assertEqual(f"{photo['webp_ratio']:.4f}", "0.9041")
        self.assertEqual(photo["below_e9"], 28)

        kodak = ab.reconstruct_set_summary(csv_path, "kodak")
        self.assertEqual(kodak["images"], ab.SETS["kodak"])
        self.assertEqual(kodak["count"], 24)
        self.assertEqual(f"{kodak['e7_ratio']:.4f}", "0.9859")
        self.assertEqual(f"{kodak['e7_geomean']:.4f}", "0.9860")
        self.assertEqual(kodak["worst_image"], "kodim17")
        self.assertEqual(f"{kodak['worst_e7_ratio']:.4f}", "1.0037")

        ic = ab.reconstruct_set_summary(csv_path, "ic")
        self.assertEqual(ic["images"], ab.SETS["ic"])
        self.assertEqual(ic["count"], 14)
        self.assertEqual(f"{ic['e7_ratio']:.4f}", "0.9647")
        self.assertEqual(f"{ic['e7_geomean']:.4f}", "0.9517")
        self.assertEqual(ic["worst_image"], "ic_nightshot_iso_1600")
        self.assertEqual(f"{ic['worst_e7_ratio']:.4f}", "0.9942")

        tp = ab.reconstruct_set_summary(csv_path, "tp")
        self.assertEqual(tp["images"], ab.SETS["tp"])
        self.assertEqual(tp["count"], 10)
        self.assertEqual(f"{tp['e7_ratio']:.4f}", "0.9743")
        self.assertEqual(f"{tp['e7_geomean']:.4f}", "0.9722")
        self.assertEqual(tp["worst_image"], "tp_nightshot_iso_1600_crop")
        self.assertEqual(f"{tp['worst_e7_ratio']:.4f}", "0.9931")

        synth = ab.reconstruct_set_summary(csv_path, "synth")
        self.assertEqual(synth["images"], ab.SETS["synth"])
        self.assertEqual(synth["count"], 11)
        self.assertEqual(f"{synth['e7_ratio']:.4f}", "0.9118")
        self.assertEqual(f"{synth['e7_geomean']:.4f}", "0.5883")
        self.assertEqual(synth["worst_image"], "syn_gradient")
        self.assertEqual(f"{synth['worst_e7_ratio']:.4f}", "1.5665")

    def test_corpus_csv_provenance_and_failure_preservation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ppm_dir = root / "ppm"
            ppm_dir.mkdir(parents=True, exist_ok=True)
            (ppm_dir / "kodim01.ppm").write_bytes(b"P6\n# comment\n2 1\n255\n\x01\x02\x03\x04\x05\x06")
            (ppm_dir / "syn_checker.ppm").write_bytes(b"P6\n1 2\n255\n\x01\x02\x03\x04\x05\x06")
            refs = {
                "kodim01:e7": 460550,
                "kodim01:e9": 454757,
                "kodim01:webp": 499410,
                "syn_checker:e7": 316,
                "syn_checker:e9": 210,
                "syn_checker:webp": 82,
            }
            got = [("kodim01", 456732), ("syn_checker", 5709)]
            csv_path = root / "corpus.csv"
            fake_head = "a" * 40

            def clean_runner(cmd, **kwargs):
                if cmd[:3] == ["git", "rev-parse", "--verify"]:
                    return subprocess.CompletedProcess(cmd, 0, stdout=fake_head + "\n", stderr="")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

            meta, rows = ab.write_corpus_csv(
                csv_path,
                got,
                refs,
                ppm_dir=ppm_dir,
                runner=clean_runner,
                today="2026-10-25",
            )
            self.assertEqual(meta["codec_revision"], fake_head)
            self.assertEqual(meta["kat_fold_sha256"], ab.kat_fold_sha256())
            self.assertEqual(meta["date"], "2026-10-25")
            self.assertEqual(
                rows,
                [
                    {
                        "set": "photo",
                        "image": "kodim01",
                        "width": 2,
                        "height": 1,
                        "rasterfold": 456732,
                        "jxl_e7": 460550,
                        "jxl_e9": 454757,
                        "webp": 499410,
                    },
                    {
                        "set": "synth",
                        "image": "syn_checker",
                        "width": 1,
                        "height": 2,
                        "rasterfold": 5709,
                        "jxl_e7": 316,
                        "jxl_e9": 210,
                        "webp": 82,
                    },
                ],
            )
            original_bytes = csv_path.read_bytes()

            # 1. Alternate .c source refused and existing CSV preserved.
            alt_c = root / "alt_cm.c"
            alt_c.write_text("/* variant */\n", encoding="utf-8")
            with self.assertRaises(SystemExit) as ctx:
                ab.write_corpus_csv(csv_path, got, refs, src=alt_c, ppm_dir=ppm_dir, runner=clean_runner)
            self.assertIn("--csv requires canonical", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # 2. Custom compiler flags refused and existing CSV preserved.
            with self.assertRaises(SystemExit) as ctx:
                ab.write_corpus_csv(csv_path, got, refs, flags=["-DTEST=1"], ppm_dir=ppm_dir, runner=clean_runner)
            self.assertIn("--csv refuses custom compiler flags", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # 3. Unavailable git HEAD refused and existing CSV preserved.
            def bad_rev_runner(cmd, **kwargs):
                if cmd[:3] == ["git", "rev-parse", "--verify"]:
                    return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="fatal")
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

            with self.assertRaises(SystemExit) as ctx:
                ab.write_corpus_csv(csv_path, got, refs, ppm_dir=ppm_dir, runner=bad_rev_runner)
            self.assertIn("unable to determine clean 40-char git HEAD", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # 4. Modified codec/bench source refused and existing CSV preserved.
            def dirty_runner(cmd, **kwargs):
                if cmd[:3] == ["git", "rev-parse", "--verify"]:
                    return subprocess.CompletedProcess(cmd, 0, stdout=fake_head + "\n", stderr="")
                return subprocess.CompletedProcess(cmd, 0, stdout=" M src/cm.c\n", stderr="")

            with self.assertRaises(SystemExit) as ctx:
                ab.write_corpus_csv(csv_path, got, refs, ppm_dir=ppm_dir, runner=dirty_runner)
            self.assertIn("--csv refuses modified codec/bench sources", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # 5. Duplicate image in got refused and existing CSV preserved.
            with self.assertRaises(SystemExit) as ctx:
                ab.write_corpus_csv(
                    csv_path,
                    [("kodim01", 456732), ("kodim01", 456732)],
                    refs,
                    ppm_dir=ppm_dir,
                    runner=clean_runner,
                )
            self.assertIn("duplicate or empty image", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # 6. Invalid PPM payload length refused and existing CSV preserved.
            (ppm_dir / "kodim01.ppm").write_bytes(b"P6\n2 1\n255\n\x01\x02")
            with self.assertRaises(SystemExit) as ctx:
                ab.write_corpus_csv(csv_path, got, refs, ppm_dir=ppm_dir, runner=clean_runner)
            self.assertIn("invalid PPM payload length", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # Restore valid PPM and test injected I/O failures during write/fsync/replace.
            (ppm_dir / "kodim01.ppm").write_bytes(b"P6\n# comment\n2 1\n255\n\x01\x02\x03\x04\x05\x06")
            with mock.patch("os.fsync", side_effect=OSError("disk full on fsync")):
                with self.assertRaises(SystemExit) as ctx:
                    ab.write_corpus_csv(
                        csv_path,
                        [("kodim01", 999999), ("syn_checker", 5709)],
                        refs,
                        ppm_dir=ppm_dir,
                        runner=clean_runner,
                    )
                self.assertIn("failed writing corpus CSV", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)
            self.assertEqual([p.name for p in root.iterdir() if p.name.endswith(".tmp")], [])

            with mock.patch("os.replace", side_effect=OSError("permission denied on replace")):
                with self.assertRaises(SystemExit) as ctx:
                    ab.write_corpus_csv(
                        csv_path,
                        [("kodim01", 999999), ("syn_checker", 5709)],
                        refs,
                        ppm_dir=ppm_dir,
                        runner=clean_runner,
                    )
                self.assertIn("failed writing corpus CSV", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)
            self.assertEqual([p.name for p in root.iterdir() if p.name.endswith(".tmp")], [])

            # 7. Read-back validation runs on tmp_path BEFORE os.replace, preserving csv_path on failure.
            with mock.patch.object(ab, "read_corpus_csv", side_effect=SystemExit("read-back failed")):
                with self.assertRaises(SystemExit) as ctx:
                    ab.write_corpus_csv(
                        csv_path,
                        [("kodim01", 999999), ("syn_checker", 5709)],
                        refs,
                        ppm_dir=ppm_dir,
                        runner=clean_runner,
                    )
                self.assertIn("read-back failed", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)
            self.assertEqual([p.name for p in root.iterdir() if p.name.endswith(".tmp")], [])

            # 8. Provenance change between pre-build snapshot and publication is refused and preserves csv_path.
            stale_snapshot = ("f" * 40, ab.kat_fold_sha256(), ab._provenance_digests())
            with self.assertRaises(SystemExit) as ctx:
                ab.write_corpus_csv(
                    csv_path,
                    [("kodim01", 999999), ("syn_checker", 5709)],
                    refs,
                    ppm_dir=ppm_dir,
                    runner=clean_runner,
                    expected_snapshot=stale_snapshot,
                )
            self.assertIn("--csv refused: codec/bench provenance changed during measurement", str(ctx.exception))
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # 9. Pre-existing predictable .tmp symlink or symlink destination is never followed/truncated.
            import os as os_mod

            if hasattr(os_mod, "symlink"):
                predictable_tmp = root / f".{csv_path.name}.{os_mod.getpid()}.tmp"
                try:
                    predictable_tmp.symlink_to(csv_path)
                except OSError:
                    predictable_tmp = None
                if predictable_tmp is not None:
                    with mock.patch("os.fsync", side_effect=OSError("disk full on fsync")):
                        with self.assertRaises(SystemExit):
                            ab.write_corpus_csv(
                                csv_path,
                                [("kodim01", 999999), ("syn_checker", 5709)],
                                refs,
                                ppm_dir=ppm_dir,
                                runner=clean_runner,
                            )
                    self.assertEqual(csv_path.read_bytes(), original_bytes)
                    self.assertTrue(predictable_tmp.is_symlink())
                    predictable_tmp.unlink()

    def test_drift_check_derive_facts_csv_with_and_without_comments(self) -> None:
        import importlib.util

        spec = importlib.util.spec_from_file_location("drift_check", ROOT / "scripts" / "drift-check.py")
        assert spec is not None and spec.loader is not None
        drift_check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(drift_check)

        sources = {
            "benchmarks/corpus.csv": (
                "# codec_revision=" + "b" * 40 + " kat_fold_sha256=" + "c" * 64 + " date=2026-10-25\n"
                "set,image,width,height,rasterfold,jxl_e7,jxl_e9,webp\n"
                "photo,measure,1672,941,1409028,1470703,1440564,1588708\n"
                "photo,kodim01,768,512,456732,460550,454757,499410\n"
            ),
            "benchmarks/results.csv": (
                "set,image,codec,bytes\n"
                "measure,image,rasterfold,1409028\n"
                "measure,image,jxl-e9,1440564\n"
            ),
        }
        manifest = {
            "facts": {
                "corpus_measure_e9": {
                    "kind": "csv",
                    "source": "benchmarks/corpus.csv",
                    "where": {"set": "photo", "image": "measure"},
                    "column": "jxl_e9",
                    "type": "int",
                },
                "results_measure_rf": {
                    "kind": "csv",
                    "source": "benchmarks/results.csv",
                    "where": {"set": "measure", "image": "image", "codec": "rasterfold"},
                    "column": "bytes",
                    "type": "int",
                },
            }
        }
        facts = drift_check.derive_facts(manifest, lambda rel: sources[rel])
        self.assertEqual(facts, {"corpus_measure_e9": 1440564, "results_measure_rf": 1409028})

        # Zero-match selector must raise ValueError.
        zero_manifest = {
            "facts": {
                "missing": {
                    "kind": "csv",
                    "source": "benchmarks/corpus.csv",
                    "where": {"image": "nonexistent"},
                    "column": "rasterfold",
                    "type": "int",
                }
            }
        }
        with self.assertRaises(ValueError) as ctx:
            drift_check.derive_facts(zero_manifest, lambda rel: sources[rel])
        self.assertIn("expected one CSV row, got 0", str(ctx.exception))

        # Multiple-match selector must raise ValueError.
        multi_manifest = {
            "facts": {
                "multi": {
                    "kind": "csv",
                    "source": "benchmarks/corpus.csv",
                    "where": {"set": "photo"},
                    "column": "rasterfold",
                    "type": "int",
                }
            }
        }
        with self.assertRaises(ValueError) as ctx:
            drift_check.derive_facts(multi_manifest, lambda rel: sources[rel])
        self.assertIn("expected one CSV row, got 2", str(ctx.exception))

    def test_benchmark_provenance_and_atomic_write(self) -> None:
        from unittest import mock

        fake_head = "c" * 40
        kat_sha = benchmark.kat_fold_sha256()

        def clean_runner(cmd, **kwargs):
            if cmd[:3] == ["git", "rev-parse", "--verify"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=f"{fake_head}\n", stderr="")
            if cmd[:3] == ["git", "status", "--porcelain"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if len(cmd) >= 3 and str(cmd[1]).endswith("build.py"):
                return subprocess.CompletedProcess(cmd, 0, stdout="built\n", stderr="")
            raise AssertionError(f"unexpected subprocess command: {cmd}")

        head, got_kat, digests = benchmark.provenance_snapshot(ROOT, runner=clean_runner)
        self.assertEqual(head, fake_head)
        self.assertEqual(got_kat, kat_sha)
        self.assertIn("src/cm.c", digests)

        # Dirty git status refuses benchmark provenance.
        def dirty_runner(cmd, **kwargs):
            if cmd[:3] == ["git", "rev-parse", "--verify"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=f"{fake_head}\n", stderr="")
            if cmd[:3] == ["git", "status", "--porcelain"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=" M src/cm.c\n", stderr="")
            raise AssertionError(f"unexpected subprocess command: {cmd}")

        with self.assertRaisesRegex(ValueError, "refuses modified codec/build/KAT sources"):
            benchmark.codec_provenance(ROOT, runner=dirty_runner)

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            exe_path = root / "rasterfold.exe"
            exe_path.write_bytes(b"exe")
            benchmark.ensure_fresh_executable(ROOT, exe=exe_path, runner=clean_runner)

            def failing_build_runner(cmd, **kwargs):
                return subprocess.CompletedProcess(cmd, 1, stdout="", stderr="compiler error")

            with self.assertRaisesRegex(ValueError, "failed to build fresh benchmark executable"):
                benchmark.ensure_fresh_executable(ROOT, exe=exe_path, runner=failing_build_runner)

            csv_path = root / "results.csv"
            _, seed_rows = benchmark.read_results_csv(ROOT / "benchmarks" / "results.csv", require_provenance=True)
            meta = benchmark.resolve_set_provenance(
                {},
                ["measure", "photos", "synthetic"],
                [],
                fake_head,
                "2026-10-25",
                kat_sha,
            )
            snap = (fake_head, kat_sha, digests)
            benchmark.write_csv_rows(csv_path, seed_rows[:7], meta=meta, expected_snapshot=snap, repo=ROOT, runner=clean_runner)
            original_bytes = csv_path.read_bytes()
            read_meta, read_rows = benchmark.read_results_csv(csv_path, require_provenance=True)
            self.assertEqual(read_meta, meta)
            self.assertEqual(len(read_rows), 7)

            # Mid-run provenance change refuses write and preserves existing CSV bytes.
            stale_snap = ("d" * 40, kat_sha, digests)
            with self.assertRaisesRegex(ValueError, "provenance changed during measurement"):
                benchmark.write_csv_rows(csv_path, seed_rows[:7], meta=meta, expected_snapshot=stale_snap, repo=ROOT, runner=clean_runner)
            self.assertEqual(csv_path.read_bytes(), original_bytes)

            # Injected fsync or replace error cleans up .tmp and preserves existing CSV bytes.
            with mock.patch("os.fsync", side_effect=OSError("disk full")):
                with self.assertRaises(OSError):
                    benchmark.write_csv_rows(csv_path, seed_rows[:7], meta=meta, expected_snapshot=snap, repo=ROOT, runner=clean_runner)
            self.assertEqual(csv_path.read_bytes(), original_bytes)
            self.assertEqual([p.name for p in root.iterdir() if p.name.endswith(".tmp")], [])

            with mock.patch("os.replace", side_effect=OSError("rename blocked")):
                with self.assertRaises(OSError):
                    benchmark.write_csv_rows(csv_path, seed_rows[:7], meta=meta, expected_snapshot=snap, repo=ROOT, runner=clean_runner)
            self.assertEqual(csv_path.read_bytes(), original_bytes)
            self.assertEqual([p.name for p in root.iterdir() if p.name.endswith(".tmp")], [])

    def test_parse_bench_rows_accepts_ok_raw_pal_and_rejects_failures(self) -> None:
        stdout = (
            "raw_1x1.ppm                  35  bpp=280.0000  enc=    0.0ms dec=    0.0ms  (10/10 cyc/sample) RAW\n"
            "kat_cm.ppm                  536  bpp=16.7500   enc=    0.4ms dec=    0.3ms  (100/80 cyc/sample) OK\n"
            "kat_pal.ppm                 127  bpp=3.9688    enc=    0.1ms dec=    0.1ms  (30/25 cyc/sample) PAL\n"
            "TOTAL 698  enc=0.5ms dec=0.4ms  (50/40 cyc/sample)\n"
        )
        for parser in (ab.parse_bench_rows, vsjxl.parse_bench_rows):
            rows = parser(stdout, ("raw_1x1", "kat_cm", "kat_pal"))
            self.assertEqual(rows["raw_1x1"], (35, 0.05, 0.05))
            self.assertEqual(rows["kat_cm"], (536, 0.4, 0.3))
            self.assertEqual(rows["kat_pal"], (127, 0.1, 0.1))

        for bad_line, expected_msg in (
            (
                "kat_pal.ppm                 127  bpp=3.9688  enc=  0.1ms dec=  0.1ms  (30/25 cyc/sample) MISMATCH\n",
                "bench round-trip failure on kat_pal: MISMATCH",
            ),
            (
                "kat_pal.ppm                 127  bpp=3.9688  enc=  0.1ms dec=  0.1ms  (30/25 cyc/sample) UNKNOWN\n",
                "bench round-trip failure on kat_pal: UNKNOWN",
            ),
            (
                "encode failed ppm/kat_pal.ppm\n",
                "bench encode failure: encode failed ppm/kat_pal.ppm",
            ),
        ):
            with self.assertRaises(SystemExit) as ctx:
                ab.parse_bench_rows(bad_line, ["kat_pal"])
            self.assertIn(expected_msg, str(ctx.exception))
            with self.assertRaises(RuntimeError) as rctx:
                vsjxl.parse_bench_rows(bad_line, ("kat_pal",))
            self.assertIn(expected_msg, str(rctx.exception))

        with self.assertRaises(SystemExit) as ctx:
            ab.parse_bench_rows(stdout, ["raw_1x1", "missing_img"])
        self.assertIn("bench output missing expected images: ['missing_img']", str(ctx.exception))
        with self.assertRaises(RuntimeError) as rctx:
            vsjxl.parse_bench_rows(stdout, ("raw_1x1", "missing_img"))
        self.assertIn("bench output missing expected images: ['missing_img']", str(rctx.exception))

        # Verify symbol-based variant classification takes precedence over misleading filenames.
        with tempfile.TemporaryDirectory() as tmp:
            td = Path(tmp)
            cm_named_pal = td / "palette_probe.c"
            cm_named_pal.write_bytes((ROOT / "src" / "cm.c").read_bytes())
            pal_named_cm = td / "cm_probe.c"
            pal_named_cm.write_bytes((ROOT / "src" / "pal.c").read_bytes())
            self.assertFalse(ab.is_palette_source(cm_named_pal))
            self.assertTrue(ab.is_palette_source(pal_named_cm))

        # Verify size --csv refuses to overwrite existing corpus.csv when bench output fails validation.
        with tempfile.TemporaryDirectory() as tmp:
            res_dir = Path(tmp)
            ppm_dir = res_dir / "ppm"
            ppm_dir.mkdir(parents=True, exist_ok=True)
            (ppm_dir / "syn_checker.ppm").write_bytes(b"P6\n1 1\n255\n\x00\x00\x00")
            (res_dir / "jxl_refs.json").write_text(
                json.dumps({"syn_checker:e7": 316, "syn_checker:e9": 210, "syn_checker:webp": 82}),
                encoding="utf-8",
            )
            csv_path = res_dir / "corpus.csv"
            sentinel = b"# existing corpus csv\n"
            csv_path.write_bytes(sentinel)

            for bad_out in (
                "syn_checker.ppm  297  bpp=0.5801  enc=0.1ms dec=0.1ms (1/1 cyc/sample) MISMATCH\n",
                "encode failed ppm/syn_checker.ppm\n",
            ):
                with (
                    mock.patch.object(ab, "RES", res_dir),
                    mock.patch.object(ab, "PPM", ppm_dir),
                    mock.patch.object(ab, "provenance_snapshot", return_value=("a" * 40, "b" * 64, {})),
                    mock.patch.object(ab, "build", lambda *a, **kw: None),
                    mock.patch.object(ab, "run", return_value=bad_out),
                    mock.patch.object(sys, "argv", ["ab.py", "size", "syn_checker", "--csv", str(csv_path)]),
                ):
                    with self.assertRaises(SystemExit):
                        ab.main()
                self.assertEqual(csv_path.read_bytes(), sentinel)

    def test_bench_container_round_trips_and_variant_builds(self) -> None:
        import os as os_mod
        import test_correctness as tc

        if not ab.CLANG.is_file():
            self.skipTest(f"missing mingw clang toolchain at {ab.CLANG}")

        def can_run_pe(exe_path: Path) -> bool:
            if os_mod.name == "nt":
                return True
            try:
                proc = subprocess.run(
                    [str(exe_path)],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                return proc.returncode == 0
            except OSError:
                return False

        def write_ppm(path: Path, w: int, h: int, rgb: bytes) -> None:
            path.write_bytes(f"P6\n{w} {h}\n255\n".encode("ascii") + rgb)

        with tempfile.TemporaryDirectory() as tmp:
            res_dir = Path(tmp)
            ppm_dir = res_dir / "ppm"
            ppm_dir.mkdir(parents=True, exist_ok=True)
            write_ppm(ppm_dir / "raw_1x1.ppm", 1, 1, bytes([12, 34, 56]))
            write_ppm(ppm_dir / "kat_cm.ppm", tc.KAT_W, tc.KAT_H, tc.KAT_RGB)
            write_ppm(ppm_dir / "kat_pal.ppm", tc.KAT_PAL_W, tc.KAT_PAL_H, tc.KAT_PAL_RGB)

            # Use misleading filenames to verify ab.build links by actual symbol interface.
            cm_var = res_dir / "palette_probe.c"
            cm_var.write_bytes((ROOT / "src" / "cm.c").read_bytes())
            pal_var = res_dir / "cm_probe.c"
            pal_var.write_bytes((ROOT / "src" / "pal.c").read_bytes())

            built_exes: list[Path] = []
            for idx, src in enumerate((ROOT / "src" / "cm.c", cm_var, pal_var)):
                exe = res_dir / f"bench_{idx}.exe"
                ab.build(src, exe, flags=("-O0",) if idx > 0 else ())
                self.assertTrue(exe.is_file())
                built_exes.append(exe)

            if not can_run_pe(built_exes[0]):
                self.skipTest("Windows PE execution requires Windows or WSL interop")

            for exe in built_exes:
                with mock.patch.object(ab, "RES", res_dir):
                    out = ab.run(exe, ["raw_1x1", "kat_cm", "kat_pal"], reps=1)
                self.assertIn("raw_1x1.ppm", out)
                self.assertIn(" RAW\n", out)
                self.assertIn(" OK\n", out)
                self.assertIn(" PAL\n", out)
                rows = ab.parse_bench_rows(out, ["raw_1x1", "kat_cm", "kat_pal"])
                self.assertEqual(rows["raw_1x1"][0], 35)
                self.assertEqual(rows["kat_cm"][0], len(tc.KAT_FOLD))
                self.assertEqual(rows["kat_pal"][0], len(tc.KAT_PAL2))

            # Inject a pixel mismatch in a palette variant decode and verify ab.run rejects it.
            bad_pal = res_dir / "pal_bad.c"
            bad_pal_text = (ROOT / "src" / "pal.c").read_text(encoding="utf-8").replace(
                'if (bad || dec.in != dec.end) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette stream");\n    return RF_OK;',
                'if (bad || dec.in != dec.end) return rf_fail(err, RF_ERR_FORMAT, "invalid fold: palette stream");\n    out[0] ^= 1u;\n    return RF_OK;',
                1,
            )
            bad_pal.write_text(bad_pal_text, encoding="utf-8")
            bad_exe = res_dir / "bench_bad.c.exe"
            ab.build(bad_pal, bad_exe, flags=("-O0",))
            with mock.patch.object(ab, "RES", res_dir):
                with self.assertRaises(SystemExit) as ctx:
                    ab.run(bad_exe, ["kat_pal"], reps=1)
                self.assertIn("MISMATCH", str(ctx.exception))

            # Verify RAW runs all requested repetitions and measures decode timing independently of encode timing.
            rep_cm = res_dir / "cm_rep_raw.c"
            rep_cm_text = (
                "#include <stdio.h>\n"
                + (ROOT / "src" / "cm.c").read_text(encoding="utf-8").replace(
                    "int rf_cm_encode(",
                    "int rf_cm_encode_orig(",
                    1,
                )
                + "\nstatic int g_raw_enc_calls = 0;\n"
                "int rf_cm_encode(const uint8_t *rgb, uint32_t w, uint32_t h, uint8_t *out, size_t cap, size_t *written, int threads, rf_error *err) {\n"
                "    g_raw_enc_calls++;\n"
                "    if (w == 1 && h == 1) {\n"
                "        for (volatile int spin = 0; spin < 15000000; spin++) {}\n"
                "        printf(\"RAW_ENC_CALL=%d\\n\", g_raw_enc_calls);\n"
                "    }\n"
                "    return rf_cm_encode_orig(rgb, w, h, out, cap, written, threads, err);\n"
                "}\n"
            )
            rep_cm.write_text(rep_cm_text, encoding="utf-8")
            rep_exe = res_dir / "bench_rep.exe"
            ab.build(rep_cm, rep_exe, flags=("-O0",))
            with mock.patch.object(ab, "RES", res_dir):
                rep_out = ab.run(rep_exe, ["raw_1x1"], reps=2)
            self.assertIn("RAW_ENC_CALL=1", rep_out)
            self.assertIn("RAW_ENC_CALL=2", rep_out)
            rep_rows = ab.parse_bench_rows(rep_out, ["raw_1x1"])
            self.assertGreater(rep_rows["raw_1x1"][1], rep_rows["raw_1x1"][2] * 5.0)


if __name__ == "__main__":
    unittest.main()
