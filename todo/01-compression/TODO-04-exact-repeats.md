---
schema_version: 1
id: exact-repeats
domain: 01-compression
status: active
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
- -> XREF: D01 T03 §1 -- format version 3, which §2 measures against and §3 extends before the 1.0 release freezes it
- [`docs/research.md`](../../docs/research.md) -- section 4.9 (exact modular planes), the design §4 measures and §5 ports

## Outcome

- `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` are below their JPEG XL e9 sizes.
- The photo total is no worse than milestone 4's.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no option); reporting=not-applicable (no new report); notifications=not-applicable (no new message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T04 §3; reverse=applicable @ D01 T04 §3

**Adjacency rationale:** The port changes what the encoder exports and the decoder imports, so both ship in one section.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Where the repeats are | -- |  [x]   |
|   2   |   §2    | A match model, measured | §1, D01 T03 §1 |  [x]   |
|   3   |   §3    | The match model in format version 3 | §2, D01 T03 §1 |  [x]   |
|   4   |   §4    | Exact modular planes, measured | §3, D01 T03 §1 |  [ ]   |
|   5   |   §5    | Exact modular planes in format version 3 | §4, D01 T03 §1 |  [ ]   |

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

**Corrected 2026-10-29 (plan review `docs/reviews/01-compression/D01-T04-s2.md`):** three gaps folded in. (1) `D01 T03 §1` is now declared in `§2`'s `Depends On` column (`§1, D01 T03 §1`), and `§2` waits until `D01 T03 §1` is stamped `[x]` and names that stamped baseline commit explicitly before accepting or rejecting measurements against the 7-model `30,812,732` `photo` and `6,641,390` `tp` baselines. (2) Item 1 now explicitly verifies 1-thread vs 3-thread compressed byte identity and cross-thread decoding (`1 -> 3` and `3 -> 1`) of `build/research/try.c` across the 55-image research corpus (`photo`, `tp-only`, `synth`). (3) Item 3 and the checkpoint now require held-out `tp` confirmation (`tp` total no worse than the `6,641,390` baseline, per `docs/research.md` section 2.3 item 6) alongside the four `synth` target sizes and the `photo` ceiling before saving `research/variants/match-model.patch`; a `tp` regression rejects the variant as overfitted and records the negative result.

**Default 2026-10-30:** the contract leaves `h16` open, so the prototype takes a deterministic multiplicative hash over the 8 int16 samples (`h = (h + (uint16_t)s[k]) * 0x9E3779B97F4A7C15` for `k = 0..7`, top 16 bits). It is a recorded default: another hash changes only which earlier run a collision keeps, and every candidate is verified sample by sample, so changing it costs one re-measurement.

- [x] In `build/research/try.c` (starting from the stamped `D01 T03 §1` `src/cm.c` commit), add the causal channel plane buffer, the match pointer, a 2^16-entry hash of positions per channel, and an eighth model (`NMODEL 8`, `WSTRIDE 16`) whose context is `(clamp(match_pv - pred, -15, 15) + 15) * 16 + min(match length, 15)` (`240` when `match_len == 0`), and verify 1-thread vs 3-thread `.fold` byte identity and cross-thread decoding (`1 -> 3` and `3 -> 1`) across all 55 corpus images (`photo`, `tp-only`, `synth`). Done when: `python3 research/ab.py size build/research/try.c synth -v` decodes every image and 55/55 corpus images produce identical bytes at `-t1` and `-t3` and round-trip across thread counts.
- [x] Measure `synth`, `photo`, and `tp`, and the `-t1` and `-t3` decode ratios (`python3 research/ab.py ab src/cm.c build/research/try.c -n5 -t1 measure kodim01` and `-t3`). Done when: the summary lines, the baseline commit SHA, and the four target sizes are quoted.
- [x] Record the result in `docs/research.md` section 3 and save a winner as `research/variants/match-model.patch` if and only if the bar is cleared (`syn_gradient < 616`, `syn_stripes < 271`, `syn_ui < 2,195`, `syn_text < 2,038`, `photo <= 30,828,138`, and held-out `tp <= 6,641,390`). Done when: the row exists, and the patch exists exactly when the bar and held-out confirmation were cleared.
- [x] Commit: `"research: measure a match model"`

**Test checkpoint:** 55/55 corpus images produce byte-identical `.fold` output at 1 thread and 3 threads and decode across thread counts; the four target sizes are quoted against 616 (`syn_gradient`), 271 (`syn_stripes`), 2,195 (`syn_ui`), and 2,038 (`syn_text`) bytes, the `photo` total against the stamped `D01 T03 §1` `30,812,732` baseline (`<= 30,828,138`), and the held-out `tp` total against `6,641,390` (`<= 6,641,390`); if any condition misses, `docs/research.md` section 3 records the negative result with full `synth`, `photo`, and `tp` summary lines and `-t1`/`-t3` decode ratios, and `research/variants/match-model.patch` is absent.

> **Verified:** 2026-10-30 | §2 | df0ccc95407b67cb98f15880ebefedd507357f9f
> **Review:** `docs/reviews/d01-t04-s2.json`
## 3. The Match Model in Format Version 3

> **Started:** 2026-10-30T00:58:03Z

Ports an accepted match model into version 3 before the 1.0 release freezes it (the pre-release rule in `todo/README.md`, Frozen behavior). If §2 rejected the model, this section records that and strikes its items.

**Rejected 2026-10-30:** §2 rejected the model (stamped `df0ccc9`, `docs/reviews/01-compression/D01-T04-s2.md`): `syn_gradient` 3,326 bytes against the 616 bar and `syn_stripes` 381 against 271, so `research/variants/match-model.patch` does not exist and nothing is ported. The two port items and the port commit are struck (as prose below, outside the checklist) with no owner, because no accepted variant exists to hand on; the measured photo gain is a separate lead in `todo/backlog.md` B-002, not this section's debt. What remains is proving version 3 unchanged: `src/cm.c`, `tests/rfi_ref.py`, `spec/FOLD.md`, and `tests/test_correctness.py` (with its `KAT_FOLD3`) carry no change since the `D01 T03 §1` stamp `207073e`, and the correctness suite passes.

**Port steps struck by the rejection** (the plan as written before §2 measured; never built, no owner):

~~Apply `research/variants/match-model.patch` to the version 3 path of `src/cm.c`, keeping version 2 decoding unchanged. Done when: `build.ps1` exits 0 and the decoder imports every version 2 and version 3 file back to its pixels (the reverse of the export).~~

~~Write the model into `tests/rfi_ref.py` from the spec change, update `spec/FOLD.md`, and regenerate `KAT_FOLD3`. Done when: codec and oracle agree on every round-trip case.~~

~~Commit: `"codec: version 3 predicts exact repeats from a match model"`~~

- [x] Record the rejection: the **Rejected 2026-10-30** note above cites §2's stamp and its failed bars, and the three port steps are struck as prose outside the checklist. Done when: the note and the struck steps are in this section.
- [x] Confirm version 3 is unchanged by the rejection: `git diff --stat 207073ea371317ef34ce3230ba6836bd0d445e6f HEAD -- src/cm.c src/rc.h src/rf.h tests/rfi_ref.py spec/FOLD.md tests/test_correctness.py` prints nothing, `research/variants/match-model.patch` is absent, and `python3 tests/test_correctness.py` passes every case. Done when: all three are quoted. Done 2026-10-30: the `git diff --stat` printed nothing, `test -e research/variants/match-model.patch` reports it absent, and `.venv/bin/python tests/test_correctness.py` (against `bin/linux-x64/bin/rasterfold` built after the last `src/` change) printed `39 passed, 0 failed`, including `OK   known-answer` (the `KAT_FOLD` freeze check) and `OK   oracle`.
- [x] Commit: `"todo: record the match model's rejection in version 3"`

**Test checkpoint:** `python3 tests/test_correctness.py` passes every case on the unchanged version 3 path, and the `git diff --stat` above prints nothing. The accepted-model checkpoint (`python research/ab.py size synth -v` showing the four target images below JPEG XL e9) does not apply: §2's rejection left nothing to port, and the missed target passes to `D05 T01 §5`'s missed-milestone policy.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

> **Verified:** 2026-10-30 | §3 | 3bc5bdb6a23447e2aa24d8ccadee9ee96d4d7ef8
> **Review:** `docs/reviews/d01-t04-s3.json`
## 4. Exact Modular Planes, Measured

**Origin:** discovered run=28dc11967304 2026-10-30 -- missed-milestone policy (`D99 T01 §4`) after `D01 T04 §2` missed the Milestone 5 `syn_gradient` and `syn_stripes` bars (`D05 T01 §5`); follow-up 1 of up to 3

**Needs:** Idle machine (timing)

Implements `docs/research.md` section 4.9 as a prototype in `build/research/try.c` and measures it against Milestone 5's bars. The two images the match model could not bring under JPEG XL e9 are exact linear planes modulo 256 in every channel, and their bytes go to wraparounds and row starts, not to a per-sample floor (section 4.9 quotes the per-stream sizes). A flag coded when a modular gradient was exact at W and at N says "this sample equals the modular gradient" and skips the residual.

Baseline, measured 2026-10-30 on `src/cm.c` (unchanged since `D01 T03 §1`'s `207073e`): `python3 research/ab.py size src/cm.c synth -v` gives `synth` total 206,229 (`syn_gradient` 4,227, e9 616; `syn_stripes` 368, e9 271; `syn_ui` 1,410, e9 2,195; `syn_text` 1,447, e9 2,038); `photo` total 30,812,732 and `tp` total 6,641,390 (`D01 T04 §2`'s re-measured baseline, same code); `python3 research/ab.py bench -r3 -t1 measure kodim01` TOTAL enc 760.5 ms, dec 793.8 ms; `-t3` enc 402.3 ms, dec 405.1 ms.

Design, fixed before any run (the implementer does not choose these): for each channel, on that channel's own coded sample values `v` (0 to 255), `gp(x, y)` is `(W + N - NW) & 255` when `x > 0` and `y > 0`, `(2N - NN) & 255` when `x == 0` and `y >= 2`, `(2W - WW) & 255` when `y == 0` and `x >= 2`, and undefined otherwise; `ex(x, y)` is true when `gp(x, y)` is defined and `v(x, y) == gp(x, y)`. The gate is `x > 0 && y > 0 && ex(x - 1, y) && ex(x, y - 1)`. Inside the gate the encoder codes one binary decision `f = (v == gp)` on the channel's range coder before any residual decision, with a 16-bit probability from one of 64 direct counters per channel, chosen by `min(run, 15) * 4 + ex(x + 1, y - 1) * 2 + ex(x - 1, y - 1)` (`ex(x + 1, y - 1)` is false in the last column; `run` counts the consecutive gated hits in this row ending at `x - 1`, reset at each row start and by a gated miss), starting at one half and updated with the codec's existing visit-count rate schedule (`RATE`, the visit count capped at 60 as for the model rows). On `f == 1` the sample is `gp` and no residual decision is coded; the residual `r = fold(v - predv)` still feeds every later state update (`r0`, LMS, the WP errors, `xe`, `xf`) exactly as if it had been coded, while the residual counters and mixer weights are not updated. Outside the gate, and on `f == 0`, the residual is coded as today. The parameter grid is listed now and nothing else is tried: the counter context (the 64 above, or `min(run, 15)` alone) times the rate (the `RATE` schedule, or a fixed shift of 5), four variants; `photo` picks among variants that clear every bar, `tp` confirms.

- [ ] Build `build/research/try.c` from `src/cm.c` with the design above (version 3 path only), and check it: one-thread and three-thread output byte-identical and round-tripping on all 55 distinct corpus images (`synth`, `photo`, `tp`). Done when: the identity count is quoted (55/55) with the command that produced it.
- [ ] Measure each of the four variants with `python3 research/ab.py size build/research/try.c synth -v`, `photo`, and `tp`, and the chosen one's time with `python3 research/ab.py ab src/cm.c build/research/try.c -n5 -t1 measure kodim01` and `-t3`, alternating. Done when: every figure is quoted against the bar: `syn_gradient` below 616 and `syn_stripes` below 271 bytes, `syn_ui` below 2,195 and `syn_text` below 2,038, `photo` total at most 30,828,138 (baseline +0.05%) with no `photo` image more than 0.1% larger, `tp` total at most 6,644,710 (baseline +0.05%), and min-time encode and decode B/A at most 1.03 at `-t1` and at `-t3` (Milestone 5's no-regression rule).
- [ ] On a pass, export the winner as `research/variants/exact-plane.patch` against `src/cm.c`; on a miss, record the negative result as a row of `docs/research.md` section 3.2 with the full `synth`, `photo`, and `tp` summary lines, update section 4.9, and leave no patch. Done when: the patch exists with the bar quoted beside it, or the row does.
- [ ] Commit: `"research: measure exact modular planes"`

**Test checkpoint:** `python3 research/ab.py size build/research/try.c synth -v` shows the chosen variant's `syn_gradient` and `syn_stripes` against 616 and 271, `photo` and `tp` totals are quoted against 30,828,138 and 6,644,710, the `ab` ratios against 1.03, and the 55/55 thread identity count; a miss ships the measurement and the section 3.2 row, and §5 takes its rejection path.

## 5. Exact Modular Planes in Format Version 3

**Origin:** discovered run=28dc11967304 2026-10-30 -- missed-milestone policy (`D99 T01 §4`) after `D01 T04 §2` missed the Milestone 5 bars (`D05 T01 §5`); the port of follow-up 1

Ports §4's accepted flag into format version 3 before the 1.0 release freezes it (the pre-release rule in `todo/README.md`, Frozen behavior). If §4 rejected the flag, this section records that: the port items below move out of the checklist as struck prose (an unticked struck item in a shipped section fails `validate`, the lesson of §3), a ticked record item cites §4's stamp, and a ticked item proves version 3 unchanged (`git diff --stat` of `src/cm.c`, `tests/rfi_ref.py`, `spec/FOLD.md`, and `tests/test_correctness.py` from §4's stamp prints nothing, and `python3 tests/test_correctness.py` passes).

- [ ] Apply `research/variants/exact-plane.patch` to the version 3 path of `src/cm.c`, keeping version 2 decoding unchanged. Done when: `python3 scripts/build.py --target linux-x64` and `--target windows-x64` exit 0 with no warning, and the decoder imports every version 2 and version 3 corpus file back to its pixels at `--threads 1` and `--threads 3`.
- [ ] Specify the flag in `spec/FOLD.md` (method 1, version 3: `gp`, `ex`, the gate, the 64 counters and their update, and that a hit codes no residual), write it into `tests/rfi_ref.py` from that text (not from the C), and regenerate `KAT_FOLD3` in `tests/test_correctness.py`. Done when: codec and oracle agree byte for byte on every round-trip case, including the `gradient` and `stripes` cases.
- [ ] Run `python3 tests/fuzz_decode.py 300 1` on Linux and Windows. Done when: every mutated file is rejected or decoded without a crash, with the seed quoted.
- [ ] Commit: `"codec: version 3 codes exact modular planes with a flag"`

**Test checkpoint:** `python3 tests/test_correctness.py` passes every case with the oracle agreeing; `python3 research/ab.py size src/cm.c synth -v` shows `syn_gradient` below 616 and `syn_stripes` below 271 bytes.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
