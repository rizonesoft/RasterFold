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
> **Current state (verified 2026-09-29):** A CM stream that runs out keeps decoding against zero padding until the image ends, and only then is rejected (`in != end`); a 32768x1024 header with a 20-byte payload therefore decodes about 100 million samples before exit 4. `tests/fuzz_decode.py` mutates valid files with the checksums recomputed; its longest recorded run is 600 iterations (587 rejected, 13 valid, no crash, hang, or mismatch).
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

---

## 1. Stop Decoding a Stream That Has Run Out

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

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0
- [ ] `python scripts/todo-graph.py validate` clean
