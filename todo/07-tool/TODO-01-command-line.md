---
schema_version: 1
id: command-line
domain: 07-tool
status: draft
title: "TODO-01 -- The Version 1 Command Line"
depends_on: []
frozen: true
track: T1
---

# TODO-01 -- The Version 1 Command Line

> **Goal:** `rasterfold.exe` is a complete tool for 1.0: it converts between `.fold` and BMP, PNG, and PPM, verifies files without writing them, reports what a file holds as text or JSON, names its version, and keeps its promise never to overwrite a file unless told to.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** The commands are `encode`, `decode`, and `inspect`, with `--time` and `--threads N`; input and output are the 24-bit BMP subset only; an existing target file is always refused; there is no `--version`, no `verify`, no JSON output, and no PNG or PPM support.
<!-- claim: count "rasterfold inspect \[--time\] input.fold" src/main.c = 1 -->
<!-- claim: absent src/png.c -->

## Inputs

- [`src/main.c`](../../src/main.c) -- the command line
- [`src/bmp.c`](../../src/bmp.c) -- the reader and writer pattern §2 follows
- -> XREF: D06 T01 §3 -- the library the command line is rebuilt on
- -> XREF: D08 T03 §1 -- the command-line reference generated from `rasterfold help`

## Outcome

- `rasterfold encode|decode|inspect|verify|help|version` work as `D08 T03 §1`'s generated reference describes, with exit codes 0 to 5 unchanged.
- PNG (8-bit RGB and RGBA without alpha loss refused cleanly until alpha exists), PPM, and BMP in and out with exact pixels.

**Adjacency:** list=not-applicable (a command-line tool has no records to browse); document=applicable @ D07 T01 §1; settings=applicable @ D07 T01 §1; reporting=applicable @ D07 T01 §1; notifications=applicable @ D07 T01 §4; permissions=applicable @ D07 T01 §4; audit=not-applicable (git history is the audit); exchange=applicable @ D07 T01 §2; reverse=applicable @ D07 T01 §2

**Adjacency rationale:** Document is the written output file; settings are the options; reporting is `inspect` and `verify`; notifications and permissions are the exit codes, messages, and the overwrite refusal; exchange and reverse are the formats in and out.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Commands and options | D06 T01 §3 |  [ ]   |
|   2   |   §2    | PNG in and out | §1 |  [ ]   |
|   3   |   §3    | PPM in and out | §1 |  [ ]   |
|   4   |   §4    | Exit codes, messages, and overwrite | §1 |  [ ]   |

---

## 1. Commands and Options

**Job:** A user can write a document file with `encode` or `decode`, configure the settings (`--threads`, `--overwrite`), and read a summary report from `inspect` or `verify` as text or JSON.

- [ ] Add `verify` (decode in memory, check every checksum and the stream ends, print the pixel hash, write nothing), `inspect --json`, `help` (the usage text), and `version` / `--version` (codec version, format versions read and written, API version) to `src/main.c`. Done when: each command's output matches the reference `D08 T03 §1` generates. Cheaper substitute: `verify` implemented as decode to a temporary file.
- [ ] Choose input and output formats by file extension (`.bmp`, `.png`, `.ppm`, `.fold`), refusing an unknown extension with exit 1. Done when: `rasterfold encode in.png out.fold` and `decode in.fold out.ppm` work.
- [ ] Commit: `"cli: verify, JSON inspect, help, and version"`

**Test checkpoint:** new cases in `tests/test_correctness.py` for each command and each extension pair pass; `python tests/test_correctness.py` passes every case.

**Freeze check:** the commands add no byte to the `.fold` format: `KAT_FOLD` unchanged.

## 2. PNG In and Out

**Job:** A user can import a PNG, export a PNG, and get identical pixels back (the reverse of every conversion).

PNG needs zlib's inflate and deflate, written here in C without a runtime: inflate complete (every block type, every filter), deflate as a simple greedy LZ77 with fixed and dynamic Huffman codes (the output is lossless; its size is not the point).

- [ ] Write `src/zlib.c` (inflate and deflate with the Adler-32 check) and `src/png.c` (8-bit RGB and RGBA in, 8-bit RGB out; an alpha channel other than fully opaque is refused with exit 3 until alpha exists). Done when: every PNG in `testdata/photos/` decodes to the pixels Pillow reads, and every PNG written re-reads identically in Pillow. Cheaper substitute: a PNG writer using only stored blocks.
- [ ] Add PNG cases to `tests/test_correctness.py` (including interlaced input, 16-bit input refused with exit 3, a truncated file refused with exit 4) and PNG seeds to `tests/fuzz_decode.py`'s reader fuzzing. Done when: the cases pass and 2,000 PNG mutations produce no crash or hang.
- [ ] Commit: `"cli: read and write PNG"`

**Test checkpoint:** the Pillow comparison over `testdata/photos/*.png` quoted; `python tests/test_correctness.py` passes.

## 3. PPM In and Out

- [ ] Write P6 PPM (maxval 255) reading and writing in `src/ppm.c`, refusing other maxvals and P3 with exit 3. Done when: PPM round trips match Pillow. Cheaper substitute: none needed; it is small.
- [ ] Commit: `"cli: read and write PPM"`

**Test checkpoint:** PPM cases in `tests/test_correctness.py` pass.

## 4. Exit Codes, Messages, and Overwrite

**Job:** A user who is refused (an existing file, a bad input) can read the notification (a stable exit code and message) and rely on the overwrite policy: an existing file is replaced only with `--overwrite`.

- [ ] Add `--overwrite`, which replaces an existing destination through the same `.partial`-then-rename path, and keep refusing without it. Done when: a test shows the destination replaced only with the flag, and never left half-written.
- [ ] List every message and exit code in `docs/facts.json` (from `D08 T02 §1`) and check that `src/*.c` emits no message missing from it. Done when: the check fails on an added unlisted message.
- [ ] Commit: `"cli: explicit overwrite and a catalogue of messages"`

**Test checkpoint:** the overwrite and refusal cases pass; the message catalogue check's failure on an unlisted message quoted, then removed.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
