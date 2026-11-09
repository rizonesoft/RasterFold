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
> **Current state (verified 2026-09-29, updated 2026-11-09):** The research sets are `photo` (measure, 24 Kodak, 14 imagecompression.info crops), `tp` (the 10-photo test pack), and `synth` (11 generated images). All three have been looked at while tuning. §1 added the `heldout` set (30 Wikimedia Commons photographs and 10 locally rendered screenshots, frozen in `research/heldout.json`), which is never used to choose a parameter.
<!-- claim: count "\"heldout\"" research/ab.py = 5 -->

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
|   1   |   §1    | The held-out set | -- |  [x]   |
|   2   |   §2    | Validate the milestones on it | §1 |  [ ]   |

---

## 1. The Held-Out Set

> **Started:** 2026-11-09T00:53:30Z

**Needs:** Network (downloads)

Photographs come from a second openly licensed source (Wikimedia Commons featured pictures under CC0, CC BY, or CC BY-SA, attribution recorded per file); screenshots are captured locally from the RasterFold HTML docs and other content this repository owns, so no licence question arises. Once added, the set is never used to choose a parameter.

- [x] Add a `heldout` step to `research/corpus.py` that downloads at least 30 photographs by their pinned URLs, cuts 1280x960 center crops, and records source page, source revision, download SHA-256, licence name and version with its URL, author, an alteration note for the crop, and RGB SHA-256 in `research/heldout.json`. Each selected file is inspected as a photograph at authoring (no diagrams, maps, or illustrations) and its licence metadata is verified against the Commons API then, with the verification kept in the manifest. Downloads are bounded, written to a temporary file, validated as an image of at least 1280x960, and only then published; a failure exits nonzero naming the source, never silently skips an entry, and never replaces a selection after compression results exist. Once authored, the manifest is frozen: later runs verify the crops against its hashes and refuse a mismatch without rewriting it, and reference sizes are reused only for a verified set. Done when: `python research/corpus.py` writes the crops and the manifest. Cheaper substitute: more Kodak images, which the tuning has already seen.
- [x] Capture at least 10 screenshots of locally rendered pages with a headless browser into the same set: the staged site pages `index`, `getting-started`, `installation`, `usage`, `examples`, `format`, `c-api`, `benchmarks`, `limitations`, and `troubleshooting` from `python3 scripts/build-docs.py --stage build/site-source`, rendered to HTML with the pinned `markdown-it` through `npx` (recorded decision 2026-11-09: this host has no Jekyll or Ruby, which CI uses; the corpus authoring step uses the same staged Markdown either way, and reversing it costs a re-render and new screenshot hashes), then captured at 1280x960 with the locally cached headless Chromium shell. Done when: they are listed in `research/heldout.json` with their source pages, the browser binary and version, the rendering tool, and the repository revision.
- [x] Add `heldout` to `SETS` in `research/ab.py` (extending the CSV set labels and read-back with it), record JPEG XL e7, e9, and WebP references for it, and add a `python research/corpus.py --verify-heldout` mode that, without regenerating anything, requires at least 30 photographs and 10 screenshots, unique names, 1280x960 dimensions, exact agreement between the manifest and `SETS["heldout"]`, and a matching RGB SHA-256 for every file. Done when: `python research/ab.py size heldout` prints all three ratios over the verified set.
- [x] Commit: `"research: add a held-out validation set"`

**Test checkpoint:** `python3 scripts/windows.py research/ab.py size src/cm.c heldout` over 40 images prints `total=36633922 jxl=38421779 ratio=0.9535 geomean=0.9866 worst=1.2372 (ho_s02) e9=37850228 e9_ratio=0.9679 e9_geomean=1.0239 e9_worst=1.4236 (ho_s02) webp=40259242 webp_ratio=0.9100 webp_geomean=1.2775 webp_worst=5.1232 (ho_s10)`, and `python research/corpus.py --verify-heldout` prints `heldout verified: 30 photographs, 10 screenshots, hashes match` (2026-11-09).

> **Verified:** 2026-11-09 | §1 | ae1392ca1f046c774bf1bdcbb3cbf4816bbe405c
> **Review:** `docs/reviews/d00-t03-s1.json`
## 2. Validate the Milestones on It

> **Started:** 2026-11-09T09:41:23Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Corrected 2026-11-09 (plan review `docs/reviews/00-workspace/D00-T03-s2.md`):** the exact predicates are: Milestone 4, photographs: total at most 0.982 of JPEG XL e9 and at most 0.970 of e7, at least 31 of 39 (79.5%) below e9, at least 38 of 39 (97.4%) at or below e7, so on 30 held-out photographs at least 24 below e9 and at least 30 at or below e7 (the scaled counts rounded in the gate's favour are reported beside the exact fractions); Milestone 7, photographs: at least 34 of 39 (87.2%) below e9, on 30 photographs at least 27, and no photograph more than 1.0% larger than e9 (`e9_worst <= 1.0100`, applied directly); Milestone 8, screenshots: total and geometric mean below lossless WebP and every screenshot at most `max(1.1 * WebP, WebP + 32)` bytes from exact bytes. The timing and memory parts of milestones 4 and 7 are not size predicates and cannot be validated by this section's size measurement; they are reported as not applicable here, not as held. Milestone 9's separate 1% generalisation rule (counts reported, not gated) is out of scope. Per-image sizes come from verbose output grouped by the manifest's `kind`; class totals, geometric means, worst images, and predicate results are reported per class, never only as the combined aggregate. After `docs/research.md` gains the held-out row, `docs/research-summary.md` is regenerated with `python3 scripts/build-docs.py --write` and verified with `--check`.

- [ ] Measure `python research/ab.py size heldout` at the codec of milestone 8 and compare with milestones 4, 7, and 8's targets (photos below JPEG XL e9, every photo below e9, screenshots against WebP). Done when: each target is reported held or missed, with the worst images named. Cheaper substitute: retuning a constant on the held-out images, which turns them into tuning data.
- [ ] Record the results in `docs/research.md` section 1. Done when: the held-out row exists beside `photo`, `tp`, and `synth`.
- [ ] Commit: `"research: validate the compression milestones on held-out images"`

**Test checkpoint:** the `ab.py size heldout` summary lines quoted against each milestone's target; a miss files follow-ups under the missed-milestone policy (`D99 T01 §4`) instead of retuning.

## Verification

- [ ] `python scripts/todo-graph.py validate` clean
