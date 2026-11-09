---
schema_version: 1
id: milestones-new-data
domain: 05-milestones
status: draft
title: "TODO-06 -- Milestones 51 to 60: New Data and Long-Term Robustness (6.0)"
depends_on: []
track: G6
---

# TODO-06 -- Milestones 51 to 60: New Data and Long-Term Robustness (6.0)

> **Goal:** Milestones 51 to 60 of the roadmap in `docs/research.md` section 9 are reached in order, each proven by a gate that measures its target and the no-regression rule against the milestone before it.

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
|   1   |  §1   | Plan milestone 51: Learned model at the default level | D05 T05 §20 |  [ ]   |
|   2   |  §2   | Milestone 51 gate: Learned model at the default level | §1 |  [ ]   |
|   3   |  §3   | Plan milestone 52: Transmitted priors | §2 |  [ ]   |
|   4   |  §4   | Milestone 52 gate: Transmitted priors | §3 |  [ ]   |
|   5   |  §5   | Plan milestone 53: Camera raw | §4 |  [ ]   |
|   6   |  §6   | Milestone 53 gate: Camera raw | §5 |  [ ]   |
|   7   |  §7   | Plan milestone 54: Medical and scientific imagery | §6 |  [ ]   |
|   8   |  §8   | Milestone 54 gate: Medical and scientific imagery | §7 |  [ ]   |
|   9   |  §9   | Plan milestone 55: Error resilience | §8 |  [ ]   |
|  10   |  §10  | Milestone 55 gate: Error resilience | §9 |  [ ]   |
|  11   |  §11  | Plan milestone 56: Progressive decoding | §10 |  [ ]   |
|  12   |  §12  | Milestone 56 gate: Progressive decoding | §11 |  [ ]   |
|  13   |  §13  | Plan milestone 57: Lossless edit deltas | §12 |  [ ]   |
|  14   |  §14  | Milestone 57 gate: Lossless edit deltas | §13 |  [ ]   |
|  15   |  §15  | Plan milestone 58: Hardware decoder | §14 |  [ ]   |
|  16   |  §16  | Milestone 58 gate: Hardware decoder | §15 |  [ ]   |
|  17   |  §17  | Plan milestone 59: Twenty-year compatibility | §16 |  [ ]   |
|  18   |  §18  | Milestone 59 gate: Twenty-year compatibility | §17 |  [ ]   |
|  19   |  §19  | Plan milestone 60: RasterFold 6.0 | §18 |  [ ]   |
|  20   |  §20  | Milestone 60 gate: RasterFold 6.0 | §19 |  [ ]   |

---

## 1. Plan Milestone 51: Learned model at the default level

Milestone 51's work is authored here, one milestone ahead, from what milestone 50 actually measured: its target is *the default photo size at least 2% smaller at no more than 1.2 times decode time*. The design notes are `docs/research.md` section 9 (milestone 51) and the sections of `docs/research.md` it points at; the baseline is milestone 50's gate stamp (`D05 T05 §20`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 50's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §2`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §2'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 51 -- learned model at the default level"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §2'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 2. Milestone 51 Gate: Learned model at the default level

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 51: the default photo size at least 2% smaller at no more than 1.2 times decode time.

Milestone 51 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 50 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 50's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 50's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`. Done when: the default photo size at least 2% smaller at no more than 1.2 times decode time, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 50's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 50's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 51 -- learned model at the default level"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 3. Plan Milestone 52: Transmitted priors

Milestone 52's work is authored here, one milestone ahead, from what milestone 51 actually measured: its target is *images under 1 megapixel at least 1% smaller*. The design notes are `docs/research.md` section 9 (milestone 52) and the sections of `docs/research.md` it points at; the baseline is milestone 51's gate stamp (`D05 T06 §2`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 51's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §4`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §4'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 52 -- transmitted priors"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §4'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 4. Milestone 52 Gate: Transmitted priors

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 52: images under 1 megapixel at least 1% smaller.

Milestone 52 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 51 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 51's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 51's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: images under 1 megapixel at least 1% smaller, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 51's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 51's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 52 -- transmitted priors"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 5. Plan Milestone 53: Camera raw

Milestone 53's work is authored here, one milestone ahead, from what milestone 52 actually measured: its target is *raw sensor data smaller than DNG's lossless JPEG and than JPEG XL on the same raw files*. The design notes are `docs/research.md` section 9 (milestone 53) and the sections of `docs/research.md` it points at; the baseline is milestone 52's gate stamp (`D05 T06 §4`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 52's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §6`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §6'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 53 -- camera raw"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §6'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 6. Milestone 53 Gate: Camera raw

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 53: raw sensor data smaller than DNG's lossless JPEG and than JPEG XL on the same raw files.

Milestone 53 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 52 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 52's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 52's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with a raw corpus. Done when: raw sensor data smaller than DNG's lossless JPEG and than JPEG XL on the same raw files, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 52's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 52's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 53 -- camera raw"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 7. Plan Milestone 54: Medical and scientific imagery

Milestone 54's work is authored here, one milestone ahead, from what milestone 53 actually measured: its target is *16-bit grayscale and multi-channel images smaller than JPEG-LS and JPEG XL on public medical and satellite sets*. The design notes are `docs/research.md` section 9 (milestone 54) and the sections of `docs/research.md` it points at; the baseline is milestone 53's gate stamp (`D05 T06 §6`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 53's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §8`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §8'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 54 -- medical and scientific imagery"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §8'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 8. Milestone 54 Gate: Medical and scientific imagery

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 54: 16-bit grayscale and multi-channel images smaller than JPEG-LS and JPEG XL on public medical and satellite sets.

Milestone 54 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 53 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 53's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 53's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with the medical and satellite corpora. Done when: 16-bit grayscale and multi-channel images smaller than JPEG-LS and JPEG XL on public medical and satellite sets, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 53's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 53's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 54 -- medical and scientific imagery"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 9. Plan Milestone 55: Error resilience

Milestone 55's work is authored here, one milestone ahead, from what milestone 54 actually measured: its target is *a file with a damaged byte still yields every undamaged tile*. The design notes are `docs/research.md` section 9 (milestone 55) and the sections of `docs/research.md` it points at; the baseline is milestone 54's gate stamp (`D05 T06 §8`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 54's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §10`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §10'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 55 -- error resilience"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §10'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 10. Milestone 55 Gate: Error resilience

**Job:** A user can rely on milestone 55: a file with a damaged byte still yields every undamaged tile.

Milestone 55 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 54 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 54's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 54's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with a corruption-injection test. Done when: a file with a damaged byte still yields every undamaged tile, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 54's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 54's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 55 -- error resilience"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 11. Plan Milestone 56: Progressive decoding

Milestone 56's work is authored here, one milestone ahead, from what milestone 55 actually measured: its target is *a usable preview from the first 5% of the bytes, lossless at the end, for at most 1% size cost*. The design notes are `docs/research.md` section 9 (milestone 56) and the sections of `docs/research.md` it points at; the baseline is milestone 55's gate stamp (`D05 T06 §10`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 55's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §12`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §12'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 56 -- progressive decoding"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §12'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 12. Milestone 56 Gate: Progressive decoding

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 56: a usable preview from the first 5% of the bytes, lossless at the end, for at most 1% size cost.

Milestone 56 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 55 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 55's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 55's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: a usable preview from the first 5% of the bytes, lossless at the end, for at most 1% size cost, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 55's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 55's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 56 -- progressive decoding"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 13. Plan Milestone 57: Lossless edit deltas

Milestone 57's work is authored here, one milestone ahead, from what milestone 56 actually measured: its target is *an edited copy of an image stored in at most 10% of a full file*. The design notes are `docs/research.md` section 9 (milestone 57) and the sections of `docs/research.md` it points at; the baseline is milestone 56's gate stamp (`D05 T06 §12`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 56's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §14`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §14'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 57 -- lossless edit deltas"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §14'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 14. Milestone 57 Gate: Lossless edit deltas

**Job:** A user can rely on milestone 57: an edited copy of an image stored in at most 10% of a full file.

Milestone 57 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 56 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 56's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 56's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with an edit corpus. Done when: an edited copy of an image stored in at most 10% of a full file, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 56's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 56's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 57 -- lossless edit deltas"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 15. Plan Milestone 58: Hardware decoder

Milestone 58's work is authored here, one milestone ahead, from what milestone 57 actually measured: its target is *an FPGA decoder of the fast or tiny profile at 100 megapixels per second or more, byte-exact with the software*. The design notes are `docs/research.md` section 9 (milestone 58) and the sections of `docs/research.md` it points at; the baseline is milestone 57's gate stamp (`D05 T06 §14`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 57's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §16`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §16'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 58 -- hardware decoder"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §16'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 16. Milestone 58 Gate: Hardware decoder

**Needs:** FPGA board

**Job:** A user can rely on milestone 58: an FPGA decoder of the fast or tiny profile at 100 megapixels per second or more, byte-exact with the software.

Milestone 58 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 57 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 57's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 57's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with the FPGA testbench on the conformance suite. Done when: an FPGA decoder of the fast or tiny profile at 100 megapixels per second or more, byte-exact with the software, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 57's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 57's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 58 -- hardware decoder"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 17. Plan Milestone 59: Twenty-year compatibility

Milestone 59's work is authored here, one milestone ahead, from what milestone 58 actually measured: its target is *every file written by every release since version 2 decodes in the newest release, proven by a permanent compatibility corpus*. The design notes are `docs/research.md` section 9 (milestone 59) and the sections of `docs/research.md` it points at; the baseline is milestone 58's gate stamp (`D05 T06 §16`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 58's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §18`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §18'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 59 -- twenty-year compatibility"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §18'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 18. Milestone 59 Gate: Twenty-year compatibility

**Job:** A user can rely on milestone 59: every file written by every release since version 2 decodes in the newest release, proven by a permanent compatibility corpus.

Milestone 59 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 58 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 58's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 58's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with the compatibility corpus in CI. Done when: every file written by every release since version 2 decodes in the newest release, proven by a permanent compatibility corpus, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 58's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 58's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 59 -- twenty-year compatibility"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 19. Plan Milestone 60: RasterFold 6.0

Milestone 60's work is authored here, one milestone ahead, from what milestone 59 actually measured: its target is *`rasterfold-v6.0.0`: the learned default model, raw and scientific data, resilience, progressive decoding, edit deltas, and the hardware reference*. The design notes are `docs/research.md` section 9 (milestone 60) and the sections of `docs/research.md` it points at; the baseline is milestone 59's gate stamp (`D05 T06 §18`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 59's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T06 §20`). Done when: `python scripts/todo-graph.py resolve 'D05 T06 §20'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 60 -- rasterfold 6.0"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T06 §20'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 20. Milestone 60 Gate: RasterFold 6.0

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 60: `rasterfold-v6.0.0`: the learned default model, raw and scientific data, resilience, progressive decoding, edit deltas, and the hardware reference.

Milestone 60 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 59 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 59's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 59's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`. Done when: `rasterfold-v6.0.0`: the learned default model, raw and scientific data, resilience, progressive decoding, edit deltas, and the hardware reference, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 59's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 59's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Push the tag `rasterfold-v6.0.0`. Done when: the release workflow publishes the package and its checksums.
- [ ] Run the backlog review for `rasterfold-v6.0.0`: promote each backlog entry into a section or record the operator's deferral on it. Done when: every entry carries a `reviewed: rasterfold-v6.0.0` field or has been promoted.
- [ ] Commit: `"milestone: reach milestone 60 -- rasterfold 6.0"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## Verification

- [ ] `python research/milestones.py --check` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
