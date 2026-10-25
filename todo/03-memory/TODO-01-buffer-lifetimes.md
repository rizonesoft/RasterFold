---
schema_version: 1
id: buffer-lifetimes
domain: 03-memory
status: draft
title: "TODO-01 -- Buffer Lifetimes and Copies"
depends_on: []
track: M1
---

# TODO-01 -- Buffer Lifetimes and Copies

> **Goal:** Encode and decode hold no buffer they do not need: the decoder reads streams straight from the input file, writes pixels straight into the output BMP, and the encoder codes straight into its output buffer. `alloc_peak` for `measure/image.bmp` falls from 10.57 MB to about 9.1 MB on decode and from 17.19 MB to about 12.4 MB on encode, with every byte written unchanged.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** After §1 (`rf_read_file` allocating `size + 8` zeroed bytes in `src/sys.c` and `src/sys_posix.c`, `rdec_init` pointing directly at the payload with an in-struct 16-byte zero-padded `tail` buffer in `src/cm.c`, and deleting the heap stream `copy`), `bin/windows-x64/bin/rasterfold.exe` on `measure/image.bmp` reports `inspect --time` `alloc_peak=9160964` (`peak_working_set=15044608`, down `1,409,000` bytes from `10569964`) and `decode --time` `alloc_peak=9440166` (`peak_working_set=15409152`, down `1,129,830` bytes from `10569996`, held during `rf_bmp_store`'s `img.rgb` `4,720,056` + BMP `out` `4,720,110` until §2); `encode --time` reports `alloc_peak=17192296` (RGB 4.72 MB, the output file 4.72 MB, three stream buffers 4.72 MB, the models 2.95 MB). `rf_decode` returns packed RGB and `rf_bmp_store` builds a second image; `rf_cm_encode` allocates `3 * (w*h + 64)` bytes of stream buffers.
<!-- claim: count "uint8_t \*copy = rf_alloc" src/cm.c = 0 -->
<!-- claim: count "uint8_t \*tmp = rf_alloc\(each \* 3\)" src/cm.c = 1 -->
<!-- claim: exists src/bmp.c -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- sections 6.1 to 6.3, the designs and the byte arithmetic
- [`src/cm.c`](../../src/cm.c) -- `rf_cm_decode` (the stream copy) and `rf_cm_encode` (the stream buffers)
- [`src/fold.c`](../../src/fold.c), [`src/bmp.c`](../../src/bmp.c), [`src/main.c`](../../src/main.c) -- the decode path from file to BMP
- [`src/sys.c`](../../src/sys.c), [`src/sys_posix.c`](../../src/sys_posix.c) -- `rf_read_file`, which §1 pads on Windows and POSIX targets

## Outcome

- `decode --time` on `measure/image.bmp` reports `alloc_peak` at most 9.2 MB, and `encode --time` at most 12.5 MB.
- `python tests/test_correctness.py` and `python tests/fuzz_decode.py 2000` pass, and `KAT_FOLD` is unchanged.

**Adjacency:** all=not-applicable (internal buffer handling; the files read and written, the options, and the output are unchanged)

**Adjacency rationale:** The decoder's input handling changes in §1, which is why §1 carries a fuzz run; the file format and the BMP written do not change.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Decode streams straight from the input file   | --         |  [ ]   |
|   2   |   §2    | Decode straight into the BMP                  | §1         |  [ ]   |
|   3   |   §3    | Encode straight into the output buffer        | --         |  [ ]   |

---

## 1. Decode Streams Straight from the Input File

> **Started:** 2026-10-25T14:20:07Z

The padded stream copy exists so `rdec_bit` can load two bytes past a stream's end without a bounds check. It is unnecessary: the decoder never uses a byte past `end` in a stream it accepts (it loads `in[0]` and `in[1]` but shifts out what it does not consume), the bytes after streams G and R are the next stream's, and only stream B's two bytes lie past the file.

- [x] Allocate `size + 8` bytes in `rf_read_file` (`src/sys.c` and `src/sys_posix.c`); `rf_alloc` zeroes them, and add a focused platform test verifying that `rf_read_file` returns the unchanged file size, byte-identical contents, and eight readable zero bytes after `data + size` on both Windows and POSIX. Done when: the eight bytes after every file read are zero and verified by test on both Windows and POSIX (`test_rf_read_file_eight_byte_zero_padding` in `tests/test_artifacts.py`). Cheaper substitute: keeping the copy and padding only stream B. **Corrected 2026-10-25:** added `src/sys_posix.c` alongside `src/sys.c` since `D00 T04 §1` added the POSIX platform layer, and added a direct test of the `rf_read_file` 8-byte zero padding per the `--kind plan` review (`P2`).
- [x] Point `rdec_init` at the payload directly in `rf_cm_decode`, delete the heap `copy`, and transition each stream's last bytes through an in-struct 16-byte zero-padded `tail` buffer in `rdec` so `rdec_bit` never reads past a stream's `sz[c]` boundary even on unpadded borrowed C API buffers (`rf_decode_image` and `rf_inspect`); add guard-page-backed exact-length buffer tests in `tests/test_api.py` covering valid and checksum-correct malformed/truncated streams at 1 and 3 threads. Done when: `decode --time` on `measure/image.bmp` reports `alloc_peak` about 1.41 MB lower in `rf_cm_decode` (`inspect --time` drops from `10569964` to `9160964` bytes, and `decode --time` drops from `10569996` to `9440166` bytes until §2 removes the second 4.72 MB image buffer in `rf_bmp_store`), and exact-length guard-page-backed C API inputs decode and reject cleanly without reading past `bytes + size` (`test_guard_page_exact_length_inputs` in `tests/test_api.py`). **Corrected 2026-10-25:** added the in-struct `rdec.tail` boundary transition and guard-page C API tests per the `--kind plan` review (`P1`) so borrowed `size`-byte buffers in `rf_decode_image` and `rf_inspect` (`include/rasterfold.h`) never read past `bytes + size`, and noted that in `decode --time`, `rf_bmp_store` holds `img.rgb` (`4,720,056`) + BMP `out` (`4,720,110`) = `9,440,166` bytes after `rf_cm_decode` finishes until §2 decodes straight into the BMP buffer.
- [x] Commit: `"codec: decode streams straight from the input file"`

**Test checkpoint:** `python3 tests/test_correctness.py` (`33 passed, 0 failed`), `python3 tests/test_api.py` and `python3 scripts/windows.py tests/test_api.py` (`Ran 9 tests OK`, including `test_guard_page_exact_length_inputs` across 1 and 3 threads and valid/exhausted streams), and `python3 tests/test_artifacts.py` (`Ran 5 tests OK`, including `test_rf_read_file_eight_byte_zero_padding` on both `linux-x64` and `windows-x64`) pass every case; `python3 tests/fuzz_decode.py 2000 3` reports `2000 mutated files: 1960 rejected, 40 still valid, no crash, hang or mismatch`; on `measure/image.bmp`, Windows `bin/windows-x64/bin/rasterfold.exe` `inspect --time` drops from `rf_stat alloc_peak=10569964` to `rf_stat alloc_peak=9160964` (`-1,409,000` bytes, `-1.41 MB`) and `decode --time` drops from `rf_stat alloc_peak=10569996` (`peak_working_set=16576512`) to `rf_stat alloc_peak=9440166` (`peak_working_set=15409152`), and Linux `bin/linux-x64/bin/rasterfold` `inspect --time` drops to `rf_stat alloc_peak=9160964` and `decode --time` drops from `rf_stat alloc_peak=10570204` to `rf_stat alloc_peak=9440166`.

## 2. Decode Straight into the BMP

`rf_decode` writes packed RGB and `rf_bmp_store` copies it into a bottom-up BGR image: a second 4.72 MB buffer and a full pass on `measure/image.bmp`.

- [ ] Give `rf_cm_decode` an output pointer and row stride, writing each sample at `54 + stride * (h - 1 - y) + 3 * x + (2 - rgb_index)` with `stride = (3 * w + 3) & ~3`, and read G back from the same place (method 1 reads G at the same pixel for R and B). Done when: `decode` writes a BMP byte-identical to today's for every correctness case. Cheaper substitute: converting in place after decoding, which keeps the pass.
- [ ] Build the BMP header around the decoded pixels in `src/main.c`, keeping `rf_decode`'s packed RGB output for `inspect` and the tests. Done when: `decode --time` reports `alloc_peak` about 4.72 MB lower than after §1.
- [ ] Commit: `"codec: decode straight into the output BMP"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case; the BMP files of `measure/image.bmp` decoded before and after are byte-identical (hashes quoted); `alloc_peak` quoted.

## 3. Encode Straight into the Output Buffer

`rf_encode` allocates the output file and `rf_cm_encode` three more stream buffers of `w*h + 64` bytes each.

- [ ] Give stream `c` the region at offset `32 + 12 + c * (w*h + 64)` of the output buffer (grown by 256 bytes), and after coding `memmove` streams R and B down behind G. A stream that overflows its region makes the result raw, as today. Done when: `encode` writes files byte-identical to today's for every correctness case. Cheaper substitute: shrinking the stream buffers by guessing a compression ratio.
- [ ] Measure `encode --time` on `measure/image.bmp`. Done when: `alloc_peak` is about 4.72 MB lower.
- [ ] Commit: `"codec: encode straight into the output buffer"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case, the raw-fallback case (`noise`) included; `alloc_peak` quoted before and after.

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0
- [ ] `python tests/measure_final.py` PASS, with the decode peak working set quoted
- [ ] `python tests/benchmark.py -n 3` rerun and the README memory figures updated
- [ ] `python scripts/todo-graph.py validate` clean
