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
> **Current state (verified 2026-09-29):** `Bin/rasterfold.exe decode --time` on the `measure/image.bmp` file reports `alloc_peak=10569996` (the input file 1.41 MB, the RGB image 4.72 MB, the models 2.95 MB, and the padded stream copy 1.41 MB) and `peak_working_set=17305600`; `encode --time` reports `alloc_peak=17192296` (RGB 4.72 MB, the output file 4.72 MB, three stream buffers 4.72 MB, the models 2.95 MB). `rf_cm_decode` copies the streams into a padded buffer (`src/cm.c`); `rf_decode` returns packed RGB and `rf_bmp_store` builds a second image; `rf_cm_encode` allocates `3 * (w*h + 64)` bytes of stream buffers.
<!-- claim: count "uint8_t \*copy = rf_alloc" src/cm.c = 1 -->
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

- [ ] Allocate `size + 8` bytes in `rf_read_file` (`src/sys.c` and `src/sys_posix.c`); `rf_alloc` zeroes them. Done when: the eight bytes after every file read are zero on both Windows and POSIX. Cheaper substitute: keeping the copy and padding only stream B. **Corrected 2026-10-25:** added `src/sys_posix.c` alongside `src/sys.c` since `D00 T04 §1` added the POSIX platform layer.
- [ ] Point `rdec_init` at the payload directly in `rf_cm_decode` and delete the copy. Done when: `decode --time` on `measure/image.bmp` reports `alloc_peak` about 1.41 MB lower in `rf_cm_decode` (`inspect --time` drops from `10569996` to `9160996` bytes, and `decode --time` drops from `10569996` to `9437238` bytes until §2 removes the second 4.72 MB image buffer in `rf_bmp_store`). **Corrected 2026-10-25:** noted that in `decode --time`, `rf_bmp_store` holds `img.rgb` (`4,718,592`) + BMP `out` (`4,718,646`) = `9,437,238` bytes after `rf_cm_decode` finishes until §2 decodes straight into the BMP buffer.
- [ ] Commit: `"codec: decode streams straight from the input file"`

**Test checkpoint:** `python tests/test_correctness.py` passes every case, the corruption and truncation cases included; `python tests/fuzz_decode.py 2000 3` reports no crash, hang, or mismatch; the old and new `alloc_peak` lines are quoted.

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
