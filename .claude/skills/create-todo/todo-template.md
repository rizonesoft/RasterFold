---
schema_version: 1
id: CHANGEME-unique-kebab-id
domain: CHANGEME-NN-domain
status: draft
title: "TODO-NN -- CHANGEME Title"
depends_on: []
track: CHANGEME
---

# TODO-NN -- CHANGEME Title

> **Goal:** CHANGEME: one paragraph. What is true when this file is finished, in plain terms.

> [!IMPORTANT]
> **Current state (verified CHANGEME-YYYY-MM-DD):** CHANGEME: what exists RIGHT NOW, before this TODO runs. Name real files and real gaps.
<!-- claim: exists CHANGEME/path/that/exists -->

## Inputs

- [CHANGEME spec, measurement, or docs/research.md item](CHANGEME-path) -- what this TODO consumes from it
- -> XREF: CHANGEME DNN TNN §N -- the related work and what flows each way (reciprocate in the target file)

## Outcome

- CHANGEME: observable end state, not an activity.
- CHANGEME: a second end state.

**Adjacency:** CHANGEME: all=not-applicable (reason) or all nine keys exactly once, separated by semicolons.

**Adjacency rationale:** CHANGEME: the substantive decisions behind the line above, in prose.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | CHANGEME deliverable | -- |  [ ]   |
|   2   |   §2    | CHANGEME deliverable | §1 |  [ ]   |

---

## 1. CHANGEME Section Title

CHANGEME: one paragraph of context: why this section exists and what it must not break.

- [ ] CHANGEME micro-step in `backtick/path`. Done when: CHANGEME observable end state. Cheaper substitute: CHANGEME the wrong thing.
- [ ] Commit: `"CHANGEME: one-line commit message"`

**Test checkpoint:** CHANGEME: the falsifiable command or drive that proves this section. It must be able to fail.

## 2. CHANGEME Section Title

CHANGEME: one paragraph of context.

(An experiment section names its measurement and its stop condition in the context paragraph: "Keep only if the photo total improves by at least 0.1% and the worst image does not get worse by more than 0.1%; otherwise ship the negative result." A section that changes the bitstream or file safety carries a `**Freeze check:**` line beside its checkpoint and the file sets `frozen: true`. Host-bound sections carry a `**Needs:**` line from the closed list in `todo/README.md`, e.g. `**Needs:** ffmpeg with libjxl (benchmarks)`.)

- [ ] CHANGEME micro-step in `backtick/path`. Done when: CHANGEME observable end state.
- [ ] Commit: `"CHANGEME: one-line commit message"`

**Test checkpoint:** CHANGEME: the falsifiable proof.

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0: build, correctness suite, fuzz, TODO gates green
- [ ] `python tests/test_correctness.py` exits 0 with every case run, including this file's new cases
- [ ] CHANGEME: file-level checks this file owes as a whole (the corpus sizes re-measured, `python tests/measure_final.py` PASS, `benchmarks/results.md` refreshed, a long fuzz run)
- [ ] `python scripts/todo-graph.py validate` clean
