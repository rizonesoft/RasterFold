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

- [`docs/research.md`](../../docs/research.md) -- sections 5.3 (the SIMD mixer) and 5.4 (strips)
- [`research/ab.py`](../../research/ab.py) -- `size` for the strip cost, `ab` for the mixer speed
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

A strip starts with fresh model state, so it pays the learning cost again. The cost is measured without writing any strip code: code each strip as a separate image and add up the sizes. Bar: 4 strips cost at most 0.3% on the `photo` total, confirmed on `tp`.

- [ ] Add `research/strips.py`, which cuts every image of a set into `S` horizontal strips of rows `floor(s*H/S)` to `floor((s+1)*H/S) - 1` as PPM files under `build/research/strips/S<n>/`, runs `research/ab.py size` over them, and prints the summed size against the whole-image size per image and in total. Done when: `python research/strips.py photo 2` and `photo 4` print their totals. Cheaper substitute: estimating the cost from one image.
- [ ] Run it for `S = 2` and `S = 4` on `photo` and `tp`, and for `S = 4` on the images of height 768 or less (the ones a height-based rule would split least). Done when: the totals and the worst images are quoted.
- [ ] Record the result in `docs/research.md` section 3 with the accept or reject decision against the bar. Done when: the row exists and `D01 T03 §2`'s context paragraph can cite it.
- [ ] Commit: `"research: measure the size cost of horizontal strips"`

**Test checkpoint:** `python research/strips.py photo 4` output quoted against the bar.

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
