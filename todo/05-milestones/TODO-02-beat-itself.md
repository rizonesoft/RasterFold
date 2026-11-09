---
schema_version: 1
id: milestones-beat-itself
domain: 05-milestones
status: draft
title: "TODO-02 -- Milestones 11 to 20: RasterFold Beats RasterFold (2.0)"
depends_on: []
track: G2
---

# TODO-02 -- Milestones 11 to 20: RasterFold Beats RasterFold (2.0)

> **Goal:** Milestones 11 to 20 of the roadmap in `docs/research.md` section 9 are reached in order, each proven by a gate that measures its target and the no-regression rule against the milestone before it.

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
|   1   |  §1   | Plan milestone 11: SIMD decisions | D05 T01 §10 |  [ ]   |
|   2   |  §2   | Milestone 11 gate: SIMD decisions | §1, D02 T01 §3, D02 T02 §2 |  [ ]   |
|   3   |  §3   | Plan milestone 12: Two-layer mixing | §2 |  [ ]   |
|   4   |  §4   | Milestone 12 gate: Two-layer mixing | §3 |  [ ]   |
|   5   |  §5   | Plan milestone 13: Stronger prediction | §4 |  [ ]   |
|   6   |  §6   | Milestone 13 gate: Stronger prediction | §5 |  [ ]   |
|   7   |  §7   | Plan milestone 14: Adaptive colour transform | §6 |  [ ]   |
|   8   |  §8   | Milestone 14 gate: Adaptive colour transform | §7 |  [ ]   |
|   9   |  §9   | Plan milestone 15: Effort levels | §8 |  [ ]   |
|  10   |  §10  | Milestone 15 gate: Effort levels | §9 |  [ ]   |
|  11   |  §11  | Plan milestone 16: Max-effort encoder search | §10 |  [ ]   |
|  12   |  §12  | Milestone 16 gate: Max-effort encoder search | §11 |  [ ]   |
|  13   |  §13  | Plan milestone 17: Trained starting state | §12 |  [ ]   |
|  14   |  §14  | Milestone 17 gate: Trained starting state | §13 |  [ ]   |
|  15   |  §15  | Plan milestone 18: Streaming | §14 |  [ ]   |
|  16   |  §16  | Milestone 18 gate: Streaming | §15 |  [ ]   |
|  17   |  §17  | Plan milestone 19: Many-core scaling | §16 |  [ ]   |
|  18   |  §18  | Milestone 19 gate: Many-core scaling | §17 |  [ ]   |
|  19   |  §19  | Plan milestone 20: RasterFold 2.0 | §18 |  [ ]   |
|  20   |  §20  | Milestone 20 gate: RasterFold 2.0 | §19 |  [ ]   |

---

## 1. Plan Milestone 11: SIMD decisions

-> SOURCE: research-7.2-grayscale

- [ ] Preserve the promoted backlog obligation in `docs/research.md` and this milestone's work sections: Measure generic grayscale-channel detection and single-channel coding on a diverse grayscale set plus photo/tp/synth; retain the no-image-specific-tuning rule and require a new method/version if accepted. Done when: a named section owns its implementation and tests, or a measured negative result is retained without silently dropping the obligation.

Milestone 11's work is authored here, one milestone ahead, from what milestone 10 actually measured: its target is *one-thread decode at least 10% faster than 1.0, bytes within 0.1%*. The design notes are `docs/research.md` section 9 (milestone 11) and the sections of `docs/research.md` it points at; the baseline is milestone 10's gate stamp (`D05 T01 §10`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 10's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §2`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §2'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 11 -- simd decisions"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §2'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 2. Milestone 11 Gate: SIMD decisions

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 11: one-thread decode at least 10% faster than 1.0, bytes within 0.1%.

Milestone 11 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 10 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 10's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 10's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`. Done when: one-thread decode at least 10% faster than 1.0, bytes within 0.1%, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 10's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 10's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 11 -- simd decisions"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 3. Plan Milestone 12: Two-layer mixing

Milestone 12's work is authored here, one milestone ahead, from what milestone 11 actually measured: its target is *photo total at least 0.4% smaller than milestone 11*. The design notes are `docs/research.md` section 9 (milestone 12) and the sections of `docs/research.md` it points at; the baseline is milestone 11's gate stamp (`D05 T02 §2`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 11's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §4`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §4'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 12 -- two-layer mixing"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §4'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 4. Milestone 12 Gate: Two-layer mixing

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 12: photo total at least 0.4% smaller than milestone 11.

Milestone 12 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 11 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 11's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 11's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: photo total at least 0.4% smaller than milestone 11, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 11's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 11's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 12 -- two-layer mixing"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 5. Plan Milestone 13: Stronger prediction

Milestone 13's work is authored here, one milestone ahead, from what milestone 12 actually measured: its target is *photo total at least 0.5% smaller than milestone 12*. The design notes are `docs/research.md` section 9 (milestone 13) and the sections of `docs/research.md` it points at; the baseline is milestone 12's gate stamp (`D05 T02 §4`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 12's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §6`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §6'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 13 -- stronger prediction"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §6'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 6. Milestone 13 Gate: Stronger prediction

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 13: photo total at least 0.5% smaller than milestone 12.

Milestone 13 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 12 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 12's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 12's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: photo total at least 0.5% smaller than milestone 12, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 12's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 12's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 13 -- stronger prediction"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 7. Plan Milestone 14: Adaptive colour transform

Milestone 14's work is authored here, one milestone ahead, from what milestone 13 actually measured: its target is *photo total at least 0.3% smaller than milestone 13, decode time within the no-regression envelope*. The design notes are `docs/research.md` section 9 (milestone 14) and the sections of `docs/research.md` it points at; the baseline is milestone 13's gate stamp (`D05 T02 §6`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 13's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §8`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §8'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 14 -- adaptive colour transform"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §8'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 8. Milestone 14 Gate: Adaptive colour transform

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 14: photo total at least 0.3% smaller than milestone 13, decode time within the no-regression envelope.

Milestone 14 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 13 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 13's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 13's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`. Done when: photo total at least 0.3% smaller than milestone 13, decode time within the no-regression envelope, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 13's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 13's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 14 -- adaptive colour transform"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 9. Plan Milestone 15: Effort levels

Milestone 15's work is authored here, one milestone ahead, from what milestone 14 actually measured: its target is *a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged*. The design notes are `docs/research.md` section 9 (milestone 15) and the sections of `docs/research.md` it points at; the baseline is milestone 14's gate stamp (`D05 T02 §8`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 14's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §10`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §10'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 15 -- effort levels"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §10'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 10. Milestone 15 Gate: Effort levels

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 15: a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged.

Milestone 15 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 14 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 14's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 14's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`, `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 14's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 14's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 15 -- effort levels"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 11. Plan Milestone 16: Max-effort encoder search

Milestone 16's work is authored here, one milestone ahead, from what milestone 15 actually measured: its target is *the max level at least 1% smaller than the default, its decode time within the no-regression envelope of the default's*. The design notes are `docs/research.md` section 9 (milestone 16) and the sections of `docs/research.md` it points at; the baseline is milestone 15's gate stamp (`D05 T02 §10`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 15's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §12`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §12'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 16 -- max-effort encoder search"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §12'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 12. Milestone 16 Gate: Max-effort encoder search

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 16: the max level at least 1% smaller than the default, its decode time within the no-regression envelope of the default's.

Milestone 16 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 15 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 15's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 15's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`. Done when: the max level at least 1% smaller than the default, its decode time within the no-regression envelope of the default's, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 15's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 15's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 16 -- max-effort encoder search"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 13. Plan Milestone 17: Trained starting state

Milestone 17's work is authored here, one milestone ahead, from what milestone 16 actually measured: its target is *images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set*. The design notes are `docs/research.md` section 9 (milestone 17) and the sections of `docs/research.md` it points at; the baseline is milestone 16's gate stamp (`D05 T02 §12`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 16's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §14`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §14'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 17 -- trained starting state"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §14'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 14. Milestone 17 Gate: Trained starting state

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 17: images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set.

Milestone 17 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 16 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 16's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 16's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size kodak` and `heldout`. Done when: images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 16's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 16's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 17 -- trained starting state"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 15. Plan Milestone 18: Streaming

-> SOURCE: readme-limitation-streaming

- [ ] Preserve the promoted backlog obligation in `docs/research.md` and this milestone's work sections: Plan bounded-row encode/decode buffers and the matching API/thread schedule, proving memory on images above the current limit before raising it. Done when: a named section owns its implementation and tests, or a measured negative result is retained without silently dropping the obligation.

Milestone 18's work is authored here, one milestone ahead, from what milestone 17 actually measured: its target is *peak memory on a 24-megapixel image at most one third of milestone 17's*. The design notes are `docs/research.md` section 9 (milestone 18) and the sections of `docs/research.md` it points at; the baseline is milestone 17's gate stamp (`D05 T02 §14`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 17's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §16`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §16'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 18 -- streaming"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §16'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 16. Milestone 18 Gate: Streaming

**Job:** A user can rely on milestone 18: peak memory on a 24-megapixel image at most one third of milestone 17's.

Milestone 18 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 17 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 17's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 17's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `bin/windows-x64/bin/rasterfold.exe decode --time` on a 24-megapixel image. Done when: peak memory on a 24-megapixel image at most one third of milestone 17's, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 17's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 17's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 18 -- streaming"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 17. Plan Milestone 19: Many-core scaling

Milestone 19's work is authored here, one milestone ahead, from what milestone 18 actually measured: its target is *decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2%*. The design notes are `docs/research.md` section 9 (milestone 19) and the sections of `docs/research.md` it points at; the baseline is milestone 18's gate stamp (`D05 T02 §16`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 18's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §18`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §18'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 19 -- many-core scaling"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §18'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 18. Milestone 19 Gate: Many-core scaling

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 19: decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2%.

Milestone 19 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 18 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 18's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 18's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`, `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns). Done when: decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2%, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 18's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 18's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 19 -- many-core scaling"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## 19. Plan Milestone 20: RasterFold 2.0

Milestone 20's work is authored here, one milestone ahead, from what milestone 19 actually measured: its target is *`rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third*. The design notes are `docs/research.md` section 9 (milestone 20) and the sections of `docs/research.md` it points at; the baseline is milestone 19's gate stamp (`D05 T02 §18`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 19's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T02 §20`). Done when: `python scripts/todo-graph.py resolve 'D05 T02 §20'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 20 -- rasterfold 2.0"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T02 §20'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 20. Milestone 20 Gate: RasterFold 2.0

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 20: `rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third.

Milestone 20 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 19 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 19's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 19's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`): up to three follow-up experiments from `docs/research.md` are filed through `add-todo`, and the operator is asked only when all three miss.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`. Done when: `rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 19's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 19's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Push the tag `rasterfold-v2.0.0`. Done when: the release workflow publishes the package and its checksums.
- [ ] Run the backlog review for `rasterfold-v2.0.0`: promote each backlog entry into a section or record the operator's deferral on it. Done when: every entry carries a `reviewed: rasterfold-v2.0.0` field or has been promoted.
- [ ] Commit: `"milestone: reach milestone 20 -- rasterfold 2.0"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead.

## Verification

- [ ] `python research/milestones.py --check` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
