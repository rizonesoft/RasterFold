---
schema_version: 1
id: exact-repeats
domain: 01-compression
status: draft
title: "TODO-04 -- Exact Repeats: a Match Model"
depends_on: []
frozen: true
track: C4
---

# TODO-04 -- Exact Repeats: a Match Model

> **Goal:** Content that repeats exactly (gradients whose rows are shifted copies, stripes, UI panels, text) is predicted from the earlier copy, so the generated images JPEG XL e9 and WebP beat today become smaller than JPEG XL e9, with photographs no worse.

> [!IMPORTANT]
> **Current state (verified 2026-09-29; re-verified 2026-10-29):** Method 1 predicts only from the nearest neighbours; nothing looks further back. Sizes in bytes at authoring (`python research/ab.py size synth -v` and the 2026-09-29 e9 and WebP run): `syn_gradient` 4,521 (JPEG XL e9 616, WebP 180), `syn_stripes` 384 (e9 271, WebP 150), `syn_ui` 2,107 (e9 2,195, WebP 1,346), `syn_text` 2,118 (e9 2,038, WebP 1,198). On current `src/cm.c` (7 models, `D01 T03 §1`) and `src/pal.c` (method 3, `D01 T01 §4`), `python3 research/ab.py size src/cm.c synth -v` measures `syn_gradient` 4,227, `syn_stripes` 368, `syn_ui` 1,410 (already below e9 via method 3), and `syn_text` 1,447 (already below e9 via method 3), with `synth` total 206,229, `photo` total 30,812,732, and `tp` total 6,641,390. Design notes: `docs/research.md` section 7, item 4, and backlog entry `B-003` promoted here.
<!-- claim: count "match" src/cm.c = 0 -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- section 7 item 4 and section 8 (lpaq's match model, WebP's backward references)
- [`src/cm.c`](../../src/cm.c) -- `cm_model` and `code_bit`, where a match model adds a context and a mixer input
- -> XREF: D01 T03 §1 -- format version 3, which §3 extends before the 1.0 release freezes it

## Outcome

- `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` are below their JPEG XL e9 sizes.
- The photo total is no worse than milestone 4's.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no option); reporting=not-applicable (no new report); notifications=not-applicable (no new message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T04 §3; reverse=applicable @ D01 T04 §3

**Adjacency rationale:** The port changes what the encoder exports and the decoder imports, so both ship in one section.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Where the repeats are | -- |  [x]   |
|   2   |   §2    | A match model, measured | §1 |  [ ]   |
|   3   |   §3    | The match model in format version 3 | §2, D01 T03 §1 |  [ ]   |

---

## 1. Where the Repeats Are

> **Started:** 2026-10-27T22:58:21Z

Before designing the model, measure what it could catch: how many samples equal a sample at a fixed offset earlier in the image.

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T04-s1.md`):** three gaps folded in. (1) The checkpoint's "shifted-row share above 90%" is unattainable on `syn_gradient` as generated (`tests/test_correctness.py`: R = `3x mod 256`, G = `5y mod 256`, B = `(x + 2y) mod 256`): one common shift matches at most one third of samples, and per-channel best shifts give R 100% at shift 0, G 0%, B 100% at shift +2 (pooled 66.67%); the checkpoint below now states those exact expectations, verified against the corpus PPM before this correction was written. (2) Both metrics are now defined exactly in item 1: raw RGB channel planes (not the codec's signed CM planes, because §2's design question is where content repeats, before any transform), per-(image, channel) best shift, retrospective coverage for the match metric, and denominators over rows 1 to H-1 only. (3) The earlier-row metric gets exact-count fixtures through a `--self-test` mode, and the noise tolerance is numeric.

- [x] Add `research/repeats.py`, which reports per image, on raw RGB channel planes: (A) for each channel and each shift `-8..8`, the share of samples in rows 1 to H-1 equal to the sample one row above at that shift, reporting the best shift and share per channel and their pooled mean; and (B) retrospective earlier-row match coverage: scanning each channel row left to right, the longest match of the current segment against any segment lying entirely in earlier rows of that channel (at most 16 candidate positions per 8-sample key), where a match of 8 or more samples covers those samples and skips past them, reported as covered samples over samples in rows 1 to H-1. A `--self-test` mode checks exact counts on built-in fixtures. Done when: `python3 research/repeats.py synth` and `photo` print both shares per image. Cheaper substitute: eyeballing the images.
- [x] Record the shares in `docs/research.md` section 7 item 4. Done when: the item quotes them.
- [x] Commit: `"research: measure exact repeats in the corpus"`

**Test checkpoint:** `python3 research/repeats.py synth` shows `syn_gradient` metric A per-channel best shares of 100% (shift 0), 0%, and 100% (shift +2), pooled 66.67%, and `syn_noise` pooled metric A below 1% with metric B at 0%; `python3 research/repeats.py --self-test` passes its exact-count fixtures (an 8-sample earlier-row match covers 8 samples, a 7-sample match covers none, a match may not cross into the current row, and overlapping coverage is counted once).

> **Verified:** 2026-10-29 | §1 | 64c66a977c99d78f3ad39ee1d8b6ee5849552f3c
> **Review:** `docs/reviews/d01-t04-s1.json`
## 2. A Match Model, Measured

> **Started:** 2026-10-29T18:02:34Z

**Needs:** ffmpeg with libjxl (benchmarks)

-> SOURCE: webp-gap-match-model

The model keeps, per channel, a pointer to the best earlier match (found by hashing the last 8 samples of the current row into a table of positions, and following the pointer while predictions hold); its prediction and match length become a context model and a mixer input. Bar: the four target images below JPEG XL e9 with the photo total no more than 0.05% worse; otherwise the negative result ships.

**Corrected 2026-10-29:** `src/cm.c` now has 7 models (`NMODEL 7`, `WSTRIDE 8`) after `D01 T03 §1`, so adding the match model in `build/research/try.c` makes it an eighth model (`NMODEL 8`, `WSTRIDE 16`, weights `0..7` initialized to `19661` and bias `64` at slot `8`, with the mixer dot product and weight update covering all 9 slots `0..8`). Because `ch->rows` is a 3-row ring buffer (`ro0`, `ro1`, `ro2`), `build/research/try.c` allocates a causal plane buffer `(size_t)w * h * sizeof(int16_t)` and a `2^16`-entry `uint32_t` hash table (`0xFFFFFFFFu` empty) per channel, keeping 1-thread and 3-thread outputs byte-identical. At sample `(x, y)` in channel `c`, before coding `pv`, if an active match pointer `match_pos < (uint32_t)(y * w)` exists (`match_len > 0`), the match prediction is `match_pv = plane[match_pos]`, the clamped residual is `d_m = clamp(match_pv - pred, -15, 15)` shifted by `+15` into `0..30`, and `k7 = (uint32_t)((d_m + 15) * 16 + min(match_len, 15))` (`0..495 < NROWS`); when `match_len == 0`, `k7 = 240` (`d_m = 0`, `match_len = 0`). After coding/decoding `pv = plane[y * w + x]`, if `match_len > 0 && pv == match_pv && match_pos + 1 < (uint32_t)(y * w)`, advance `match_pos++` and `match_len++`; else reset `match_len = 0`, and when `match_len == 0 && y > 0 && x >= 7`, hash `plane[y * w + x - 7 .. y * w + x]` into `htab[h16]`: if `cand = htab[h16]` satisfies `cand >= 7 && cand + 1 < (uint32_t)(y * w)` and all 8 samples `plane[cand - 7 .. cand]` equal `plane[y * w + x - 7 .. y * w + x]`, set `match_pos = cand + 1` and `match_len = 8`. At the end of row `y`, all 8-sample positions ending in row `y` (`pos >= 7`) are written into `htab` so matches only reference strictly earlier rows. Both acceptance and rejection outcomes are covered by the checkpoint below.

- [ ] In `build/research/try.c`, add the causal channel plane buffer, the match pointer, a 2^16-entry hash of positions per channel, and an eighth model (`NMODEL 8`, `WSTRIDE 16`) whose context is `(clamp(match_pv - pred, -15, 15) + 15) * 16 + min(match length, 15)` (`240` when `match_len == 0`). Done when: `python3 research/ab.py size build/research/try.c synth -v` decodes every image.
- [ ] Measure `synth`, `photo`, and `tp`, and the `-t1` (and `-t3`) decode ratio (`python3 research/ab.py ab src/cm.c build/research/try.c -n5 measure kodim01`). Done when: the summary lines and the four target sizes are quoted.
- [ ] Record the result in `docs/research.md` section 3 and save a winner as `research/variants/match-model.patch` if and only if the bar is cleared. Done when: the row exists, and the patch exists exactly when the bar was cleared.
- [ ] Commit: `"research: measure a match model"`

**Test checkpoint:** the four target sizes against 616 (`syn_gradient`), 271 (`syn_stripes`), 2,195 (`syn_ui`), and 2,038 (`syn_text`) bytes, and the `photo` total against milestone 4's `30,812,732` bytes (bar: all four below JPEG XL e9 and `photo <= 30,828,138`; if missed, `docs/research.md` section 3 records the negative result with full `synth`, `photo`, and `tp` summary lines and `-t1`/`-t3` decode ratios, and `research/variants/match-model.patch` is absent).

## 3. The Match Model in Format Version 3

Ports an accepted match model into version 3 before the 1.0 release freezes it (the pre-release rule in `todo/README.md`, Frozen behavior). If §2 rejected the model, this section records that and strikes its items.

- [ ] Apply `research/variants/match-model.patch` to the version 3 path of `src/cm.c`, keeping version 2 decoding unchanged. Done when: `build.ps1` exits 0 and the decoder imports every version 2 and version 3 file back to its pixels (the reverse of the export).
- [ ] Write the model into `tests/rfi_ref.py` from the spec change, update `spec/FOLD.md`, and regenerate `KAT_FOLD3`. Done when: codec and oracle agree on every round-trip case.
- [ ] Commit: `"codec: version 3 predicts exact repeats from a match model"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case; `python research/ab.py size synth -v` shows the four target images below JPEG XL e9.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
