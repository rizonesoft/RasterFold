---
schema_version: 1
id: screen-content
domain: 01-compression
status: draft
title: "TODO-07 -- Screen Content"
depends_on: []
track: C7
---

# TODO-07 -- Screen Content

> **Goal:** On screen content with many colours (browser screenshots of documents: anti-aliased text, flat backgrounds, tables and code blocks) RasterFold holds Milestone 8's result against lossless WebP, so Milestone 9 (`D05 T01 §9`) holds on the held-out screenshots: their total at most `1.0076` and their geometric mean at most `0.8859` of WebP's, with every round designed and decided on development screenshots that share nothing with the held-out set.

> [!IMPORTANT]
> **Current state (verified 2026-11-12):** On the 10 held-out screenshots (`ho_s01` to `ho_s10`, 1280x960 renders of documentation pages, `research/heldout.json`) RasterFold totals `3,326,125` bytes against lossless WebP's `711,772` (`4.6730`; geometric mean `4.6534`; worst `ho_s10` `5.1232`) and is `1.3518` of JPEG XL e9 (`python3 scripts/windows.py research/ab.py size src/cm.c heldout -v -t1` at `ee7093c`). Those images carry far more than 256 colours, so they are coded by method 1 (context mixing); the palette methods cannot reach them. A match model as an eighth method 1 model was tried and shipped negative (`D01 T04 §2`, `docs/research.md` section 3), and an exact-repeat context exists in method 1 (section 4.7). The held-out images may not be used to choose anything; the development set `screens` (§1, `research/screens.json`, ten screenshots of pages that are not held out) is what rounds are designed on. **Corrected 2026-11-12 (§1):** this sentence said no development set existed.
<!-- claim: exists research/heldout.json -->
<!-- claim: count "\"kind\": \"screenshot\"" research/heldout.json = 10 -->
<!-- claim: exists research/screens.json -->
<!-- claim: count "\"kind\": \"screenshot\"" research/screens.json = 10 -->

## Inputs

- [`research/heldout.json`](../../research/heldout.json) and `research/corpus.py` (`HELDOUT_PAGES`, `_screenshot_pages`) -- the held-out screenshots, their renderer (markdown-it 14.1.0 and the pinned headless Chromium shell, 1280x960) and the pages that may not be reused
- [`docs/research.md`](../../docs/research.md) -- section 3 (Already tried: the match model `D01 T04 §2`, the copy runs and colour caches of `D01 T06`), section 4.7 (exact-repeat context), section 4.13 (where WebP wins on generated images), section 8 (lossless WebP)
- The WebP lossless bitstream specification (RFC 9649, section 4 transforms and section 5 backward references and the colour cache) -- the mechanism the rounds are measured against
- The Milestone 9 gate (`D05 T01 §9`), which depends on every section of this file and records each round's held-out figures and its rounds used

## Outcome

- `python3 scripts/windows.py research/ab.py size src/cm.c screens -v -t1` measures a frozen development set of screenshots that shares no image and no source page with the held-out set.
- The bytes RasterFold spends on screenshots are attributed, and each round is a planned experiment with a stop condition decided on the development set, shipped or recorded as a negative result.
- The Milestone 9 gate's screenshot figures hold, or the gate closes accepted-missed at its 13-round cap.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no option); reporting=applicable @ D01 T07 §2; notifications=not-applicable (no message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history and the research record are the audit); exchange=applicable @ D01 T07 §1; reverse=not-applicable (an experiment is shipped or recorded negative, never undone in place)

**Adjacency rationale:** The development set is a new exchange boundary (images rendered from repository pages and frozen by hash, like the held-out set), and the attribution is the report later rounds route by. Nothing else here has a user surface: the codec's command line and files are unchanged until a round ships, and a shipped round carries its own format duties.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | A development set of screenshots | -- |  [ ]   |
|   2   |   §2    | Where the bytes go on screenshots, and round 1 planned | §1 |  [ ]   |

---

## 1. A Development Set of Screenshots

> **Started:** 2026-11-12T13:23:56Z

**Origin:** discovered run=32b2775e0d5a 2026-11-12 -- Milestone 9 gate (`D05 T01 §9`) misses on the held-out screenshots and its plan review requires every follow-up round to be designed on development inputs that share nothing with `research/heldout.json`; a prerequisite of the gate's planned rounds.

Every round needs screenshots to measure while it is designed, and the held-out ones may not be those. This section renders ten more, from repository pages that are not held out, with exactly the held-out renderer, and freezes them by hash so a later run measures the same pixels. It changes no codec source and no held-out image.

The pages, decided here: the published pages `api`, `cli`, `contributing`, `research-summary`, `security`, `licenses`, `release-history` and `downloads` from `build/site-source/` (staged by `python3 scripts/build-docs.py --stage build/site-source`), and the unpublished documents `docs/vision.md` and `docs/codec-distribution.md`. None is in `HELDOUT_PAGES`. Each is rendered as `_screenshot_pages` renders a held-out page (the same markdown-it version, HTML template, browser shell and 1280x960 window), named `dv_s01` to `dv_s10`, and recorded in `research/screens.json` (set `screens`, `frozen: true`, each entry with its source page, the source file's SHA-256, the repository revision, the renderer and browser, and the RGB SHA-256), so a later run verifies the pixels and refuses to regenerate from a changed page. Reversal cost: delete the manifest and the set name.

**Corrected 2026-11-12 (plan review `docs/reviews/01-compression/D01-T07-s1.md`):** three gaps folded in. (1) Hosts: the renderer runs the Linux headless Chromium shell, so the set is authored in WSL/Linux, once, with `python3 research/corpus.py --author-screens`, which renders with the held-out set's recorded browser version (refusing any other) and the pinned markdown-it; routine runs stay on Windows (`python3 scripts/windows.py research/corpus.py`) and never render. (2) Frozen pixels are committed: authoring writes each image as `research/screens-assets/dv_sNN.png` beside `research/screens.json`, as the held-out screenshots live in `research/heldout-assets/`, so a missing PPM is recovered by a file copy on any host and a later change to a source page cannot alter the set. (3) The measurement checks the freeze: `research/ab.py size` verifies every `screens` image against `research/screens.json` before measuring, as it does for `heldout`, and refuses a mismatch.

- [x] Add the `screens` set to `research/corpus.py` (render, freeze to `research/screens.json`, verify on later runs and refuse a mismatch without rewriting the manifest, as `heldout` does) and to `SETS` in `research/ab.py`. Done when: `python3 research/corpus.py --author-screens` in WSL, with no cached development PPMs, writes `research/screens.json`, `research/screens-assets/dv_s01.png` to `dv_s10.png` and `build/research/ppm/dv_s01.ppm` to `dv_s10.ppm`; `python3 scripts/windows.py research/corpus.py` then recovers and verifies them and adds their JPEG XL and WebP reference sizes; and a second run verifies them unchanged. Completed 2026-11-12: with no development PPMs cached, `.venv/bin/python research/corpus.py --author-screens` in WSL printed `screens verified: 10 screenshots, hashes match, no overlap with the held-out set` and wrote `research/screens.json` and the ten PNGs in `research/screens-assets/` (2.0 MB); `python3 scripts/windows.py research/corpus.py` printed the same line and `105 images in ...ppm`, and so did a second run.
- [x] Refuse overlap and broken freezes. Done when: `research/corpus.py` refuses a development page that is in `HELDOUT_PAGES` and a development image whose RGB SHA-256 equals a held-out one; a missing development PPM is recovered from its committed PNG; authoring refuses a changed source page against the manifest and a browser version other than the held-out set's; `research/ab.py size` refuses a development PPM whose pixels changed, invoked directly; and after every refusal `research/screens.json` is byte-identical. Each is a test in `tests/test_research_refs.py`. Completed 2026-11-12: class `ScreensSetTests` in `tests/test_research_refs.py` (6 tests, OK): the committed set is disjoint by page and pixels and uses the held-out browser version; held-out page and held-out pixels refused; missing PPM recovered from its PNG and refused without it; changed source page and another browser version refused at authoring; changed pixels refused by `ab.verify_frozen` and `verify_screens`, and an unfrozen set refused; the manifest byte-identical after each refusal.
- [x] Measure the set. Done when: `python3 scripts/windows.py research/ab.py size src/cm.c screens -v -t1` prints a per-image line for all ten and a summary line with the JPEG XL e9 and WebP columns, quoted in the checkpoint. Completed 2026-11-12: `python3 scripts/windows.py research/ab.py size src/cm.c screens -v -t1` printed ten per-image lines and `total=3383489 jxl=2758793 ratio=1.2264 geomean=1.2274 worst=1.3202 (dv_s02) e9=2419197 e9_ratio=1.3986 e9_geomean=1.3992 e9_worst=1.5385 (dv_s02) webp=670020 webp_ratio=5.0498 webp_geomean=4.8625`.
- [x] Commit: `"research: a development set of screenshots"`

**Test checkpoint:** Driven run with evidence, 2026-11-12. Authoring in WSL with no cached development PPMs (`ls build/research/ppm/dv_*` counted 0 first): `.venv/bin/python research/corpus.py --author-screens` printed `screens verified: 10 screenshots, hashes match, no overlap with the held-out set` and wrote `research/screens.json` (`dv_s01` to `dv_s10`: `api`, `cli`, `contributing`, `research-summary`, `security`, `licenses`, `release-history`, `downloads`, `vision`, `codec-distribution`, browser version equal to the held-out set's). `python3 scripts/windows.py research/ab.py size src/cm.c screens -v -t1` printed `total=3383489 jxl=2758793 ratio=1.2264 geomean=1.2274 worst=1.3202 (dv_s02) e9=2419197 e9_ratio=1.3986 e9_geomean=1.3992 e9_worst=1.5385 (dv_s02) webp=670020 webp_ratio=5.0498 webp_geomean=4.8625`, with the ten per-image lines `dv_s01 316214 jxl= 263338 1.2008 e9= 233010 1.3571 webp= 64698 4.8875`; `dv_s02 262931 jxl= 199160 1.3202 e9= 170898 1.5385 webp= 54286 4.8434`; `dv_s03 335364 jxl= 276982 1.2108 e9= 243562 1.3769 webp= 66548 5.0394`; `dv_s04 431324 jxl= 358300 1.2038 e9= 317030 1.3605 webp= 88574 4.8696`; `dv_s05 130091 jxl= 101929 1.2763 e9= 89805 1.4486 webp= 30692 4.2386`; `dv_s06 354643 jxl= 304135 1.1661 e9= 269966 1.3137 webp= 80340 4.4143`; `dv_s07 420869 jxl= 351301 1.1980 e9= 305655 1.3769 webp= 77108 5.4582`; `dv_s08 118943 jxl= 99968 1.1898 e9= 87954 1.3523 webp= 31152 3.8181`; `dv_s09 553546 jxl= 429967 1.2874 e9= 370026 1.4960 webp= 88844 6.2305`; `dv_s10 459564 jxl= 373713 1.2297 e9= 331291 1.3872 webp= 87778 5.2355`: the same picture as the held-out screenshots (`4.6730`), so the set is a faithful stand-in. The second `python3 scripts/windows.py research/corpus.py` printed `screens verified: 10 screenshots, hashes match, no overlap with the held-out set`. `.venv/bin/python tests/test_research_refs.py` passed, including the six `ScreensSetTests`.

## 2. Where the Bytes Go on Screenshots, and Round 1 Planned

**Origin:** discovered run=32b2775e0d5a 2026-11-12 -- Milestone 9 gate (`D05 T01 §9`): the first round must be routed by a measurement, not guessed; a prerequisite of the gate's planned rounds. This section is not itself a round (`D99 T01 §8`: one round is one follow-up experiment section).

WebP is about 4.7 times smaller on screenshots. Before anything is designed this section measures, on the development set only, where RasterFold's bytes go and which repetition WebP can exploit that method 1 does not. It changes no codec source.

- [ ] Attribute method 1's bytes on each development screenshot with `bench.exe --breakdown` (per channel and decision kind, payload verified byte-identical), as `docs/research.md` section 4.13 did for `synth`. Done when: the per-channel and per-decision bytes of all ten are tabulated.
- [ ] Measure the repetition available, with a stdlib Python script under `research/` that reads the development PPMs: the share of pixels equal to their left, above, and above-left neighbours; the share covered by exact matches of at least 4, 8 and 32 pixels against any earlier position (hash of 4-pixel runs, longest match), with the distance distribution split into same row, previous rows within 64 rows, and further; the number of distinct colours; and an estimate of the bits a backward-reference coding with a 256-entry colour cache would spend (literal, cache hit and copy counts times plain entropy-coded costs). Done when: the figures are tabulated per image and in total.
- [ ] Record the attribution as a new subsection of `docs/research.md` section 4 ("Where WebP wins on screenshots"), naming the leading hypothesis and the designs it rules out against section 3's Already tried rows. Done when: `python3 scripts/drift-check.py` exits 0 and the subsection cites every figure's command.
- [ ] Plan round 1 through `plan-experiment` as `§3` of this file, with its baseline, measurement commands on `screens`, `photo`, `tp` and `synth`, its stop condition (at least a 30% reduction of the development screenshots' total with `photo` and `tp` no more than 0.1% larger and no photograph more than 0.5% larger), its format duties if it adds a coding method, and its `Origin:` line naming it round 1 of at most 13 for `D05 T01 §9`. Done when: `python3 scripts/todo-graph.py validate` exits 0 with `§3` in Phase 8 of the plan.
- [ ] Commit: `"research: where the bytes go on screenshots"`

**Test checkpoint:** Corpus measurement: the attribution table and the repetition figures for the ten development screenshots quoted with their commands; `docs/research.md` gains the subsection; `§3` exists with a stop condition and `python3 scripts/todo-graph.py validate` exits 0. No held-out image is read by any command of this section.

## Verification

- [ ] `python3 scripts/todo-graph.py validate` clean
- [ ] `python3 scripts/todo-claims.py` holds for this file
- [ ] The Milestone 9 gate (`D05 T01 §9`) lists every round of this file with its held-out figures
