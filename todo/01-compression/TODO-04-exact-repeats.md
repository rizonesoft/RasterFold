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
> **Current state (verified 2026-09-29):** Method 1 predicts only from the nearest neighbours; nothing looks further back. Sizes in bytes (`python research/ab.py size synth -v` and the 2026-09-29 e9 and WebP run): `syn_gradient` 4,521 (JPEG XL e9 616, WebP 180), `syn_stripes` 384 (e9 271, WebP 150), `syn_ui` 2,107 (e9 2,195, WebP 1,346), `syn_text` 2,118 (e9 2,038, WebP 1,198). Design notes: `docs/research.md` section 7, item 4, and backlog entry `B-003` promoted here.
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
|   1   |   §1    | Where the repeats are | -- |  [ ]   |
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

## 2. A Match Model, Measured

**Needs:** ffmpeg with libjxl (benchmarks)

-> SOURCE: webp-gap-match-model

The model keeps, per channel, a pointer to the best earlier match (found by hashing the last 8 samples of the current row into a table of positions, and following the pointer while predictions hold); its prediction and match length become a context model and a mixer input. Bar: the four target images below JPEG XL e9 with the photo total no more than 0.05% worse; otherwise the negative result ships.

- [ ] In `build/research/try.c`, add the match pointer, a 2^16-entry hash of positions per channel, and a sixth model whose context is `(predicted residual clamped to ±15) * 16 + min(match length, 15)`. Done when: `python research/ab.py size build/research/try.c synth -v` decodes every image.
- [ ] Measure `synth`, `photo`, and `tp`, and the `-t1` decode ratio. Done when: the summary lines and the four target sizes are quoted.
- [ ] Record the result in `docs/research.md` section 3 and save a winner as `research/variants/match-model.patch`. Done when: the row exists, and the patch exists exactly when the bar was cleared.
- [ ] Commit: `"research: measure a match model"`

**Test checkpoint:** the four target sizes against 616, 271, 2,195, and 2,038 bytes, and the photo total against milestone 4's.

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
