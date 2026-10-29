---
schema_version: 1
id: parallel-and-simd
domain: 02-speed
status: draft
title: "TODO-02 -- Parallel Strips and the SIMD Mixer: Measurements"
depends_on: []
track: S2
---

# TODO-02 -- Parallel Strips and the SIMD Mixer: Measurements

> **Goal:** Two speed ideas that change the bitstream are measured before anyone ports them: the size cost of coding an image as independent horizontal strips (which lets decode use more than three threads), and the speed gain of an int16 SIMD mixer (which costs 0.09% in size). Each ends as an accepted or rejected result that `D01 T03 §2` or a later format version acts on.

> [!IMPORTANT]
> **Current state (verified 2026-10-25; baseline timings remain 2026-09-29):** Method 1 runs one thread per channel, three in all; a 12-thread CPU leaves nine idle. The mixer uses int32 weights with an int64 dot product and a `_mm_madd_epi16` rounded update into those int32 weights (`D02 T01 §4`); the research harness measured an int16 mixer at scale 2^12 with a rounded saturating update at +0.09% size against it (`docs/research.md` section 3.3), speed unmeasured. Decode of `measure/image.bmp` takes 0.58 of JPEG XL's time with three threads (`benchmarks/results.md`, 2026-09-29).
<!-- claim: count "_mm_madd_epi16\(_mm_unpacklo_epi16\(sv, ones\), ev\)" src/cm.c = 2 -->
<!-- claim: count "int64_t dot = " src/cm.c = 2 -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- sections 5.3 (strips) and 5.4 (the SIMD mixer)
- [`research/ab.py`](../../research/ab.py) -- `size` (`ab.build` and `ab.parse_bench_rows`) for the strip cost, `ab` for the mixer speed
- -> XREF: D01 T03 §2 -- ports strips into format version 3 when §1 accepts their size cost

## Outcome

- The size cost of 2 and 4 strips is measured on `photo` and `tp`, and accepted (at most 0.3% for 4 strips) or rejected.
- The int16 mixer's decode speed is measured; it is accepted for a later format version only if decode gets at least 5% faster.

**Adjacency:** all=not-applicable (research measurements; nothing here changes what the tool reads, writes, or reports)

**Adjacency rationale:** The format duties belong to the port (`D01 T03 §2` for strips, a later version for the mixer).

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | The size cost of horizontal strips            | --         |  [ ]   |
|   2   |   §2    | The speed of an int16 SIMD mixer              | --         |  [ ]   |

---

## 1. The Size Cost of Horizontal Strips

> **Started:** 2026-10-29T18:20:58Z

A strip starts with fresh model state, so it pays the learning cost again. The cost is measured without writing any strip code: code each strip as a separate image and add up the sizes. Bar: 4 strips cost at most 0.3% on the `photo` total, confirmed on `tp`.

**Corrected 2026-10-29:** (1) `docs/research.md` section 5.3 is horizontal strips and section 5.4 is the int16 SIMD mixer (swapped in `## Inputs` above). (2) `research/ab.py size` requires `ppm/<name>.ppm` paths and per-image `jxl_refs.json` keys (which exist only for the 55 whole corpus images, not for strip crops), so `research/strips.py` imports `research/ab.py` (`ab.build`, `ab.parse_bench_rows`, `ab.ppm_dimensions`, `ab.SETS`) to build `build/research/benchS.exe` and measure both the whole images and the strip PPMs written under `build/research/strips/S<n>/<name>_s<s>.ppm`. (3) Coding `S` strips as `S` standalone `.fold` files via `benchS.exe` includes `(S - 1) * 32` extra `.fold` container header bytes per image compared with `D01 T03 §2`'s single-container strip payload (which carries one 32-byte header, a 1-byte strip count `S`, and `3 * S` 4-byte stream sizes); `research/strips.py` reports both the raw summed standalone `.fold` size (`raw_sum`) and the single-container payload-adjusted size (`adj_sum = raw_sum - (S - 1) * 32 + 1` per image), along with percentage deltas (`+X.XXX%`), geometric means of per-image ratios, and worst images, on both the current 7-model `src/cm.c` (matching stamped `D01 T02 §7`, `fb27c31197ce87977979d1527eb1e0ee202c3d9b`, `photo` `30,812,732`, `tp` `6,641,390`) and the 5-model `build/research/cm_m1.c` baseline (matching stamped `D05 T01 §3`, `b181fb1edef63b42d29350a8b24096307a8596ee`, `photo` `30,865,211`, `tp` `6,651,583`). (4) Held-out confirmation requires `S = 4` to cost at most `+0.300%` on `photo` AND at most `+0.300%` on held-out `tp`; if either exceeds `+0.300%`, fixed 4-strip splitting is rejected and the negative result is recorded in `docs/research.md` section 3 (`### 3.4 Speed`). A `--self-test` mode verifies exact row partitioning (`floor(s*H/S)` to `floor((s+1)*H/S) - 1`) and byte-exact reconstruction of the source PPM rows.

- [ ] Add `research/strips.py`, which cuts every image of a set (or subset filtered by `--max-height <H>`) into `S` horizontal strips of rows `floor(s*H/S)` to `floor((s+1)*H/S) - 1` as PPM files under `build/research/strips/S<n>/`, measures them through `research/ab.py` (`ab.build` and `ab.parse_bench_rows`), prints the per-image and total raw summed and container-adjusted sizes, percentage deltas, geometric means, and worst images against the whole-image size, and provides a `--self-test` mode checking exact row partitioning and concatenation. Done when: `python3 research/strips.py --self-test` passes and `python3 research/strips.py photo 2` and `photo 4` print their totals. Cheaper substitute: estimating the cost from one image.
- [ ] Run it for `S = 2` and `S = 4` on `photo` and `tp`, and for `S = 4` on the images of height 768 or less (`python3 research/strips.py photo 4 --max-height 768`, the 24 Kodak images in `photo`, plus all 10 `tp` images which are `<= 768`), on both `src/cm.c` (7-model `D01 T02 §7`) and `--cm build/research/cm_m1.c` (5-model `D05 T01 §3`). Done when: the totals, geometric means, and worst images are quoted.
- [ ] Record the result in `docs/research.md` section 3 with the accept or reject decision against the bar (`<= +0.300%` on `photo` and confirmed `<= +0.300%` on `tp`). Done when: the row exists and `D01 T03 §2`'s context paragraph can cite it.
- [ ] Commit: `"research: measure the size cost of horizontal strips"`

**Test checkpoint:** `python3 research/strips.py --self-test` passes; `python3 research/strips.py photo 4` and `python3 research/strips.py tp 4` outputs (plus `photo 2`, `tp 2`, and `photo 4 --max-height 768`) are quoted against the 0.3% (`+0.300%`) bar on `photo` and held-out `tp`, with the accept/reject decision recorded in `docs/research.md` section 3.

## 2. The Speed of an Int16 SIMD Mixer

**Needs:** Idle machine (timing)

The mixer is about a third of each decision's time. With int16 weights, the dot product and the update become a few SSE instructions. Bar: decode at least 5% faster at one thread (`research/ab.py ab`), for the known +0.09% size.

- [ ] In `build/research/try.c`, keep the five stretch values and the bias input as int16 lanes, compute the dot product with `_mm_madd_epi16` and a horizontal add, and update the weights with a multiply, a rounding add, a shift, and a saturating add (scale 2^12, rounded update with shift 16, as the research harness measured). Done when: `python research/ab.py size build/research/try.c photo` decodes every image and shows the +0.09% cost within 0.02%.
- [ ] Measure with `python research/ab.py ab src/cm.c build/research/try.c -n5 -t1 measure kodim01` and `-t3`. Done when: both ratios are quoted.
- [ ] Record the result in `docs/research.md` section 3; if it clears the bar, save `research/variants/int16-mixer.patch` for a later format version and file its port through `add-todo`. Done when: the row exists, and the patch and the filed section exist exactly when the bar was cleared.
- [ ] Commit: `"research: measure an int16 SIMD mixer"`

**Test checkpoint:** the quoted `ab` ratios against the 5% bar and the `size photo` line against 0.9712.

## Verification

- [ ] Both sections' rows in `docs/research.md` section 3 exist and match their stamps
- [ ] `python scripts/todo-graph.py validate` clean
