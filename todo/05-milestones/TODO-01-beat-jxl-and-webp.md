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
> **Current state (verified 2026-09-29):** No milestone gate has run. This file is generated from `research/milestones.py`; edit the table there and run `python research/milestones.py --write`, never this file by hand.
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

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 1: `pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9.

Milestone 1 of the roadmap (`docs/research.md` section 9). It is the baseline every later milestone is measured against. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `pwsh scripts/check-all.ps1`, `python scripts/review_prompt.py ci-wait` on the last push, `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: `pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9, each figure quoted with its command.
- [ ] Record the baseline figures (`python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`) as the reference for milestone 2. Done when: they are quoted in this section's stamp.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9 and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 1 -- locked baseline"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

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
