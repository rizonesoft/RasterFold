---
schema_version: 1
id: milestones-beyond-still-files
domain: 05-milestones
status: draft
title: "TODO-04 -- Milestones 31 to 40: Beyond Single Still Files (4.0)"
depends_on: []
track: G4
---

# TODO-04 -- Milestones 31 to 40: Beyond Single Still Files (4.0)

> **Goal:** Milestones 31 to 40 of the roadmap in `docs/research.md` section 9 are reached in order, each proven by a gate that measures its target and the no-regression rule against the milestone before it.

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
|   1   |  §1   | Plan milestone 31: Language bindings and a streaming API | D05 T03 §20 |  [ ]   |
|   2   |  §2   | Milestone 31 gate: Language bindings and a streaming API | §1 |  [ ]   |
|   3   |  §3   | Plan milestone 32: More formats | §2 |  [ ]   |
|   4   |  §4   | Milestone 32 gate: More formats | §3 |  [ ]   |
|   5   |  §5   | Plan milestone 33: Lossless JPEG recompression | §4 |  [ ]   |
|   6   |  §6   | Milestone 33 gate: Lossless JPEG recompression | §5 |  [ ]   |
|   7   |  §7   | Plan milestone 34: Image sequences | §6 |  [ ]   |
|   8   |  §8   | Milestone 34 gate: Image sequences | §7 |  [ ]   |
|   9   |  §9   | Plan milestone 35: Gigapixel images | §8 |  [ ]   |
|  10   |  §10  | Milestone 35 gate: Gigapixel images | §9 |  [ ]   |
|  11   |  §11  | Plan milestone 36: Browser decoding | §10 |  [ ]   |
|  12   |  §12  | Milestone 36 gate: Browser decoding | §11 |  [ ]   |
|  13   |  §13  | Plan milestone 37: Host plugins | §12 |  [ ]   |
|  14   |  §14  | Milestone 37 gate: Host plugins | §13 |  [ ]   |
|  15   |  §15  | Plan milestone 38: Second implementation | §14 |  [ ]   |
|  16   |  §16  | Milestone 38 gate: Second implementation | §15 |  [ ]   |
|  17   |  §17  | Plan milestone 39: Automatic effort | §16 |  [ ]   |
|  18   |  §18  | Milestone 39 gate: Automatic effort | §17 |  [ ]   |
|  19   |  §19  | Plan milestone 40: RasterFold 4.0 | §18 |  [ ]   |
|  20   |  §20  | Milestone 40 gate: RasterFold 4.0 | §19 |  [ ]   |

---

## 1. Plan Milestone 31: Language bindings and a streaming API

Milestone 31's work is authored here, one milestone ahead, from what milestone 30 actually measured: its target is *Python, Rust, and C# bindings over the 1.x C API and a streaming callback API, each with byte-exact round trips and API overhead at most 1%*. The design notes are `docs/research.md` section 9 (milestone 31) and the sections of `docs/research.md` it points at; the baseline is milestone 30's gate stamp (`D05 T03 §20`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 30's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §2`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §2'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 31 -- language bindings and a streaming api"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §2'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 2. Milestone 31 Gate: Language bindings and a streaming API

**Job:** A user can rely on milestone 31: Python, Rust, and C# bindings over the 1.x C API and a streaming callback API, each with byte-exact round trips and API overhead at most 1%.

Milestone 31 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 30 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 30's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 30's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with the bindings' test suites. Done when: Python, Rust, and C# bindings over the 1.x C API and a streaming callback API, each with byte-exact round trips and API overhead at most 1%, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 30's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 30's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 31 -- language bindings and a streaming api"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 3. Plan Milestone 32: More formats

Milestone 32's work is authored here, one milestone ahead, from what milestone 31 actually measured: its target is *TIFF, 16-bit PNG, and PAM in and out with exact pixels*. The design notes are `docs/research.md` section 9 (milestone 32) and the sections of `docs/research.md` it points at; the baseline is milestone 31's gate stamp (`D05 T04 §2`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 31's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §4`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §4'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 32 -- more formats"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §4'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 4. Milestone 32 Gate: More formats

**Job:** A user can rely on milestone 32: TIFF, 16-bit PNG, and PAM in and out with exact pixels.

Milestone 32 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 31 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 31's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 31's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with round trips against Pillow and libtiff. Done when: TIFF, 16-bit PNG, and PAM in and out with exact pixels, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 31's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 31's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 32 -- more formats"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 5. Plan Milestone 33: Lossless JPEG recompression

Milestone 33's work is authored here, one milestone ahead, from what milestone 32 actually measured: its target is *JPEG files stored at least 2% smaller than JPEG XL's JPEG recompression, the original JPEG rebuilt bit for bit*. The design notes are `docs/research.md` section 9 (milestone 33) and the sections of `docs/research.md` it points at; the baseline is milestone 32's gate stamp (`D05 T04 §4`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 32's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §6`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §6'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 33 -- lossless jpeg recompression"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §6'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 6. Milestone 33 Gate: Lossless JPEG recompression

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 33: JPEG files stored at least 2% smaller than JPEG XL's JPEG recompression, the original JPEG rebuilt bit for bit.

Milestone 33 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 32 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 32's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 32's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with a JPEG corpus against `cjxl --lossless_jpeg=1`. Done when: JPEG files stored at least 2% smaller than JPEG XL's JPEG recompression, the original JPEG rebuilt bit for bit, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 32's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 32's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 33 -- lossless jpeg recompression"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 7. Plan Milestone 34: Image sequences

Milestone 34's work is authored here, one milestone ahead, from what milestone 33 actually measured: its target is *bursts and screen recordings smaller than lossless animated JPEG XL and animated WebP*. The design notes are `docs/research.md` section 9 (milestone 34) and the sections of `docs/research.md` it points at; the baseline is milestone 33's gate stamp (`D05 T04 §6`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 33's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §8`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §8'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 34 -- image sequences"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §8'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 8. Milestone 34 Gate: Image sequences

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A user can rely on milestone 34: bursts and screen recordings smaller than lossless animated JPEG XL and animated WebP.

Milestone 34 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 33 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 33's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 33's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with a sequence corpus. Done when: bursts and screen recordings smaller than lossless animated JPEG XL and animated WebP, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 33's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 33's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 34 -- image sequences"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 9. Plan Milestone 35: Gigapixel images

Milestone 35's work is authored here, one milestone ahead, from what milestone 34 actually measured: its target is *the size limit raised from 32 megapixels to 4 gigapixels with peak memory proportional to width*. The design notes are `docs/research.md` section 9 (milestone 35) and the sections of `docs/research.md` it points at; the baseline is milestone 34's gate stamp (`D05 T04 §8`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 34's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §10`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §10'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 35 -- gigapixel images"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §10'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 10. Milestone 35 Gate: Gigapixel images

**Job:** A user can rely on milestone 35: the size limit raised from 32 megapixels to 4 gigapixels with peak memory proportional to width.

Milestone 35 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 34 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 34's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 34's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with `decode --time` on a 1-gigapixel image. Done when: the size limit raised from 32 megapixels to 4 gigapixels with peak memory proportional to width, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 34's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 34's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 35 -- gigapixel images"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 11. Plan Milestone 36: Browser decoding

Milestone 36's work is authored here, one milestone ahead, from what milestone 35 actually measured: its target is *decoding in a browser at most twice native time, from a WebAssembly module under 200 KB*. The design notes are `docs/research.md` section 9 (milestone 36) and the sections of `docs/research.md` it points at; the baseline is milestone 35's gate stamp (`D05 T04 §10`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 35's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §12`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §12'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 36 -- browser decoding"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §12'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 12. Milestone 36 Gate: Browser decoding

**Job:** A user can rely on milestone 36: decoding in a browser at most twice native time, from a WebAssembly module under 200 KB.

Milestone 36 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 35 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 35's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 35's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with a headless-browser timing harness. Done when: decoding in a browser at most twice native time, from a WebAssembly module under 200 KB, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 35's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 35's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 36 -- browser decoding"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 13. Plan Milestone 37: Host plugins

Milestone 37's work is authored here, one milestone ahead, from what milestone 36 actually measured: its target is *open and save with exact pixels in Pillow, ffmpeg, libvips, and GIMP*. The design notes are `docs/research.md` section 9 (milestone 37) and the sections of `docs/research.md` it points at; the baseline is milestone 36's gate stamp (`D05 T04 §12`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 36's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §14`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §14'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 37 -- host plugins"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §14'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 14. Milestone 37 Gate: Host plugins

**Job:** A user can rely on milestone 37: open and save with exact pixels in Pillow, ffmpeg, libvips, and GIMP.

Milestone 37 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 36 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 36's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 36's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with a round-trip test in each host. Done when: open and save with exact pixels in Pillow, ffmpeg, libvips, and GIMP, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 36's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 36's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 37 -- host plugins"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 15. Plan Milestone 38: Second implementation

Milestone 38's work is authored here, one milestone ahead, from what milestone 37 actually measured: its target is *an independent Rust decoder, written from the spec alone, byte-identical on the conformance suite*. The design notes are `docs/research.md` section 9 (milestone 38) and the sections of `docs/research.md` it points at; the baseline is milestone 37's gate stamp (`D05 T04 §14`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 37's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §16`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §16'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 38 -- second implementation"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §16'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 16. Milestone 38 Gate: Second implementation

**Job:** A user can rely on milestone 38: an independent Rust decoder, written from the spec alone, byte-identical on the conformance suite.

Milestone 38 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 37 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 37's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 37's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with the conformance suite. Done when: an independent Rust decoder, written from the spec alone, byte-identical on the conformance suite, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 37's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 37's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 38 -- second implementation"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 17. Plan Milestone 39: Automatic effort

Milestone 39's work is authored here, one milestone ahead, from what milestone 38 actually measured: its target is *at least 90% of the max level's gain at no more than 1.5 times the default encode time*. The design notes are `docs/research.md` section 9 (milestone 39) and the sections of `docs/research.md` it points at; the baseline is milestone 38's gate stamp (`D05 T04 §16`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 38's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §18`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §18'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 39 -- automatic effort"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §18'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 18. Milestone 39 Gate: Automatic effort

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 39: at least 90% of the max level's gain at no more than 1.5 times the default encode time.

Milestone 39 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 38 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 38's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 38's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python research/ab.py ab <previous> <current> -n5` at `-t1` and `-t3`, and `python research/vsjxl.py -n5`. Done when: at least 90% of the max level's gain at no more than 1.5 times the default encode time, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 38's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 38's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Commit: `"milestone: reach milestone 39 -- automatic effort"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## 19. Plan Milestone 40: RasterFold 4.0

Milestone 40's work is authored here, one milestone ahead, from what milestone 39 actually measured: its target is *`rasterfold-v4.0.0`: bindings, more formats, JPEG recompression, sequences, gigapixel images, browser decoding, and plugins, with no regression on 3.0 files*. The design notes are `docs/research.md` section 9 (milestone 40) and the sections of `docs/research.md` it points at; the baseline is milestone 39's gate stamp (`D05 T04 §18`), never a figure from memory. The target above is an estimate: this section re-derives it from that baseline, states every bar with its tolerance (set totals, or a count with a worst-image margin, rather than every image; a byte floor for images under 16,384 pixels; timing from pooled alternating runs), and keeps time and memory inside the gate's cost envelope unless the target itself names a speed or memory figure (operator decision 2026-10-30, `D99 T01 §4`).

- [ ] Read milestone 39's gate stamp and `docs/research.md` sections 3 and 9, and quote the baseline this milestone's target is measured from in this section as a `**Corrected YYYY-MM-DD:**` note. Done when: the note quotes the figures with their commands.
- [ ] Author the milestone's work sections through `plan-experiment` (and `create-todo` for a new file) in the domains that own the work, each with its measurement and stop condition, placed in this milestone's phase of `todo/implementation-plan.md` above the gate row. Done when: `python scripts/todo-graph.py validate` exits 0 with the new sections in place. Cheaper substitute: one vague section named after the milestone.
- [ ] Add every new work section to the Depends On of the gate (`D05 T04 §20`). Done when: `python scripts/todo-graph.py resolve 'D05 T04 §20'` lists them among its dependencies.
- [ ] Commit: `"todo: plan milestone 40 -- rasterfold 4.0"`

**Test checkpoint:** `python scripts/todo-graph.py validate` and `plan --check` exit 0, and `resolve 'D05 T04 §20'` exits 4 naming at least one new work section, proving the gate waits on the authored work.

## 20. Milestone 40 Gate: RasterFold 4.0

**Needs:** Idle machine (timing)

**Job:** A user can rely on milestone 40: `rasterfold-v4.0.0`: bindings, more formats, JPEG recompression, sequences, gigapixel images, browser decoding, and plugins, with no regression on 3.0 files.

Milestone 40 of the roadmap (`docs/research.md` section 9). It keeps everything milestone 39 reached, within the cost envelope the operator set on 2026-10-30 (`D99 T01 §4`): the `photo` and `tp` totals no more than 0.1% larger and no photograph more than 0.5% larger; encode and decode time at most 1.10x milestone 39's on the `measure`, `photo`, `tp`, and `tp-only` totals and at most 1.20x on the `synth` total, at `-t1` and `-t3`, and at most 1.5x on any image of at least 16,384 pixels (a smaller image at most 1 ms slower), each ratio from per-image minima of alternating runs pooled until two disjoint pools agree (a ratio that moves between pools by more than its bar is noise and is re-measured on an idle machine, never counted as a miss) with any single-image outlier confirmed at `-n7`; codec working memory (decode `alloc_peak` less the input file's bytes, which `alloc_peak` holds) at most 5% above milestone 39's; and every older file still decoding. A target that names its own size, time, or memory figure replaces the matching envelope figure. If the target is missed, the missed-milestone policy applies (`D99 T01 §4`, capped by `D99 T01 §8`, operator decision 2026-11-10): follow-up experiments from `docs/research.md` are filed through `add-todo` and measured against this gate's bar, one round per experiment section, up to a hard cap of 13 rounds per gate, with the rounds used and their refs recorded in this gate. At the cap the gate closes accepted-missed: the missed figures are recorded in `docs/research.md` sections 1 and 9, the stamp states the target was missed and the cap reached (the target text is never rewritten to fit the measurement, and no claim of reaching it is made), the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. A later planning section may still file fresh work toward the recorded gap as ordinary sections; they are not gate rounds.

- [ ] Measure the target with `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`. Done when: `rasterfold-v4.0.0`: bindings, more formats, JPEG recompression, sequences, gigapixel images, browser decoding, and plugins, with no regression on 3.0 files, each figure quoted with its command.
- [ ] Measure the no-regression rule against milestone 39's gate stamp: `python research/ab.py size photo`, `tp`, and `synth` (with the JPEG XL e9 and WebP columns), `python tests/benchmark.py -n 3`, `decode --time`, and `python tests/test_correctness.py`. Done when: each rule holds, quoted beside milestone 39's figures.
- [ ] Record the milestone in `docs/research.md` sections 1 and 9, append the gate's summary line to the Unreleased section of `CHANGELOG.md`, and regenerate the benchmarks and the HTML docs (`python scripts/build-docs.py`). Done when: `python scripts/drift-check.py` exits 0.
- [ ] Push the tag `rasterfold-v4.0.0`. Done when: the release workflow publishes the package and its checksums.
- [ ] Run the backlog review for `rasterfold-v4.0.0`: promote each backlog entry into a section or record the operator's deferral on it. Done when: every entry carries a `reviewed: rasterfold-v4.0.0` field or has been promoted.
- [ ] Commit: `"milestone: reach milestone 40 -- rasterfold 4.0"`

**Test checkpoint:** every target figure and every no-regression figure quoted from a run at the commit; a figure that misses its bar blocks the stamp and triggers the missed-milestone policy instead, except at the 13-round cap (`D99 T01 §8`), where the stamp records the gate as accepted-missed with the figures and the 13 rounds quoted and the target text unchanged.

## Verification

- [ ] `python research/milestones.py --check` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
