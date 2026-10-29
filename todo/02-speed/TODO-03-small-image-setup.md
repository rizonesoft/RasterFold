---
schema_version: 1
id: small-image-setup
domain: 02-speed
status: draft
title: "TODO-03 -- Small-Image Setup Cost"
depends_on: []
track: S3
---

# TODO-03 -- Small-Image Setup Cost

> **Goal:** Method 1 stops paying a fixed per-image cost for model state a small image never reaches. Today every encode and decode initializes all of its counter tables before the first pixel, so a 4,095-pixel image spends about a quarter of its time on setup. When this file is done, a tiny image's encode and decode are measurably faster, large images are no slower, and every `.fold` byte is unchanged.

> [!IMPORTANT]
> **Current state (verified 2026-10-30):** `cm_alloc` in `src/cm.c` takes one block from `rf_alloc` holding three channels of `NMODEL` (7) x `NROWS` (2048) x `NODES` (32) `uint16_t` counters (`917,504` bytes per channel, `2,752,512` in all) plus `WSEL` (128) x `NODES` x `WSTRIDE` (8) `int32_t` mixer weights per channel (`131,072` bytes). `rf_alloc` already returns zero-filled memory (`HeapAlloc` with `HEAP_ZERO_MEMORY` in `src/sys.c`, `posix_memalign` then `memset` in `src/sys_posix.c`), and `cm_alloc` then writes every counter again: node 0 (the row visit count) to `0`, and every decision node to `32768` (probability one half), and every weight to `19661` or `0`. A row's counters are first read when `kk[k] % NROWS` selects it (`s->cp[k]` in `src/cm.c`); a 4,095-pixel image cannot reach most rows of a 2048-row table more than a few times. The operator's Milestone 4 decision (2026-10-29, quoted in `todo/99-manual/TODO-01-operator-setup.md` Current state) asked for this section: "I'd also file a speed section for the small-image cost (only clear the tables the image uses, or clear them in place)." Baseline (2026-10-30, machine under other load, load average 5 to 7.5; six alternating A/B calls of `python3 scripts/windows.py --exe build/research/benchB.exe -r30 -t<1|3> build/research/ppm/syn_odd.ppm`, best of 180, `ms = cyc * 4095 * 3 / 3.9e6`): `syn_odd` (65x63) at `-t1` encodes in `425` cyc/sample (`1.339` ms) and decodes in `443` (`1.395` ms), and at `-t3` encodes in `360` (`1.134` ms) and decodes in `357` (`1.125` ms); Milestone 3's 5-model build (`build/research/cm_m1.c`, `benchA.exe`) took `330`, `338`, `253`, and `261` cyc/sample, so the two extra models' tables alone cost `+0.30` to `+0.34` ms.
<!-- claim: count "^#define NMODEL 7" src/cm.c = 1 -->
<!-- claim: count "^#define NROWS 2048" src/cm.c = 1 -->
<!-- claim: count "^#define NODES 32" src/cm.c = 1 -->
<!-- claim: count "ch->ctr\[i\] = \(i % NODES\) \? 32768 : 0;" src/cm.c = 1 -->
<!-- claim: count "HEAP_ZERO_MEMORY" src/sys.c = 1 -->

## Inputs

- [`src/cm.c`](../../src/cm.c) -- `cm_alloc` (the table initialization), the counter reads and updates in the per-decision coding path, and `s->cp[k]` (row selection)
- [`src/sys.c`](../../src/sys.c), [`src/sys_posix.c`](../../src/sys_posix.c) -- `rf_alloc`, which already zero-fills
- [`docs/research.md`](../../docs/research.md) -- section 2, the measurement protocol, and section 3, the "Already tried" table that records a miss
- [`research/ab.py`](../../research/ab.py) and [`research/bench.c`](../../research/bench.c) -- `ab` for set totals, and the `cyc/sample` field of `bench.c` for sub-millisecond images
- [`todo/05-milestones/TODO-01-beat-jxl-and-webp.md`](../05-milestones/TODO-01-beat-jxl-and-webp.md) -- Milestone 4's checkpoint, which measured this cost and allowed images under 16,384 pixels a fixed `+1.0` ms instead of the ratio

## Outcome

- `syn_odd` encodes and decodes at least 10% faster than the baseline at `-t1` and at `-t3`, measured by the `cyc/sample` protocol below, or the miss is recorded as a negative result.
- No set total (`photo`, `tp`, `synth`) gets slower by more than 0.5% at `-t1` or `-t3`, and every `.fold` byte is unchanged.

**Adjacency:** all=not-applicable (internal state setup; the files read and written, the options, and the output are unchanged)

**Adjacency rationale:** Only when the codec's model state is initialized changes; the format, the decoder's checks, and the output bytes stay as they are, which the known-answer vectors and the oracle prove.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Initialize only the model state an image uses | --         |  [ ]   |

---

## 1. Initialize Only the Model State an Image Uses

**Needs:** Idle machine (timing)

Every method 1 encode and decode writes all `2.75` MB of counters and `384` KB of weights before the first decision, on top of the allocator's own zero-fill, whatever the image's size. Two designs remove that pass without changing any coded probability, and both are tried in a copy of `src/cm.c` before either is ported. (a) **Biased storage:** store each decision counter as `p ^ 0x8000`, so its starting value of `32768` is stored as `0` and the allocator's zero-fill is the whole initialization; every counter read and write in the coding path applies the same XOR, and node 0 (the visit count, starting at `0`) is stored as is. (b) **First-touch rows:** keep a per-model bitmap of initialized rows (`NROWS` bits per model per channel), test it where `s->cp[k]` selects a row, and write that row's `NODES` starting values on its first use. The weights are left as they are unless (a) or (b) clears the bar and the weights' share is then still measurable. The candidates are listed here before any run; no constant, branch, or rule looks at an image's dimensions, name, hash, or colour statistics. Bar: keep the faster passing candidate only if `syn_odd` is at least 10% faster (min `cyc/sample` ratio `<= 0.900`) for encode and decode at both `-t1` and `-t3`, no set total is slower than `1.005` at `-t1` or `-t3`, and every byte is unchanged; otherwise ship the negative result and do not port.

- [ ] Re-measure the baseline on an idle machine (load average under 1) with six alternating calls each of the current and an unchanged `bench.exe` (`python3 scripts/windows.py --exe build/research/benchA.exe -r30 -t<1|3> build/research/ppm/syn_odd.ppm`, `benchA.exe` and `benchB.exe` both built from `src/cm.c` by `python3 scripts/windows.py research/ab.py ab src/cm.c src/cm.c syn_odd -n1`), recording the best `cyc/sample` of 180. Done when: the four baseline figures and their A-vs-A spread are quoted with the date and load. Cheaper substitute: the `ms` columns, which round a 1 ms image to a tenth.
- [ ] Build candidate (a) in `build/research/try-a.c` and candidate (b) in `build/research/try-b.c`, each a copy of `src/cm.c`. Done when: `python3 scripts/windows.py research/ab.py size build/research/try-a.c photo` (then `tp` and `synth`) and the same for `try-b.c` print the same totals as `src/cm.c` (`30,812,732`, `6,641,390`, `206,229` on 2026-10-30) with every image round-tripping.
- [ ] Measure `syn_odd` for each candidate against `src/cm.c` by the `cyc/sample` protocol at `-t1` and `-t3`, and the set totals with `python3 scripts/windows.py research/ab.py ab -v src/cm.c build/research/try-<a|b>.c <photo|tp|synth> -n5 -t<1|3>`, repeating any figure within 1% of the bar until two runs agree. Done when: every ratio is quoted against the bar.
- [ ] If a candidate clears the bar, port the faster one into `src/cm.c`. Done when: `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` pass with `KAT_FOLD3`, `KAT_PAL3`, `KAT_PAL2_3` and the version 2 vectors unchanged, `python3 tests/fuzz_decode.py 300 1` reports no crash, hang, or mismatch, `python3 scripts/windows.py research/ab.py compat <the commit before the port>` (no `--allow-diff`) reports `identical=True` on `photo`, `tp`, `tp-only`, and `synth`, and the threads `1` and `3` outputs are byte-identical. Struck with a pointer to the stamp when neither candidate clears the bar.
- [ ] Record the result in `docs/research.md`: a section 3 row (the winner and its figures, or both candidates' figures in the "Already tried" table). Done when: the row exists and its figures match the stamp.
- [ ] Commit: `"speed: initialize only the model state an image uses"`

**Test checkpoint:** the quoted baseline and candidate `cyc/sample` figures for `syn_odd` at `-t1` and `-t3` against the `0.900` bar, the `ab -v` set totals at `-t1` and `-t3` against `1.005`, and, when ported, `python3 tests/test_correctness.py` output with the known-answer vectors unchanged and the `size` totals equal to `30,812,732`, `6,641,390`, and `206,229`.

## Verification

- [ ] `python3 scripts/check-all.py` -- exits 0: build, correctness suite, fuzz, TODO gates green
- [ ] `python3 tests/test_correctness.py` exits 0 with the known-answer vectors unchanged
- [ ] The section's `docs/research.md` row exists and matches its stamp
- [ ] `python3 scripts/todo-graph.py validate` clean
