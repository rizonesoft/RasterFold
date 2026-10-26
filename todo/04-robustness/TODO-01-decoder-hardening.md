---
schema_version: 1
id: decoder-hardening
domain: 04-robustness
status: done
title: "TODO-01 -- Decoder Hardening"
depends_on: []
track: R1
---

# TODO-01 -- Decoder Hardening

> **Goal:** A file that lies about its size costs the decoder milliseconds, not seconds, and the decoder has survived a long fuzz campaign from seeds that reach every method, with the seeds and results on record.

> [!IMPORTANT]
> **Current state (verified 2026-10-26):** After §1 (`cm_t.abort` in `src/cm.c`), when `!enc` any CM channel whose `dec.in` reaches `dec.lim` (`end + 2` after `rdec_tail` in `src/rc.h`) at the end of a row sets `bad = 1` and release-stores `abort = 1`, and every channel checks `abort` with acquire ordering after `wait_done` and at row end and release-stores `done = h` on exit so no thread blocks in `wait_done` (including `y >= XROWS` backpressure); a 4096x4096 CM header with a minimal 27-byte CM payload (`[5, 5, 5]` + three 5-byte `0x80` streams) drops from `13.34 s` (`--threads 1`) / `3.94 s` (`--threads 3`) to `25.02 ms` (`--threads 1`) / `20.43 ms` (`--threads 3`) before exiting with code 4 (`invalid fold: cm stream`). After §2 (`tests/fuzz_decode.py` and `docs/fuzz.md`), `seed_records()` builds 15 seeds covering methods `0`, `1`, `2`, and `3` with `w >= 36`, `h >= 34 > XROWS = 32`, and `w * h > 1200`, `mutate()` applies method-specific mutations across all four methods with checksums recomputed, `verify_mutation()` checks bidirectional agreement with `tests/rfi_ref.py` on both acceptance (`returncode == 0`) and refusal (`returncode == 4`), and the 20,000-mutation campaign (`python3 tests/fuzz_decode.py 5000 1..4`) records `19,061` rejected and `939` valid files with no crash, hang, or mismatch. After §3, `tests/rfi_ref.py` and `spec/FOLD.md` agree on BMP padding, acceptance bounds, and parenthesized counter updates.
<!-- claim: count "d->in = d->in > d->lim \? d->lim : d->in;" src/rc.h = 1 -->
<!-- claim: exists tests/fuzz_decode.py -->
<!-- claim: exists docs/fuzz.md -->

## Inputs

- [`src/cm.c`](../../src/cm.c) -- `rdec_bit`, `run_channel`, and `run_pipelined`, where §1 adds the early exit
- [`tests/fuzz_decode.py`](../../tests/fuzz_decode.py) -- the fuzzer §2 extends
- [`docs/research.md`](../../docs/research.md) -- sections 5.5 and 7.1
- -> XREF: D01 T01 §2, D01 T01 §4 -- method 2, whose decoder §2's seeds must reach

## Outcome

- A 4096x4096 header with a 20-byte CM payload fails with exit code 4 in under a second.
- A fuzz campaign of at least 20,000 mutations over seeds covering methods 0, 1, and (once it exists) 2 reports no crash, hang, or oracle mismatch, with its seeds quoted.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no option); reporting=not-applicable (the existing messages); notifications=applicable @ D04 T01 §1; permissions=applicable @ D04 T01 §1; audit=not-applicable (git history is the audit); exchange=applicable @ D04 T01 §2; reverse=not-applicable (no encoder change)

**Adjacency rationale:** Notifications and permissions are the refusal: a hostile file is refused quickly with exit 4 and the existing message. Exchange is every method a decoder must refuse cleanly when malformed.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Stop decoding a stream that has run out       | --         |  [x]   |
|   2   |   §2    | A long fuzz campaign over every method        | §1, D01 T01 §2, D01 T01 §4 |  [x]   |
|   3   |   §3    | Oracle and spec agree on padding and counters | --         |  [x]   |

---

## 1. Stop Decoding a Stream That Has Run Out

> **Started:** 2026-10-25T16:36:30Z

**Job:** A user who opens a damaged or hostile file can see it refused within a second, by the same notification as today (exit 4 and one message line), under the unchanged file-safety policy.

Once a channel's read pointer has reached `end + 2`, the file will be rejected anyway, so every further sample is wasted work. The channel threads wait on each other's progress counters, so an early exit must release every waiter. The refusal stays the tool's one notification to the user (exit 4 and the existing message line), and the file-safety policy is unchanged.

- [x] Add an `abort` flag (`volatile int abort;`, initialized to `0` in `cm_alloc`) to `cm_t` in `src/cm.c`; when `!enc`, after each row in `run_channel` and `run_pipelined`, a channel whose `dec.in` has reached `dec.lim` (`end + 2` after `rdec_tail`) sets its `bad` flag and release-stores `abort = 1`, and every channel checks `abort` with acquire ordering both after `wait_done` before starting a row and at the end of the row, then stops. Done when: a truncated stream is rejected with the same message as today (`invalid fold: cm stream`). Cheaper substitute: a check per sample, which costs the hot loop. **Corrected 2026-10-25:** guarded the `dec.in >= dec.lim` and `abort` checks with `if (!enc)` and specified `cm_alloc` initialization plus acquire/release atomic ordering on `m->abort` and an acquire check immediately after `wait_done` before processing row `y` per the `--kind plan` review (`P2`).
- [x] Publish `done = h` (`__atomic_store_n(&m->ch[c].done, (int32_t)m->h, __ATOMIC_RELEASE)`) from every stopping channel when exiting `run_channel` on `!enc` (after storing `abort = 1` when triggering abort) so any channel waiting on it in `wait_done` (including the `y >= XROWS` backpressure wait on `ch[2].done`) is released and immediately sees `abort == 1` before running another row. Done when: no thread waits forever when any one channel stops early (a test with each channel's stream truncated in turn on an image taller than `XROWS` completes under a bounded timeout at both `--threads 1` and `--threads 3`). **Corrected 2026-10-25:** release-store `done = h` on loop exit after setting `abort = 1` and test with `h > XROWS` per the `--kind plan` review (`P2`).
- [x] Add cases to `tests/test_correctness.py` that write a 4096x4096 CM header with both a 20-byte payload (pre-`run()` framing check) and a structurally valid 27-byte CM payload (`12`-byte size table `[5, 5, 5]` + three 5-byte `0x80` streams that enter `run()` and exhaust on row 0, checksums recomputed) and assert exit 4 within one second at `--threads 1` and `--threads 3` (verifying that the 27-byte payload takes multi-second full-image decode before the fix and milliseconds after), plus cases truncating each of the three streams in turn on an image taller than `XROWS` (`photo(64, 64)`, `h = 64 > 32`) both at 5 bytes (row-0 exhaustion) and at 75% of the valid stream length (exhausting after row 32 so `y >= XROWS` backpressure waits execute before abort) at `--threads 1` and `--threads 3` with subprocess timeouts. Done when: the cases pass, the refusal notification is the existing exit 4 and message line, and the file-safety policy (no output file, no `.partial` left) holds in each. **Corrected 2026-10-25:** added the structurally valid 27-byte CM payload (`12 + 5 + 5 + 5` bytes) alongside the 20-byte framing case so the timing regression reaches `run()` and `run_channel`/`run_pipelined`, and required both `--threads 1` and `--threads 3` with subprocess timeouts and both row-0 and post-`XROWS` (75% stream length on `64x64`) per-channel truncation per the `--kind plan` and `--kind completion` reviews (`P1`, `P2`, `in-scope`).
- [x] Commit: `"codec: stop decoding a stream that has run out"`

**Test checkpoint:** `truncated-abort` in `tests/test_correctness.py` passes on Linux and Windows (`python3 tests/test_correctness.py`: `34 passed, 0 failed`; `python3 scripts/windows.py tests/test_correctness.py -k truncated-abort`: `1 passed, 0 failed`): before the fix, the 4096x4096 27-byte CM payload (`cm4096_27b`) took `13,337 ms` (`--threads 1`) and `3,944 ms` (`--threads 3`); after the fix, `cm4096_20b` fails with exit 4 (`rasterfold: invalid fold: cm streams`) in `104.75 ms` (`--threads 1`) and `57.51 ms` (`--threads 3`), `cm4096_27b` fails with exit 4 (`rasterfold: invalid fold: cm stream`) in `25.02 ms` (`--threads 1`) and `20.43 ms` (`--threads 3`), and per-channel truncated streams on `photo(64, 64)` (`h = 64 > XROWS = 32`, both 5-byte row-0 truncation `trunc_ch0..2` and 75%-length post-`XROWS` truncation `trunc_mid_ch0..2`) fail with exit 4 (`rasterfold: invalid fold: cm stream`) in under `65 ms` at `--threads 1` and `--threads 3`, with no output BMP or `.partial` file left behind; `python3 tests/fuzz_decode.py 2000 5` reports `2000 mutated files: 1948 rejected, 52 still valid, no crash, hang or mismatch`; `python3 research/ab.py ab <A> src/cm.c measure kodim01 -n5` confirms no speed regression (`-t1`: `B/A: enc 0.994 dec 0.995`; `-t3`: `B/A: enc 1.063 dec 0.981`); and `python3 scripts/build.py --target all` and `python3 scripts/tidy.py` (`16/16`) pass cleanly.

> **Verified:** 2026-10-25 | §1 | 651f3ebf34d7b73580b60f7bdeb0f039d01a0eaf
> **Review:** `docs/reviews/d04-t01-s1.json`
## 2. A Long Fuzz Campaign over Every Method

> **Started:** 2026-10-26T03:08:30Z

**Needs:** Idle machine (timing)

**Job:** A user can import any file of any method and get either identical pixels or a clean refusal, as a 20,000-mutation campaign shows.

The recorded fuzz runs are short. A campaign of 20,000 mutations over seeds that reach every method, including method 2's decoder once `D01 T01 §2` ships it, is the evidence that the decoder refuses cleanly: every method a decoder imports is fuzzed.

> [!NOTE]
> **Corrected 2026-10-26:** After `D01 T01 §2` (`RF_METHOD_PAL = 2u`) and `D01 T01 §4` (`RF_METHOD_PAL2 = 3u`), the decoder imports four methods (`0` raw, `1` CM, `2` 5-bit palette, `3` 7-bit palette with two-colour deduction). `seeds()` in `tests/fuzz_decode.py` builds 15 seeds covering all four methods with asserted dimensions (`w >= 36 and h >= 34 and w * h > 1200`, larger than the previous `40x30`, `17x9`, `64x3`, `32x24`, and `16x8` seeds) and methods -- a `64x48` high-entropy noise image (`method == 0`), larger crops (`h >= 34 > XROWS = 32`: `64x40` for `kodim05.png`, `48x36` for `kodim01.png` and `kodim03.png`, and `36x34` for the remaining 7 photos) of all 10 test-pack photos in `testdata/photos/*.png` (`method == 1`), `64x48` 2-colour and 200-colour images encoded with `method == 2` (`pal_encode(..., method=rf.METHOD_PAL)`), and `64x48` 2-colour and 200-colour images encoded with `method == 3` (`encode_image`) -- ordered with methods `0, 1, 2, 3` in the first 6 slots so `python3 tests/fuzz_decode.py 10 1` mutates files of every method (`0, 1, 2, 3`) and reports per-method mutation counts (`methods 0:<n0>, 1:<n1>, 2:<n2>, 3:<n3>`) alongside the summary line. `mutate()` in `tests/fuzz_decode.py` adds dedicated `method == 0` (`rf.METHOD_RAW`) mutation branches (bit flips, payload truncation/extension, header dimension changes, header `payload_size` mismatch, and random byte replacements) rather than falling through to the `method == 1` 3-stream size table; `verify_mutation()` passes `stdin=subprocess.DEVNULL`, asserts that neither `.bmp` nor `.bmp.partial` remains on exit 4, and checks oracle agreement (`rf.decode_image(blob)`) on both native acceptance (`returncode == 0`) and native refusal (`returncode == 4`), retaining `mismatch<i>.fold` on any disagreement (with a `fuzz-harness` regression case in `tests/test_correctness.py` proving that native exit 4 on an oracle-valid input fails the harness). `docs/fuzz.md` records the full 15-seed table (index, name, method, dimensions, byte size) and the four 5,000-mutation runs and is registered under `coverage` (`role: "historical"`) in `docs/drift.json`.

- [x] Add seed images to `tests/fuzz_decode.py`: larger crops (`h >= 34 > XROWS = 32`, `w * h > 1200`: `64x40` on `kodim05.png`, `48x36` on `kodim01.png` and `kodim03.png`, and `36x34` on the remaining 7 PNGs in `testdata/photos/`) of each test-pack photo (method 1 with long streams), a `64x48` noise image (method 0), and `64x48` 2-colour and 200-colour images (methods 2 and 3, plus dedicated `method == 0` mutation branches in `mutate()`, `stdin=subprocess.DEVNULL`, `.partial` cleanup checks, bidirectional native-vs-oracle acceptance/refusal verification on every mutation, per-method mutation counts in the summary line, and a `fuzz-harness` regression test in `tests/test_correctness.py`). Done when: `python3 tests/fuzz_decode.py 10 1` (and `python3 scripts/windows.py tests/fuzz_decode.py 10 1`) reports mutations of files of every method the decoder imports (`methods 0:..., 1:..., 2:..., 3:...`) and `python3 tests/test_correctness.py -k fuzz-harness` passes.
- [x] Run `python3 tests/fuzz_decode.py 5000 <seed>` for four seeds (`1`, `2`, `3`, `4`) and record the 15-seed table (seed index, name, method, dimensions, `.fold` byte size) and the four summary lines in `docs/fuzz.md` (with `docs/fuzz.md` added to `coverage` in `docs/drift.json`). Done when: the file lists the 15 seeds with their methods and dimensions plus 20,000 mutations with no crash, hang, or mismatch and `python3 scripts/drift-check.py` exits 0.
- [x] Commit: `"test: fuzz every method with 20,000 mutations"`

**Test checkpoint:** `python3 tests/fuzz_decode.py 10 1` and `python3 scripts/windows.py tests/fuzz_decode.py 10 1` both print `10 mutated files (methods 0:1, 1:5, 2:2, 3:2): 10 rejected, 0 still valid, no crash, hang or mismatch`; the four 5,000-mutation runs recorded in `docs/fuzz.md` print `5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4769 rejected, 231 still valid, no crash, hang or mismatch` (`seed = 1`), `5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4751 rejected, 249 still valid, no crash, hang or mismatch` (`seed = 2`), `5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4757 rejected, 243 still valid, no crash, hang or mismatch` (`seed = 3`), and `5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4784 rejected, 216 still valid, no crash, hang or mismatch` (`seed = 4`) -- `20,000` mutated files total (`methods 0:1336, 1:13324, 2:2672, 3:2668`, `19,061` rejected, `939` still valid, `0` crashes, `0` hangs, `0` oracle mismatches); `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` print `OK   fuzz-harness` (`39 passed, 0 failed`); and `python3 scripts/drift-check.py` exits 0 (`drift: 270 supported documents; 0 error(s); 0 deterministic repair(s)`).

> **Verified:** 2026-10-26 | §2 | 54d537c5591e585630c115b7f25d2b35c2fbc896
> **Review:** `docs/reviews/d04-t01-s2.json`
## 3. Oracle and Spec Agree on Padding and Counters

> **Started:** 2026-10-25T17:03:00Z

**Origin:** discovered run=4865d816f23f 2026-10-01 -- oracle-writer audit in the Claude agent routing probe, verified by the lead

**Job:** A user can encode any BMP the spec's accepted subset describes, and the independent oracle accepts and decodes exactly the same files as the codec.

Admission: a defect in shipped work. `spec/FOLD.md:362` says row padding bytes are ignored, and `src/bmp.c` never inspects them, but the oracle's `read_bmp24` rejects any nonzero padding byte (`tests/rfi_ref.py:576-578`, `FormatError("bmp padding")`). The oracle is meant to check the codec against the spec, so a stricter oracle hides any divergence on padded input instead of proving agreement. The spec and the C agree; the oracle is the side that moves. Second, `spec/FOLD.md:253` writes the counter update as `c_m = c_m + (((65535*bit) - c_m) * rate_m) >> 16`, which under C precedence reads `(c_m + ...) >> 16`, while `src/cm.c:241` and `tests/rfi_ref.py:236` both compute `c_m + ((...) >> 16)`. The spec declares no operator conventions, so an oracle written from the text alone would follow the misreading. The code is right and the text gains parentheses; no byte changes. Third, the oracle skips acceptance rules the spec states, so it accepts files the codec must refuse; an oracle that accepts more than the spec cannot prove the codec's refusals.

-> SOURCE: oracle-bmp-padding-2026-10-01

- [x] Remove the nonzero-padding rejection from `read_bmp24` in `tests/rfi_ref.py`, written from the spec rule rather than from `src/bmp.c`. Done when: the oracle accepts a BMP whose padding bytes are nonzero.
- [x] Add a case to `tests/test_correctness.py` that encodes a BMP with an odd width and nonzero padding bytes through both the codec and the oracle and asserts identical `.fold` bytes and identical decoded pixels. Done when: the case fails on the current oracle and passes after the change.
- [x] Add the spec's remaining acceptance rules to the oracle, from the spec text: in `read_bmp24`, `biWidth` and `|biHeight|` in 1..32768, at most 33554432 pixels, and `bfSize == 0` or `pixel_end <= bfSize <= file_size` (`spec/FOLD.md:354-360`; `bfSize` is read and discarded as `_bf_size` at `tests/rfi_ref.py:553`, and `MAX_DIM`/`MAX_PIXELS` are applied only to `.fold` headers at line 497); in the `.fold` reader, rejection of a file longer than 256 MiB (`spec/FOLD.md:330`; the oracle checks only the 32-byte minimum at line 478). Cover otherwise-valid inputs whose pixel payloads satisfy `need` (`biWidth = 32769, biHeight = 1`, `biWidth = 1, biHeight = ±32769`, and `biWidth = 32768, biHeight = 1025`) so the new dimension/pixel checks rather than truncation reject them, both invalid `bfSize` bounds (`0 < bfSize < pixel_end` and `bfSize > file_size`) and valid boundary cases (`bfSize == 0` and `pixel_end == bfSize < file_size`), and a `256 MiB + 1` `.fold` blob rejected at the file-size check. Done when: each new check's regression fails on the unpatched oracle for that specific check and passes after the change, with codec and oracle agreeing on every refusal and acceptance.
- [x] Parenthesize the counter update at `spec/FOLD.md:253` as `c_m = c_m + ((((65535*bit) - c_m) * rate_m) >> 16)`, add one sentence near the top of the spec stating that operators follow C11 precedence on 32-bit or wider signed integers, regenerate `docs/format.md` via `python3 scripts/build-docs.py --write`, and renew the `spec/FOLD.md` review receipt in `docs/drift.json`. Done when: every formula in the spec's coding sections parses under C precedence to what `tests/rfi_ref.py` computes, `python3 scripts/build-docs.py --check` and `python3 scripts/drift-check.py` exit 0, and `KAT_FOLD` is unchanged.
- [x] Commit: `"spec, test: oracle and spec agree on BMP padding and counter updates"`

**Test checkpoint:** the new `tests/test_correctness.py` case passes, quoted, and the full `python3 tests/test_correctness.py` run passes every case (correctness test, proof 3): before the oracle update, `python3 tests/test_correctness.py -k padding` failed with `FAIL padding: bmp padding` and `python3 tests/test_correctness.py -k acceptance` failed with `FAIL acceptance: bmp_w_32769: oracle accepted BMP that codec refused (image exceeds size limit)`; after the update, `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` print `OK   padding`, `OK   acceptance`, `OK   corrupt`, and `36 passed, 0 failed` with `KAT_FOLD` unchanged (`OK   oracle`, `OK   known-answer`), `python3 scripts/build-docs.py --check` prints `build-docs: current`, and `python3 scripts/drift-check.py` prints `drift: 250 supported documents; 0 error(s); 0 deterministic repair(s)`.

> **Verified:** 2026-10-25 | §3 | 1a9f08782b79f663fbc7265d13104a08b795c464
> **Review:** `docs/reviews/d04-t01-s3.json`
## Verification

- [x] `pwsh scripts/check-all.ps1` -- exits 0 (`python3 scripts/check-all.py`: `check-all: 29/29 passed`; `python3 tests/test_correctness.py`: `39 passed, 0 failed`; `python3 tests/fuzz_decode.py 5000 1..4`: `20,000` mutated files across methods `0, 1, 2, 3`, `19,061` rejected, `939` valid, no crash, hang, or mismatch; `python3 scripts/windows.py tests/measure_final.py`: `PASS`)
- [x] `python scripts/todo-graph.py validate` clean (`32 todos, 229 sections -- 0 fatal, 0 warning(s)`)
