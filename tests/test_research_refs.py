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

    def test_timing_comparisons_build_both_sides_without_breakdown(self) -> None:
        """A timing comparison builds both variants lean; only the single-variant bench keeps the breakdown harness."""
        cmds: list[list[str]] = []

        def fake_compile(cmd, **kwargs):
            cmds.append(list(cmd))
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(ab.subprocess, "run", side_effect=fake_compile):
            ab.build(ROOT / "src" / "cm.c", Path(tmp) / "inst.exe")
            ab.build(ROOT / "src" / "cm.c", Path(tmp) / "lean.exe", breakdown=False)
            ab.build(ROOT / "src" / "pal.c", Path(tmp) / "lean_pal.exe", breakdown=False)
            ab.build(ROOT / "src" / "sc.c", Path(tmp) / "lean_sc.exe", breakdown=False)
        self.assertNotIn("-DRF_NO_BREAKDOWN=1", cmds[0])
        self.assertIn("-DRF_NO_BREAKDOWN=1", cmds[1])
        self.assertIn("-DRF_NO_BREAKDOWN=1", cmds[2])
        self.assertIn("-DRF_NO_BREAKDOWN=1", cmds[3])

        built: list[tuple[str, bool]] = []

        def fake_build(src, out, flags=(), breakdown=True):
            built.append((Path(out).name, breakdown))

        def fake_run(exe, imgs, reps=1):
            rows = "".join(f"{n}.ppm  100  bpp=1.0000  enc=    1.0ms dec=    1.0ms  (1/1 cyc/sample) OK\n" for n in imgs)
            return rows + f"TOTAL {100 * len(imgs)}  enc=1.0ms dec=1.0ms  (1/1 cyc/sample)\n"

        with tempfile.TemporaryDirectory() as tmp:
            res_dir = Path(tmp)
            (res_dir / "ppm").mkdir()
            with (
                mock.patch.object(ab, "RES", res_dir),
                mock.patch.object(ab, "PPM", res_dir / "ppm"),
                mock.patch.object(ab, "build", side_effect=fake_build),
                mock.patch.object(ab, "run", side_effect=fake_run),
                mock.patch.object(sys, "argv", ["ab.py", "ab", "src/cm.c", "build/research/try.c", "-n2", "alpha"]),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                ab.main()
        self.assertEqual(built, [("benchA.exe", False), ("benchB.exe", False)])

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
            self.assertIn(
                "- `rasterfold` uses its default thread count (up to nine channel and strip threads); `rasterfold-1t` passes `--threads 1`.",
                md,
            )
            self.assertNotIn("three threads (one per channel)", md)
            self.assertIn("retained sets (photos, synthetic) keep their previously recorded measurements", md)
            self.assertIn("Retained from existing `benchmarks/results.csv` (not re-measured in this run).", md)
            self.assertIn("| jxl-e9 | 1440564 |", md)
            self.assertIn("| jxl-e9 | -- | -- | -- | -- | -- | -- |", md)
            for r in reloaded:
                if r["pixels_match"]:
                    self.assertIn(str(r["bytes"]), md)
                else:
                    self.assertIn("FAIL", md)

    def test_abpool_self_test(self) -> None:
        import abpool

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = abpool.main(["--self-test"])
        self.assertEqual(rc, 0, buf.getvalue())

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
        by_img = {r["image"]: r for r in rows}

        photo = ab.reconstruct_set_summary(csv_path, "photo")
        self.assertEqual(photo["images"], ab.SETS["photo"])
        self.assertEqual(photo["count"], 39)
        self.assertEqual(photo["rasterfold"], 30634873)
        self.assertEqual(photo["jxl_e7"], 31782058)
        self.assertEqual(photo["jxl_e9"], 31400714)
        self.assertEqual(photo["webp"], 34138176)
        self.assertEqual(f"{photo['e7_ratio']:.4f}", "0.9639")
        self.assertEqual(f"{photo['e7_geomean']:.4f}", "0.9622")
        self.assertEqual(photo["worst_image"], "kodim17")
        self.assertEqual(f"{photo['worst_e7_ratio']:.4f}", "0.9934")
        self.assertEqual(f"{photo['e9_ratio']:.4f}", "0.9756")
        self.assertEqual(f"{photo['webp_ratio']:.4f}", "0.8974")
        self.assertEqual(photo["below_e9"], 35)

        kodak = ab.reconstruct_set_summary(csv_path, "kodak")
        self.assertEqual(kodak["images"], ab.SETS["kodak"])
        self.assertEqual(kodak["count"], 24)
        self.assertEqual(f"{kodak['e7_ratio']:.4f}", "0.9729")
        self.assertEqual(f"{kodak['e7_geomean']:.4f}", "0.9726")
        self.assertEqual(kodak["worst_image"], "kodim17")
        self.assertEqual(f"{kodak['worst_e7_ratio']:.4f}", "0.9934")

        ic = ab.reconstruct_set_summary(csv_path, "ic")
        self.assertEqual(ic["images"], ab.SETS["ic"])
        self.assertEqual(ic["count"], 14)
        self.assertEqual(f"{ic['e7_ratio']:.4f}", "0.9602")
        self.assertEqual(f"{ic['e7_geomean']:.4f}", "0.9453")
        self.assertEqual(ic["worst_image"], "ic_nightshot_iso_1600")
        self.assertEqual(f"{ic['worst_e7_ratio']:.4f}", "0.9904")

        tp = ab.reconstruct_set_summary(csv_path, "tp")
        self.assertEqual(tp["images"], ab.SETS["tp"])
        self.assertEqual(tp["count"], 10)
        self.assertEqual(f"{tp['e7_ratio']:.4f}", "0.9666")
        self.assertEqual(f"{tp['e7_geomean']:.4f}", "0.9625")
        self.assertEqual(tp["worst_image"], "tp_nightshot_iso_1600_crop")
        self.assertEqual(f"{tp['worst_e7_ratio']:.4f}", "0.9897")

        synth = ab.reconstruct_set_summary(csv_path, "synth")
        self.assertEqual(synth["images"], ab.SETS["synth"])
        self.assertEqual(synth["count"], 11)
        self.assertEqual(f"{synth['e7_ratio']:.4f}", "0.8862")
        self.assertEqual(f"{synth['e7_geomean']:.4f}", "0.2870")
        self.assertEqual(synth["worst_image"], "syn_noise")
        self.assertEqual(f"{synth['worst_e7_ratio']:.4f}", "0.9241")

        for set_name, count in (("photo", 39), ("kodak", 24), ("ic", 14), ("tp", 10), ("synth", 11)):
            summary = ab.reconstruct_set_summary(csv_path, set_name)
            imgs = ab.SETS[set_name]
            self.assertEqual(summary["images"], imgs)
            self.assertEqual(summary["count"], count)
            exp_rf = sum(by_img[i]["rasterfold"] for i in imgs)
            exp_e7 = sum(by_img[i]["jxl_e7"] for i in imgs)
            exp_e9 = sum(by_img[i]["jxl_e9"] for i in imgs)
            exp_wb = sum(by_img[i]["webp"] for i in imgs)
            exp_e7_gm = math.exp(sum(math.log(by_img[i]["rasterfold"] / by_img[i]["jxl_e7"]) for i in imgs) / count)
            exp_e9_gm = math.exp(sum(math.log(by_img[i]["rasterfold"] / by_img[i]["jxl_e9"]) for i in imgs) / count)
            exp_wb_gm = math.exp(sum(math.log(by_img[i]["rasterfold"] / by_img[i]["webp"]) for i in imgs) / count)
            exp_worst_ratio, exp_worst_img = max((by_img[i]["rasterfold"] / by_img[i]["jxl_e7"], i) for i in imgs)
            exp_below_e9 = sum(1 for i in imgs if by_img[i]["rasterfold"] < by_img[i]["jxl_e9"])
            self.assertEqual(summary["rasterfold"], exp_rf)
            self.assertEqual(summary["jxl_e7"], exp_e7)
            self.assertEqual(summary["jxl_e9"], exp_e9)
            self.assertEqual(summary["webp"], exp_wb)
            self.assertAlmostEqual(summary["e7_ratio"], exp_rf / exp_e7, places=9)
            self.assertAlmostEqual(summary["e9_ratio"], exp_rf / exp_e9, places=9)
            self.assertAlmostEqual(summary["webp_ratio"], exp_rf / exp_wb, places=9)
            self.assertAlmostEqual(summary["e7_geomean"], exp_e7_gm, places=9)
            self.assertAlmostEqual(summary["e9_geomean"], exp_e9_gm, places=9)
            self.assertAlmostEqual(summary["webp_geomean"], exp_wb_gm, places=9)
            self.assertEqual(summary["worst_image"], exp_worst_img)
            self.assertAlmostEqual(summary["worst_e7_ratio"], exp_worst_ratio, places=9)
            self.assertEqual(summary["below_e9"], exp_below_e9)

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
                self.assertEqual(rows["kat_cm"][0], len(tc.KAT_FOLD3))
                self.assertEqual(rows["kat_pal"][0], len(tc.KAT_PAL4))

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
                "    if (w == 1 && h == 1) {\n"
                "        g_raw_enc_calls++;\n"
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

    def test_compat_allow_diff_and_m1_baseline_build(self) -> None:
        import os as os_mod
        import test_correctness as tc

        # 1. Option parsing and mode validation for --allow-diff.
        allowed, cleaned = ab.extract_allow_diff_args(["7c3f7c7", "--allow-diff", "synth"])
        self.assertEqual(allowed, {"synth"})
        self.assertEqual(cleaned, ["7c3f7c7"])
        allowed2, cleaned2 = ab.extract_allow_diff_args(["--allow-diff=photo,tp-only", "7c3f7c7"])
        self.assertEqual(allowed2, {"photo", "tp-only"})
        self.assertEqual(cleaned2, ["7c3f7c7"])

        for bad_args in (
            ["--allow-diff"],
            ["--allow-diff", "-v"],
            ["--allow-diff="],
            ["--allow-diff", "unknown_set"],
            ["--allow-diff=synth,bogus"],
            ["--unknown-flag"],
        ):
            with self.assertRaises(SystemExit):
                ab.extract_allow_diff_args(bad_args)

        with mock.patch.object(sys, "argv", ["ab.py", "size", "photo", "--allow-diff", "synth"]):
            with self.assertRaises(SystemExit) as ctx:
                ab.main()
            self.assertIn("only supported in compat mode", str(ctx.exception))

        # 2. verify_compat_sets enforces thread determinism, allowed-set scoping,
        # both-version decode fidelity, and bench_m1 size agreement.
        sets_map = {
            "photo": ["p1"],
            "tp": ["t1"],
            "tp-only": ["to1"],
            "synth": ["s_same", "s_pal"],
        }
        pixels = {
            name: (4, 4, bytes([(i + idx) & 0xFF for i in range(48)]))
            for idx, name in enumerate(("p1", "t1", "to1", "s_same", "s_pal"))
        }
        base_blobs = {name: f"m1:{name}".encode("ascii") for name in pixels}
        cand_blobs = {
            name: (b"cand:s_pal_pal2" if name == "s_pal" else base_blobs[name])
            for name in pixels
        }
        bench_sizes = {name: len(blob) for name, blob in base_blobs.items()}

        def load_img(img: str) -> tuple[int, int, bytes]:
            return pixels[img]

        def make_enc(overrides: dict[tuple[str, str, str], bytes] | None = None):
            ov = overrides or {}

            def _enc(tag: str, t: str, img: str, _w: int, _h: int, _rgb: bytes) -> bytes:
                if (tag, t, img) in ov:
                    return ov[(tag, t, img)]
                return base_blobs[img] if tag == "m1" else cand_blobs[img]

            return _enc

        def make_dec(bad_key: tuple[str, str, str] | None = None):
            def _dec(_tag: str, t: str, img: str, src_tag: str, _blob: bytes) -> tuple[int, int, bytes]:
                w, h, rgb = pixels[img]
                if bad_key == (src_tag, t, img):
                    return (w, h, bytes([rgb[0] ^ 1]) + rgb[1:])
                return (w, h, rgb)

            return _dec

        lines = ab.verify_compat_sets(
            sets_map,
            load_img,
            make_enc(),
            make_dec(),
            {"synth"},
            bench_m1_sizes=bench_sizes,
        )
        self.assertEqual(len(lines), 4)
        self.assertTrue(any(l.startswith("photo ") and "identical=True" in l and "bench_m1_verified=True" in l for l in lines))
        self.assertTrue(any(l.startswith("synth ") and "identical=False (1/2 changed)" in l and "cand_decoded=True" in l for l in lines))

        # Rejects synthetic diff when synth is not in allow_diff_sets.
        with self.assertRaises(SystemExit) as ctx:
            ab.verify_compat_sets(sets_map, load_img, make_enc(), make_dec(), set(), bench_m1_sizes=bench_sizes)
        self.assertIn("MISMATCH on s_pal in set synth", str(ctx.exception))

        # Rejects photo diff even when synth is in allow_diff_sets.
        with self.assertRaises(SystemExit) as ctx:
            ab.verify_compat_sets(
                sets_map,
                load_img,
                make_enc({("cand", "1", "p1"): b"cand:p1_diff", ("cand", "3", "p1"): b"cand:p1_diff"}),
                make_dec(),
                {"synth"},
                bench_m1_sizes=bench_sizes,
            )
        self.assertIn("MISMATCH on p1 in set photo", str(ctx.exception))

        # Rejects thread nondeterminism on m1 or cand even inside an allowed-diff set.
        for tag in ("m1", "cand"):
            with self.assertRaises(SystemExit) as ctx:
                ab.verify_compat_sets(
                    sets_map,
                    load_img,
                    make_enc({(tag, "3", "s_pal"): b"nondeterministic_t3"}),
                    make_dec(),
                    {"synth"},
                    bench_m1_sizes=bench_sizes,
                )
            self.assertIn(f"MISMATCH on s_pal ({tag} t=3)", str(ctx.exception))

        # Rejects decode mismatch on either m1 or cand files at either thread count.
        for src_tag in ("m1", "cand"):
            for t in ("1", "3"):
                with self.assertRaises(SystemExit) as ctx:
                    ab.verify_compat_sets(
                        sets_map,
                        load_img,
                        make_enc(),
                        make_dec((src_tag, t, "s_pal")),
                        {"synth"},
                        bench_m1_sizes=bench_sizes,
                    )
                self.assertIn(f"DECODE MISMATCH on s_pal ({src_tag}->cand t={t})", str(ctx.exception))

        # Rejects bench_m1 size disagreement with m1 CLI output.
        bad_bench = dict(bench_sizes)
        bad_bench["s_pal"] += 1
        with self.assertRaises(SystemExit) as ctx:
            ab.verify_compat_sets(
                sets_map,
                load_img,
                make_enc(),
                make_dec(),
                {"synth"},
                bench_m1_sizes=bad_bench,
            )
        self.assertIn("BENCH MISMATCH on s_pal", str(ctx.exception))

        # 3. Verify ab.build links cm_m1.c against m1_src/ container/codec sources (without current pal.c).
        if not ab.CLANG.is_file():
            self.skipTest(f"missing mingw clang toolchain at {ab.CLANG}")

        with tempfile.TemporaryDirectory() as tmp:
            res_dir = Path(tmp)
            m1_dir = res_dir / "m1_src"
            m1_dir.mkdir(parents=True, exist_ok=True)
            ppm_dir = res_dir / "ppm"
            ppm_dir.mkdir(parents=True, exist_ok=True)
            (ppm_dir / "kat_pal.ppm").write_bytes(
                f"P6\n{tc.KAT_PAL_W} {tc.KAT_PAL_H}\n255\n".encode("ascii") + tc.KAT_PAL_RGB
            )
            (ppm_dir / "solid_16x16.ppm").write_bytes(
                b"P6\n16 16\n255\n" + bytes([40, 80, 120] * (16 * 16))
            )

            # Populate m1_src with pre-palette sources (fold.c calling only rf_cm_encode/rf_cm_decode, no pal.c).
            for fname in ("rf.h", "rc.h", "bmp.c", "crc32.c", "cm.c"):
                (m1_dir / fname).write_bytes((ROOT / "src" / fname).read_bytes())
            fold_no_pal = (
                (ROOT / "src" / "fold.c")
                .read_text(encoding="utf-8")
                .replace(
                    "    size_t pal_n = 0;\n"
                    "    rc = rf_pal_encode(img->rgb, img->width, img->height, f + RF_HEADER_SIZE, raw - 1, &pal_n, err);\n"
                    "    if (rc) { rf_free(f); return rc; }\n"
                    "    uint8_t pal_small[2048];\n"
                    "    uint8_t *pal_buf = 0;\n"
                    "    uint32_t pal_count = 0;\n"
                    "    size_t cm_cap = raw - 1;\n"
                    "    if (pal_n > 0) {\n"
                    "        pal_count = (uint32_t)f[RF_HEADER_SIZE] + 1u;\n"
                    "        if (pal_n <= sizeof(pal_small)) {\n"
                    "            pal_buf = pal_small;\n"
                    "        } else {\n"
                    "            pal_buf = rf_alloc(pal_n);\n"
                    "            if (!pal_buf) { rf_free(f); return rf_fail(err, RF_ERR_LIMIT, \"out of memory\"); }\n"
                    "        }\n"
                    "        memcpy(pal_buf, f + RF_HEADER_SIZE, pal_n);\n"
                    "        cm_cap = pal_n;\n"
                    "    }\n"
                    "    size_t n = 0;\n"
                    "    rc = rf_cm_encode(img->rgb, img->width, img->height, f + RF_HEADER_SIZE, cm_cap, &n, threads, err);\n"
                    "    if (rc) { if (pal_buf != pal_small) rf_free(pal_buf); rf_free(f); return rc; }\n"
                    "    uint32_t method = RF_METHOD_CM;\n"
                    "    if (n == 0) {\n"
                    "        if (pal_n > 0) {\n"
                    "            memcpy(f + RF_HEADER_SIZE, pal_buf, pal_n);\n"
                    "            n = pal_n;\n"
                    "            method = RF_METHOD_PAL3;\n"
                    "        } else {\n"
                    "            memcpy(f + RF_HEADER_SIZE, img->rgb, raw);\n"
                    "            n = raw;\n"
                    "            method = RF_METHOD_RAW;\n"
                    "        }\n"
                    "    }\n"
                    "    if (pal_buf != pal_small) rf_free(pal_buf);\n"
                    "    if (n > 1 && (pal_n == 0 || pal_count > 16u)) {\n"
                    "        size_t sc_n = 0;\n"
                    "        rc = rf_sc2_encode(img->rgb, img->width, img->height, f + RF_HEADER_SIZE, n, &sc_n, err);\n"
                    "        if (rc) { rf_free(f); return rc; }\n"
                    "        if (sc_n > 0) {\n"
                    "            n = sc_n;\n"
                    "            method = RF_METHOD_SC2;\n"
                    "        }\n"
                    "    }\n",
                    "    size_t n = 0;\n"
                    "    rc = rf_cm_encode(img->rgb, img->width, img->height, f + RF_HEADER_SIZE, raw - 1, &n, threads, err);\n"
                    "    if (rc) { rf_free(f); return rc; }\n"
                    "    uint32_t method = RF_METHOD_CM;\n"
                    "    if (n == 0) {\n"
                    "        memcpy(f + RF_HEADER_SIZE, img->rgb, raw);\n"
                    "        n = raw;\n"
                    "        method = RF_METHOD_RAW;\n"
                    "    }\n",
                )
                .replace(
                    "    else if (hdr.method == RF_METHOD_SC || hdr.method == RF_METHOD_SC2) {\n"
                    "        if ((rc = rf_sc_decode(payload, hdr.payload_size, rgb, 0, hdr.width, hdr.height, hdr.method, err)) != RF_OK) { rf_free(rgb); return rc; }\n"
                    "    } else if (hdr.method >= RF_METHOD_PAL) {\n"
                    "        if ((rc = rf_pal_decode(payload, hdr.payload_size, rgb, 0, hdr.width, hdr.height, hdr.method, err)) != RF_OK) { rf_free(rgb); return rc; }\n"
                    "    }",
                    "",
                )
                .replace(
                    "    else if (hdr.method == RF_METHOD_SC || hdr.method == RF_METHOD_SC2) {\n"
                    "        if ((rc = rf_sc_decode(payload, hdr.payload_size, *bmp, stride, hdr.width, hdr.height, hdr.method, err)) != RF_OK) {\n"
                    "            rf_free(*bmp);\n"
                    "            *bmp = 0;\n"
                    "            *bmp_size = 0;\n"
                    "            return rc;\n"
                    "        }\n"
                    "    } else if (hdr.method >= RF_METHOD_PAL) {\n"
                    "        if ((rc = rf_pal_decode(payload, hdr.payload_size, *bmp, stride, hdr.width, hdr.height, hdr.method, err)) != RF_OK) {\n"
                    "            rf_free(*bmp);\n"
                    "            *bmp = 0;\n"
                    "            *bmp_size = 0;\n"
                    "            return rc;\n"
                    "        }\n"
                    "    }",
                    "",
                )
            )
            self.assertNotIn("rf_pal_encode", fold_no_pal)
            self.assertNotIn("rf_pal_decode", fold_no_pal)
            self.assertNotIn("rf_sc2_encode", fold_no_pal)
            self.assertNotIn("rf_sc_decode", fold_no_pal)
            (m1_dir / "fold.c").write_text(fold_no_pal, encoding="utf-8")
            cm_m1_path = res_dir / "cm_m1.c"
            cm_m1_path.write_bytes((ROOT / "src" / "cm.c").read_bytes())

            bench_m1_exe = res_dir / "bench_m1.exe"
            bench_cur_exe = res_dir / "bench_cur.exe"
            with mock.patch.object(ab, "RES", res_dir):
                self.assertTrue(ab.is_m1_baseline_source(cm_m1_path))
                self.assertFalse(ab.is_m1_baseline_source(ROOT / "src" / "cm.c"))
                ab.build(cm_m1_path, bench_m1_exe, flags=("-O0",))
                ab.build(ROOT / "src" / "cm.c", bench_cur_exe, flags=("-O0",))

                if os_mod.name == "nt" or subprocess.run(
                    [str(bench_m1_exe)],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                ).returncode == 0:
                    out_m1 = ab.run(bench_m1_exe, ["kat_pal", "solid_16x16"], reps=1)
                    out_cur = ab.run(bench_cur_exe, ["kat_pal", "solid_16x16"], reps=1)
                    self.assertIn(" RAW\n", out_m1)
                    self.assertIn(" OK\n", out_m1)
                    self.assertNotIn(" PAL\n", out_m1)
                    self.assertIn(" PAL\n", out_cur)
                    rows_m1 = ab.parse_bench_rows(out_m1, ["kat_pal", "solid_16x16"])
                    rows_cur = ab.parse_bench_rows(out_cur, ["kat_pal", "solid_16x16"])
                    self.assertEqual(rows_cur["kat_pal"][0], len(tc.KAT_PAL4))
                    self.assertEqual(rows_m1["kat_pal"][0], 32 + tc.KAT_PAL_W * tc.KAT_PAL_H * 3)
                    self.assertLess(rows_cur["solid_16x16"][0], rows_m1["solid_16x16"][0])

                # 4. Also verify Milestone 1's 7-parameter rf_cm_decode signature (without stride) builds and runs cleanly.
                rf_h_7arg = (
                    (m1_dir / "rf.h")
                    .read_text(encoding="utf-8")
                    .replace(
                        "int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, int threads, int version, rf_error *err);",
                        "int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *rgb, uint32_t w, uint32_t h, int threads, rf_error *err);",
                    )
                )
                self.assertIn(
                    "int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *rgb, uint32_t w, uint32_t h, int threads, rf_error *err);",
                    rf_h_7arg,
                )
                (m1_dir / "rf.h").write_text(rf_h_7arg, encoding="utf-8")
                fold_7arg = (
                    fold_no_pal.replace(
                        "rf_cm_decode(payload, hdr.payload_size, rgb, 0, hdr.width, hdr.height, threads, (int)version, err)",
                        "rf_cm_decode(payload, hdr.payload_size, rgb, hdr.width, hdr.height, threads, err)",
                    ).replace(
                        "rf_cm_decode(payload, hdr.payload_size, *bmp, stride, hdr.width, hdr.height, threads, (int)version, err)",
                        "rf_cm_decode(payload, hdr.payload_size, *bmp, hdr.width, hdr.height, threads, err)",
                    )
                )
                (m1_dir / "fold.c").write_text(fold_7arg, encoding="utf-8")
                cm_7arg = (
                    (ROOT / "src" / "cm.c")
                    .read_text(encoding="utf-8")
                    .replace(
                        "int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *out, size_t stride, uint32_t w, uint32_t h, int threads, int version, rf_error *err) {",
                        "int rf_cm_decode(const uint8_t *in, size_t n, uint8_t *out, uint32_t w, uint32_t h, int threads, rf_error *err) {\n    size_t stride = 0;\n    int version = 3;",
                    )
                )
                cm_m1_path.write_text(cm_7arg, encoding="utf-8")
                bench_m1_7arg_exe = res_dir / "bench_m1_7arg.exe"
                ab.build(cm_m1_path, bench_m1_7arg_exe, flags=("-O0",))
                if os_mod.name == "nt" or subprocess.run(
                    [str(bench_m1_7arg_exe)],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                ).returncode == 0:
                    out_m1_7arg = ab.run(bench_m1_7arg_exe, ["kat_pal", "solid_16x16"], reps=1)
                    rows_m1_7arg = ab.parse_bench_rows(out_m1_7arg, ["kat_pal", "solid_16x16"])
                    self.assertEqual(rows_m1_7arg["kat_pal"][0], rows_m1["kat_pal"][0])
                    self.assertEqual(rows_m1_7arg["solid_16x16"][0], rows_m1["solid_16x16"][0])
                    p_no_bd = subprocess.run(
                        [str(bench_m1_7arg_exe), "--breakdown", "ppm/solid_16x16.ppm"],
                        stdin=subprocess.DEVNULL,
                        capture_output=True,
                        text=True,
                        cwd=res_dir,
                    )
                    self.assertEqual(p_no_bd.returncode, 2)
                    self.assertIn("--breakdown is not supported", p_no_bd.stderr)

    def test_bench_breakdown_reconciles_channels_kinds_and_variants(self):
        import os as os_mod
        import re
        import test_correctness as tc

        with tempfile.TemporaryDirectory() as td:
            res_dir = Path(td) / "research"
            ppm_dir = res_dir / "ppm"
            ppm_dir.mkdir(parents=True)

            # 1. Write a PAL image (kat_pal), a RAW image (raw_1x1), and a CM image (>256 colours with smooth regions + spikes |r| >= 64)
            if not ab.CLANG.is_file():
                self.skipTest(f"missing mingw clang toolchain at {ab.CLANG}")
            pal_ppm = f"P6\n{tc.KAT_PAL_W} {tc.KAT_PAL_H}\n255\n".encode("ascii") + tc.KAT_PAL_RGB
            (ppm_dir / "kat_pal.ppm").write_bytes(pal_ppm)
            (ppm_dir / "raw_1x1.ppm").write_bytes(b"P6\n1 1\n255\n\x10\x20\x30")

            def write_noisy_ppm(name: str, w: int, h: int) -> None:
                raw_pixels = bytearray(w * h * 3)
                for y in range(h):
                    for x in range(w):
                        idx = (y * w + x) * 3
                        spike = 96 if ((x & 7) == 3 and (y & 3) == 2) else 0
                        raw_pixels[idx + 0] = ((x * 3 + y * 5 + spike) + (x & 1)) & 0xFF
                        raw_pixels[idx + 1] = ((x * 4 + y * 2) ^ (spike >> 1)) & 0xFF
                        raw_pixels[idx + 2] = ((x * 2 + y * 7 + spike) - (y & 1)) & 0xFF
                (ppm_dir / name).write_bytes(f"P6\n{w} {h}\n255\n".encode("ascii") + bytes(raw_pixels))

            write_noisy_ppm("noisy_cm.ppm", 64, 32)
            write_noisy_ppm("noisy_s2.ppm", 16, 512)
            write_noisy_ppm("noisy_s3.ppm", 16, 768)

            bench_exe = res_dir / "bench_bd.exe"
            with mock.patch.object(ab, "RES", res_dir):
                ab.build(ROOT / "src" / "cm.c", bench_exe, flags=("-O0",))

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

            if not can_run_pe(bench_exe):
                self.skipTest("cannot execute Windows PE binaries on this host")

            p_prof_bd = subprocess.run(
                [str(bench_exe), "-p", "--breakdown", "ppm/noisy_cm.ppm"],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                cwd=res_dir,
            )
            self.assertEqual(p_prof_bd.returncode, 2)
            self.assertIn("-p and --breakdown cannot be combined", p_prof_bd.stderr)

            fixture_ppms = (
                "ppm/noisy_cm.ppm",
                "ppm/noisy_s2.ppm",
                "ppm/noisy_s3.ppm",
                "ppm/kat_pal.ppm",
                "ppm/raw_1x1.ppm",
            )
            fixture_names = ["noisy_cm", "noisy_s2", "noisy_s3", "kat_pal", "raw_1x1"]

            def run_bd(*extra_args, exe=bench_exe):
                proc = subprocess.run(
                    [str(exe), *extra_args, *fixture_ppms],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    cwd=res_dir,
                )
                self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)
                self.assertNotIn("MISMATCH", proc.stdout)
                return proc.stdout

            out_plain = run_bd("-r1", "-t1")
            out_r1_t1 = run_bd("-r1", "-t1", "--breakdown")
            out_r2_t1 = run_bd("-r2", "-t1", "--breakdown")
            out_r1_t3 = run_bd("-r1", "-t3", "--breakdown")

            rows_plain = ab.parse_bench_rows(out_plain, fixture_names)
            rows_bd = ab.parse_bench_rows(out_r1_t1, fixture_names)
            for name in fixture_names:
                self.assertEqual(rows_plain[name][0], rows_bd[name][0])

            bd_lines_r1_t1 = [ln for ln in out_r1_t1.splitlines() if ln.startswith("  breakdown ")]
            bd_lines_r2_t1 = [ln for ln in out_r2_t1.splitlines() if ln.startswith("  breakdown ")]
            bd_lines_r1_t3 = [ln for ln in out_r1_t3.splitlines() if ln.startswith("  breakdown ")]
            self.assertEqual(bd_lines_r1_t1, bd_lines_r2_t1)
            self.assertEqual(bd_lines_r1_t1, bd_lines_r1_t3)
            self.assertEqual(len(bd_lines_r1_t1), 14)
            self.assertIn("breakdown kat_pal.ppm          non-CM method=4 (PAL)", bd_lines_r1_t1[12])
            self.assertIn("breakdown raw_1x1.ppm          non-CM method=0 (RAW)", bd_lines_r1_t1[13])

            for idx, (stem, strips) in enumerate((("noisy_cm", 1), ("noisy_s2", 1), ("noisy_s3", 3))):
                ch_re = re.compile(
                    rf"^  breakdown {re.escape(stem)}\.ppm\s+(G|R-G|B-G|ALL)\s+"
                    r"stream=\s*(\d+)\s+\(\s*[\d.]+%\)\s+"
                    r"dec=\s*([\d.]+)\s+"
                    r"zero=\s*([\d.]+)\s+\(\s*[\d.]+%\)\s+"
                    r"sign=\s*([\d.]+)\s+\(\s*[\d.]+%\)\s+"
                    r"exp=\s*([\d.]+)\s+\(\s*[\d.]+%\)\s+"
                    r"mant=\s*([\d.]+)\s+\(\s*[\d.]+%\)\s+"
                    r"flush=\s*([+-][\d.]+)"
                    r"(?:\s+hdr=(\d+)\s+fold=(\d+))?$"
                )
                parsed = {}
                for ln in bd_lines_r1_t1[idx * 4 : (idx + 1) * 4]:
                    m = ch_re.match(ln)
                    self.assertIsNotNone(m, msg=f"unmatched breakdown line: {ln}")
                    tag = m.group(1)
                    parsed[tag] = {
                        "stream": int(m.group(2)),
                        "dec": float(m.group(3)),
                        "zero": float(m.group(4)),
                        "sign": float(m.group(5)),
                        "exp": float(m.group(6)),
                        "mant": float(m.group(7)),
                        "flush": float(m.group(8)),
                        "hdr": int(m.group(9)) if m.group(9) else None,
                        "fold": int(m.group(10)) if m.group(10) else None,
                    }
                for tag in ("G", "R-G", "B-G", "ALL"):
                    row = parsed[tag]
                    self.assertGreater(row["zero"], 0.0)
                    self.assertGreater(row["sign"], 0.0)
                    self.assertGreater(row["exp"], 0.0)
                    self.assertGreater(row["mant"], 100.0)
                    self.assertAlmostEqual(
                        row["zero"] + row["sign"] + row["exp"] + row["mant"],
                        row["dec"],
                        delta=0.3,
                    )
                    self.assertAlmostEqual(row["dec"] + row["flush"], row["stream"], delta=0.3)
                    max_flush = (24.0 if tag == "ALL" else 8.0) * strips
                    self.assertGreaterEqual(row["flush"], 0.0)
                    self.assertLessEqual(row["flush"], max_flush)

                self.assertEqual(
                    parsed["G"]["stream"] + parsed["R-G"]["stream"] + parsed["B-G"]["stream"],
                    parsed["ALL"]["stream"],
                )
                self.assertEqual(parsed["ALL"]["hdr"], 32 + 1 + 12 * strips)
                self.assertEqual(parsed["ALL"]["stream"] + parsed["ALL"]["hdr"], parsed["ALL"]["fold"])
                self.assertEqual(parsed["ALL"]["fold"], rows_bd[stem][0])

            # 2. Verify a variant CM source that changes the bitstream is also instrumented via RF_BENCH_CM_PATH
            variant_cm = res_dir / "cm_variant.c"
            variant_cm.write_text(
                (ROOT / "src" / "cm.c")
                .read_text(encoding="utf-8")
                .replace(
                    "for (int k = 0; k < LMSN; k++) ch->lms[k] = (nmodel == NMODEL && c == 2 && k == 6) ? 20000 : 0;",
                    "for (int k = 0; k < LMSN; k++) ch->lms[k] = (c == 2 && k == 6) ? 16384 : 0;",
                ),
                encoding="utf-8",
            )
            bench_var_exe = res_dir / "bench_var.exe"
            with mock.patch.object(ab, "RES", res_dir):
                ab.build(variant_cm, bench_var_exe, flags=("-O0",))
            out_var = run_bd("-r1", "-t1", "--breakdown", exe=bench_var_exe)
            rows_var = ab.parse_bench_rows(out_var, ["noisy_cm", "kat_pal", "raw_1x1"])
            self.assertNotEqual(rows_var["noisy_cm"][0], rows_bd["noisy_cm"][0])

            # 3. Verify a breakdown payload mismatch reports the failure reason on stderr and exits nonzero
            bad_bd_cm = res_dir / "cm_bad_bd.c"
            bad_bd_cm.write_text(
                (ROOT / "src" / "cm.c")
                .read_text(encoding="utf-8")
                .replace(
                    "*written = total;",
                    "#ifdef RF_BENCH_BREAKDOWN_PASS\n        out[0] ^= 0xFFu;\n#endif\n        *written = total;",
                ),
                encoding="utf-8",
            )
            bench_bad_exe = res_dir / "bench_bad_bd.exe"
            with mock.patch.object(ab, "RES", res_dir):
                ab.build(bad_bd_cm, bench_bad_exe, flags=("-O0",))
            proc_bad = subprocess.run(
                [str(bench_bad_exe), "-r1", "-t1", "--breakdown", "ppm/noisy_cm.ppm"],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                cwd=res_dir,
            )
            self.assertEqual(proc_bad.returncode, 1, msg=proc_bad.stdout + proc_bad.stderr)
            self.assertIn("MISMATCH", proc_bad.stdout)
            self.assertIn("instrumented payload mismatch", proc_bad.stderr)

    def test_vsjxl_webp_photos_parses_args_and_runs_alternating_benchmark(self) -> None:
        args = vsjxl.parse_args(["--webp-photos", "-n3", "--threads", "9"])
        self.assertTrue(args.webp_photos)
        self.assertEqual(args.n, 3)
        self.assertEqual(args.threads, 9)

        args_def = vsjxl.parse_args(["--webp-photos", "-n", "2"])
        self.assertTrue(args_def.webp_photos)
        self.assertEqual(args_def.n, 2)
        self.assertIsNone(args_def.threads)

        import benchmark

        with tempfile.TemporaryDirectory() as tmp:
            td = Path(tmp)
            fake_exe = td / "rasterfold"
            fake_exe.write_bytes(b"fake")
            calls: list[list[str]] = []

            def fake_run(cmd: list[str]) -> tuple[int, int, int]:
                calls.append(list(cmd))
                out_path = Path(cmd[-1])
                out_path.write_bytes(b"out")
                if "encode" in cmd:
                    return (0, 100, 0)
                if str(fake_exe) in cmd[0]:
                    return (0, 80000, 0)
                if "-c:v" in cmd:
                    return (0, 100, 0)
                return (0, 100000, 0)

            buf = io.StringIO()
            with (
                mock.patch.object(vsjxl, "TMP", td),
                mock.patch.object(vsjxl.os, "name", "posix"),
                mock.patch.object(vsjxl, "_ffmpeg_bin", return_value="ffmpeg"),
                mock.patch.object(benchmark, "run", side_effect=fake_run),
                contextlib.redirect_stdout(buf),
            ):
                vsjxl.run_webp_photos(fake_exe, 3, 9)

            out = buf.getvalue()
            self.assertIn("TOTAL threads=9: rf=800000 (0.80s) webp=1000000 (1.00s) ratio=0.800 geomean=0.800", out)
            rf_dec_calls = [c for c in calls if "decode" in c and str(fake_exe) in c[0]]
            self.assertEqual(len(rf_dec_calls), 30)
            self.assertTrue(all("--threads" in c and "9" in c for c in rf_dec_calls))

            with (
                mock.patch.object(vsjxl.os, "name", "posix"),
                mock.patch.object(vsjxl.subprocess, "check_output", return_value="C:\\wsl\\tmp\\in.bmp\n") as m_wsl,
            ):
                self.assertEqual(vsjxl._ff_path("/mnt/c/ffmpeg.exe", td / "in.bmp"), "C:\\wsl\\tmp\\in.bmp")
                m_wsl.assert_called_once()
                self.assertEqual(vsjxl._ff_path("ffmpeg", td / "in.bmp"), str(td / "in.bmp"))


class ScreensSetTests(unittest.TestCase):
    """The development screenshots (D01 T07 section 1): disjoint from the held-out set, frozen, recoverable."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.ppm = self.root / "ppm"
        self.ppm.mkdir()
        self.assets = self.root / "screens-assets"
        self.assets.mkdir()
        self.manifest = self.root / "screens.json"
        self.held = json.loads((ROOT / "research" / "heldout.json").read_text(encoding="utf-8"))
        from PIL import Image
        self.Image = Image
        entries = []
        for i, name in enumerate(ab.SETS["screens"], 1):
            im = Image.new("RGB", (1280, 960), (i, 2 * i, 3 * i))
            im.save(self.assets / f"{name}.png")
            im.save(self.ppm / f"{name}.ppm")
            entries.append({"name": name, "kind": "screenshot", "page": f"page-{i}", "source": f"docs/page-{i}.md",
                            "source_sha256": "0" * 64, "browser_version": "test", "repository_revision": "0" * 40,
                            "rgb_sha256": corpus._rgb_sha256(im), "width": 1280, "height": 960})
        self.entries = entries
        self.write_manifest(entries)
        for name, value in (("SCREENS_MANIFEST", self.manifest), ("SCREENS_ASSETS", self.assets)):
            patcher = mock.patch.object(corpus, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def write_manifest(self, entries) -> None:
        self.manifest.write_text(json.dumps({"set": "screens", "frozen": True, "images": entries}, indent=2) + "\n",
                                 encoding="utf-8")

    def refused(self, needle: str, call) -> None:
        before = self.manifest.read_bytes()
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as caught:
            call()
        self.assertIn(needle, str(caught.exception))
        self.assertEqual(self.manifest.read_bytes(), before, "a refusal leaves the manifest byte-identical")

    def test_the_committed_set_is_disjoint_from_the_held_out_set(self) -> None:
        committed = json.loads((ROOT / "research" / "screens.json").read_text(encoding="utf-8"))["images"]
        self.assertEqual([e["name"] for e in committed], ab.SETS["screens"])
        held_pages = set(corpus.HELDOUT_PAGES)
        held_hashes = {e["rgb_sha256"] for e in self.held["images"]}
        for e in committed:
            self.assertNotIn(e["page"], held_pages)
            self.assertNotIn(e["rgb_sha256"], held_hashes)
            self.assertTrue((ROOT / "research" / "screens-assets" / f"{e['name']}.png").is_file())
        self.assertEqual({e["browser_version"] for e in committed},
                         {e["browser_version"] for e in self.held["images"] if e["kind"] == "screenshot"})

    def test_a_held_out_page_or_held_out_pixels_are_refused(self) -> None:
        page = [dict(e) for e in self.entries]
        page[3]["page"] = corpus.HELDOUT_PAGES[0]
        self.refused(f"page {corpus.HELDOUT_PAGES[0]!r} is a held-out page", lambda: corpus._refuse_heldout_overlap(page))
        pixels = [dict(e) for e in self.entries]
        pixels[5]["rgb_sha256"] = self.held["images"][0]["rgb_sha256"]
        self.refused("its pixels equal a held-out image", lambda: corpus._refuse_heldout_overlap(pixels))
        with mock.patch.object(corpus, "SCREENS_PAGES", [(corpus.HELDOUT_PAGES[1], "x.md")]):
            self.refused("is a held-out page", lambda: corpus.author_screens(self.ppm))
        # A held-out page under another label is caught by its source path, staged or original.
        for source in ("build/site-source/index.md", "build/site-source/./index.md", "docs/index.md", "README.md"):
            relabelled = [dict(e) for e in self.entries]
            relabelled[2]["source"] = source
            self.refused(f"source {source!r} is a held-out page's source", lambda: corpus._refuse_heldout_overlap(relabelled))
            with mock.patch.object(corpus, "SCREENS_PAGES", [("api", source)]):
                self.refused(f"source {source!r} is a held-out page's source", lambda: corpus.author_screens(self.ppm))

    def test_a_missing_ppm_is_recovered_from_its_committed_png(self) -> None:
        (self.ppm / "dv_s04.ppm").unlink()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            corpus.screens(self.ppm)
        self.assertTrue((self.ppm / "dv_s04.ppm").is_file())
        self.assertIn("screens verified: 10 screenshots", out.getvalue())
        (self.ppm / "dv_s04.ppm").unlink()
        (self.assets / "dv_s04.png").unlink()
        self.refused("research/screens-assets/dv_s04.png is not committed", lambda: corpus.screens(self.ppm))

    def test_a_changed_source_page_is_refused_at_authoring(self) -> None:
        import hashlib
        repo = self.root / "repo"
        (repo / "docs").mkdir(parents=True)
        (repo / "docs" / "site-content.json").write_bytes((ROOT / "docs" / "site-content.json").read_bytes())
        entries = [dict(e) for e in self.entries]
        for e in entries:
            source = repo / e["source"]
            source.write_text(f"# {e['page']}\n", encoding="utf-8")
            e["source_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        self.write_manifest(entries)
        with mock.patch.object(corpus, "REPO", repo), mock.patch.object(corpus.subprocess, "run"):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                corpus.author_screens(self.ppm)              # unchanged sources: recovered and verified, not re-rendered
            self.assertIn("screens verified: 10 screenshots", out.getvalue())
            (repo / "docs" / "page-4.md").write_text("# page-4, edited after the freeze\n", encoding="utf-8")
            self.refused("docs/page-4.md differs from the frozen source hash", lambda: corpus.author_screens(self.ppm))

    def test_another_browser_version_is_refused_at_authoring(self) -> None:
        self.manifest.unlink()
        shots = [{"page": p, "png": self.assets / "dv_s01.png", "browser": "shell (Other Chrome 1.0)", "source_sha256": "0" * 64}
                 for p, _ in corpus.SCREENS_PAGES]
        with mock.patch.object(corpus.subprocess, "run"), mock.patch.object(corpus, "_screenshot_pages", return_value=shots), \
                contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as caught:
            corpus.author_screens(self.ppm)
        self.assertIn("rendered with another browser than the held-out set's", str(caught.exception))
        self.assertFalse(self.manifest.exists(), "a refused authoring writes no manifest")

    def test_changed_pixels_are_refused_before_measuring(self) -> None:
        target = self.ppm / "dv_s07.ppm"
        data = bytearray(target.read_bytes())
        data[-1] ^= 1
        target.write_bytes(bytes(data))
        with mock.patch.object(ab, "FROZEN_MANIFESTS", (("screens", str(self.manifest)),)), \
                mock.patch.object(ab, "REPO", Path("/")), self.assertRaises(SystemExit) as caught:
            ab.verify_frozen(["dv_s07"], ppm_dir=self.ppm)
        self.assertIn("screens dv_s07: RGB SHA-256 mismatch against the frozen manifest", str(caught.exception))
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as caught:
            corpus.verify_screens(self.ppm)
        self.assertIn("screens dv_s07: RGB SHA-256 mismatch", str(caught.exception))
        # The size command itself refuses before building anything.
        repo = self.root / "repo"
        (repo / "research").mkdir(parents=True)
        (repo / "research" / "ab.py").write_bytes((ROOT / "research" / "ab.py").read_bytes())
        (repo / "research" / "screens.json").write_bytes(self.manifest.read_bytes())
        (repo / "research" / "heldout.json").write_bytes((ROOT / "research" / "heldout.json").read_bytes())
        (repo / "build" / "research" / "ppm").mkdir(parents=True)
        (repo / "build" / "research" / "ppm" / "dv_s07.ppm").write_bytes(target.read_bytes())
        result = subprocess.run([sys.executable, str(repo / "research" / "ab.py"), "size", "dv_s07"],
                                capture_output=True, text=True, timeout=120)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("screens dv_s07: RGB SHA-256 mismatch against the frozen manifest", result.stderr)
        self.assertFalse((repo / "build" / "research" / "benchS.exe").exists(), "nothing was built")
        # An unfrozen set is never measured.
        with mock.patch.object(ab, "FROZEN_MANIFESTS", (("screens", "no/such/screens.json"),)), self.assertRaises(SystemExit) as caught:
            ab.verify_frozen(["dv_s01"], ppm_dir=self.ppm)
        self.assertIn("refusing to measure an unfrozen set", str(caught.exception))


class ScreenRepetitionTests(unittest.TestCase):
    """research/screenrep.py on images whose parse is known by hand (D01 T07 section 2)."""

    @staticmethod
    def image(pixels):
        return b"".join(p.to_bytes(3, "big") for p in pixels)

    def setUp(self) -> None:
        import screenrep
        self.sr = screenrep

    def test_prefix_codes_follow_rfc_9649(self) -> None:
        # Value -> (symbol, extra bits): 1..4 have no extra bits, then two symbols per power of two.
        expect = {1: (0, 0), 4: (3, 0), 5: (4, 1), 6: (4, 1), 7: (5, 1), 8: (5, 1), 9: (6, 2), 12: (6, 2), 13: (7, 2),
                  4096: (23, 10)}
        for value, code in expect.items():
            self.assertEqual(self.sr.prefix_code(value), code, value)
        with self.assertRaises(ValueError):
            self.sr.prefix_code(0)

    def test_a_repeated_run_in_one_row_is_one_copy(self) -> None:
        a, b, c, d = 0x102030, 0x405060, 0x708090, 0xA0B0C0
        got = self.sr.parse(8, 1, self.image([a, b, c, d, a, b, c, d]))
        self.assertEqual(got["tokens"], {"literal": 4, "copy": 1})
        self.assertEqual(got["copy_pixels"], 4)
        self.assertEqual(got["distance_classes"], {"same row": 1})

    def test_a_run_crossing_a_row_boundary_and_an_image_of_one_colour(self) -> None:
        got = self.sr.parse(4, 2, self.image([0x123456] * 8))
        self.assertEqual(got["tokens"], {"literal": 1, "copy": 1})
        self.assertEqual(got["copy_pixels"], 7)                       # the copy runs on across the row end
        self.assertEqual(got["distance_classes"], {"left": 1})
        # One literal (three channels of one value each: 0 bits), one copy, two kinds: 2 bits; extra bits of length 7.
        self.assertEqual(got["bits"]["literal G"], 0.0)
        self.assertEqual(got["bits"]["extra bits"], 1.0)
        self.assertAlmostEqual(got["bytes"], (2 + 1) / 8)

    def test_an_above_copy(self) -> None:
        a, b, c = 0x010203, 0x040506, 0x070809
        got = self.sr.parse(3, 3, self.image([a, b, c] * 3))
        self.assertEqual(got["tokens"], {"literal": 3, "copy": 1})
        self.assertEqual(got["distance_classes"], {"above": 1})

    def test_a_cache_collision_and_a_cache_hit(self) -> None:
        by_index = {}
        colliding = None
        for colour in range(1, 1 << 16):
            idx = self.sr.cache_index(colour * 251)
            if idx in by_index:
                colliding = (by_index[idx], colour * 251)
                break
            by_index[idx] = colour * 251
        x, y = colliding
        self.assertEqual(self.sr.cache_index(x), self.sr.cache_index(y))
        self.assertEqual(self.sr.parse(3, 1, self.image([x, y, x]))["tokens"], {"literal": 3})   # y evicted x
        z = next(c for c in range(1, 1 << 16) if self.sr.cache_index(c) != self.sr.cache_index(x))
        self.assertEqual(self.sr.parse(3, 1, self.image([x, z, x]))["tokens"], {"literal": 2, "cache": 1})

    def test_a_distance_beyond_the_window_is_never_used(self) -> None:
        self.assertEqual(self.sr.MAX_DISTANCE, (1 << 20) - 120)
        self.assertEqual(self.sr.prefix_code(self.sr.MAX_DISTANCE + 120), (39, 18))   # the last distance symbol
        self.assertEqual(self.sr.prefix_code(self.sr.MAX_DISTANCE + 121)[0], 40)      # one more would need a 41st
        a, b, c, d = 0x102030, 0x405060, 0x708090, 0xA0B0C0
        with mock.patch.object(self.sr, "MAX_DISTANCE", 3):
            got = self.sr.parse(8, 1, self.image([a, b, c, d, a, b, c, d]))
        self.assertEqual(got["tokens"].get("copy", 0), 0)                             # distance 4 is outside
        with mock.patch.object(self.sr, "MAX_DISTANCE", 4):
            got = self.sr.parse(8, 1, self.image([a, b, c, d, a, b, c, d]))
        self.assertEqual(got["tokens"], {"literal": 4, "copy": 1})
        self.assertEqual(got["max_distance_symbol"], 3)

    def test_pixels_that_never_repeat_are_all_literals(self) -> None:
        seen, colours = set(), []
        for colour in range(1, 1 << 20, 7919):
            if self.sr.cache_index(colour) not in seen:
                seen.add(self.sr.cache_index(colour))
                colours.append(colour)
            if len(colours) == 8:
                break
        got = self.sr.parse(8, 1, self.image(colours))
        self.assertEqual(got["tokens"], {"literal": 8})
        self.assertEqual(got["copy_pixels"], 0)

    def test_only_verified_development_screenshots_are_read(self) -> None:
        with self.assertRaises(SystemExit) as caught:
            self.sr.main(["ho_s01"])
        self.assertIn("reads only the development screenshots", str(caught.exception))
        with mock.patch.object(corpus, "verify_screens", side_effect=SystemExit("screens dv_s01: RGB SHA-256 mismatch")), \
                mock.patch.object(self.sr, "read_ppm") as read, self.assertRaises(SystemExit) as caught:
            self.sr.main(["dv_s01"])
        self.assertIn("RGB SHA-256 mismatch", str(caught.exception))
        read.assert_not_called()


class ScreenCodeTests(unittest.TestCase):
    """research/screencode.py, the round 1 prototype (D01 T07 section 3): known parses and exact round trips."""

    def setUp(self) -> None:
        import screencode
        self.sc = screencode

    @staticmethod
    def image(pixels):
        return b"".join(p.to_bytes(3, "big") for p in pixels)

    def round_trip(self, width, height, data):
        payload = self.sc.encode(width, height, data)
        self.assertEqual(self.sc.decode(width, height, payload), data)
        return payload

    def toks(self, width, height, data):
        return list(self.sc.tokens(width, height, data))

    def test_a_one_colour_image_overlapping_copies_and_the_longest_length(self) -> None:
        data = self.image([0x336699] * 5000)
        L, C, P = self.sc.LITERAL, self.sc.CACHE, self.sc.COPY
        self.assertEqual(self.toks(100, 50, data), [(L, 0x336699), (P, 4096, 1), (P, 903, 1)])   # overlap: distance 1
        self.round_trip(100, 50, data)
        self.assertEqual(self.sc.prefix_code(4096 - 3)[0], 23)

    def test_a_cache_hit(self) -> None:
        x = 0x102030
        z = next(c for c in range(1, 1 << 16) if self.sc.cache_index(c) != self.sc.cache_index(x))
        self.assertEqual(self.toks(3, 1, self.image([x, z, x])),
                         [(self.sc.LITERAL, x), (self.sc.LITERAL, z), (self.sc.CACHE, self.sc.cache_index(x))])
        self.round_trip(3, 1, self.image([x, z, x]))

    def test_a_2d_mapped_distance_and_one_beyond_the_map(self) -> None:
        width = 100
        self.assertEqual(self.sc.distance_code(width, width), 1)               # the pixel above: (0, 1)
        self.assertEqual(self.sc.distance_code(width + 1, width), 3)           # above-left: (1, 1)
        self.assertEqual(self.sc.distance_code(width - 1, width), 4)           # above-right: (-1, 1)
        self.assertEqual(self.sc.distance_code(20 * width, width), 20 * width + 120)
        for v in range(1, 121):
            self.assertEqual(self.sc.distance_code(self.sc.map_distance(v, width), width) <= v, True)
        # A row repeated 20 rows later is copied from beyond the map, and round-trips.
        row = [0x010000 * (i % 7) + 0x000100 * (i % 11) + i for i in range(width)]
        noise = [((i * 2654435761) & 0xFFFFFF) for i in range(width * 19)]
        data = self.image(row + noise + row)
        toks = self.toks(width, 21, data)
        self.assertIn((self.sc.COPY, width, 20 * width), toks)
        self.round_trip(width, 21, data)

    def test_the_window_and_the_largest_distance_symbol(self) -> None:
        self.assertEqual(self.sc.WINDOW, (1 << 20) - 120)
        self.assertEqual(self.sc.prefix_code(self.sc.WINDOW + 120)[0], 39)
        self.assertEqual(self.sc.prefix_value(*self.sc.prefix_code(self.sc.WINDOW + 120)[::2]), self.sc.WINDOW + 120)
        a, b, c, d = 0x102030, 0x405060, 0x708090, 0xA0B0C0
        with mock.patch.object(self.sc, "WINDOW", 3):
            self.assertNotIn(self.sc.COPY, [t[0] for t in self.toks(8, 1, self.image([a, b, c, d, a, b, c, d]))])

    def test_prefix_codes_round_trip_every_value_up_to_the_window(self) -> None:
        for value in list(range(1, 5000)) + [self.sc.WINDOW + 120, 524289, 786432, 786433]:
            symbol, nbits, extra = self.sc.prefix_code(value)
            self.assertEqual(nbits, 0 if symbol < 4 else (symbol - 2) >> 1)
            self.assertEqual(self.sc.prefix_value(symbol, extra), value)

    def test_a_bucket_collision_is_visited_and_does_not_hide_a_match(self) -> None:
        # Two different 4-pixel keys in one bucket: the chain visits the colliding entry and still finds the match.
        import itertools
        seen = {}
        pair = None
        for k in itertools.count(1):
            key = self.image([k, k + 1, k + 2, k + 3])
            bucket = self.sc.fnv_bucket(key)
            if bucket in seen:
                pair = (seen[bucket], k)
                break
            seen[bucket] = k
        k1, k2 = pair
        a = [k1, k1 + 1, k1 + 2, k1 + 3]
        b = [k2, k2 + 1, k2 + 2, k2 + 3]
        data = self.image(a + b + a)
        toks = self.toks(12, 1, data)
        self.assertEqual(toks[-1], (self.sc.COPY, 4, 8))
        self.round_trip(12, 1, data)

    def test_refusals(self) -> None:
        with self.assertRaises(self.sc.Refused) as caught:
            self.sc.decode(4, 1, b"\x00\x00\x00\x00")
        self.assertIn("shorter than 5 bytes", str(caught.exception))
        payload = self.round_trip(4, 1, self.image([1, 2, 3, 4]))
        with self.assertRaises(self.sc.Refused) as caught:
            self.sc.decode(4, 1, payload + b"\x00")
        self.assertIn("stream ends at byte", str(caught.exception))

    def test_noise_and_small_images_round_trip(self) -> None:
        import random
        rng = random.Random(7)
        for width, height in ((1, 1), (1, 9), (9, 1), (37, 11)):
            self.round_trip(width, height, bytes(rng.randrange(256) for _ in range(3 * width * height)))
            self.round_trip(width, height, self.image([rng.choice([0, 0xFFFFFF, 0x808080]) for _ in range(width * height)]))


class ScreenCode2Tests(unittest.TestCase):
    """research/screencode2.py, the round 2 prototype (D01 T07 section 4): method 6 parse, grammar, and refusals."""

    def setUp(self) -> None:
        import screencode2
        self.sc2 = screencode2

    @staticmethod
    def image(pixels):
        return b"".join(p.to_bytes(3, "big") for p in pixels)

    def round_trip(self, width: int, height: int, data: bytes) -> tuple[bytes, int, dict[str, int]]:
        split: dict[str, int] = {}
        payload, cov4 = self.sc2.encode_info(width, height, data, split=split)
        self.assertEqual(self.sc2.decode(width, height, payload), data)
        return payload, cov4, split

    def test_1x1_and_p0_literal_and_l4096_inherited_copy(self) -> None:
        pay1, cov1, sp1 = self.round_trip(1, 1, self.image([0x112233]))
        self.assertEqual(cov1, 0)
        self.assertGreaterEqual(len(pay1), 5)
        self.assertEqual(sum(sp1.values()), len(pay1))
        data = self.image([0x336699] * 5000)
        _, cov5000, _ = self.round_trip(100, 50, data)
        self.assertEqual(cov5000, 4999)
        self.assertEqual(self.sc2.prefix_code(4096 - 1)[0], 23)
        self.assertEqual(self.sc2.prefix_code(1 << 25)[0], 49)

    def test_inherited_l2_stem_orig_ring_wrap_and_rep_queues(self) -> None:
        # Row 0 establishes a non-inherited copy at distance D = 6 (L = 4); row 1 at the same column
        # inherits D = 6 from orig[p - width] to copy a 2-pixel stem (L = 2, so cov4 counts only row 0's 4).
        w, h = 16, 4
        px = [(i * 2654435761) & 0xFFFFFF for i in range(w * h)]
        motif4 = [0xAA1122, 0xBB3344, 0xCC5566, 0xDD7788]
        px[2:6] = motif4
        px[8:12] = motif4  # p = 8 copies from q = 2 -> D = 6, L = 4
        stem2 = [0xEE9900, 0xFFAA11]
        px[w + 2 : w + 4] = stem2
        px[w + 8 : w + 10] = stem2  # p = w + 8 inherits D = 6 from orig[8] across orig_ring wrap-around
        # Row 3 exercises non-inherited multi-row copy with dy in rep_dy (dy = 2) and dx == 0 vs signed dx.
        motif5 = [0x123456, 0x234567, 0x345678, 0x456789]
        px[w + 11 : w + 15] = motif5
        px[3 * w + 11 : 3 * w + 15] = motif5  # dy = 2 (in initial rep_dy), dx = 0
        _, cov4, split = self.round_trip(w, h, self.image(px))
        self.assertGreaterEqual(cov4, 8)
        self.assertGreater(split.get("inh_copy", 0), 0)
        self.assertGreater(split.get("new_copy", 0), 0)

    def test_implicit_same_row_at_borders_and_signed_dx(self) -> None:
        # py == 0 forces same_row = 1; px_col == 0 (py >= 1) forces same_row = 0 with negative dx (qx > 0).
        from unittest import mock

        w, h = 16, 3
        px = [((i + 1) * 7919) & 0xFFFFFF for i in range(w * h)]
        m_top = [0x101010, 0x202020, 0x303030, 0x404040]
        px[1:5] = m_top
        px[6:10] = m_top  # p = 6 (py == 0): non-inherited same_row copy (L = 4, dx = 5, implicit same_row = 1)
        m_col0 = [0x505050, 0x606060, 0x707070, 0x808080]
        px[11:15] = m_col0
        px[2 * w : 2 * w + 4] = m_col0  # p = 32 (px_col == 0, py == 2): qy == 0, qx == 11 -> dy = 2, dx = -11 < 0

        models = []
        real_model = self.sc2.Model6
        with mock.patch.object(self.sc2, "Model6", side_effect=lambda: (models.append(real_model()) or models[-1])):
            _, cov4, split = self.round_trip(w, h, self.image(px))
        self.assertEqual(cov4, 8)
        self.assertEqual(split.get("inh_copy", 0), 0)
        self.assertGreater(split.get("new_copy", 0), 0)
        self.assertEqual(len(models), 2)
        for m in models:
            # Both copies sit on implicit borders (py == 0 and px_col == 0), so is_same_row is never coded.
            self.assertEqual([c.n for c in m.is_same_row], [0, 0])
            # Row 0 copy coded dx = 5 in dx0_tree; col 0 copy coded dy = 2 in rep_dy and negative dx in dx_sign/dx_tree.
            self.assertTrue(any(c.n > 0 for c in m.dx0_tree))
            self.assertEqual(m.is_rep_dy.n, 1)
            self.assertGreater(m.is_rep_dy.c, 32768)
            self.assertEqual(m.dx_zero[1].n, 1)
            self.assertLess(m.dx_zero[1].c, 32768)
            self.assertEqual(m.dx_sign.n, 1)
            self.assertGreater(m.dx_sign.c, 32768)
            self.assertTrue(any(c.n > 0 for c in m.dx_tree))

        # Non-border positive dx (px_col > qx) exercises explicit is_same_row and dx_sign bit 0.
        px2 = [((i + 1) * 7919) & 0xFFFFFF for i in range(w * h)]
        px2[2:6] = m_top
        px2[2 * w + 9 : 2 * w + 13] = m_top  # py == 2, px_col == 9, qy == 0, qx == 2 -> dy = 2, dx = +7 > 0
        models2 = []
        with mock.patch.object(self.sc2, "Model6", side_effect=lambda: (models2.append(real_model()) or models2[-1])):
            _, cov4_pos, _ = self.round_trip(w, h, self.image(px2))
        self.assertEqual(cov4_pos, 4)
        for m in models2:
            self.assertEqual(m.is_same_row[0].n, 1)
            self.assertLess(m.is_same_row[0].c, 32768)
            self.assertEqual(m.dx_sign.n, 1)
            self.assertLess(m.dx_sign.c, 32768)

    def test_eq_w_eq_n_mtf_and_11bit_cache(self) -> None:
        # Build 20 distinct colours so early colours fall out of the 16-entry MTF list into the 2048-entry cache,
        # while adjacent pairs exercise EQ_W (both wn_eq == 0 and wn_eq == 1) and EQ_N.
        cols = [((i + 1) * 104729) & 0xFFFFFF for i in range(20)]
        seen_slots: set[int] = set()
        uniq_cols: list[int] = []
        c = 1
        while len(uniq_cols) < 20:
            col = (c * 104729) & 0xFFFFFF
            s = self.sc2.cache_index(col)
            if s not in seen_slots and col not in uniq_cols:
                seen_slots.add(s)
                uniq_cols.append(col)
            c += 1
        cols = uniq_cols
        extra17 = []
        while len(extra17) < 17:
            col = (c * 104729) & 0xFFFFFF
            s = self.sc2.cache_index(col)
            if s not in seen_slots and col not in cols and col not in extra17:
                seen_slots.add(s)
                extra17.append(col)
            c += 1
        row0 = cols + [cols[19], cols[18]] + cols[:16]
        # Evict cols[1] from MTF using extra17, then emit cols[1] (11-bit CACHE hit), plus 8 isolated EQ_N hits!
        eq_n_tail = [cols[i] if (i % 2 == 0) else extra17[i] for i in range(16)]
        row1 = [cols[0], cols[0], cols[2]] + extra17 + [cols[1], cols[1]] + eq_n_tail
        # Row 2 starts with [cols[0], cols[0], ...]: at x=1, W=cols[0] and N=row1[1]=cols[0] (wn_eq == 1),
        # followed by isolated EQ_W pairs so eq_w also flushes bytes.
        row2 = [cols[0], cols[0], cols[3], cols[3], cols[4], cols[4], cols[5], cols[5]] + extra17 + cols[:13]
        self.assertEqual(len(row0), 38)
        self.assertEqual(len(row1), 38)
        self.assertEqual(len(row2), 38)
        _, _, split = self.round_trip(38, 3, self.image(row0 + row1 + row2))
        for k in ("eq_w", "eq_n", "mtf", "cache", "literal"):
            self.assertGreater(split.get(k, 0), 0, k)

    def test_fnv_bucket_collision_matches_past_colliding_key(self) -> None:
        import itertools
        seen = {}
        pair = None
        for k in itertools.count(1):
            key = self.image([k, k + 1, k + 2, k + 3])
            bucket = self.sc2.fnv_bucket(key)
            if bucket in seen:
                pair = (seen[bucket], k)
                break
            seen[bucket] = k
        k1, k2 = pair
        a = [k1, k1 + 1, k1 + 2, k1 + 3]
        b = [k2, k2 + 1, k2 + 2, k2 + 3]
        _, cov4, _ = self.round_trip(12, 1, self.image(a + b + a))
        self.assertEqual(cov4, 4)

    def test_only_verified_development_screenshots_are_measured(self) -> None:
        with self.assertRaises(SystemExit) as caught:
            self.sc2.main(["ho_s01"])
        self.assertIn("measures only the development screenshots", str(caught.exception))
        with mock.patch.object(corpus, "verify_screens", side_effect=SystemExit("screens dv_s01: RGB SHA-256 mismatch")), \
                self.assertRaises(SystemExit) as caught:
            self.sc2.main(["dv_s01"])
        self.assertIn("RGB SHA-256 mismatch", str(caught.exception))

    def test_all_14_stream_refusals_and_trailing_bytes(self) -> None:
        # 1. Payload shorter than 5 bytes.
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(4, 1, b"\x00\x00\x00\x00")
        # Trailing byte after valid stream.
        valid, _, _ = self.round_trip(4, 1, self.image([1, 2, 3, 4]))
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(4, 1, valid + b"\x00")

        def build_p0(m: object) -> tuple[object, object]:
            enc = self.sc2.Encoder()
            self.sc2.put_tree(enc, m.lit_g[4], 10, 8)
            self.sc2.put_tree(enc, m.lit_rg[0], 20, 8)
            self.sc2.put_tree(enc, m.lit_bg[0], 30, 8)
            return enc, m

        # 2. Inherited length symbol > 23.
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 1)
        self.sc2.put_tree(enc, m.len_tree[1], 24, 5)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(4, 1, enc.finish())

        # 3. Inherited length past end of image (p = 1, L = 4 on 4x1 -> p + L = 5 > 4).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 1)
        sym, nb, ex = self.sc2.prefix_code(4 - 1)
        self.sc2.put_tree(enc, m.len_tree[1], sym, 5)
        self.sc2.put_extra(enc, m.len_extra[1][sym], nb, ex)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(4, 1, enc.finish())

        # 4. Non-inherited length symbol > 23.
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 25, 5)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(8, 1, enc.finish())

        # 5. Non-inherited length > 4096 (symbol 23 with extra = 1023 -> prefix_value = 4096 -> L = 4099 > 4096).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 23, 5)
        self.sc2.put_extra(enc, m.len_extra[0][23], 10, 1023)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(5000, 1, enc.finish())

        # 6. same_row dx0_tree symbol > 39 (py = 0 forces same_row = 1).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 0, 5)
        self.sc2.put_tree(enc, m.dx0_tree, 40, 6)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(8, 1, enc.finish())

        # 7. same_row dx > px_col (at p = 1 on 8x1, px_col = 1, dx = 2).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 0, 5)
        self.sc2.put_tree(enc, m.dx0_tree, 1, 6)  # dx = 2 > px_col = 1
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(8, 1, enc.finish())

        # Helper to reach p = 1 on 1x8 (where px_col = 0, py = 1 -> implicit same_row = 0!).
        # 8. dy_tree symbol > 39.
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 0, 5)  # L = 4 on 1x8
        enc.coded(m.is_rep_dy, 0)
        self.sc2.put_tree(enc, m.dy_tree, 40, 6)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(1, 8, enc.finish())

        # 9. dy > py (at p = 1 on 1x8, py = 1, choose rep_dy[1] = 2 > 1).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 0, 5)
        enc.coded(m.is_rep_dy, 1)
        enc.coded(m.rep_dy_b0, 0)
        enc.coded(m.rep_dy_b1[0], 1)  # ridx = 1 -> dy = 2 > py = 1
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(1, 8, enc.finish())

        # 10. dx_tree symbol > 49 (at p = 1 on 1x8, py = 1, dy = 1, dx != 0, sym = 50).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 0, 5)
        enc.coded(m.is_rep_dy, 1)
        enc.coded(m.rep_dy_b0, 1)  # dy = 1
        enc.coded(m.dx_zero[1], 0)
        enc.coded(m.dx_sign, 0)
        self.sc2.put_tree(enc, m.dx_tree, 50, 6)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(1, 8, enc.finish())

        # 11. qx = px_col - dx out of 0..width - 1 (at p = 1 on 1x8, px_col = 0, dx = +1 -> qx = -1).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
        enc.coded(m.is_inherit[self.sc2.TK_LIT], 0)
        self.sc2.put_tree(enc, m.len_tree[0], 0, 5)
        enc.coded(m.is_rep_dy, 1)
        enc.coded(m.rep_dy_b0, 1)  # dy = 1
        enc.coded(m.dx_zero[1], 0)
        enc.coded(m.dx_sign, 0)  # dx = +1
        self.sc2.put_tree(enc, m.dx_tree, 0, 6)  # adx = 1
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(1, 8, enc.finish())

        # 12. D = dy * width + dx > WINDOW (width = 2048, height = 520, copy row 0..511 first via L=4096*256? Faster:
        #     mock WINDOW = 5 on 4x4: copy p=1..4 with D=1, then at p=8 (py=2, px_col=0) dy=2, dx=0 -> D=8 > 5).
        with mock.patch.object(self.sc2, "WINDOW", 5):
            m = self.sc2.Model6()
            enc, _ = build_p0(m)
            # p = 1: inherited copy of L = 7 at D = 1 -> reaches p = 8 (py = 2, px_col = 0 on 4x4)
            enc.coded(m.is_copy[self.sc2.TK_LIT], 1)
            enc.coded(m.is_inherit[self.sc2.TK_LIT], 1)  # cands = [1] since maxd = 1
            sym, nb, ex = self.sc2.prefix_code(7 - 1)
            self.sc2.put_tree(enc, m.len_tree[1], sym, 5)
            self.sc2.put_extra(enc, m.len_extra[1][sym], nb, ex)
            # p = 8 (py = 2, px_col = 0 -> implicit same_row = 0): dy = 2 <= py, dx = 0 -> D = 8 > WINDOW = 5!
            enc.coded(m.is_copy[self.sc2.TK_COPY], 1)
            enc.coded(m.is_inherit[self.sc2.TK_COPY], 0)
            self.sc2.put_tree(enc, m.len_tree[0], 0, 5)  # L = 4
            enc.coded(m.is_rep_dy, 1)
            enc.coded(m.rep_dy_b0, 0)
            enc.coded(m.rep_dy_b1[0], 1)  # dy = 2
            enc.coded(m.dx_zero[1], 1)  # dx = 0
            with self.assertRaises(self.sc2.Refused):
                self.sc2.decode(4, 4, enc.finish())

        # 13. midx >= mtf_count (at p = 1 on 4x1, mtf_count == 1, midx = 1).
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 0)
        enc.coded(m.is_eq_w[self.sc2.TK_LIT * 2 + 0], 0)
        enc.coded(m.is_mtf[self.sc2.TK_LIT], 1)
        self.sc2.put_tree(enc, m.mtf_tree, 1, 4)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(4, 1, enc.finish())

        # 14. Unset 11-bit cache slot (at p = 1 on 4x1, only slot(rgb0) is set).
        rgb0 = (30 << 16) | (10 << 8) | 40
        bad_slot = (self.sc2.cache_index(rgb0) + 1) & 2047
        m = self.sc2.Model6()
        enc, _ = build_p0(m)
        enc.coded(m.is_copy[self.sc2.TK_LIT], 0)
        enc.coded(m.is_eq_w[self.sc2.TK_LIT * 2 + 0], 0)
        enc.coded(m.is_mtf[self.sc2.TK_LIT], 0)
        enc.coded(m.is_cache[self.sc2.TK_LIT], 1)
        self.sc2.put_tree(enc, m.cache_tree, bad_slot, 11)
        with self.assertRaises(self.sc2.Refused):
            self.sc2.decode(4, 1, enc.finish())

    def test_bench_warms_harness_before_timing_and_profiling(self) -> None:
        bench_src = (ROOT / "research" / "bench.c").read_text(encoding="utf-8")
        defn = bench_src.find("static void warm_harness(int threads)")
        call = bench_src.find("warm_harness(threads > 0 ? threads : 1);")
        prof_start = bench_src.find("if (prof) {", call if call >= 0 else 0)
        timed_loop = bench_src.find("double t0 = cyc_ms();", call if call >= 0 else 0)
        self.assertGreaterEqual(defn, 0)
        self.assertGreater(call, defn)
        self.assertGreater(prof_start, call)
        self.assertGreater(timed_loop, prof_start)


if __name__ == "__main__":
    unittest.main()
