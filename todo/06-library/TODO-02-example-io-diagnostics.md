---
schema_version: 1
id: example-io-diagnostics
domain: 06-library
status: draft
title: "TODO-02 -- Example Host-I/O Failure Diagnostics"
depends_on: []
track: L2
---

# TODO-02 -- Example Host-I/O Failure Diagnostics

> **Goal:** Ensure every host-I/O, size-bound, and allocation failure path in `examples/encode.c` and `examples/decode.c` writes a clear human-readable diagnostic to `stderr` before exiting nonzero, with automated tests proving non-silent refusal.

> [!IMPORTANT]
> **Current state (verified 2026-11-13):** `examples/decode.c` and `examples/encode.c` report codec API errors (`rf_error`) and existing-destination refusals on `stderr`, but several host `stdio` / allocation / oversized-input guard branches return nonzero without printing a `stderr` message.
<!-- claim: exists examples/encode.c -->
<!-- claim: exists examples/decode.c -->

## Inputs

- [`examples/encode.c`](../../examples/encode.c), [`examples/decode.c`](../../examples/decode.c), [`examples/ppm.h`](../../examples/ppm.h) -- the shipped C API example programs
- [`tests/test_api.py`](../../tests/test_api.py) -- existing example compilation and refusal tests
- -> XREF: D09 T01 §4 -- release backlog review for `rasterfold-v1.0.0` that promoted `B-004`

## Outcome

- Every nonzero exit path in `examples/encode.c`, `examples/decode.c`, and `examples/ppm.h` emits an explicit `stderr` diagnostic naming the failed operation (usage, open, seek/tell/stat, oversized input, allocation, short read, codec call, existing destination, or output write/close), and `tests/test_api.py` drives unreadable/unseekable/write-failing fixtures to verify non-empty `stderr` diagnostics on refusal.

**Adjacency:** all=not-applicable (standalone C API SDK examples in `examples/` and their unit tests in `tests/test_api.py`; no shipped codec, library ABI, CLI command, or converter UI workflow changes)

**Adjacency rationale:** Only `stderr` error messages in the SDK sample programs `examples/encode.c`, `examples/decode.c`, and `examples/ppm.h` and their tests change.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Stderr diagnostics and fixtures for example host-I/O failures | D06 T01 §5, D05 T01 §10 |  [ ]   |

---

## 1. Stderr Diagnostics and Fixtures for Example Host-I/O Failures

**Origin:** discovered run=36a212a58ece 2026-11-13 -- promoted from `B-004` during `D09 T01 §4` release backlog review

-> SOURCE: D06-T01-S5-completion

**Job:** A developer running the C API `encode` and `decode` example programs exports and imports `.fold` and `.ppm` documents, refuses unsafe overwrites or unreadable paths, and receives a clear `stderr` notification on every host-I/O, size-limit, or allocation failure.

`D06 T01 §5` shipped `examples/encode.c` and `examples/decode.c` with codec-error and overwrite diagnostics, noting in its completion review (`B-004`) that `fseek`, `ftell`, `malloc`, oversized-input, short-read, and output-write failure branches still exited silently.

- [ ] Audit every nonzero return path in `examples/encode.c`, `examples/decode.c`, and `examples/ppm.h` and add a concise `fprintf(stderr, ...)` diagnostic to every silent host-I/O, oversized-input, and allocation branch while keeping C11 `-Wall -Wextra -Werror` compilation clean on Linux and Windows. Done when: zero nonzero return paths in `examples/` can exit without writing to `stderr`. Cheaper substitute: adding a generic `"error\n"` line that hides whether reading, sizing, allocating, or writing failed.
- [ ] Add tests in `tests/test_api.py` exercising directory/FIFO/missing-input and unwritable/blocked output paths (plus unit/fault-injected oversized/write checks where portable across Linux and Windows) on the compiled example binaries, asserting nonzero exit, non-empty `stderr` naming the failure, and no leftover output file. Regenerate docs if any mirrored example snippet changes (`python3 scripts/build-docs.py --check`). Done when: `python3 -m unittest discover -s tests -p test_api.py` passes on Linux and Windows (`scripts/windows.py`).
- [ ] Commit: `"examples: report stderr diagnostics on every host I/O failure"`

**Test checkpoint:** `python3 -m unittest discover -s tests -p test_api.py` and `python3 scripts/windows.py tests/test_api.py` pass with the new host-I/O diagnostic cases green, and `python3 scripts/check-all.py --all-targets` exits 0.

## Verification

- [ ] `python3 -m unittest discover -s tests -p test_api.py` exits 0
- [ ] `python3 scripts/check-all.py --all-targets` exits 0
- [ ] `python3 scripts/todo-graph.py validate` clean
