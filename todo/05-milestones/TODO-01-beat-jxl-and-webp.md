---
schema_version: 1
id: milestones-beat-jxl-and-webp
domain: 05-milestones
status: draft
title: "TODO-01 -- Milestones 1 to 10: Beat JPEG XL e9 and WebP, then Release 1.0"
depends_on: []
track: G1
---

# TODO-01 -- Milestones 1 to 10: Beat JPEG XL e9 and WebP, then Release 1.0

> **Goal:** Milestones 1 to 10 of the roadmap in `docs/research.md` section 9 are reached in order, each proven by a gate that measures its target and the no-regression rule against the milestone before it.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** Milestone 1 (`§1`, Locked baseline) is reached; Milestones 2 to 10 remain open. This file is generated from `research/milestones.py`; edit the table there and run `python research/milestones.py --write`, never this file by hand.
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
|   1   |  §1   | Milestone 1 gate: Locked baseline | D00 T01 §1, D00 T01 §2, D00 T01 §3, D00 T01 §4, D00 T01 §5, D00 T02 §1, D00 T02 §2, D00 T02 §3, D08 T01 §1, D08 T01 §2, D08 T01 §3, D08 T02 §1, D08 T02 §2, D08 T02 §3, D08 T02 §4, D08 T02 §5, D08 T02 §6, D08 T02 §7, D00 T04 §1, D00 T04 §2, D00 T04 §3, D00 T04 §4, D08 T04 §1, D99 T01 §3, D99 T01 §4 |  [ ]   |
|   2   |  §2   | Milestone 2 gate: Same bytes, leaner and harder | D03 T01 §1, D03 T01 §2, D03 T01 §3, D04 T01 §1, D02 T01 §1, D02 T01 §2, §1 |  [ ]   |
|   3   |  §3   | Milestone 3 gate: Few-colour images solved | D01 T01 §1, D01 T01 §2, D01 T01 §3, D04 T01 §2, §2 |  [ ]   |
|   4   |  §4   | Milestone 4 gate: Format version 3: beat JPEG XL e9 on photographs | D01 T02 §1, D01 T02 §2, D01 T02 §3, D01 T02 §4, D01 T02 §5, D01 T02 §6, D01 T02 §7, D01 T03 §1, §3 |  [ ]   |
|   5   |  §5   | Milestone 5 gate: Exact repeats | D01 T04 §1, D01 T04 §2, D01 T04 §3, §4 |  [ ]   |
|   6   |  §6   | Milestone 6 gate: Faster than WebP to decode | D02 T02 §1, D01 T03 §2, §5 |  [ ]   |
|   7   |  §7   | Milestone 7 gate: Every photograph beats JPEG XL e9 | D01 T05 §1, D01 T05 §2, §6 |  [ ]   |
|   8   |  §8   | Milestone 8 gate: Beat WebP on generated images | D01 T06 §1, D01 T06 §2, D01 T06 §3, §7 |  [ ]   |
|   9   |  §9   | Milestone 9 gate: Holds on images never tuned on | D00 T03 §1, D00 T03 §2, §8 |  [ ]   |
|  10   |  §10  | Milestone 10 gate: RasterFold 1.0 | D06 T01 §1, D06 T01 §2, D06 T01 §3, D06 T01 §4, D06 T01 §5, D07 T01 §1, D07 T01 §2, D07 T01 §3, D07 T01 §4, D08 T03 §1, D08 T03 §2, D09 T01 §1, D09 T01 §2, D09 T01 §3, D09 T01 §4, §9 |  [ ]   |

---

## 1. Milestone 1 Gate: Locked baseline

> **Started:** 2026-10-25T12:41:00Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 1: `pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9.

Milestone 1 of the roadmap (`docs/research.md` section 9). It is the baseline every later milestone is measured against. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

**Corrected 2026-10-25:** Per the pre-build `--kind plan` review (`docs/reviews/05-milestones/D05-T01-s1.md`), this gate also verifies post-push CI read-back on the completed candidate SHA across `build.yml`, `plan.yml` (including its Jekyll HTML build and `--site-dir` check), and `pages.yml` (`python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml`), runs `python3 scripts/windows.py tests/measure_final.py` (`PASS` on `measure/image.bmp`, required by `docs/research.md` section 2.1), and records a fresh `decode --time` allocator peak baseline on `measure/image.bmp` (`bin/windows-x64/bin/rasterfold.exe` `alloc_peak=10569996` at 3 threads and `alloc_peak=10569964` at `--threads 1`; `bin/linux-x64/bin/rasterfold` `alloc_peak=10570204` at 3 threads; `10.57 MB`) together with `research/vsjxl.py -n5` and `research/ab.py ab src/cm.c src/cm.c measure -n5` at `-t1` and `-t3` for Milestone 2.

- [x] Measure the target with `pwsh scripts/check-all.ps1 -AllTargets`, `python3 scripts/windows.py tests/measure_final.py`, `python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml` on the completed candidate push, and `python3 research/ab.py size photo -v`, `tp -v`, and `synth -v` (with the JPEG XL e9 and WebP columns). Done when: `pwsh scripts/check-all.ps1 -AllTargets` is green with every gate `PASS` (`check-all: 29/29 passed`); `tests/measure_final.py` exits 0 (`PASS`); `build.yml`, `plan.yml`, and `pages.yml` are green on the pushed candidate; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python3 scripts/drift-check.py` exits 0; and the corpus matches the 2026-09-29 baseline (`photo`: `0.9712` of JPEG XL e7 and `0.9829` of e9, `28 of 39` photos below e9), each figure quoted with its command.
- [x] Record the baseline figures (`python3 research/ab.py size photo -v`, `tp -v`, and `synth -v` with the JPEG XL e9 and WebP columns; `tests/benchmark.py measure -n 3` and `benchmarks/results.csv`; `bin/windows-x64/bin/rasterfold.exe decode --time` at 3 threads and `--threads 1` plus `bin/linux-x64/bin/rasterfold decode --time` on `measure/image.bmp`; `python3 scripts/windows.py research/vsjxl.py -n5`; and `python3 research/ab.py ab src/cm.c src/cm.c measure -n5` at `-t1` and `-t3`) as the reference for milestone 2. Done when: they are recorded in `docs/research.md` section 1 and quoted in this section's checkpoint and stamp.
- [x] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the documentation sources and tables (`python3 scripts/build-docs.py --write` and `python3 research/milestones.py --write`), verifying HTML generation and `python3 scripts/drift-check.py`. Done when: `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, and `python3 scripts/drift-check.py` exit 0.
- [ ] Commit: `"milestone: reach milestone 1 -- locked baseline"`

**Test checkpoint:** `pwsh scripts/check-all.ps1 -AllTargets` exits 0 (`check-all: 29/29 passed`); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `rasterfold bytes=1409028 decode_wall_us=295854 decode_peak_ws=16576512 decode_codec_us=183840 pixels=True` vs `jxl bytes=1470703 decode_wall_us=459807 decode_peak_ws=51281920`); `python3 research/ab.py size photo -v` prints `total=30865211 jxl=31782058 ratio=0.9712 geomean=0.9728 worst=1.0037 (kodim17) e9=31400714 e9_ratio=0.9829 e9_geomean=0.9883 e9_worst=1.0193 (kodim16) webp=34138176 webp_ratio=0.9041 webp_geomean=0.8932 webp_worst=1.0104 (ic_deer) measure=1409028 (0.9581 e9=0.9781 webp=0.8869)` with `28 of 39` photos below e9; `python3 research/ab.py size tp -v` prints `total=6651583 jxl=6826895 ratio=0.9743 geomean=0.9722 worst=0.9931 (tp_nightshot_iso_1600_crop) e9=6740249 e9_ratio=0.9868 e9_geomean=0.9862 e9_worst=1.0131 (tp_nightshot_iso_1600_crop) webp=7368948 webp_ratio=0.9027 webp_geomean=0.8811 webp_worst=1.0073 (tp_deer_crop)`; `python3 research/ab.py size synth -v` prints `total=214773 jxl=226544 ratio=0.9480 geomean=1.1986 worst=18.0665 (syn_checker) e9=223034 e9_ratio=0.9630 e9_geomean=2.2860 e9_worst=27.1857 (syn_checker) webp=201248 webp_ratio=1.0672 webp_geomean=3.6626 webp_worst=69.6220 (syn_checker)`; `tests/benchmark.py measure -n 3` exits 0 and is committed in `benchmarks/results.csv` and `benchmarks/results.md` (`failures: 0`, `rasterfold` `1409028` bytes, `encode_us=308760` / `0.194x` `jxl`, `decode_us=319315` / `0.647x` `jxl`, `encode_peak=16699392`, `decode_peak=16580608`, `rasterfold-1t` `encode_us=640604` / `0.402x` `jxl`, `decode_us=601611` / `1.220x` `jxl`, `encode_peak=16625664`, `decode_peak=16461824`, `jxl` `1470703` bytes, `encode_us=1595197`, `decode_us=493267`, `jxl-1t` `decode_us=721246`, `jxl-e9` `1440564` bytes, `encode_us=10492083`, `decode_us=491123`, `webp` `1588708` bytes, `encode_us=23548198`, `decode_us=219895`; `benchmarks/corpus.csv` verified); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` prints `alloc_peak=10569996` (`10.57 MB`, `codec_us=183820`, `total_us=240767`, `peak_working_set=16576512`) at 3 threads and `alloc_peak=10569964` (`10.57 MB`, `codec_us=488047`, `total_us=557786`, `peak_working_set=16465920`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` prints `alloc_peak=10570204`, `10.57 MB`, `codec_us=196505`, `total_us=205643`); `python3 scripts/windows.py research/vsjxl.py -n5` prints `rf_enc median 292.8 ms min 271.6`, `rf_dec median 287.7 ms min 284.0`, `jxl_enc median 1582.9 ms min 1549.6`, `jxl_dec median 480.8 ms min 465.2`, `decode ratio (median) 0.598 encode ratio 0.185`; `python3 research/ab.py ab src/cm.c src/cm.c measure -n5` prints `-t1` `enc min=482.7..484.1 med=496.4..499.6 dec min=453.5..467.2 med=481.7..498.2` and `-t3` `enc min=152.2..168.6 med=177.9..187.6 dec min=167.3..177.9 med=185.2..193.2`; `python3 research/milestones.py --check`, `python3 scripts/build-docs.py --check`, and `python3 scripts/drift-check.py` exit 0; and `python3 scripts/ci_status.py` and `python3 scripts/ci_status.py --workflow pages.yml` report `build.yml: green at 35eb5c4d1176 (https://github.com/rizonesoft/RasterFold/actions/runs/40197473962)`, `plan.yml: green at 35eb5c4d1176 (https://github.com/rizonesoft/RasterFold/actions/runs/40197473971)`, and `pages.yml: green at 35eb5c4d1176 (https://github.com/rizonesoft/RasterFold/actions/runs/40197473951)` on `35eb5c4d11766d01026604bc788ab4663d9de073` (and `91b43d439f0c19b4a42d0ac9fe62a63d4f125ba4` at runs `40195950978`, `40195950906`, `40195950947`).

## 2. Milestone 2 Gate: Same bytes, leaner and harder

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 2: every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second.

Milestone 2 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 1 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `bin/windows-x64/bin/rasterfold.exe decode --time`, `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`, `python tests/test_correctness.py`. Done when: every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 1's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 1's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 2 -- same bytes, leaner and harder"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 3. Milestone 3 Gate: Few-colour images solved

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 3: method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean.

Milestone 3 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 2 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 2's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 2's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 3 -- few-colour images solved"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 4. Milestone 4 Gate: Format version 3: beat JPEG XL e9 on photographs

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 4: format version 3 with the accepted model changes: photo total at most 0.975 of JPEG XL e9, test pack at most 0.980, at least 35 of 39 photos below e9, three-thread decode at most 0.6 of JPEG XL's.

Milestone 4 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 3 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`. Done when: format version 3 with the accepted model changes: photo total at most 0.975 of JPEG XL e9, test pack at most 0.980, at least 35 of 39 photos below e9, three-thread decode at most 0.6 of JPEG XL's, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 3's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 3's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 4 -- format version 3: beat jpeg xl e9 on photographs"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 5. Milestone 5 Gate: Exact repeats

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 5: a match model in version 3: `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` below JPEG XL e9 (616, 271, 2195, and 2038 bytes); photos no worse than milestone 4.

Milestone 5 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 4 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: a match model in version 3: `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` below JPEG XL e9 (616, 271, 2195, and 2038 bytes); photos no worse than milestone 4, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 4's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 4's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 5 -- exact repeats"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 6. Milestone 6 Gate: Faster than WebP to decode

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 6: horizontal strips in version 3: photo decode wall time below lossless WebP's on the 12-thread development machine; strip size cost at most 0.3%.

Milestone 6 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 5 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python tests/benchmark.py -n 3`, `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: horizontal strips in version 3: photo decode wall time below lossless WebP's on the 12-thread development machine; strip size cost at most 0.3%, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 5's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 5's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 6 -- faster than webp to decode"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 7. Milestone 7 Gate: Every photograph beats JPEG XL e9

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 7: 39 of 39 photos and 10 of 10 test-pack photos below JPEG XL e9; encode at most 0.3 of JPEG XL e7's time.

Milestone 7 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 6 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`. Done when: 39 of 39 photos and 10 of 10 test-pack photos below JPEG XL e9; encode at most 0.3 of JPEG XL e7's time, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 6's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 6's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 7 -- every photograph beats jpeg xl e9"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 8. Milestone 8 Gate: Beat WebP on generated images

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 8: the generated set's total and geometric mean below lossless WebP's, and every generated image within 1.1 times WebP's size.

-> SOURCE: webp-second-target

Milestone 8 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 7 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: the generated set's total and geometric mean below lossless WebP's, and every generated image within 1.1 times WebP's size, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 7's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 7's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 8 -- beat webp on generated images"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 9. Milestone 9 Gate: Holds on images never tuned on

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 9: milestones 4, 7, and 8 hold on the held-out set (`python research/ab.py size heldout`) without any retuning.

Milestone 9 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 8 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size heldout` against JPEG XL e9 and WebP. Done when: milestones 4, 7, and 8 hold on the held-out set (`python research/ab.py size heldout`) without any retuning, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 8's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 8's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 9 -- holds on images never tuned on"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 10. Milestone 10 Gate: RasterFold 1.0

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 10: `rasterfold-v1.0.0` released: format version 3 frozen; Windows/Linux x64 and Android ARM64/x64 CLI, shared/static C libraries and the complete native converter; BMP, PNG, and PPM in the CLI; the full HTML docs published; 100,000 fuzz mutations clean; smaller than JPEG XL e9 and lossless WebP on every set, and faster and leaner than JPEG XL on encode, decode, and memory.

Milestone 10 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 9 reached: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `python tests/fuzz_decode.py`. Done when: `rasterfold-v1.0.0` released: format version 3 frozen; Windows/Linux x64 and Android ARM64/x64 CLI, shared/static C libraries and the complete native converter; BMP, PNG, and PPM in the CLI; the full HTML docs published; 100,000 fuzz mutations clean; smaller than JPEG XL e9 and lossless WebP on every set, and faster and leaner than JPEG XL on encode, decode, and memory, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 9's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 9's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 10 -- rasterfold 1.0"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## Verification

- [ ] `python research/milestones.py --check` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
