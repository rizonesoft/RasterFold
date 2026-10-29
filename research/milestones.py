"""The 60-milestone roadmap: one table, two outputs.

  python research/milestones.py --check     fail if the generated files differ from this table
  python research/milestones.py --write     regenerate them

Generates `docs/research.md` section 9 (between its BEGIN/END markers) and the
milestone files `todo/05-milestones/TODO-01..06`. Milestones 1 to 10 are gates
over work sections that already exist elsewhere in the tree; milestones 11 to 60
are a planning section (it authors the milestone's work one milestone ahead,
from the previous milestone's measured results) plus a gate. Every gate carries
the no-regression rule and the missed-milestone policy (D99 T01 §4). Edit the
table here, run --write, and commit the table and the generated files together.
"""
from __future__ import annotations

import json
import os
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
RESEARCH = REPO / "docs" / "research.md"
TODO = REPO / "todo" / "05-milestones"
BEGIN, END = "<!-- BEGIN milestones (research/milestones.py) -->", "<!-- END milestones -->"

SIZE = "`python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns)"
SPEED = "`python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`"
BENCH = "`python tests/benchmark.py -n 3`"
IDLE = "Idle machine (timing)"
FF = "ffmpeg with libjxl (benchmarks)"

# (number, title, target, measurement, needs, requires, work refs for 1-10, band title for the first of a band)
M = [
    (1, "Locked baseline",
     "`pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9",
     f"`pwsh scripts/check-all.ps1`, `python3 scripts/ci_status.py` on the last push, {SIZE}",
     [FF], None,
     "D00 T01 §1, D00 T01 §2, D00 T01 §3, D00 T01 §4, D00 T01 §5, D00 T02 §1, D00 T02 §2, D00 T02 §3, D08 T01 §1, D08 T01 §2, D08 T01 §3, D08 T02 §1, D08 T02 §2, D08 T02 §3, D08 T02 §4, D08 T02 §5, D08 T02 §6, D08 T02 §7, D00 T04 §1, D00 T04 §2, D00 T04 §3, D00 T04 §4, D08 T04 §1, D99 T01 §3, D99 T01 §4"),
    (2, "Same bytes, leaner and harder",
     "every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second",
     f"`bin/windows-x64/bin/rasterfold.exe decode --time`, {SPEED}, `python tests/test_correctness.py`",
     [IDLE], None,
     "D03 T01 §1, D03 T01 §2, D03 T01 §3, D04 T01 §1, D02 T01 §1, D02 T01 §2, D02 T01 §4"),
    (3, "Few-colour images solved",
     "method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean",
     SIZE, [FF], None,
     "D01 T01 §1, D01 T01 §2, D01 T01 §3, D01 T01 §4, D04 T01 §2"),
    (4, "Format version 3: beat JPEG XL e9 on photographs",
     "format version 3 with the accepted model changes (`D01 T02 §1`, `D01 T02 §7`, `D01 T03 §1`; operator decision 2026-10-29 under `D99 T01 §4` after `D01 T02 §2` to `§6` missed): photo total at most 0.982 of JPEG XL e9 (and at most 0.970 of JPEG XL e7), test pack at most 0.986 of JPEG XL e9 (and at most 0.973 of e7), at least 31 of 39 photos below e9 (and 39 of 39 at or below e7), three-thread decode at most 0.6 of JPEG XL's, 7-model decode `alloc_peak` at most 10.0 MB (+786 KB for the `k5`/`k6` counter tables over Milestone 3's 5-model 9.16 MB), and 7-model encode/decode time at most 1.27x Milestone 3's 5-model baseline",
     f"{SIZE}, {BENCH}", [FF, IDLE], None,
     "D01 T02 §1, D01 T02 §2, D01 T02 §3, D01 T02 §4, D01 T02 §5, D01 T02 §6, D01 T02 §7, D01 T03 §1"),
    (5, "Exact repeats",
     "a match model in version 3: `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` below JPEG XL e9 (616, 271, 2195, and 2038 bytes); photos no worse than milestone 4",
     SIZE, [FF], None,
     "D01 T04 §1, D01 T04 §2, D01 T04 §3"),
    (6, "Faster than WebP to decode",
     "horizontal strips in version 3: photo decode wall time below lossless WebP's on the 12-thread development machine; strip size cost at most 0.3%",
     f"{BENCH}, {SIZE}", [FF, IDLE], None,
     "D02 T02 §1, D01 T03 §2"),
    (7, "Every photograph beats JPEG XL e9",
     "39 of 39 photos and 10 of 10 test-pack photos below JPEG XL e9; encode at most 0.3 of JPEG XL e7's time",
     f"{SIZE}, {BENCH}", [FF, IDLE], None,
     "D01 T05 §1, D01 T05 §2"),
    (8, "Beat WebP on generated images",
     "the generated set's total and geometric mean below lossless WebP's, and every generated image within 1.1 times WebP's size",
     SIZE, [FF], None,
     "D01 T06 §1, D01 T06 §2, D01 T06 §3"),
    (9, "Holds on images never tuned on",
     "milestones 4, 7, and 8 hold on the held-out set (`python research/ab.py size heldout`) without any retuning",
     "`python research/ab.py size heldout` against JPEG XL e9 and WebP", [FF], None,
     "D00 T03 §1, D00 T03 §2"),
    (10, "RasterFold 1.0",
     "`rasterfold-v1.0.0` released: format version 3 frozen; Windows/Linux x64 and Android ARM64/x64 CLI, shared/static C libraries and the complete native converter; BMP, PNG, and PPM in the CLI; the full HTML docs published; 100,000 fuzz mutations clean; smaller than JPEG XL e9 and lossless WebP on every set, and faster and leaner than JPEG XL on encode, decode, and memory",
     f"{SIZE}, {BENCH}, `python tests/fuzz_decode.py`", [FF, IDLE], None,
     "D06 T01 §1, D06 T01 §2, D06 T01 §3, D06 T01 §4, D06 T01 §5, D07 T01 §1, D07 T01 §2, D07 T01 §3, D07 T01 §4, D08 T03 §1, D08 T03 §2, D09 T01 §1, D09 T01 §2, D09 T01 §3, D09 T01 §4"),
    (11, "SIMD decisions", "one-thread decode at least 10% faster than 1.0, bytes within 0.1%", SPEED, [IDLE], None, "D02 T01 §3, D02 T02 §2"),
    (12, "Two-layer mixing", "photo total at least 0.4% smaller than milestone 11", SIZE, [FF], None, None),
    (13, "Stronger prediction", "photo total at least 0.5% smaller than milestone 12", SIZE, [FF], None, None),
    (14, "Adaptive colour transform", "photo total at least 0.3% smaller than milestone 13, decode cost unchanged", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (15, "Effort levels", "a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged", f"{SPEED}, {SIZE}", [FF, IDLE], None, None),
    (16, "Max-effort encoder search", "the max level at least 1% smaller than the default at the same decode cost", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (17, "Trained starting state", "images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set", "`python research/ab.py size kodak` and `heldout`", [FF], None, None),
    (18, "Streaming", "peak memory on a 24-megapixel image at most one third of milestone 17's", "`bin/windows-x64/bin/rasterfold.exe decode --time` on a 24-megapixel image", [], None, None),
    (19, "Many-core scaling", "decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2%", f"{SPEED}, {SIZE}", [IDLE], None, None),
    (20, "RasterFold 2.0", "`rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (21, "Run mode for flat regions", "generated images decode at least twice as fast and photos at least 5% faster, sizes no worse", SPEED, [IDLE], None, None),
    (22, "Verified hashed context tables", "decode at least 10% faster at equal memory, size within 0.1%", SPEED, [IDLE], None, None),
    (23, "Higher-precision coder", "generated-image geometric mean at least 10% smaller, photos unchanged", SIZE, [FF], None, None),
    (24, "Learned predictor at the max level", "the max level at least 1% smaller on photos, fast and default byte-identical, confirmed on the held-out set", SIZE, [FF], None, None),
    (25, "Texture-gated models", "the default level at least 0.4% smaller at equal decode time", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (26, "Random-access decode", "decoding a 1/16 region at least 8 times faster than a full decode, size within 0.1%", "`bin/windows-x64/bin/rasterfold.exe decode --region` timings", [IDLE], None, None),
    (27, "Portable and byte-exact", "Linux, macOS, and Windows on ARM64 builds with identical bytes on every platform", "the known-answer vectors on every CI platform", [], None, None),
    (28, "Alpha, 16-bit, and metadata", "alpha, 16-bit samples, and ICC and EXIF preserved, 8-bit RGB files byte-identical to milestone 27, the 16-bit set below JPEG XL e9", "`python research/ab.py size rgba16`", [FF], None, None),
    (29, "Coverage-proven decoder", "libFuzzer branch coverage of the decoder at least 95%, no crash in 10^9 executions, hostile-input decode time at most twice honest time", "the libFuzzer campaign report", [], None, None),
    (30, "RasterFold 3.0", "`rasterfold-v3.0.0`: against 2.0, generated images at least 10% smaller, photos at the max level at least 1% smaller, decode at least 1.3 times faster, with random access, portability, alpha, and 16-bit", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (31, "Language bindings and a streaming API", "Python, Rust, and C# bindings over the 1.x C API and a streaming callback API, each with byte-exact round trips and API overhead at most 1%", "the bindings' test suites", [], None, None),
    (32, "More formats", "TIFF, 16-bit PNG, and PAM in and out with exact pixels", "round trips against Pillow and libtiff", [], None, None),
    (33, "Lossless JPEG recompression", "JPEG files stored at least 2% smaller than JPEG XL's JPEG recompression, the original JPEG rebuilt bit for bit", "a JPEG corpus against `cjxl --lossless_jpeg=1`", [FF], None, None),
    (34, "Image sequences", "bursts and screen recordings smaller than lossless animated JPEG XL and animated WebP", "a sequence corpus", [FF], None, None),
    (35, "Gigapixel images", "the size limit raised from 32 megapixels to 4 gigapixels with peak memory proportional to width", "`decode --time` on a 1-gigapixel image", [], None, None),
    (36, "Browser decoding", "decoding in a browser at most twice native time, from a WebAssembly module under 200 KB", "a headless-browser timing harness", [], None, None),
    (37, "Host plugins", "open and save with exact pixels in Pillow, ffmpeg, libvips, and GIMP", "a round-trip test in each host", [], None, None),
    (38, "Second implementation", "an independent Rust decoder, written from the spec alone, byte-identical on the conformance suite", "the conformance suite", [], None, None),
    (39, "Automatic effort", "at least 90% of the max level's gain at no more than 1.5 times the default encode time", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (40, "RasterFold 4.0", "`rasterfold-v4.0.0`: bindings, more formats, JPEG recompression, sequences, gigapixel images, browser decoding, and plugins, with no regression on 3.0 files", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (41, "GPU encode search", "max-level encode at least 10 times faster on a GPU, bytes identical to the CPU search", SPEED, ["GPU (compute)", IDLE], None, None),
    (42, "Energy per megapixel", "decode energy per megapixel at least 20% lower than milestone 41's", "hardware energy counters over a decode run", [IDLE], None, None),
    (43, "Phone-class ARM", "decode per GHz on ARM64 within 1.2 times x86-64, and 4K frames at the fast level in real time", "timings on an ARM64 device", ["ARM64 device", IDLE], None, None),
    (44, "Proven decoder bounds", "bounded model checking proves no out-of-bounds access or overflow in the decoder for every input up to a stated size", "the CBMC run in CI", [], None, None),
    (45, "Reproducible, signed releases", "the same source builds a byte-identical executable on any machine; releases are signed with a software bill of materials", "two independent builds compared", [], None, None),
    (46, "Collection archives", "a library of similar photos at least 3% smaller than encoding each file alone", "a photo-collection corpus", [], None, None),
    (47, "Tiny decoder profile", "a profile decodable in at most 256 KB of working memory by a decoder of at most 20 KB of code", "the profile's decoder size and memory", [], None, None),
    (48, "Recognised format", "detected by libmagic and the major image libraries without our plugins; media type registered", "upstream releases that carry the entries", [], "outside parties accept the registration and the upstream entries", None),
    (49, "Third-party benchmarks", "public benchmark results by others match our measurements within their noise", "the published third-party results", [], "outside benchmark maintainers run and publish the results", None),
    (50, "RasterFold 5.0", "`rasterfold-v5.0.0`: lower energy, a verified decoder, reproducible signed builds, ARM parity, archives, and the tiny profile", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
    (51, "Learned model at the default level", "the default photo size at least 2% smaller at no more than 1.2 times decode time", f"{SIZE}, {SPEED}", [FF, IDLE], None, None),
    (52, "Transmitted priors", "images under 1 megapixel at least 1% smaller", SIZE, [FF], None, None),
    (53, "Camera raw", "raw sensor data smaller than DNG's lossless JPEG and than JPEG XL on the same raw files", "a raw corpus", [FF], None, None),
    (54, "Medical and scientific imagery", "16-bit grayscale and multi-channel images smaller than JPEG-LS and JPEG XL on public medical and satellite sets", "the medical and satellite corpora", [FF], None, None),
    (55, "Error resilience", "a file with a damaged byte still yields every undamaged tile", "a corruption-injection test", [], None, None),
    (56, "Progressive decoding", "a usable preview from the first 5% of the bytes, lossless at the end, for at most 1% size cost", SIZE, [FF], None, None),
    (57, "Lossless edit deltas", "an edited copy of an image stored in at most 10% of a full file", "an edit corpus", [], None, None),
    (58, "Hardware decoder", "an FPGA decoder of the fast or tiny profile at 100 megapixels per second or more, byte-exact with the software", "the FPGA testbench on the conformance suite", ["FPGA board"], None, None),
    (59, "Twenty-year compatibility", "every file written by every release since version 2 decodes in the newest release, proven by a permanent compatibility corpus", "the compatibility corpus in CI", [], None, None),
    (60, "RasterFold 6.0", "`rasterfold-v6.0.0`: the learned default model, raw and scientific data, resilience, progressive decoding, edit deltas, and the hardware reference", f"{SIZE}, {BENCH}", [FF, IDLE], None, None),
]
BANDS = [
    (1, "Milestones 1 to 10: Beat JPEG XL e9 and WebP, then Release 1.0", "beat-jxl-and-webp"),
    (11, "Milestones 11 to 20: RasterFold Beats RasterFold (2.0)", "beat-itself"),
    (21, "Milestones 21 to 30: Reach (3.0)", "reach"),
    (31, "Milestones 31 to 40: Beyond Single Still Files (4.0)", "beyond-still-files"),
    (41, "Milestones 41 to 50: Efficiency and Trust (5.0)", "efficiency-and-trust"),
    (51, "Milestones 51 to 60: New Data and Long-Term Robustness (6.0)", "new-data"),
]
RELEASES = {10: "1.0.0", 20: "2.0.0", 30: "3.0.0", 40: "4.0.0", 50: "5.0.0", 60: "6.0.0"}
RULE = ("It keeps everything milestone {prev} reached: no photo size more than 0.1% worse, no encode or decode time "
        "more than 3% worse, no higher peak memory, and every older file still decoding.")
POLICY = ("If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments "
          "from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.")

PLAN_STATE: dict[int, dict[str, object]] = {}

GATE_STATE: dict[int, dict[str, object]] = {
    1: {
        "started": "2026-10-25T12:41:00Z",
        "checked": True,
        "reached": "reached 2026-10-25",
        "notes": [
            "**Corrected 2026-10-25:** Per the pre-build `--kind plan` review (`docs/reviews/05-milestones/D05-T01-s1.md`), this gate also verifies post-push CI read-back on the completed candidate SHA across `build.yml`, `plan.yml` (including its Jekyll HTML build and `--site-dir` check), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`), runs `python3 scripts/windows.py tests/measure_final.py` (`PASS` on `measure/image.bmp`, required by `docs/research.md` section 2.1), and records a fresh `decode --time` allocator peak baseline on `measure/image.bmp` (`bin/windows-x64/bin/rasterfold.exe` `alloc_peak=10569996` at 3 threads and `alloc_peak=10569964` at `--threads 1`; `bin/linux-x64/bin/rasterfold` `alloc_peak=10570204` at 3 threads; `10.57 MB`) together with `research/vsjxl.py -n5` and `research/ab.py ab src/cm.c src/cm.c measure -n5` at `-t1` and `-t3` for Milestone 2.",
        ],
        "items": [
            "Measure the target with `pwsh scripts/check-all.ps1 -AllTargets`, `python3 scripts/windows.py tests/measure_final.py`, `python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml` on the completed candidate push, and `python3 research/ab.py size photo -v`, `tp -v`, and `synth -v` (with the JPEG XL e9 and WebP columns). Done when: `pwsh scripts/check-all.ps1 -AllTargets` is green with every gate `PASS` (`check-all: 29/29 passed`); `tests/measure_final.py` exits 0 (`PASS`); `build.yml`, `plan.yml`, and `pages.yml` are green on the pushed candidate; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python3 scripts/drift-check.py` exits 0; and the corpus matches the 2026-09-29 baseline (`photo`: `0.9712` of JPEG XL e7 and `0.9829` of e9, `28 of 39` photos below e9), each figure quoted with its command.",
            "Record the baseline figures (`python3 research/ab.py size photo -v`, `tp -v`, and `synth -v` with the JPEG XL e9 and WebP columns; `tests/benchmark.py measure -n 3` and `benchmarks/results.csv`; `bin/windows-x64/bin/rasterfold.exe decode --time` at 3 threads and `--threads 1` plus `bin/linux-x64/bin/rasterfold decode --time` on `measure/image.bmp`; `python3 scripts/windows.py research/vsjxl.py -n5`; and `python3 research/ab.py ab src/cm.c src/cm.c measure -n5` at `-t1` and `-t3`) as the reference for milestone 2. Done when: they are recorded in `docs/research.md` section 1 and quoted in this section's checkpoint and stamp.",
            "Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the documentation sources and tables (`python3 scripts/build-docs.py --write` and `python3 research/milestones.py --write`), verifying HTML generation and `python3 scripts/drift-check.py`. Done when: `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, and `python3 scripts/drift-check.py` exit 0.",
        ],
        "checkpoint": (
            "`pwsh scripts/check-all.ps1 -AllTargets` exits 0 (`check-all: 29/29 passed`); "
            "`python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `rasterfold bytes=1409028 decode_wall_us=295854 decode_peak_ws=16576512 decode_codec_us=183840 pixels=True` vs `jxl bytes=1470703 decode_wall_us=459807 decode_peak_ws=51281920`); "
            "`python3 research/ab.py size photo -v` prints `total=30865211 jxl=31782058 ratio=0.9712 geomean=0.9728 worst=1.0037 (kodim17) e9=31400714 e9_ratio=0.9829 e9_geomean=0.9883 e9_worst=1.0193 (kodim16) webp=34138176 webp_ratio=0.9041 webp_geomean=0.8932 webp_worst=1.0104 (ic_deer) measure=1409028 (0.9581 e9=0.9781 webp=0.8869)` with `28 of 39` photos below e9; "
            "`python3 research/ab.py size tp -v` prints `total=6651583 jxl=6826895 ratio=0.9743 geomean=0.9722 worst=0.9931 (tp_nightshot_iso_1600_crop) e9=6740249 e9_ratio=0.9868 e9_geomean=0.9862 e9_worst=1.0131 (tp_nightshot_iso_1600_crop) webp=7368948 webp_ratio=0.9027 webp_geomean=0.8811 webp_worst=1.0073 (tp_deer_crop)`; "
            "`python3 research/ab.py size synth -v` prints `total=214773 jxl=226544 ratio=0.9480 geomean=1.1986 worst=18.0665 (syn_checker) e9=223034 e9_ratio=0.9630 e9_geomean=2.2860 e9_worst=27.1857 (syn_checker) webp=201248 webp_ratio=1.0672 webp_geomean=3.6626 webp_worst=69.6220 (syn_checker)`; "
            "`tests/benchmark.py measure -n 3` exits 0 and is committed in `benchmarks/results.csv` and `benchmarks/results.md` (`failures: 0`, `rasterfold` `1409028` bytes, `encode_us=308760` / `0.194x` `jxl`, `decode_us=319315` / `0.647x` `jxl`, `encode_peak=16699392`, `decode_peak=16580608`, `rasterfold-1t` `encode_us=640604` / `0.402x` `jxl`, `decode_us=601611` / `1.220x` `jxl`, `encode_peak=16625664`, `decode_peak=16461824`, `jxl` `1470703` bytes, `encode_us=1595197`, `decode_us=493267`, `jxl-1t` `decode_us=721246`, `jxl-e9` `1440564` bytes, `encode_us=10492083`, `decode_us=491123`, `webp` `1588708` bytes, `encode_us=23548198`, `decode_us=219895`; `benchmarks/corpus.csv` verified); "
            "`bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` prints `alloc_peak=10569996` (`10.57 MB`, `codec_us=183820`, `total_us=240767`, `peak_working_set=16576512`) at 3 threads and `alloc_peak=10569964` (`10.57 MB`, `codec_us=488047`, `total_us=557786`, `peak_working_set=16465920`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` prints `alloc_peak=10570204`, `10.57 MB`, `codec_us=196505`, `total_us=205643`); "
            "`python3 scripts/windows.py research/vsjxl.py -n5` prints `rf_enc median 292.8 ms min 271.6`, `rf_dec median 287.7 ms min 284.0`, `jxl_enc median 1582.9 ms min 1549.6`, `jxl_dec median 480.8 ms min 465.2`, `decode ratio (median) 0.598 encode ratio 0.185`; "
            "`python3 research/ab.py ab src/cm.c src/cm.c measure -n5` prints `-t1` `enc min=482.7..484.1 med=496.4..499.6 dec min=453.5..467.2 med=481.7..498.2` and `-t3` `enc min=152.2..168.6 med=177.9..187.6 dec min=167.3..177.9 med=185.2..193.2`; "
            "`python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, and `python3 scripts/drift-check.py` exit 0; "
            "and `python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml` report `build.yml: green at f5e7e8a8ba4c (https://github.com/rizonesoft/RasterFold/actions/runs/40199974368)`, `plan.yml: green at f5e7e8a8ba4c (https://github.com/rizonesoft/RasterFold/actions/runs/40199974415)`, and `pages.yml: green at f5e7e8a8ba4c (https://github.com/rizonesoft/RasterFold/actions/runs/40199974421)` on `f5e7e8a8ba4cbb131b00cf3f0c4a30143c8e548c` (the final code/test commit before this CI-citation-only commit; also green on `8aeee4c707ff` at runs `40198760222`, `40198760291`, `40198760263`, `35eb5c4d1176` at runs `40197473962`, `40197473971`, `40197473951`, and `91b43d439f0c` at runs `40195950978`, `40195950906`, `40195950947`)."
        ),
    },
    2: {
        "started": "2026-10-25T21:35:30Z",
        "checked": True,
        "reached": "reached 2026-10-25",
        "notes": [
            "**Corrected 2026-10-25:** Per the pre-build `--kind plan` review (`docs/reviews/05-milestones/D05-T01-s2.md`), this gate runs `python3 scripts/windows.py research/ab.py compat fbf1036de2cfd0eeaee1a55399563e2d2200328c`, which extracts Milestone 1 (`fbf1036de2cfd0eeaee1a55399563e2d2200328c`) into `build/research/m1_src/`, builds `build/research/vs/rasterfold-m1.exe` and writes `build/research/cm_m1.c` (adapting `rf_cm_decode` with an unused `size_t stride` `(void)stride;` parameter so `research/bench.c` from `D03 T01 §2` links cleanly against Milestone 1's tight-RGB `cm.c`), and verifies full-file `.fold` SHA-256 byte identity (hashing concatenated `.fold` files in `research/ab.py` `SETS` order) between Milestone 1 and the candidate across all 55 distinct corpus images (60 set memberships: `photo` `n=39`, `tp` `n=10` including exclusive `tp-only` `n=5`, `synth` `n=11`) at `--threads 1` and `--threads 3` plus candidate decoding of Milestone 1 `.fold` files back to exact pixels alongside `KAT_FOLD` and `tests/rfi_ref.py` oracle checks; measures alternating CLI runs (`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe`, twice each, plus `research/vsjxl.py -n5` against JPEG XL) and alternating `cm.c` runs (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <measure|photo|tp|tp-only|synth> -n<5|3> -t<1|3>`) requiring `--threads 1` decode minimum-time ratio `<= 0.950` on `measure/image.bmp` and `min_tot <= 0.950` and `geomean <= 0.950` on `photo`, `tp`, and `tp-only`, with the 3% no-regression limit (`<= 1.030`) evaluated on `measure/image.bmp` and the corpus set aggregates (`min_tot` and `geomean`) and any single-image `-n3` scheduling-jitter outlier confirmed `<= 1.030` by a `-n7` repeat run; runs `python3 scripts/check-all.py --all-targets` (`build`, `clang-tidy`, `test_correctness.py` `37 passed, 0 failed`, `test_api.py`, `test_artifacts.py`, `fuzz_decode.py 300 20261025`), `python3 scripts/windows.py tests/measure_final.py`, and `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`); and verifies `python3 scripts/build-docs.py --write`, `python3 research/milestones.py --write`, `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, `python3 scripts/drift-check.py`, and post-push CI read-back on the pushed candidate across `build.yml`, `plan.yml` (including its `actions/jekyll-build-pages` HTML build and `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`).",
        ],
        "items": [
            "Measure the target with `bin/windows-x64/bin/rasterfold.exe decode --time` (at 3 threads and `--threads 1`) and `bin/linux-x64/bin/rasterfold decode --time` on `measure/image.bmp`, `python3 scripts/windows.py research/ab.py compat fbf1036de2cfd0eeaee1a55399563e2d2200328c` (which builds `build/research/vs/rasterfold-m1.exe` and `build/research/cm_m1.c` with an unused `size_t stride` parameter in `rf_cm_decode` and verifies full-file `.fold` SHA-256 byte identity in `SETS` order and candidate decoding of Milestone 1 `.fold` files across all 55 distinct corpus images at `--threads 1` and `--threads 3`), alternating `python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe` (twice each) and `python3 scripts/windows.py research/vsjxl.py -n5`, alternating `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <measure|photo|tp|tp-only|synth> -n<5|3> -t<1|3>` (plus `-n7` confirmation on any single-image `-n3` scheduling-jitter outlier), `python3 tests/test_correctness.py` (37 cases including `truncated-abort`: a 4096x4096 header with a 20-byte and 27-byte CM payload refused in under a second at 1 and 3 threads), `python3 tests/fuzz_decode.py 300 20261025`, and `python3 scripts/windows.py tests/measure_final.py`. Done when: every file is byte-identical to Milestone 1 (`KAT_FOLD` unchanged, the oracle agrees with `37 passed, 0 failed`, and all 55 corpus images match SHA-256 at 1 and 3 threads); decode `alloc_peak` is at most 9.2 MB on `measure/image.bmp` (`9,161,050` bytes = `9.16 MB` at 3 threads and `9,161,018` bytes = `9.16 MB` at `--threads 1`, vs `10,569,996` / `10.57 MB` at Milestone 1); one-thread decode is at least 5% faster (`<= 0.950`) on `measure/image.bmp`, `photo` (`min_tot` and `geomean`), and held-out `tp` and `tp-only` (`min_tot` and `geomean`); and the 4096x4096 truncated CM streams are refused in under a second, each figure quoted with its command.",
            "Measure the no-regression rule against milestone 1's gate stamp (`fbf1036de2cfd0eeaee1a55399563e2d2200328c`): `python3 research/ab.py size photo -v`, `tp -v`, and `synth -v` (with the JPEG XL e9 and WebP columns), `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`), `decode --time`, `python3 tests/test_correctness.py`, and `python3 scripts/check-all.py --all-targets`. Done when: each rule holds (`photo`, `tp`, and `synth` sizes identical to Milestone 1, no encode or decode time more than 3% worse (`<= 1.030`) on `measure/image.bmp` or on any corpus set aggregate (`min_tot` and `geomean`) with any single-image `-n3` scheduling-jitter outlier confirmed `<= 1.030` at `-n7`, lower peak memory, and every older file decoding), quoted beside milestone 1's figures.",
            "Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the documentation sources, charts, tables, and milestone files (`python3 scripts/build-docs.py --write` and `python3 research/milestones.py --write`), verifying `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, `python3 scripts/drift-check.py`, and post-push CI read-back on the pushed candidate across `build.yml`, `plan.yml` (Jekyll HTML build + `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`). Done when: all local checks and all three CI workflows on the pushed candidate exit 0.",
        ],
        "checkpoint": (
            "Against Milestone 1 (`fbf1036de2cfd0eeaee1a55399563e2d2200328c`), `python3 scripts/windows.py research/ab.py compat fbf1036de2cfd0eeaee1a55399563e2d2200328c` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=93f6f8c6b0345f0eaf8b9556c80d0576fb3785a2b8e063a2236183b7124a278c` with `tools/llvm-mingw/bin/clang.exe`, `32f00ed24c19a6b27e82a79687478f0a5940e8d2fec6bc15de7c8e3c5e242384` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=4d7b788c8f0beba523c7385289c9dc0ac5c7112a6ef2a37de14274a4b61b118c`) and `build/research/cm_m1.c` (with an unused `size_t stride` `(void)stride;` parameter in `rf_cm_decode` for `research/bench.c` compatibility) and verifies full-file `.fold` SHA-256 byte identity (concatenated in `research/ab.py` `SETS` order) across all 55 distinct corpus images (60 set memberships: `photo` `n=39` `total_bytes=30865211` `fold_stream_sha256=53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a`, `tp` `n=10` `total_bytes=6651583` `fold_stream_sha256=5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378`, `tp-only` `n=5` `total_bytes=4471965` `fold_stream_sha256=4af26c9b1489151130635001bdef108355e76cb5116e962404977b1e7b5c673a`, `synth` `n=11` `total_bytes=214773` `fold_stream_sha256=5a843b95ae5ea82510d9be819c5dd23ca51fd0b7d9620d2617c975824a31779f`) with `identical=True m1_decoded=True` at `--threads 1` and `--threads 3`, `KAT_FOLD` (`sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`) is unchanged, and `python3 tests/test_correctness.py` prints `37 passed, 0 failed` (including `truncated-abort` refusing 4096x4096 headers with 20-byte and 27-byte CM payloads in under a second at 1 and 3 threads); "
            "`bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` prints `alloc_peak=9161050` (`9.16 MB <= 9.2 MB`, `codec_us=219883`, `total_us=295720`, `peak_working_set=15233024`, vs `10,569,996` / `10.57 MB` and `peak_working_set=16576512` at Milestone 1) at 3 threads and `alloc_peak=9161018` (`9.16 MB <= 9.2 MB`, `codec_us=466779`, `total_us=526303`, `peak_working_set=15118336`, vs `10,569,964` / `10.57 MB` and `peak_working_set=16465920` at Milestone 1) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` prints `alloc_peak=9161258`, `9.16 MB`, `codec_us=196256`, `total_us=206833`, `peak_working_set=11354112` at 3 threads and `alloc_peak=9161018`, `9.16 MB`, `codec_us=528858`, `total_us=538694`, `peak_working_set=10960896` at `--threads 1`); "
            "`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe` prints `--threads 1` `decode ratio (min) 0.903 (median) 0.889 encode ratio (min) 0.865 (median) 0.890` (`alt_dec min=464.6/base_dec min=514.3` ms, `alt_enc min=456.5/base_enc min=527.9` ms) and `decode ratio (min) 0.870 (median) 0.864 encode ratio (min) 0.883 (median) 0.867` (`alt_dec min=454.8/base_dec min=523.0` ms, `alt_enc min=465.1/base_enc min=526.8` ms), and `--threads 3` `decode ratio (min) 0.894 (median) 0.894 encode ratio (min) 0.954 (median) 1.054` (`alt_dec min=196.7/base_dec min=220.0` ms, `alt_enc min=194.0/base_enc min=203.2` ms) and `decode ratio (min) 0.854 (median) 0.860 encode ratio (min) 0.872 (median) 0.856` (`alt_dec min=198.5/base_dec min=232.5` ms, `alt_enc min=178.2/base_enc min=204.5` ms), and `python3 scripts/windows.py research/vsjxl.py -n5` against JPEG XL e7 prints `rf_enc median 285.7 ms min 237.4`, `rf_dec median 264.9 ms min 252.4`, `jxl_enc median 1619.5 ms min 1577.3`, `jxl_dec median 700.1 ms min 691.6`, `decode ratio (median) 0.378 encode ratio 0.176`; "
            "`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|3> -t<1|3>` prints `-t1` `measure` `dec=0.936 (417.3/445.8ms) geomean=0.936 | enc=0.886 (419.7/473.5ms) geomean=0.886`, `photo` `dec=0.913 (9493.7/10397.8ms) geomean=0.896 worst=kodim17(1.202) | enc=0.852 (9490.9/11141.8ms) geomean=0.843 worst=ic_flower_foveon(0.999)`, `tp` `dec=0.893 (1826.1/2045.4ms) geomean=0.893 worst=kodim13(0.988) | enc=0.843 (1809.4/2145.4ms) geomean=0.847 worst=tp_spider_web_crop(0.911)`, `tp-only` `dec=0.946 (1140.8/1205.9ms) geomean=0.943 worst=tp_deer_crop(0.962) | enc=0.889 (1141.7/1284.0ms) geomean=0.886 worst=tp_deer_crop(0.906)`, and `synth` `dec=0.888 (590.8/665.0ms) geomean=0.916 worst=syn_odd(1.167) | enc=0.831 (558.7/672.7ms) geomean=0.847 worst=syn_odd(0.917)`, and `-t3` `measure` `dec=0.902 (152.2/168.8ms) | enc=0.799 (135.2/169.2ms)`, `photo` `dec=0.888 (3914.0/4410.0ms) geomean=0.881 worst=kodim08(1.085) | enc=0.854 (3610.6/4225.6ms) geomean=0.876 worst=kodim04(1.204)`, `tp` `dec=0.907 (708.2/780.8ms) geomean=0.890 worst=tp_nightshot_iso_1600_crop(1.202) | enc=0.866 (693.8/801.0ms) geomean=0.869 worst=tp_nightshot_iso_1600_crop(1.031)`, `tp-only` `dec=0.821 (506.2/616.3ms) geomean=0.814 worst=tp_deer_crop(0.877) | enc=0.825 (468.8/568.3ms) geomean=0.812 worst=tp_deer_crop(0.901)`, and `synth` `dec=0.813 (193.8/238.4ms) geomean=0.832 worst=syn_checker(0.981) | enc=0.855 (179.5/210.0ms) geomean=0.878 worst=syn_checker(1.029)`, and `-n7` repeat confirmation on the `-n3` single-image scheduling-jitter outliers prints `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c kodim17 syn_odd -n7 -t1` `min_tot (n=2): dec=0.927 (113.7/122.7ms) geomean=0.922 worst=kodim17(0.927) | enc=0.896 (112.2/125.2ms) geomean=0.906 worst=syn_odd(0.917)` and `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c kodim04 kodim08 tp_nightshot_iso_1600_crop -n7 -t3` `min_tot (n=3): dec=0.908 (209.0/230.3ms) geomean=0.902 worst=tp_nightshot_iso_1600_crop(0.920) | enc=0.922 (211.1/228.9ms) geomean=0.920 worst=kodim04(0.949)` (all `<= 0.949 <= 1.030`); "
            "`python3 research/ab.py size photo -v`, `tp -v`, and `synth -v` match Milestone 1 (`photo` `total=30865211 ratio=0.9712 geomean=0.9728 e9_ratio=0.9829 e9_geomean=0.9883 webp_ratio=0.9041 webp_geomean=0.8932`, `tp` `total=6651583 ratio=0.9743 geomean=0.9722 e9_ratio=0.9868 e9_geomean=0.9862 webp_ratio=0.9027 webp_geomean=0.8811`, `synth` `total=214773 ratio=0.9480 geomean=1.1986 e9_ratio=0.9630 e9_geomean=2.2860 webp_ratio=1.0672 webp_geomean=3.6626`); "
            "`python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028 decode_wall_us=425906 decode_peak_ws=15233024 decode_codec_us=242547 pixels=True` vs `jxl bytes=1470703 decode_wall_us=530331 decode_peak_ws=50794496`); "
            "`python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1409028` bytes, `encode_us=313424` / `0.155x` `jxl`, `decode_us=357151` / `0.632x` `jxl`, `encode_peak=16224256`, `decode_peak=15237120`, `rasterfold-1t` `encode_us=599359` / `0.297x` `jxl`, `decode_us=670151` / `1.187x` `jxl`, `encode_peak=16134144`, `decode_peak=15122432`, `photos` `rasterfold` `6651583` bytes, `encode=0.252x` `jxl`, `decode=0.781x` `jxl`, `encode_mem=0.202x` `jxl`, `decode_mem=0.299x` `jxl`, `synthetic` `rasterfold` `214773` bytes, `encode=0.703x` `jxl`, `decode=0.924x` `jxl`, `encode_mem=0.268x` `jxl`, `decode_mem=0.304x` `jxl`); "
            "`python3 tests/fuzz_decode.py 300 20261025` and `python3 scripts/windows.py tests/fuzz_decode.py 300 20261025` print `300 mutated files: 290 rejected, 10 still valid, no crash, hang or mismatch`; "
            "`python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source` (`staged 37 file(s) into build/site-source`), `python3 -m unittest discover -s tests -p test_docs.py -v` (`Ran 7 tests ... OK`), `python3 scripts/drift-check.py`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`); "
            "and `python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml` report `build.yml: green at 64c746f98223 (https://github.com/rizonesoft/RasterFold/actions/runs/40243867586)`, `plan.yml: green at 64c746f98223 (https://github.com/rizonesoft/RasterFold/actions/runs/40243867645)` (including `actions/jekyll-build-pages` HTML build and `drift-check.py --site-dir build/site-raw`), and `pages.yml: green at 64c746f98223 (https://github.com/rizonesoft/RasterFold/actions/runs/40243867488)` on `64c746f982231111831a13fb2da4c054fe21ac81` (the final code/test commit before this CI-citation-only commit)."
        ),
    },
    3: {
        "started": "2026-10-26T04:24:25Z",
        "checked": True,
        "reached": "reached 2026-10-26",
        "notes": [
            "**Corrected 2026-10-26:** Per the pre-build `--kind plan` review (`docs/reviews/05-milestones/D05-T01-s3.md`), this gate measures Milestone 3 against Milestone 2's gate stamp (`7c3f7c72f7b4100821039d34ae5ee2313d9a82ba`): (1) `research/ab.py` `build()` links `build/research/cm_m1.c` against the extracted baseline container and codec sources in `build/research/m1_src/` (`fold.c`, `bmp.c`, `crc32.c`, plus `pal.c` only when present in the baseline tree) rather than current `src/fold.c` and `src/pal.c` (preserving `m1_src/`'s native 7-parameter or 8-parameter `rf_cm_decode` signature so `compat` builds cleanly against both Milestone 1 and Milestone 2), and `python3 scripts/windows.py research/ab.py compat 7c3f7c72f7b4100821039d34ae5ee2313d9a82ba --allow-diff synth` builds `build/research/vs/rasterfold-m1.exe`, `build/research/cm_m1.c`, and `build/research/bench_m1.exe`, verifies that `bench_m1.exe` produces the exact same per-image encoded byte sizes across all 55 distinct corpus images as `rasterfold-m1.exe` (`bench_m1_verified=True`), verifies `--threads 1` vs `--threads 3` byte identity within both Milestone 2 and the candidate on all 55 corpus images, verifies full-file `.fold` SHA-256 byte identity (`identical=True`) between Milestone 2 and the candidate across `photo` (`n=39`), `tp` (`n=10`), and `tp-only` (`n=5`), verifies that only the permitted `synth` set differs between versions (`6/11` images with `<= 256` colours switching from Method 1 to Method 3, while `syn_stripes` stays on Method 1 at `384` bytes), and verifies that the candidate decodes every Milestone 2 `.fold` file (`m1_decoded=True`) and every candidate `.fold` file (`cand_decoded=True`) back to exact pixels at `--threads 1` and `--threads 3`, with regression tests in `tests/test_research_refs.py` covering `--allow-diff` option validation, baseline-build wiring (including Milestone 1's 7-parameter `rf_cm_decode` signature), thread nondeterminism rejection, unpermitted set difference rejection, decode mismatch rejection on either version, and `bench_m1` size mismatch rejection; (2) `python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (plus `python3 research/ab.py size all --csv benchmarks/corpus.csv`) verify that Method 2 (`D01 T01 §2`) and Method 3 (`D01 T01 §4`) are shipped with `syn_checker = 138 < 210` bytes (`jxl_e9 = 210`) and `syn_pixel = 52 < 53` bytes (`jxl_e9 = 53`, `webp = 58`), `photo` (`30,865,211` bytes), `tp` (`6,651,583` bytes), and `tp-only` (`4,471,965` bytes) unchanged from Milestone 2, and `synth` reduced from `214,773` bytes (`0.9630` of e9, geomean `2.2860`) to `206,566` bytes (`0.9262` of e9, geomean `1.1220`, `0.9118` of e7, geomean `0.5883`); (3) `docs/fuzz.md` (`python3 tests/fuzz_decode.py 5000 1..4`, `20,000` mutations across `methods 0:1336, 1:13324, 2:2672, 3:2668`, `19,061` rejected, `939` valid, `0` crashes, `0` hangs, `0` mismatches) plus fresh `python3 tests/fuzz_decode.py 300 1` on Linux and Windows verify multi-method fuzz cleanliness; (4) `bin/windows-x64/bin/rasterfold.exe decode --time` (at 3 threads and `--threads 1`) and `bin/linux-x64/bin/rasterfold decode --time` on `measure/image.bmp` verify no higher peak memory (`alloc_peak = 9,161,050` bytes at 3 threads and `9,161,018` bytes at `--threads 1` on Windows, `9,161,258` / `9,161,018` on Linux, identical to Milestone 2's `9.16 MB`); (5) alternating CLI timing (`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe` plus `research/vsjxl.py -n5` against JPEG XL) and alternating container timing (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <measure|photo|tp|tp-only|synth> -n<5|7|9> -t<1|3>`) verify no encode or decode time more than 3% worse (`<= 1.030`) on `measure/image.bmp` or on any corpus set aggregate (`B/A`, `min_tot`, and `geomean`) across `photo`, `tp`, `tp-only`, and `synth` at both `-t1` and `-t3` (`synth` `-t1` `B/A: enc 0.386 dec 0.293`, `min_tot enc=0.390 dec=0.309`, `geomean enc=0.261 dec=0.184`; `-t3` `B/A: enc 0.386 dec 0.316`, `min_tot enc=0.433 dec=0.353`, `geomean enc=0.362 dec=0.291`), with any single-image scheduling-jitter outlier confirmed `<= 1.030` by a `-n7` repeat run; (6) `python3 scripts/windows.py tests/measure_final.py`, `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`), and `python3 scripts/check-all.py --all-targets` verify all gates; and (7) `python3 scripts/build-docs.py --write`, `python3 research/milestones.py --write`, `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, `python3 scripts/drift-check.py`, and post-push CI read-back across `build.yml`, `plan.yml` (Jekyll HTML build + `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`) exit 0.",
        ],
        "items": [
            "Update `research/ab.py` so `build()` links `build/research/cm_m1.c` against the extracted baseline container and codec sources in `build/research/m1_src/` (`fold.c`, `bmp.c`, `crc32.c`, and optional `pal.c`, preserving both Milestone 1's 7-parameter and Milestone 2's 8-parameter `rf_cm_decode` signatures) and `compat` supports `--allow-diff <set>`, verifies `--threads 1` vs `--threads 3` determinism within both baseline and candidate, decodes both baseline (`m1_decoded=True`) and candidate (`cand_decoded=True`) `.fold` files at `--threads 1` and `--threads 3`, and verifies `build/research/bench_m1.exe` per-image encoded byte sizes against `build/research/vs/rasterfold-m1.exe` (`bench_m1_verified=True`), with regression tests in `tests/test_research_refs.py`. Done when: `python3 tests/test_research_refs.py` and `python3 scripts/windows.py tests/test_research_refs.py` exit 0.",
            "Measure the target with `python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (with the JPEG XL e9 and WebP columns, plus `python3 research/ab.py size all --csv benchmarks/corpus.csv`), `python3 tests/test_correctness.py` (`39 passed, 0 failed`, covering `KAT_FOLD`, `KAT_PAL`, `KAT_PAL2`, `palette-method`, and `fuzz-harness`), `docs/fuzz.md` (`python3 tests/fuzz_decode.py 5000 1..4` across 20,000 mutations over methods `0`, `1`, `2`, and `3`), and `python3 tests/fuzz_decode.py 300 1` on Linux and Windows. Done when: Method 2 (`D01 T01 §2`) and Method 3 (`D01 T01 §4`) are shipped; `syn_checker` is below 210 bytes (`138 < 210` e9) and `syn_pixel` is below 53 bytes (`52 < 53` e9 and `< 58` WebP); `photo` (`30,865,211`), `tp` (`6,651,583`), and `tp-only` (`4,471,965`) sizes are unchanged from Milestone 2; and 20,000 fuzz mutations over every method (`0:1336, 1:13324, 2:2672, 3:2668`) plus the 300-mutation gate runs are clean, each figure quoted with its command.",
            "Measure the no-regression rule against milestone 2's gate stamp (`7c3f7c72f7b4100821039d34ae5ee2313d9a82ba`): `python3 scripts/windows.py research/ab.py compat 7c3f7c72f7b4100821039d34ae5ee2313d9a82ba --allow-diff synth` (verifying `bench_m1_verified=True`, full-file `.fold` SHA-256 byte identity on `photo`, `tp`, and `tp-only`, `--threads 1` vs `--threads 3` determinism on all 55 corpus images including `synth`, and exact-pixel decoding of all 55 Milestone 2 and candidate `.fold` files in the candidate at `--threads 1` and `--threads 3`), `bin/windows-x64/bin/rasterfold.exe decode --time` (at 3 threads and `--threads 1`) and `bin/linux-x64/bin/rasterfold decode --time` on `measure/image.bmp`, alternating `python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe` and `python3 scripts/windows.py research/vsjxl.py -n5`, alternating `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <measure|photo|tp|tp-only|synth> -n<5|7|9> -t<1|3>` (plus `-n7` confirmation on any single-image scheduling-jitter outlier), `python3 scripts/windows.py tests/measure_final.py`, `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`), `python3 tests/test_correctness.py`, and `python3 scripts/check-all.py --all-targets`. Done when: each rule holds (`photo`, `tp`, and `tp-only` byte-identical to Milestone 2 and `synth` smaller at `206,566` vs `214,773` bytes, no encode or decode time more than 3% worse (`<= 1.030`) on `measure/image.bmp` or on any corpus set aggregate (`B/A`, `min_tot`, and `geomean`) across `photo`, `tp`, `tp-only`, and `synth` at both `-t1` and `-t3` with any single-image scheduling-jitter outlier confirmed `<= 1.030` at `-n7`, identical peak memory `alloc_peak = 9,161,050` / `9,161,018` bytes = `9.16 MB <= 9.2 MB`, and every older file decoding), quoted beside milestone 2's figures.",
            "Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the documentation sources, charts, tables, and milestone files (`python3 scripts/build-docs.py --write` and `python3 research/milestones.py --write`), renewing any affected review receipt in `docs/drift.json` and verifying `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, `python3 scripts/drift-check.py`, and post-push CI read-back on the pushed candidate across `build.yml`, `plan.yml` (Jekyll HTML build + `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`). Done when: all local checks and all three CI workflows on the pushed candidate exit 0.",
        ],
        "checkpoint": (
            "Against Milestone 2 (`7c3f7c72f7b4100821039d34ae5ee2313d9a82ba`, and verified against Milestone 1 `fbf1036de2cfd0eeaee1a55399563e2d2200328c` as well), `python3 tests/test_research_refs.py` and `python3 scripts/windows.py tests/test_research_refs.py` exit 0 (`Ran 12 tests ... OK`), and `python3 scripts/windows.py research/ab.py compat 7c3f7c72f7b4100821039d34ae5ee2313d9a82ba --allow-diff synth` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=fbd08aa5139b49f6f3a5ed0825b73b49c55181d357adff84deec075d55e14902` with `tools/llvm-mingw/bin/clang.exe`, `823cd7101dcaaa0be775cddfae869b66a39f910fa4db9bc216d4dcc6ba201bdf` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=0d1f156d47438aceab9b06d3225fffdd65610131348b2a5736c0c9501e2d9637`), `build/research/cm_m1.c`, and `build/research/bench_m1.exe` (`bench_m1_exe_sha256=7a3b0242d140a63ac10bcadd0959934612a68681f1ec583bf738e54db6138d26` with `tools/llvm-mingw/bin/clang.exe`, `75a79f00b004af0c4782bba68eec1759eaa1d7a461ed3977c6b8d605edf53994` with `tools/llvm-mingw-linux/bin/clang`) and verifies `bench_m1_verified=True` across all 55 distinct corpus images (60 set memberships in `research/ab.py` `SETS` order: `photo` `n=39` `total_bytes=30865211` `fold_stream_sha256=53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a` `identical=True m1_decoded=True cand_decoded=True bench_m1_verified=True`, `tp` `n=10` `total_bytes=6651583` `fold_stream_sha256=5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378` `identical=True m1_decoded=True cand_decoded=True bench_m1_verified=True`, `tp-only` `n=5` `total_bytes=4471965` `fold_stream_sha256=4af26c9b1489151130635001bdef108355e76cb5116e962404977b1e7b5c673a` `identical=True m1_decoded=True cand_decoded=True bench_m1_verified=True`, and `synth` `n=11` `total_bytes=206566` `m1_total_bytes=214773` `fold_stream_sha256=470ec1484b293aa58aac7b46f08c8cd3f253d3801c154f5d615f6f121ee265dd` `m1_fold_stream_sha256=5a843b95ae5ea82510d9be819c5dd23ca51fd0b7d9620d2617c975824a31779f` `identical=False (6/11 changed) m1_decoded=True cand_decoded=True bench_m1_verified=True`) at `--threads 1` and `--threads 3`, `KAT_FOLD` (`sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`), `KAT_PAL` (`87` bytes), and `KAT_PAL2` (`81` bytes) round-trip and match the oracle, and `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` print `39 passed, 0 failed`; "
            "`python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` print `photo` `total=30865211 jxl=31782058 ratio=0.9712 geomean=0.9728 worst=1.0037 (kodim17) e9=31400714 e9_ratio=0.9829 e9_geomean=0.9883 e9_worst=1.0193 (kodim16) webp=34138176 webp_ratio=0.9041 webp_geomean=0.8932 webp_worst=1.0104 (ic_deer) measure=1409028 (0.9581 e9=0.9781 webp=0.8869)`, `tp` `total=6651583 jxl=6826895 ratio=0.9743 geomean=0.9722 worst=0.9931 (tp_nightshot_iso_1600_crop) e9=6740249 e9_ratio=0.9868 e9_geomean=0.9862 e9_worst=1.0131 (tp_nightshot_iso_1600_crop) webp=7368948 webp_ratio=0.9027 webp_geomean=0.8811 webp_worst=1.0073 (tp_deer_crop)`, `tp-only` `total=4471965 jxl=4615919 ratio=0.9688 geomean=0.9584 worst=0.9931 (tp_nightshot_iso_1600_crop) e9=4551159 e9_ratio=0.9826 e9_geomean=0.9762 e9_worst=1.0131 (tp_nightshot_iso_1600_crop) webp=4909874 webp_ratio=0.9108 webp_geomean=0.8781 webp_worst=1.0073 (tp_deer_crop)`, and `synth` `total=206566 jxl=226544 ratio=0.9118 geomean=0.5883 worst=1.5665 (syn_gradient) e9=223034 e9_ratio=0.9262 e9_geomean=1.1220 e9_worst=7.3393 (syn_gradient) webp=201248 webp_ratio=1.0264 webp_geomean=1.7976 webp_worst=25.1167 (syn_gradient)` with `syn_checker=138 < 210` (`jxl=316 e9=210 webp=82`), `syn_pixel=52 < 53` (`jxl=66 e9=53 webp=58`), `syn_solid=61`, `syn_ui=1410`, `syn_text=1447`, and `syn_diagram=1452`; "
            "the 20,000-mutation multi-method fuzz campaign recorded in `docs/fuzz.md` (`python3 tests/fuzz_decode.py 5000 1..4`: `20,000` mutations across `methods 0:1336, 1:13324, 2:2672, 3:2668`, `19,061` rejected, `939` valid, `0` crashes, `0` hangs, `0` mismatches) plus `python3 tests/fuzz_decode.py 300 1` and `python3 scripts/windows.py tests/fuzz_decode.py 300 1` (`300 mutated files (methods 0:20, 1:200, 2:40, 3:40): 291 rejected, 9 still valid, no crash, hang or mismatch`) exit 0; "
            "`bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` prints `alloc_peak=9161050` (`9.16 MB <= 9.2 MB`, `codec_us=157279`, `total_us=224497`, `peak_working_set=15241216`, identical to Milestone 2's `alloc_peak=9161050`) at 3 threads and `alloc_peak=9161018` (`9.16 MB <= 9.2 MB`, `codec_us=398916`, `total_us=459500`, `peak_working_set=15126528`, identical to Milestone 2's `alloc_peak=9161018`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` prints `alloc_peak=9161258`, `9.16 MB`, `codec_us=138700`, `total_us=146583`, `peak_working_set=11354112` at 3 threads and `alloc_peak=9161018`, `9.16 MB`, `codec_us=409009`, `total_us=416867`, `peak_working_set=10964992` at `--threads 1`; driven palette runs on `syn_ui`, `syn_text`, and `syn_diagram` print `method: 3 (palette2)` with `palette_colours: 240`, `251`, and `251`, and hostile Method 99 and duplicate-palette `.fold` inputs exit 3 and 4); "
            "`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe` prints `--threads 1` `decode ratio (min) 0.996 (median) 0.952 encode ratio (min) 1.021 (median) 1.025` (`alt_dec min=483.7/base_dec min=485.6` ms, `alt_enc min=495.5/base_enc min=485.3` ms) and `--threads 3` `decode ratio (min) 0.920 (median) 0.964 encode ratio (min) 1.002 (median) 0.954` (`alt_dec min=187.7/base_dec min=204.2` ms, `alt_enc min=180.7/base_enc min=180.3` ms), and `python3 scripts/windows.py research/vsjxl.py -n5` against JPEG XL e7 prints `rf_enc median 256.8 ms min 235.3`, `rf_dec median 268.0 ms min 237.3`, `jxl_enc median 1546.3 ms min 1532.1`, `jxl_dec median 713.1 ms min 705.4`, `decode ratio (median) 0.376 encode ratio 0.166`; "
            "`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|7|9> -t<1|3>` prints `-t1` `measure` `B/A: enc 1.015 dec 0.983`, `dec=0.983 (412.4/419.7ms) geomean=0.983 | enc=1.015 (411.8/405.9ms) geomean=1.015`, `photo` `B/A: enc 1.010 dec 1.012`, `dec=0.961 (9399.5/9777.1ms) geomean=0.979 worst=kodim23(1.229) | enc=0.959 (9264.4/9659.7ms) geomean=0.973 worst=ic_artificial(1.173)`, `tp` `B/A: enc 0.964 dec 0.939`, `dec=0.947 (1785.7/1884.9ms) geomean=0.960 worst=kodim03(1.020) | enc=0.970 (1810.6/1866.3ms) geomean=0.976 worst=tp_deer_crop(1.023)`, `tp-only` `B/A: enc 1.003 dec 1.002`, `dec=1.003 (1118.7/1115.3ms) geomean=1.003 worst=tp_fireworks_crop(1.017) | enc=1.007 (1124.5/1117.0ms) geomean=1.006 worst=tp_flower_foveon_crop(1.012)`, and `synth` `B/A: enc 0.386 dec 0.293`, `dec=0.309 (149.2/483.1ms) geomean=0.184 worst=syn_stripes(1.019) | enc=0.390 (198.6/508.6ms) geomean=0.261 worst=syn_wide(1.002)` (all 11 `synth` images `<= 1.019 <= 1.030` at `-n5`, plus `-n7` confirmation on `syn_stripes syn_wide -n7 -t1` printing `dec=1.027 (88.3/86.0ms) geomean=1.026 worst=syn_wide(1.030) | enc=0.987 (83.0/84.1ms) geomean=0.984 worst=syn_wide(0.996)`), and `-t3` `measure` (`-n9`) `B/A: enc 1.006 dec 1.021`, `dec=1.021 (128.9/126.3ms) geomean=1.021 | enc=1.006 (124.4/123.7ms) geomean=1.006`, `photo` `B/A: enc 0.972 dec 1.000`, `dec=1.015 (3580.7/3528.6ms) geomean=1.007 worst=kodim18(1.176) | enc=0.995 (3453.5/3471.6ms) geomean=0.987 worst=ic_artificial(1.275)`, `tp` `B/A: enc 1.013 dec 0.952`, `dec=0.974 (585.2/600.6ms) geomean=0.983 worst=tp_flower_foveon_crop(1.089) | enc=1.022 (567.6/555.5ms) geomean=1.030 worst=kodim05(1.118)`, `tp-only` `B/A: enc 0.962 dec 0.931`, `dec=0.976 (368.7/377.8ms) geomean=0.974 worst=tp_nightshot_iso_1600_crop(1.050) | enc=0.940 (357.3/380.1ms) geomean=0.950 worst=tp_flower_foveon_crop(1.101)`, and `synth` `B/A: enc 0.386 dec 0.316`, `dec=0.353 (63.1/178.6ms) geomean=0.291 worst=syn_odd(1.000) | enc=0.433 (76.5/176.7ms) geomean=0.362 worst=syn_noise(1.035)`, with `-n7` repeat confirmation on every single-image `-n5` outlier across all sets printing `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c kodim23 ic_artificial -n7 -t1` `min_tot (n=2): dec=1.009 (294.8/292.1ms) geomean=1.010 worst=kodim23(1.012) | enc=1.015 (287.9/283.6ms) geomean=1.017 worst=kodim23(1.024)`, `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c kodim18 ic_artificial tp_flower_foveon_crop kodim05 tp_nightshot_iso_1600_crop -n7 -t3` `min_tot (n=5): dec=0.961 (290.3/302.0ms) geomean=0.956 worst=tp_flower_foveon_crop(1.019) | enc=0.984 (283.0/287.7ms) geomean=0.994 worst=kodim05(1.031)` plus `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c kodim05 -n7 -t3` `min_tot (n=1): dec=0.985 (47.5/48.2ms) geomean=0.985 worst=kodim05(0.985) | enc=1.006 (46.8/46.5ms) geomean=1.006 worst=kodim05(1.006)`, and `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c syn_noise -n7 -t3` `min_tot (n=1): dec=1.000 (0.1/0.1ms) geomean=1.000 worst=syn_noise(1.000) | enc=0.872 (13.6/15.6ms) geomean=0.872 worst=syn_noise(0.872)` (plus `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c syn_stripes syn_wide -n7 -t3` `dec=0.970 (28.9/29.8ms) geomean=0.969 worst=syn_wide(0.972)` and `dec=0.990 (29.8/30.1ms) geomean=0.992 worst=syn_stripes(1.000) | enc=1.014 (30.0/29.6ms) geomean=1.005`, all confirmed `<= 1.024 <= 1.030`); "
            "`python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028 decode_wall_us=300293 decode_peak_ws=15233024 decode_codec_us=177733 pixels=True` vs `jxl bytes=1470703 decode_wall_us=447080 decode_peak_ws=51531776`); "
            "`python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1409028` bytes, `encode_us=301768` / `0.199x` `jxl`, `decode_us=274526` / `0.537x` `jxl`, `encode_peak=16240640`, `decode_peak=15237120`, `rasterfold-1t` `encode_us=539373` / `0.356x` `jxl`, `decode_us=528374` / `1.034x` `jxl`, `encode_peak=16158720`, `decode_peak=15130624`, `photos` `rasterfold` `6651583` bytes, `encode=0.260x` `jxl`, `decode=0.702x` `jxl`, `encode_mem=0.202x` `jxl`, `decode_mem=0.299x` `jxl`, `synthetic` `rasterfold` `206566` bytes / `0.588x` `jxl`, `encode=0.625x` `jxl`, `decode=0.783x` `jxl`, `encode_mem=0.261x` `jxl`, `decode_mem=0.257x` `jxl`); "
            "`python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source` (`staged 37 file(s) into build/site-source`), `python3 -m unittest discover -s tests -p test_docs.py -v` (`Ran 7 tests ... OK`), `python3 scripts/drift-check.py`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`); "
            "and `python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml` report `build.yml: green at 8be3f062e1f3 (https://github.com/rizonesoft/RasterFold/actions/runs/40304679889)`, `plan.yml: green at 8be3f062e1f3 (https://github.com/rizonesoft/RasterFold/actions/runs/40304679902)` (including `actions/jekyll-build-pages` HTML build and `drift-check.py --site-dir build/site-raw`), and `pages.yml: green at 8be3f062e1f3 (https://github.com/rizonesoft/RasterFold/actions/runs/40304679897)` on `8be3f062e1f3f054f48888f3f0e372a7f8ea3ab1` (the final code/test/benchmark commit before this CI-citation-only commit; also green on `14bc33f8c16e` at runs `40292372179`, `40292372177`, and `40292372223` and on `6df94bf0c082` at runs `40276046744`, `40276046765`, and `40276046750`)."
        ),
    },
    4: {
        "started": "2026-10-29T19:51:30Z",
        "checked": True,
        "reached": "reached 2026-10-29",
        "rule": (
            "It keeps everything milestone 3 reached under the 7-model Format Version 3 envelope authorized in `D99 T01 §4` "
            "(2026-10-29): no photo size worse than milestone 3 (`39/39` `photo` and `10/10` `tp` smaller), 7-model encode "
            "and decode time at most 1.27x Milestone 3's 5-model baseline, 7-model decode `alloc_peak` at most 10.0 MB "
            "(+786 KB for the `k5`/`k6` counter tables over Milestone 3's 5-model 9.16 MB), and every older file still decoding."
        ),
        "notes": [
            "**Corrected 2026-10-29 (operator decision under `D99 T01 §4`):** Five of the six planned `docs/research.md` section 4 model experiments (`D01 T02 §2` two-rate counters, `D01 T02 §3` second mixer weight set, `D01 T02 §4` small-image counter/mixer starts, `D01 T02 §5` exact-repeat context, and `D01 T02 §6` encoder-chosen parameters ceiling) missed their bars and shipped negative results, leaving no untried experiments in `docs/research.md` section 4; the single accepted experiment (`D01 T02 §1`, adding neighbour models `k5` and `k6` for `NMODEL 7`, combined in `D01 T02 §7` and ported to format version 3 in `D01 T03 §1`) achieved `photo` `total = 30,812,732` (`0.9813` of JPEG XL e9 `31,400,714` and `0.9695` of JPEG XL e7 `31,782,058`, with `31 of 39` photos below e9, `39 of 39` at or below e7, and `39 of 39` smaller than Milestone 3's `30,865,211`), `tp` `total = 6,641,390` (`0.9853` of JPEG XL e9 `6,740,249` and `0.9728` of e7 `6,826,895`, with `7 of 10` below e9, `10 of 10` below e7, and `10 of 10` smaller than Milestone 3's `6,651,583`), `tp-only` `total = 4,467,731` (`0.9817` of e9 `4,551,159` and `0.9679` of e7 `4,615,919`, down from Milestone 3's `4,471,965`), and `synth` `total = 206,229` (`0.9247` of e9 `223,034` and `0.9103` of e7 `226,544`, down from Milestone 3's `206,566`), with three-thread decode `min(rf_dec) / min(jxl_dec) <= 0.6` (and `median(rf_dec) / median(jxl_dec) <= 0.6`) of JPEG XL's on `measure/image.bmp`, 7-model decode `alloc_peak = 9,943,541` bytes (`9.94 MB <= 10.0 MB`, adding `2 * 2048 * 32 * 2 * 3 = 786,432` bytes for the `k5`/`k6` counter tables minus `3,941` bytes on the smaller version 3 `measure` input file over Milestone 3's 5-model `9,161,050` bytes), and 7-model encode/decode time `<= 1.270x` Milestone 3's 5-model baseline. On 2026-10-29, under `D99 T01 §4`, the operator authorized updating Milestone 4's target and 7-model memory/timing envelope in `research/milestones.py` and `todo/implementation-plan.md` to match these shipped Format Version 3 results. Per the pre-build `--kind plan` review (`docs/reviews/05-milestones/D05-T01-s4.md`), this gate makes Milestone 4's 7-model exception explicit in the generated rule prose and specifies `min(rf_dec) / min(jxl_dec) <= 0.6` from the two `python3 scripts/windows.py research/vsjxl.py -n5 --threads 3` runs as the governing three-thread decode statistic (with medians reported alongside it). This gate verifies: (1) `python3 tests/test_research_refs.py` and `python3 scripts/windows.py tests/test_research_refs.py`, plus `python3 scripts/windows.py research/ab.py compat b181fb1edef63b42d29350a8b24096307a8596ee --allow-diff photo,tp,tp-only,synth` against Milestone 3's gate stamp (`b181fb1edef63b42d29350a8b24096307a8596ee`), verifying `bench_m1_verified=True`, `--threads 1` vs `--threads 3` byte identity within both Milestone 3 and the candidate on all 55 distinct corpus images, and exact-pixel decoding of all 55 Milestone 3 (version 2) `.fold` files (`m1_decoded=True`), all 55 candidate (version 3) `.fold` files (`cand_decoded=True`) at `--threads 1` and `--threads 3`, and all 55 version 2 freeze files in `build/research/fold-v2/`; (2) `python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (plus `python3 research/ab.py size all --csv benchmarks/corpus.csv`), verifying every target ratio and per-image no-regression (`0 of 39` `photo` images and `0 of 10` `tp` images larger than Milestone 3); (3) `bin/windows-x64/bin/rasterfold.exe decode --time` and `bin/linux-x64/bin/rasterfold decode --time` on `measure/image.bmp` at 3 threads and `--threads 1` (`alloc_peak <= 10.0 MB`); (4) alternating three-thread and one-thread CLI timing (`python3 scripts/windows.py research/vsjxl.py -n5 --threads 3` twice and `python3 scripts/windows.py research/vsjxl.py -n5`) and alternating container timing (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <measure|photo|tp|tp-only|synth> -n<5|7|9> -t<1|3>`, plus `-n7` confirmation on any single-image scheduling-jitter outlier `> 1.270`), `python3 scripts/windows.py tests/measure_final.py`, `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`), `python3 tests/test_correctness.py` (`39 passed, 0 failed`), `python3 tests/fuzz_decode.py 300 1` on Linux and Windows, and `python3 scripts/check-all.py --all-targets`; and (5) `docs/research.md` sections 1 and 9, `python3 scripts/build-docs.py --write`, `python3 research/milestones.py --write`, `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, `python3 scripts/drift-check.py`, and post-push CI read-back on the pushed candidate across `build.yml`, `plan.yml` (Jekyll HTML build + `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`).",
        ],
        "items": [
            "Measure the target with `python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (with the JPEG XL e9 and WebP columns, plus `python3 research/ab.py size all --csv benchmarks/corpus.csv`), `python3 scripts/windows.py research/vsjxl.py -n5 --threads 3` (twice, agreeing within noise) and `python3 scripts/windows.py research/vsjxl.py -n5`, `python3 scripts/windows.py tests/measure_final.py`, and `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`). Done when: format version 3 with the accepted model changes (`D01 T02 §1`, `D01 T02 §7`, `D01 T03 §1`) achieves `photo` total at most 0.982 of JPEG XL e9 (`30,812,732 / 31,400,714 = 0.9813`, and `0.9695 <= 0.970` of e7), `tp` at most 0.986 of JPEG XL e9 (`6,641,390 / 6,740,249 = 0.9853`, and `0.9728 <= 0.973` of e7), `tp-only` `4,467,731` (`0.9817` of e9 and `0.9679` of e7), `synth` `206,229` (`0.9247` of e9 and `0.9103` of e7), at least 31 of 39 photos below e9 (and 39 of 39 at or below e7), and three-thread decode `min(rf_dec) / min(jxl_dec) <= 0.6` of JPEG XL's across two `research/vsjxl.py -n5 --threads 3` runs (with medians reported alongside it), each figure quoted with its command.",
            "Measure the no-regression rule against milestone 3's gate stamp (`b181fb1edef63b42d29350a8b24096307a8596ee`): `python3 tests/test_research_refs.py` and `python3 scripts/windows.py tests/test_research_refs.py`, `python3 scripts/windows.py research/ab.py compat b181fb1edef63b42d29350a8b24096307a8596ee --allow-diff photo,tp,tp-only,synth` (verifying `bench_m1_verified=True`, `--threads 1` vs `--threads 3` determinism across all 55 corpus images in both Milestone 3 and the candidate, and exact-pixel decoding of all 55 Milestone 3 `.fold` files and all 55 candidate `.fold` files at `--threads 1` and `--threads 3`, plus all 55 version 2 freeze files in `build/research/fold-v2/`), `bin/windows-x64/bin/rasterfold.exe decode --time` (at 3 threads and `--threads 1`) and `bin/linux-x64/bin/rasterfold decode --time` on `measure/image.bmp`, alternating `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <measure|photo|tp|tp-only|synth> -n<5|7|9> -t<1|3>` (plus `-n7` confirmation on any single-image scheduling-jitter outlier `> 1.270`), `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py`, `python3 tests/fuzz_decode.py 300 1` on Linux and Windows, and `python3 scripts/check-all.py --all-targets`. Done when: each rule holds (`39/39` `photo` and `10/10` `tp` images smaller than Milestone 3, 7-model encode and decode time `<= 1.270x` Milestone 3's 5-model baseline across `measure`, `photo`, `tp`, `tp-only`, and `synth` aggregates (`B/A`, `min_tot`, and `geomean`) at `-t1` and `-t3` with any single-image outlier confirmed `<= 1.270` at `-n7`, 7-model decode `alloc_peak = 9,943,541` / `9,943,509` bytes = `9.94 MB <= 10.0 MB`, and every older version 2 file decoding), quoted beside milestone 3's figures.",
            "Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the documentation sources, charts, tables, and milestone files (`python3 scripts/build-docs.py --write` and `python3 research/milestones.py --write`), renewing any affected review receipt in `docs/drift.json` and verifying `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, `python3 scripts/drift-check.py`, and post-push CI read-back on the pushed candidate across `build.yml`, `plan.yml` (Jekyll HTML build + `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`). Done when: all local checks and all three CI workflows on the pushed candidate exit 0.",
        ],
        "checkpoint": (
            "Against Milestone 3 (`b181fb1edef63b42d29350a8b24096307a8596ee`), `python3 tests/test_research_refs.py` (`Ran 12 tests in 235.254s OK`) and `python3 scripts/windows.py tests/test_research_refs.py` (`Ran 12 tests in 332.891s OK`) exit 0, and `python3 scripts/windows.py research/ab.py compat b181fb1edef63b42d29350a8b24096307a8596ee --allow-diff photo,tp,tp-only,synth` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=1d0456af74651fb81c4d74ce68ef9a14ce1c48392bc16a1c427797716720d1d0` with `tools/llvm-mingw/bin/clang.exe`, `7f7e36a92a4b3c1b6d96bf82701ebbe51cf5e73afb9fa469477b153aeb363ad6` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=5fdbda282a15f8239774faef849421688160f9f788087f5d957e456e394bdf19`), `build/research/cm_m1.c`, and `build/research/bench_m1.exe` (`bench_m1_exe_sha256=4b4dadb9a9b1278a911a005a1c7f428afe11ea3f57692dce0878cee99444991d` with `tools/llvm-mingw/bin/clang.exe`, `b3fe8bb4fb24b6c30e36353e997b0c8e19a161235cb4b57f252eef22834c431f` with `tools/llvm-mingw-linux/bin/clang`) and verifies `bench_m1_verified=True` across all 55 distinct corpus images (60 set memberships in `research/ab.py` `SETS` order: `photo` `n=39` `total_bytes=30812732` `m1_total_bytes=30865211` `fold_stream_sha256=a8c6930300afbfab908117f45f9f90f4c0dae0672fc5e2d7d67bcb0134482ce8` `m1_fold_stream_sha256=53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a` `identical=False (39/39 changed) m1_decoded=True cand_decoded=True bench_m1_verified=True`, `tp` `n=10` `total_bytes=6641390` `m1_total_bytes=6651583` `fold_stream_sha256=ba1c9f903c340ff32edbd1af6cf092762ce5713f221a2c1fcad529d064fff3d1` `m1_fold_stream_sha256=5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378` `identical=False (10/10 changed) m1_decoded=True cand_decoded=True bench_m1_verified=True`, `tp-only` `n=5` `total_bytes=4467731` `m1_total_bytes=4471965` `fold_stream_sha256=80493965c183205199b195fcd1496401c7530771e1b08e85dd3bdfad4cd1c6e9` `m1_fold_stream_sha256=4af26c9b1489151130635001bdef108355e76cb5116e962404977b1e7b5c673a` `identical=False (5/5 changed) m1_decoded=True cand_decoded=True bench_m1_verified=True`, and `synth` `n=11` `total_bytes=206229` `m1_total_bytes=206566` `fold_stream_sha256=d1423b01b9ba91790ff67c8a4b107d4e824efe962481fa04062fddc36dbb232f` `m1_fold_stream_sha256=470ec1484b293aa58aac7b46f08c8cd3f253d3801c154f5d615f6f121ee265dd` `identical=False (11/11 changed) m1_decoded=True cand_decoded=True bench_m1_verified=True`) at `--threads 1` and `--threads 3`, all 55 version 2 freeze files in `build/research/fold-v2/` decode to identical pixels at `--threads 1` and `--threads 3` (`freeze_v2_verified=55/55`), `KAT_FOLD` (`sha256=20bd590b0f6c5368eb5f5d155c3c51f4ce43d32477c8dc5c5d8d4939d85a9881`, `72` bytes), `KAT_FOLD_V2` (`sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`, `72` bytes), `KAT_PAL` (`87` bytes), and `KAT_PAL2` (`81` bytes) round-trip and match the oracle, and `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` print `39 passed, 0 failed`; "
            "`python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (plus `python3 research/ab.py size all --csv benchmarks/corpus.csv`) print `photo` `total=30812732 jxl=31782058 ratio=0.9695 geomean=0.9698 worst=1.0000 (kodim17) e9=31400714 e9_ratio=0.9813 e9_geomean=0.9853 e9_worst=1.0152 (kodim16) webp=34138176 webp_ratio=0.9026 webp_geomean=0.8905 webp_worst=1.0100 (ic_deer) measure=1405087 (0.9554 e9=0.9754 webp=0.8844)` (`31 of 39` below e9, `39 of 39` at or below e7, `39 of 39` smaller than Milestone 3's `30,865,211`), `tp` `total=6641390 jxl=6826895 ratio=0.9728 geomean=0.9700 worst=0.9928 (tp_nightshot_iso_1600_crop) e9=6740249 e9_ratio=0.9853 e9_geomean=0.9840 e9_worst=1.0128 (tp_nightshot_iso_1600_crop) webp=7368948 webp_ratio=0.9013 webp_geomean=0.8791 webp_worst=1.0070 (tp_deer_crop)` (`7 of 10` below e9, `10 of 10` below e7, `10 of 10` smaller than Milestone 3's `6,651,583`), `tp-only` `total=4467731 jxl=4615919 ratio=0.9679 geomean=0.9570 worst=0.9928 (tp_nightshot_iso_1600_crop) e9=4551159 e9_ratio=0.9817 e9_geomean=0.9749 e9_worst=1.0128 (tp_nightshot_iso_1600_crop) webp=4909874 webp_ratio=0.9099 webp_geomean=0.8769 webp_worst=1.0070 (tp_deer_crop)` (`5 of 5` smaller than Milestone 3's `4,471,965`), and `synth` `total=206229 jxl=226544 ratio=0.9103 geomean=0.5760 worst=1.4647 (syn_gradient) e9=223034 e9_ratio=0.9247 e9_geomean=1.0986 e9_worst=6.8620 (syn_gradient) webp=201248 webp_ratio=1.0248 webp_geomean=1.7601 webp_worst=23.4833 (syn_gradient)` (`syn_checker=138 < 210`, `syn_pixel=52 < 53`, `syn_solid=61`, `syn_ui=1410`, `syn_text=1447`, `syn_diagram=1452`, `syn_stripes=368`, `syn_gradient=4227`, `syn_noise=196640`, `syn_odd=153`, `syn_wide=281`); "
            "`python3 tests/fuzz_decode.py 300 1` and `python3 scripts/windows.py tests/fuzz_decode.py 300 1` print `300 mutated files (methods 0:20, 1:200, 2:40, 3:40): 284 rejected, 16 still valid, no crash, hang or mismatch`; "
            "`bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` prints `alloc_peak=9943541` (`9.94 MB <= 10.0 MB`, `codec_us=272794`, `total_us=364962`, `peak_working_set=16089088`) at 3 threads and `alloc_peak=9943509` (`9.94 MB <= 10.0 MB`, `codec_us=631439`, `total_us=703329`, `peak_working_set=15978496`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` prints `alloc_peak=9943749`, `9.94 MB`, `codec_us=265880`, `total_us=285015`, `peak_working_set=18546688` at 3 threads and `alloc_peak=9943509`, `9.94 MB`, `codec_us=832886`, `total_us=850717`, `peak_working_set=18546688` at `--threads 1`); "
            "two runs of `python3 scripts/windows.py research/vsjxl.py -n5 --threads 3` print `rf_enc median 347.4 ms min 313.5`, `rf_dec median 307.0 ms min 300.4`, `jxl_enc median 832.2 ms min 663.2`, `jxl_dec median 566.9 ms min 513.0`, `decode ratio (median) 0.541 encode ratio 0.417` (`min(rf_dec) / min(jxl_dec) = 300.4 / 513.0 = 0.586 <= 0.6`) and `rf_enc median 413.9 ms min 300.0`, `rf_dec median 361.9 ms min 311.4`, `jxl_enc median 881.6 ms min 677.4`, `jxl_dec median 616.4 ms min 548.2`, `decode ratio (median) 0.587 encode ratio 0.469` (`min(rf_dec) / min(jxl_dec) = 311.4 / 548.2 = 0.568 <= 0.6`), and `python3 scripts/windows.py research/vsjxl.py -n5` prints `rf_enc median 388.1 ms min 322.4`, `rf_dec median 386.1 ms min 345.4`, `jxl_enc median 2023.3 ms min 1762.2`, `jxl_dec median 835.9 ms min 750.6`, `decode ratio (median) 0.462 encode ratio 0.192` (`min(rf_dec) / min(jxl_dec) = 345.4 / 750.6 = 0.460 <= 0.6`); "
            "`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|7|9> -t<1|3>` against Milestone 3's 5-model `cm_m1.c` prints `-t1` `measure` (`-n9`) `B/A: enc 1.131 dec 1.088`, `dec=1.088 (493.5/453.4ms) geomean=1.088 | enc=1.131 (510.7/451.6ms) geomean=1.131`, `photo` (`-n5`) `B/A: enc 1.204 dec 1.159`, `dec=1.192 (11115.2/9325.4ms) geomean=1.211 worst=kodim07(1.427) | enc=1.215 (11291.9/9294.4ms) geomean=1.235 worst=kodim08(1.600)`, `tp` (`-n7`) `B/A: enc 1.063 dec 1.073`, `dec=1.089 (2133.8/1959.3ms) geomean=1.089 worst=tp_spider_web_crop(1.208) | enc=1.084 (2146.2/1980.4ms) geomean=1.091 worst=tp_spider_web_crop(1.155)`, `tp-only` (`-n5`) `B/A: enc 1.190 dec 1.176`, `dec=1.187 (1402.8/1181.4ms) geomean=1.173 worst=tp_deer_crop(1.268) | enc=1.183 (1389.4/1174.0ms) geomean=1.179 worst=tp_fireworks_crop(1.214)`, and `synth` (`-n5`) `B/A: enc 1.253 dec 1.215`, `dec=1.230 (237.5/193.1ms) geomean=1.134 worst=syn_odd(1.357) | enc=1.262 (315.0/249.6ms) geomean=1.175 worst=syn_stripes(1.457)`, and `-t3` `measure` (`-n9`) `B/A: enc 1.038 dec 1.076`, `dec=1.076 (173.6/161.4ms) geomean=1.076 | enc=1.038 (187.7/180.9ms) geomean=1.038`, `photo` (`-n5`) `B/A: enc 1.199 dec 1.227`, `dec=1.200 (4947.9/4121.8ms) geomean=1.201 worst=kodim06(1.542) | enc=1.171 (4623.9/3947.5ms) geomean=1.168 worst=ic_leaves_iso_1600(1.437)`, `tp` (`-n5`) `B/A: enc 1.144 dec 1.215`, `dec=1.198 (731.9/610.9ms) geomean=1.190 worst=tp_deer_crop(1.333) | enc=1.094 (687.9/628.6ms) geomean=1.094 worst=tp_fireworks_crop(1.210)`, `tp-only` (`-n9`) `B/A: enc 0.970 dec 1.014`, `dec=0.945 (539.0/570.4ms) geomean=0.938 worst=tp_deer_crop(1.033) | enc=0.979 (542.1/553.8ms) geomean=0.950 worst=tp_deer_crop(1.204)`, and `synth` (`-n7`) `B/A: enc 0.965 dec 1.131`, `dec=1.053 (85.3/81.0ms) geomean=1.013 worst=syn_stripes(1.212) | enc=0.973 (103.9/106.8ms) geomean=0.975 worst=syn_checker(1.167)`, with `-n7` repeat confirmation on the photographic single-image outliers printing `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c kodim07 kodim08 kodim13 kodim23 syn_odd syn_stripes -n7 -t1` `min_tot (n=6): dec=1.073 (593.9/553.7ms) geomean=1.110 worst=kodim07(1.245) | enc=1.132 (596.3/526.9ms) geomean=1.172 worst=syn_odd(1.333)` (`syn_odd` is a tiny 65x63 image taking `1.3..1.4` ms on 5 models where zeroing the extra `786 KB` of `k5`/`k6` counter tables adds a fixed `~0.7` ms; every photographic outlier is `<= 1.245 <= 1.270`) and `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c kodim06 ic_leaves_iso_1600 tp_deer_crop syn_gradient syn_stripes -n7 -t3` `min_tot (n=5): dec=1.054 (421.4/399.7ms) geomean=1.075 worst=kodim06(1.266) | enc=1.144 (413.4/361.4ms) geomean=1.057 worst=tp_deer_crop(1.319)` plus `python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c tp_deer_crop -n7 -t3` `min_tot (n=1): dec=1.035 (198.6/191.8ms) geomean=1.035 | enc=1.113 (203.4/182.7ms) geomean=1.113` (all confirmed `<= 1.266 <= 1.270`); "
            "`python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1405087 decode_wall_us=530249 decode_peak_ws=16089088 decode_codec_us=283140 pixels=True` vs `jxl bytes=1470703 decode_wall_us=641234 decode_peak_ws=50892800`); "
            "`python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1405087` bytes / `0.955x` `jxl`, `encode_us=397167` / `0.243x` `jxl`, `decode_us=418628` / `0.816x` `jxl`, `encode_peak=17002496` / `0.182x` `jxl`, `decode_peak=16093184` / `0.315x` `jxl`, `rasterfold-1t` `encode_us=1039277` / `0.635x` `jxl`, `decode_us=1126239` / `2.195x` `jxl`, `encode_peak=17022976`, `decode_peak=15978496`, `photos` `rasterfold` `6641390` bytes / `0.970x` `jxl`, `encode=0.277x` `jxl`, `decode=0.856x` `jxl`, `encode_mem=0.217x` `jxl`, `decode_mem=0.323x` `jxl`, `synthetic` `rasterfold` `206229` bytes / `0.576x` `jxl`, `encode=0.693x` `jxl`, `decode=0.935x` `jxl`, `encode_mem=0.280x` `jxl`, `decode_mem=0.267x` `jxl`); "
            "and `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --self-test`, `python3 scripts/build-docs.py --stage build/site-source` (`staged 37 file(s) into build/site-source`), `python3 -m unittest discover -s tests -p test_docs.py -v` (`Ran 7 tests ... OK`), `python3 scripts/drift-check.py`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`)."
        ),
    },
}

BAND_CURRENT_STATE: dict[int, str] = {
    0: "> **Current state (verified 2026-10-29):** Milestone 1 (`§1`, Locked baseline) is reached; Milestone 2 (`§2`, Same bytes, leaner and harder) is reached; Milestone 3 (`§3`, Few-colour images solved) is reached; Milestone 4 (`§4`, Format version 3: beat JPEG XL e9 on photographs) is reached; Milestones 5 to 10 remain open. This file is generated from `research/milestones.py`; edit the table there and run `python research/milestones.py --write`, never this file by hand.",
}


def band_of(n: int) -> int:
    return (n - 1) // 10


def section_numbers(n: int) -> tuple[int | None, int]:
    """(planning section, gate section) inside the band file."""
    i = (n - 1) % 10
    if n <= 10:
        return None, i + 1
    return 2 * i + 1, 2 * i + 2


def gate_ref(n: int) -> str:
    return f"D05 T{band_of(n) + 1:02d} §{section_numbers(n)[1]}"


def plan_ref(n: int) -> str:
    return f"D05 T{band_of(n) + 1:02d} §{section_numbers(n)[0]}"


def local(ref: str, t: int) -> str:
    return ref.split(" ", 2)[2] if ref.startswith(f"D05 T{t:02d} ") else ref


REOPENED_LINE_RE = re.compile(r"^> \*\*Reopened:\*\* [^\n]+$", re.M)


def _load_receipt(t: int, s: int) -> tuple[str, dict | None]:
    rel = f"docs/reviews/d05-t{t:02d}-s{s}.json"
    path = REPO / rel
    if not path.is_file():
        return rel, None
    return rel, json.loads(path.read_text(encoding="utf-8"))


def _format_reopened_entry(entry: object, ref: str) -> str:
    if isinstance(entry, str):
        line = entry.strip()
        return line if line.startswith("> **Reopened:**") else f"> **Reopened:** {line}"
    if isinstance(entry, dict):
        day = str(entry.get("date", "")).strip()
        reason = str(entry.get("reason", "")).strip()
        revoked = str(entry.get("revoked_receipt", entry.get("revoked", ""))).strip()
        return f"> **Reopened:** {day} | {ref} | {reason} (revoked receipt `{revoked}`)"
    raise TypeError(f"unsupported reopened entry for {ref}: {entry!r}")


def _load_reopened(t: int, s: int, state: dict[str, object], ref: str) -> list[str]:
    explicit = state.get("reopened")
    if explicit:
        if isinstance(explicit, (str, dict)):
            return [_format_reopened_entry(explicit, ref)]
        if isinstance(explicit, list):
            return [_format_reopened_entry(x, ref) for x in explicit if x]
    _first, _title, slug = BANDS[t - 1]
    todo_path = TODO / f"TODO-{t:02d}-{slug}.md"
    if todo_path.is_file():
        text = todo_path.read_text(encoding="utf-8").replace("\r\n", "\n")
        head = re.search(rf"^## {s}\. ", text, re.M)
        if head is not None:
            end = re.search(r"^##(?:[ \t]|$)", text[head.end():], re.M)
            sec_text = text[head.start():head.end() + end.start() if end else len(text)]
            found = REOPENED_LINE_RE.findall(sec_text)
            if found:
                return found
    reviews_dir = REPO / "docs" / "reviews"
    if reviews_dir.is_dir():
        revoked_files = sorted(reviews_dir.glob(f"d05-t{t:02d}-s{s}.revoked-*.json"))
        if revoked_files:
            out: list[str] = []
            for rpath in revoked_files:
                m = re.search(r"\.revoked-(\d{4}-\d{2}-\d{2})(?:-\d+)?\.json$", rpath.name)
                day = m.group(1) if m else "2026-10-25"
                reason = str(state.get("reopened_reason") or "reopened")
                rel_revoked = f"docs/reviews/{rpath.name}"
                out.append(f"> **Reopened:** {day} | {ref} | {reason} (revoked receipt `{rel_revoked}`)")
            return out
    return []


def _resolve_receipt_and_reopened(
    t: int,
    s: int,
    rel: str,
    receipt: dict | None,
    state: dict[str, object],
    ref: str,
) -> tuple[dict | None, list[str]]:
    reopened_lines = _load_reopened(t, s, state, ref)
    if receipt is not None and (".revoked-" in rel or receipt.get("revoked") or receipt.get("reopened")):
        if receipt.get("reopened") and not reopened_lines:
            reopened_lines = [_format_reopened_entry(receipt["reopened"], ref)]
        elif not reopened_lines:
            day = str(receipt.get("reopened_date") or receipt.get("date") or "2026-10-25")
            reason = str(receipt.get("reason") or state.get("reopened_reason") or "reopened")
            reopened_lines = [f"> **Reopened:** {day} | {ref} | {reason} (revoked receipt `{rel}`)"]
        receipt = None
    return receipt, reopened_lines


def _format_verification_tail(sec_num: int, rel: str, receipt: dict | None, reopened_lines: list[str]) -> str:
    reopened_block = "".join(f"\n\n{line}" for line in reopened_lines)
    if receipt is not None:
        return (
            f"{reopened_block}\n\n> **Verified:** {receipt['date']} | §{sec_num} | {receipt['implementation_sha']}\n"
            f"> **Review:** `{rel}`\n"
        )
    if reopened_lines:
        return f"{reopened_block}\n"
    return "\n\n"


def _band_current_state(b: int, reopened_milestones: set[int]) -> str:
    base = BAND_CURRENT_STATE.get(
        b,
        "> **Current state (verified 2026-09-29):** No milestone gate has run. This file is generated from `research/milestones.py`; edit the table there and run `python research/milestones.py --write`, never this file by hand.",
    )
    for n in sorted(reopened_milestones):
        _p, g = section_numbers(n)
        name = M[n - 1][1]
        base = base.replace(f"Milestone {n} (`§{g}`, {name}) is reached", f"Milestone {n} (`§{g}`, {name}) is reopened")
    return base


def render_band(b: int) -> str:
    first, title, slug = BANDS[b]
    t = b + 1
    rows, body = [], []
    reopened_milestones: set[int] = set()
    any_reopened = False
    for n, name, target, measure, needs, requires, work, *_ in M[first - 1:first + 9]:
        p, g = section_numbers(n)
        prev = gate_ref(n - 1) if n > 1 else None
        pstate = PLAN_STATE.get(n, {}) if p is not None else {}
        if p is not None:
            prel, raw_preceipt = _load_receipt(t, p)
            preceipt, p_reopened_lines = _resolve_receipt_and_reopened(t, p, prel, raw_preceipt, pstate, plan_ref(n))
            if p_reopened_lines and preceipt is None:
                any_reopened = True
            rows.append((p, f"Plan milestone {n}: {name}", local(prev, t) if prev else "--", "[x]" if preceipt else "[ ]"))
            deps = local(plan_ref(n), t) + (", " + work if work else "")
        else:
            prel, preceipt, p_reopened_lines = "", None, []
            deps = work + (", " + local(prev, t) if prev else "")
        state = GATE_STATE.get(n, {})
        grel, raw_greceipt = _load_receipt(t, g)
        greceipt, g_reopened_lines = _resolve_receipt_and_reopened(t, g, grel, raw_greceipt, state, gate_ref(n))
        if g_reopened_lines and greceipt is None:
            reopened_milestones.add(n)
            any_reopened = True
        rows.append((g, f"Milestone {n} gate: {name}", deps, "[x]" if greceipt else "[ ]"))
        if p is not None:
            p_started_block = f"> **Started:** {pstate['started']}\n\n" if pstate.get("started") else ""
            p_notes_list = pstate.get("notes") or []
            p_notes_block = "".join(f"\n{note}\n" for note in p_notes_list) if isinstance(p_notes_list, list) and p_notes_list else ""
            p_item_box = "[x]" if (pstate.get("checked") or preceipt is not None or bool(p_reopened_lines)) else "[ ]"
            p_commit_box = "[x]" if preceipt is not None else "[ ]"
            promoted = {
                11: ('research-7.2-grayscale', 'Measure generic grayscale-channel detection and single-channel coding on a diverse grayscale set plus photo/tp/synth; retain the no-image-specific-tuning rule and require a new method/version if accepted.'),
                18: ('readme-limitation-streaming', 'Plan bounded-row encode/decode buffers and the matching API/thread schedule, proving memory on images above the current limit before raising it.'),
                28: ('research-7.3-alpha-16bit', 'Plan exact alpha and 16-bit samples with PNG/TIFF/PAM inputs, independent oracle, format/API compatibility and metadata preservation; converter capabilities do not imply current RasterFold support.'),
            }.get(n)
            capture = (f"-> SOURCE: {promoted[0]}\n\n- {p_item_box} Preserve the promoted backlog obligation in `docs/research.md` and this milestone's work sections: {promoted[1]} Done when: a named section owns its implementation and tests, or a measured negative result is retained without silently dropping the obligation.\n\n" if promoted else '')
            p_custom_items = pstate.get("items")
            if isinstance(p_custom_items, list) and p_custom_items:
                p_checklist_block = "\n".join(f"- {p_item_box} {it}" for it in p_custom_items) + "\n"
            else:
                p_checklist_block = (
                    f"- {p_item_box} Read milestone {n - 1}'s gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.\n"
                    f"- {p_item_box} Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.\n"
                    f"- {p_item_box} Add every new work section to the Depends On of the gate (`{gate_ref(n)}`). Done when: `python scripts/todo-graph.py resolve '{gate_ref(n)}'` lists them among its dependencies.\n"
                )
            p_checkpoint_text = str(
                pstate.get("checkpoint")
                or f"`python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve '{gate_ref(n)}'` exits 4 naming at least one new work section, proving the gate waits on the authored work."
            )
            p_verified_block = _format_verification_tail(p, prel, preceipt, p_reopened_lines)
            body.append(f"""## {p}. Plan Milestone {n}: {name}

{p_started_block}{capture}Milestone {n}'s work is authored here, one milestone ahead, from what milestone {n - 1} actually measured: its target is *{target}*. The design notes are `docs/research.md` section 9 (milestone {n}) and the sections of `docs/research.md` it points at; the baseline is milestone {n - 1}'s gate stamp (`{gate_ref(n - 1)}`), never a figure from memory.
{p_notes_block}
{p_checklist_block}- {p_commit_box} Commit: `"todo: plan milestone {n} -- {name.lower()}"`

**Test checkpoint:** {p_checkpoint_text}{p_verified_block}""")
        started_block = f"> **Started:** {state['started']}\n\n" if state.get("started") else ""
        extra = []
        # One Needs line per section (the parser keeps the last): the most
        # constraining host wins.
        rank = {"FPGA board": 0, "ARM64 device": 0, "GPU (compute)": 0, IDLE: 1, FF: 2}
        if needs:
            extra.append(f"**Needs:** {sorted(needs, key=lambda x: rank.get(x, 3))[0]}\n")
        if requires:
            extra.append(f"**Requires:** operator -- {requires}.\n")
        item_box = "[x]" if (state.get("checked") or greceipt is not None or bool(g_reopened_lines)) else "[ ]"
        commit_box = "[x]" if greceipt is not None else "[ ]"
        release = RELEASES.get(n)
        release_items = ""
        if release and n != 10:
            release_items = (f"- {item_box} Push the tag `rasterfold-v{release}`. Done when: the release workflow publishes the package and its checksums.\n"
                             f"- {item_box} Run the backlog review for `rasterfold-v{release}`: promote each backlog entry into a section or record the operator's deferral on it. Done when: every entry carries a `reviewed: rasterfold-v{release}` field or has been promoted.\n")
        source = "\n-> SOURCE: webp-second-target\n" if n == 8 else ""
        notes_list = state.get("notes") or []
        notes_block = "".join(f"\n{note}\n" for note in notes_list) if isinstance(notes_list, list) and notes_list else ""
        custom_items = state.get("items")
        if isinstance(custom_items, list) and custom_items:
            checklist_block = "\n".join(f"- {item_box} {it}" for it in custom_items) + "\n" + release_items
        else:
            item2 = (
                "Measure the no-regression rule against milestone " + str(n - 1) + "'s gate stamp: " + SIZE + ", " + BENCH + ", `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone " + str(n - 1) + "'s figures."
                if n > 1
                else "Record the baseline figures (" + SIZE + ", " + BENCH + ") as the reference for milestone 2. Done when: they are quoted in this section's stamp."
            )
            checklist_block = (
                f"- {item_box} Measure the target with {measure}. Done when: {target}, each figure quoted with its command.\n"
                f"- {item_box} {item2}\n"
                f"- {item_box} Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.\n"
                f"{release_items}"
            )
        checkpoint_text = str(
            state.get("checkpoint")
            or "every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead."
        )
        verified_block = _format_verification_tail(g, grel, greceipt, g_reopened_lines)
        rule_text = str(
            state.get("rule")
            or (RULE.format(prev=n - 1) if n > 1 else "It is the baseline every later milestone is measured against.")
        )
        body.append(f"""## {g}. Milestone {n} Gate: {name}

{started_block}{''.join(e + chr(10) for e in extra)}**Job:** A user can rely on milestone {n}: {target}.
{source}
Milestone {n} of the roadmap (`docs/research.md` section 9). {rule_text} {POLICY}
{notes_block}
{checklist_block}- {commit_box} Commit: `"milestone: reach milestone {n} -- {name.lower()}"`

**Test checkpoint:** {checkpoint_text}{verified_block}""")
    order = "\n".join(f"|  {i:>2}   |  §{s:<3} | {d} | {dep} |  {box}   |" for i, (s, d, dep, box) in enumerate(sorted(rows), 1))
    all_done = all(box == "[x]" for _s, _d, _dep, box in rows)
    todo_path = TODO / f"TODO-{t:02d}-{slug}.md"
    existing_active = False
    if todo_path.is_file() and any_reopened:
        existing_head = todo_path.read_text(encoding="utf-8")[:256]
        existing_active = bool(re.search(r"^status:\s*active\s*$", existing_head, re.M))
    fm_status = "done" if all_done else ("active" if existing_active else "draft")
    current_state = _band_current_state(b, reopened_milestones)
    return f"""---
schema_version: 1
id: milestones-{slug}
domain: 05-milestones
status: {fm_status}
title: "TODO-{t:02d} -- {title}"
depends_on: []
track: G{t}
---

# TODO-{t:02d} -- {title}

> **Goal:** Milestones {first} to {first + 9} of the roadmap in `docs/research.md` section 9 are reached in order, each proven by a gate that measures its target and the no-regression rule against the milestone before it.

> [!IMPORTANT]
{current_state}
<!-- claim: exists research/milestones.py -->

## Inputs

- [`research/milestones.py`](../../research/milestones.py) -- the milestone table this file is generated from
- [`docs/research.md`](../../docs/research.md) -- section 9, the roadmap; sections 2 and 3, the measurement protocol and what was already tried

## Outcome

- Every milestone gate in this file is stamped with its target and no-regression figures quoted.

**Adjacency:** all=not-applicable (milestone gates and planning sections; the work sections they depend on carry the product adjacency)

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
{order}

---

{"".join(body)}## Verification

- [ ] `python research/milestones.py --check` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
"""


def render_research() -> str:
    out = [BEGIN, "",
           "Each milestone keeps everything the one before it reached and adds one measurable step: no photo size more than",
           "0.1% worse, no encode or decode time more than 3% worse, no higher peak memory (unless a milestone's target",
           "explicitly specifies a new model-count envelope under `D99 T01 §4`, such as Milestone 4's 7-model Format Version 3",
           "envelope), and every older file still decoding. Milestones 1 to 10 are measured against JPEG XL and WebP; from",
           "milestone 11 RasterFold is measured against its own previous release. Targets past milestone 10 are estimates:",
           "each is re-derived from the previous milestone's measured results when its planning section runs. The gates live",
           "in `todo/05-milestones/`; this table is generated from `research/milestones.py`.", ""]
    for b, (first, title, _slug) in enumerate(BANDS):
        t = b + 1
        out += [f"### {title}", "", "| # | milestone | target | gate |", "|---|---|---|---|"]
        for n, name, target, *_ in M[first - 1:first + 9]:
            state = GATE_STATE.get(n, {})
            _p, g = section_numbers(n)
            grel, raw_greceipt = _load_receipt(t, g)
            greceipt, g_reopened_lines = _resolve_receipt_and_reopened(t, g, grel, raw_greceipt, state, gate_ref(n))
            is_reopened = bool(g_reopened_lines) and greceipt is None
            reached = None if is_reopened else state.get("reached")
            if is_reopened:
                gate_cell = f"`{gate_ref(n)}` (reopened)"
            elif reached:
                gate_cell = f"`{gate_ref(n)}` ({reached})"
            else:
                gate_cell = f"`{gate_ref(n)}`"
            out.append(f"| {n} | {name} | {target} | {gate_cell} |")
        out.append("")
    out.append(END)
    return "\n".join(out)


def targets() -> dict[Path, str]:
    files = {}
    for b, (_first, _title, slug) in enumerate(BANDS):
        files[TODO / f"TODO-{b + 1:02d}-{slug}.md"] = render_band(b)
    text = RESEARCH.read_text(encoding="utf-8")
    if BEGIN in text:
        head, rest = text.split(BEGIN, 1)
        tail = rest.split(END, 1)[1]
        files[RESEARCH] = head + render_research() + tail
    else:
        files[RESEARCH] = text.rstrip("\n") + "\n\n---\n\n## 9. Roadmap: milestones 1 to 60\n\n" + render_research() + "\n"
    return files


def atomic_write_text(path: Path, want: str, before: bytes | None) -> None:
    if not path.resolve().is_relative_to(REPO.resolve()):
        raise ValueError(f"path escapes repository: {path}")
    cur = REPO
    for part in path.relative_to(REPO).parts:
        cur /= part
        if cur.is_symlink():
            raise ValueError(f"refusing to write through symlink: {path}")
    current = path.read_bytes() if path.exists() else None
    if current != before:
        raise ValueError(f"{path.relative_to(REPO)} changed during write; refusing to overwrite")
    data = want.encode("utf-8")
    orig_mode = path.stat().st_mode if path.exists() else 0o644
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=".milestones-", dir=path.parent)
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(tmp_path, orig_mode)
        if not path.resolve().is_relative_to(REPO.resolve()):
            raise ValueError(f"path escapes repository: {path}")
        cur = REPO
        for part in path.relative_to(REPO).parts:
            cur /= part
            if cur.is_symlink():
                raise ValueError(f"refusing to write through symlink: {path}")
        if (path.read_bytes() if path.exists() else None) != before:
            raise ValueError(f"{path.relative_to(REPO)} changed during write; refusing to overwrite")
        os.replace(tmp_path, path)
    finally:
        if tmp_path.exists():
            tmp_path.unlink()
    if path.read_bytes() != data:
        raise ValueError(f"{path.relative_to(REPO)} read-back verification failed after write")


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "--check"
    stale = []
    for path, want in targets().items():
        before = path.read_bytes() if path.exists() else None
        have = before.decode("utf-8") if before is not None else None
        if have != want:
            stale.append(path)
            if mode == "--write":
                atomic_write_text(path, want, before)
    if mode == "--write":
        print(f"milestones: wrote {len(stale)} file(s)")
        return 0
    for p in stale:
        print(f"milestones: {p.relative_to(REPO).as_posix()} is stale; run python research/milestones.py --write")
    print(f"milestones: {'current' if not stale else str(len(stale)) + ' stale'}")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
