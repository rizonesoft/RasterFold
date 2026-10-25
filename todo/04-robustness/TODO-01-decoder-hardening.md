---
schema_version: 1
id: decoder-hardening
domain: 04-robustness
status: draft
title: "TODO-01 -- Decoder Hardening"
depends_on: []
track: R1
---

# TODO-01 -- Decoder Hardening

> **Goal:** A file that lies about its size costs the decoder milliseconds, not seconds, and the decoder has survived a long fuzz campaign from seeds that reach every method, with the seeds and results on record.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** A CM stream that runs out transitions into `rdec.tail` (`src/cm.c`) and keeps decoding against zero padding (`d->in` clamped at `d->lim = d->end + 2`) until the image ends, and only then is rejected (`in != end`); a 32768x1024 header with a 20-byte payload therefore decodes about 100 million samples before exit 4. `tests/fuzz_decode.py` mutates valid files with the checksums recomputed; its longest recorded single-seed run is 5,000 iterations (`python3 tests/fuzz_decode.py 5000 3`: 4,895 rejected, 105 valid, no crash, hang, or mismatch), while a 20,000-mutation campaign across seeds covering every method (§2) has not yet run.
<!-- claim: count "d->in = d->in > d->lim \? d->lim : d->in;" src/cm.c = 1 -->
<!-- claim: exists tests/fuzz_decode.py -->

## Inputs

- [`src/cm.c`](../../src/cm.c) -- `rdec_bit`, `run_channel`, and `run_pipelined`, where §1 adds the early exit
- [`tests/fuzz_decode.py`](../../tests/fuzz_decode.py) -- the fuzzer §2 extends
- [`docs/research.md`](../../docs/research.md) -- sections 5.5 and 7.1
- -> XREF: D01 T01 §2 -- method 2, whose decoder §2's seeds must reach

## Outcome

- A 4096x4096 header with a 20-byte CM payload fails with exit code 4 in under a second.
- A fuzz campaign of at least 20,000 mutations over seeds covering methods 0, 1, and (once it exists) 2 reports no crash, hang, or oracle mismatch, with its seeds quoted.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no option); reporting=not-applicable (the existing messages); notifications=applicable @ D04 T01 §1; permissions=applicable @ D04 T01 §1; audit=not-applicable (git history is the audit); exchange=applicable @ D04 T01 §2; reverse=not-applicable (no encoder change)

**Adjacency rationale:** Notifications and permissions are the refusal: a hostile file is refused quickly with exit 4 and the existing message. Exchange is every method a decoder must refuse cleanly when malformed.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Stop decoding a stream that has run out       | --         |  [ ]   |
|   2   |   §2    | A long fuzz campaign over every method        | §1, D01 T01 §2 |  [ ]   |
|   3   |   §3    | Oracle and spec agree on padding and counters | --         |  [ ]   |

---

## 1. Stop Decoding a Stream That Has Run Out

> **Started:** 2026-10-25T16:36:30Z

**Job:** A user who opens a damaged or hostile file can see it refused within a second, by the same notification as today (exit 4 and one message line), under the unchanged file-safety policy.

Once a channel's read pointer has reached `end + 2`, the file will be rejected anyway, so every further sample is wasted work. The channel threads wait on each other's progress counters, so an early exit must release every waiter. The refusal stays the tool's one notification to the user (exit 4 and the existing message line), and the file-safety policy is unchanged.

- [ ] Add an `abort` flag to `cm_t` in `src/cm.c`; after each row in `run_channel` and `run_pipelined`, a channel whose `dec.in` has reached `dec.lim` sets its `bad` flag and `abort`, and every channel checks `abort` once per row and stops. Done when: a truncated stream is rejected with the same message as today. Cheaper substitute: a check per sample, which costs the hot loop.
- [ ] Publish `done = h` from a stopping channel so a channel waiting on it in `wait_done` is released. Done when: no thread waits forever when any one channel stops early (a test with each channel's stream truncated in turn completes).
- [ ] Add a case to `tests/test_correctness.py` that writes a 4096x4096 CM header with a 20-byte payload (checksums recomputed) and asserts exit 4 within one second, and cases truncating each of the three streams in turn. Done when: the cases pass, the refusal notification is the existing exit 4 and message line, and the file-safety policy (no output file, no `.partial` left) holds in each.
- [ ] Commit: `"codec: stop decoding a stream that has run out"`

**Test checkpoint:** the new cases pass with their timings quoted; `python tests/test_correctness.py` passes every case; `python tests/fuzz_decode.py 2000 5` reports no crash, hang, or mismatch.

## 2. A Long Fuzz Campaign over Every Method

**Needs:** Idle machine (timing)

**Job:** A user can import any file of any method and get either identical pixels or a clean refusal, as a 20,000-mutation campaign shows.

The recorded fuzz runs are short. A campaign of 20,000 mutations over seeds that reach every method, including method 2's decoder once `D01 T01 §2` ships it, is the evidence that the decoder refuses cleanly: every method a decoder imports is fuzzed.

- [ ] Add seed images to `tests/fuzz_decode.py`: a crop of each test-pack photo (method 1 with long streams), a noise image (method 0), and a 2-colour and a 200-colour image (method 2). Done when: `python tests/fuzz_decode.py 10 1` reports mutations of files of every method the decoder imports.
- [ ] Run `python tests/fuzz_decode.py 5000 <seed>` for four seeds and record the seeds and summary lines in `docs/fuzz.md`. Done when: the file lists 20,000 mutations with no crash, hang, or mismatch.
- [ ] Commit: `"test: fuzz every method with 20,000 mutations"`

**Test checkpoint:** the four summary lines quoted from `docs/fuzz.md`, and `python tests/fuzz_decode.py 10 1` showing every method among the seeds.

## 3. Oracle and Spec Agree on Padding and Counters

**Origin:** discovered run=4865d816f23f 2026-10-01 -- oracle-writer audit in the Claude agent routing probe, verified by the lead

**Job:** A user can encode any BMP the spec's accepted subset describes, and the independent oracle accepts and decodes exactly the same files as the codec.

Admission: a defect in shipped work. `spec/FOLD.md:362` says row padding bytes are ignored, and `src/bmp.c` never inspects them, but the oracle's `read_bmp24` rejects any nonzero padding byte (`tests/rfi_ref.py:576-578`, `FormatError("bmp padding")`). The oracle is meant to check the codec against the spec, so a stricter oracle hides any divergence on padded input instead of proving agreement. The spec and the C agree; the oracle is the side that moves. Second, `spec/FOLD.md:253` writes the counter update as `c_m = c_m + (((65535*bit) - c_m) * rate_m) >> 16`, which under C precedence reads `(c_m + ...) >> 16`, while `src/cm.c:241` and `tests/rfi_ref.py:236` both compute `c_m + ((...) >> 16)`. The spec declares no operator conventions, so an oracle written from the text alone would follow the misreading. The code is right and the text gains parentheses; no byte changes. Third, the oracle skips acceptance rules the spec states, so it accepts files the codec must refuse; an oracle that accepts more than the spec cannot prove the codec's refusals.

-> SOURCE: oracle-bmp-padding-2026-10-01

- [ ] Remove the nonzero-padding rejection from `read_bmp24` in `tests/rfi_ref.py`, written from the spec rule rather than from `src/bmp.c`. Done when: the oracle accepts a BMP whose padding bytes are nonzero.
- [ ] Add a case to `tests/test_correctness.py` that encodes a BMP with an odd width and nonzero padding bytes through both the codec and the oracle and asserts identical `.fold` bytes and identical decoded pixels. Done when: the case fails on the current oracle and passes after the change.
- [ ] Add the spec's remaining acceptance rules to the oracle, from the spec text: in `read_bmp24`, `biWidth` and `|biHeight|` in 1..32768, at most 33554432 pixels, and `bfSize == 0` or `pixel_end <= bfSize <= file_size` (`spec/FOLD.md:354-360`; `bfSize` is read and discarded as `_bf_size` at `tests/rfi_ref.py:553`, and `MAX_DIM`/`MAX_PIXELS` are applied only to `.fold` headers at line 497); in the `.fold` reader, rejection of a file longer than 256 MiB (`spec/FOLD.md:330`; the oracle checks only the 32-byte minimum at line 478). Done when: a `tests/test_correctness.py` case per rule shows the codec and the oracle refusing the same files.
- [ ] Parenthesize the counter update at `spec/FOLD.md:253` as `c_m = c_m + ((((65535*bit) - c_m) * rate_m) >> 16)`, and add one sentence near the top of the spec stating that operators follow C11 precedence on 32-bit or wider signed integers. Done when: every formula in the spec's coding sections parses under C precedence to what `tests/rfi_ref.py` computes, and `KAT_FOLD` is unchanged.
- [ ] Commit: `"spec, test: oracle and spec agree on BMP padding and counter updates"`

**Test checkpoint:** the new `tests/test_correctness.py` case passes, quoted, and the full `python3 tests/test_correctness.py` run passes every case (correctness test, proof 3).

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0
- [ ] `python scripts/todo-graph.py validate` clean
