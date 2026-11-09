---
schema_version: 1
id: held-out-validation
domain: 00-workspace
status: draft
title: "TODO-03 -- Held-Out Validation"
depends_on: []
track: W3
---

# TODO-03 -- Held-Out Validation

> **Goal:** A set of images nobody tuned on (photographs from a second licensed source and real screenshots) confirms the compression milestones without any retuning, so every roadmap claim is shown to hold on images in general.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** The research sets are `photo` (measure, 24 Kodak, 14 imagecompression.info crops), `tp` (the 10-photo test pack), and `synth` (11 generated images). All three have been looked at while tuning; there is no held-out set.
<!-- claim: count "\"heldout\"" research/ab.py = 0 -->

## Inputs

- [`research/corpus.py`](../../research/corpus.py), [`research/ab.py`](../../research/ab.py) -- where §1 adds the set
- [`testdata/photos/manifest.json`](../../testdata/photos/manifest.json) -- the provenance record format §1 follows
- [`docs/research.md`](../../docs/research.md) -- section 2.3, rule 6: tune on `photo`, confirm on held-out data

## Outcome

- `python research/ab.py size heldout` runs over at least 30 photographs and 10 real screenshots, each with a recorded source, licence, and hash.
- Milestones 4, 7, and 8 are confirmed on it, or the misses are reported.

**Adjacency:** all=not-applicable (a research data set and its measurement; no codec behavior)

**Adjacency rationale:** The set only measures; nothing a user runs changes.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | The held-out set | -- |  [ ]   |
|   2   |   §2    | Validate the milestones on it | §1 |  [ ]   |

---

## 1. The Held-Out Set

> **Started:** 2026-11-09T00:53:30Z

**Needs:** Network (downloads)

Photographs come from a second openly licensed source (Wikimedia Commons featured pictures under CC0, CC BY, or CC BY-SA, attribution recorded per file); screenshots are captured locally from the RasterFold HTML docs and other content this repository owns, so no licence question arises. Once added, the set is never used to choose a parameter.

- [ ] Add a `heldout` step to `research/corpus.py` that downloads at least 30 photographs by their pinned URLs, cuts 1280x960 center crops, and records source, licence, author, and RGB SHA-256 in `research/heldout.json`. Done when: `python research/corpus.py` writes the crops and the manifest. Cheaper substitute: more Kodak images, which the tuning has already seen.
- [ ] Capture at least 10 screenshots of locally rendered pages (the HTML docs, the benchmark report) with a headless browser into the same set. Done when: they are listed in `research/heldout.json` with their source pages.
- [ ] Add `heldout` to `SETS` in `research/ab.py`, and record JPEG XL e7, e9, and WebP references for it. Done when: `python research/ab.py size heldout` prints all three ratios.
- [ ] Commit: `"research: add a held-out validation set"`

**Test checkpoint:** `python research/ab.py size heldout` over 40 or more images; every image's hash in `research/heldout.json` matches the file on disk.

## 2. Validate the Milestones on It

**Needs:** ffmpeg with libjxl (benchmarks)

- [ ] Measure `python research/ab.py size heldout` at the codec of milestone 8 and compare with milestones 4, 7, and 8's targets (photos below JPEG XL e9, every photo below e9, screenshots against WebP). Done when: each target is reported held or missed, with the worst images named. Cheaper substitute: retuning a constant on the held-out images, which turns them into tuning data.
- [ ] Record the results in `docs/research.md` section 1. Done when: the held-out row exists beside `photo`, `tp`, and `synth`.
- [ ] Commit: `"research: validate the compression milestones on held-out images"`

**Test checkpoint:** the `ab.py size heldout` summary lines quoted against each milestone's target; a miss files follow-ups under the missed-milestone policy (`D99 T01 §4`) instead of retuning.

## Verification

- [ ] `python scripts/todo-graph.py validate` clean
