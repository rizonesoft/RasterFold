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
     "format version 3 with the accepted model changes: photo total at most 0.975 of JPEG XL e9, test pack at most 0.980, at least 35 of 39 photos below e9, three-thread decode at most 0.6 of JPEG XL's",
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
}

BAND_CURRENT_STATE: dict[int, str] = {
    0: "> **Current state (verified 2026-10-25):** Milestone 1 (`§1`, Locked baseline) is reached; Milestone 2 (`§2`, Same bytes, leaner and harder) is reached; Milestones 3 to 10 remain open. This file is generated from `research/milestones.py`; edit the table there and run `python research/milestones.py --write`, never this file by hand.",
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
        body.append(f"""## {g}. Milestone {n} Gate: {name}

{started_block}{''.join(e + chr(10) for e in extra)}**Job:** A user can rely on milestone {n}: {target}.
{source}
Milestone {n} of the roadmap (`docs/research.md` section 9). {RULE.format(prev=n - 1) if n > 1 else "It is the baseline every later milestone is measured against."} {POLICY}
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
           "0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still",
           "decoding. Milestones 1 to 10 are measured against JPEG XL and WebP; from milestone 11 RasterFold is measured",
           "against its own previous release. Targets past milestone 10 are estimates: each is re-derived from the previous",
           "milestone's measured results when its planning section runs. The gates live in `todo/05-milestones/`; this",
           "table is generated from `research/milestones.py`.", ""]
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
