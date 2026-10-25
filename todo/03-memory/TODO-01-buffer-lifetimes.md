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
> **Current state (verified 2026-10-25):** After §1 (`rf_read_file` allocating `size + 8` zeroed bytes in `src/sys.c` and `src/sys_posix.c`, `rdec_init` pointing directly at the payload with an in-struct 16-byte zero-padded `tail` buffer in `src/cm.c`, and deleting the heap stream `copy`), `bin/windows-x64/bin/rasterfold.exe` (`--threads 3`) on `measure/image.bmp` reports `inspect --time` `alloc_peak=9160964` (`peak_working_set=15044608`, down `1,409,000` bytes from `10569964`) and `decode --time` `alloc_peak=9440166` (`peak_working_set=15409152`, down `1,129,830` bytes from `10569996`, held during `rf_bmp_store`'s `img.rgb` `4,720,056` + BMP `out` `4,720,110` until §2), while Linux `bin/linux-x64/bin/rasterfold` reports `inspect --time` `alloc_peak=9160964` and `decode --time` `alloc_peak=9440166` at both `--threads 1` and `--threads 3` (down from `10569964` at `--threads 1` and `10570204` at `--threads 3`); `encode --time` reports `alloc_peak=17192296` (RGB 4.72 MB, the output file 4.72 MB, three stream buffers 4.72 MB, the models 2.95 MB). `rf_decode` returns packed RGB and `rf_bmp_store` builds a second image; `rf_cm_encode` allocates `3 * (w*h + 64)` bytes of stream buffers.
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
|   1   |   §1    | Decode streams straight from the input file   | --         |  [x]   |
|   2   |   §2    | Decode straight into the BMP                  | §1         |  [ ]   |
|   3   |   §3    | Encode straight into the output buffer        | --         |  [ ]   |

---

## 1. Decode Streams Straight from the Input File

> **Started:** 2026-10-25T14:20:07Z

The padded stream copy exists so `rdec_bit` can load two bytes past a stream's end without a bounds check. It is unnecessary: the decoder never uses a byte past `end` in a stream it accepts (it loads `in[0]` and `in[1]` but shifts out what it does not consume), the bytes after streams G and R are the next stream's, and only stream B's two bytes lie past the file.

- [x] Allocate `size + 8` bytes in `rf_read_file` (`src/sys.c` and `src/sys_posix.c`); `rf_alloc` zeroes them, and add a focused platform test verifying that `rf_read_file` returns the unchanged file size, byte-identical contents, and eight readable zero bytes after `data + size` on both Windows and POSIX. Done when: the eight bytes after every file read are zero and verified by test on both Windows and POSIX (`test_rf_read_file_eight_byte_zero_padding` in `tests/test_artifacts.py`). Cheaper substitute: keeping the copy and padding only stream B. **Corrected 2026-10-25:** added `src/sys_posix.c` alongside `src/sys.c` since `D00 T04 §1` added the POSIX platform layer, and added a direct test of the `rf_read_file` 8-byte zero padding per the `--kind plan` review (`P2`).
- [x] Point `rdec_init` at the payload directly in `rf_cm_decode`, delete the heap `copy`, and transition each stream's last bytes through an in-struct 16-byte zero-padded `tail` buffer in `rdec` so `rdec_bit` never reads past a stream's `sz[c]` boundary even on unpadded borrowed C API buffers (`rf_decode_image` and `rf_inspect`); add guard-page-backed exact-length buffer tests in `tests/test_api.py` covering valid and checksum-correct malformed/truncated streams at 1 and 3 threads. Done when: `decode --time` on `measure/image.bmp` reports `alloc_peak` about 1.41 MB lower in `rf_cm_decode` (`inspect --time` drops from `10569964` to `9160964` bytes, and `decode --time` drops from `10569996` to `9440166` bytes until §2 removes the second 4.72 MB image buffer in `rf_bmp_store`), and exact-length guard-page-backed C API inputs decode and reject cleanly without reading past `bytes + size` (`test_guard_page_exact_length_inputs` in `tests/test_api.py`). **Corrected 2026-10-25:** added the in-struct `rdec.tail` boundary transition and guard-page C API tests per the `--kind plan` review (`P1`) so borrowed `size`-byte buffers in `rf_decode_image` and `rf_inspect` (`include/rasterfold.h`) never read past `bytes + size`, and noted that in `decode --time`, `rf_bmp_store` holds `img.rgb` (`4,720,056`) + BMP `out` (`4,720,110`) = `9,440,166` bytes after `rf_cm_decode` finishes until §2 decodes straight into the BMP buffer.
- [x] Commit: `"codec: decode streams straight from the input file"`

**Test checkpoint:** `python3 tests/test_correctness.py` (`33 passed, 0 failed`), `python3 tests/test_api.py` and `python3 scripts/windows.py tests/test_api.py` (`Ran 9 tests OK`, including `test_guard_page_exact_length_inputs` across 1 and 3 threads and valid/exhausted streams), and `python3 tests/test_artifacts.py` (`Ran 5 tests OK`) plus `python3 scripts/windows.py tests/test_artifacts.py` (`Ran 5 tests OK (skipped=2)`, including `test_rf_read_file_eight_byte_zero_padding` on both `linux-x64` and `windows-x64`) pass every case; `python3 tests/fuzz_decode.py 2000 3` reports `2000 mutated files: 1960 rejected, 40 still valid, no crash, hang or mismatch`; on `measure/image.bmp`, Windows `bin/windows-x64/bin/rasterfold.exe` (`--threads 3`) `inspect --time` drops from `rf_stat alloc_peak=10569964` to `rf_stat alloc_peak=9160964` (`-1,409,000` bytes, `-1.41 MB`) and `decode --time` drops from `rf_stat alloc_peak=10569996` (`peak_working_set=16576512`) to `rf_stat alloc_peak=9440166` (`peak_working_set=15409152`), and Linux `bin/linux-x64/bin/rasterfold` drops from `alloc_peak=10569964` to `alloc_peak=9160964` on `inspect --time` (`--threads 1` and `--threads 3`) and from `alloc_peak=10569964` (`--threads 1`) / `alloc_peak=10570204` (`--threads 3`) to `alloc_peak=9440166` on `decode --time`. Across the research corpus between baseline `e620f701de2c987c85a76c2e62bf8404258ebcb5` (A) and candidate `b0827b8d5c5579c64e0db1549b9be0300997e6bd` (B), encoded sizes and decoded BMPs are byte-identical (`python3 research/ab.py size src/cm.c {photo,tp,synth}`: `photo` `total=30865211 jxl=31782058 ratio=0.9712 geomean=0.9728 worst=1.0037 (kodim17) e9=31400714 e9_ratio=0.9829 e9_geomean=0.9883 e9_worst=1.0193 (kodim16) webp=34138176 webp_ratio=0.9041 webp_geomean=0.8932 webp_worst=1.0104 (ic_deer) measure=1409028`; `tp` `total=6651583 jxl=6826895 ratio=0.9743 geomean=0.9722 worst=0.9931 (tp_nightshot_iso_1600_crop) e9=6740249 e9_ratio=0.9868`; `synth` `total=214773 jxl=226544 ratio=0.9480 geomean=1.1986 worst=18.0665 (syn_checker)`), 3-thread corpus `alloc_peak` drops on `photo` (`inspect`: `before=253066594 after=222202475 ratio=0.8780 geomean=0.8896 worst=0.9841 (ic_artificial)`; `decode`: `before=253671886 after=223314662 ratio=0.8803 geomean=0.8910 worst=1.0000 (ic_artificial)`), `tp` (`inspect`: `before=58196406 after=51545103 ratio=0.8857 geomean=0.8911 worst=0.9272 (kodim03)`; `decode`: `before=58198806 after=51547503 ratio=0.8857 geomean=0.8911 worst=0.9272 (kodim03)`), and `synth` (`inspect`: `before=37409671 after=37391826 ratio=0.9995 geomean=0.9996 worst=1.0000 (syn_noise)`; `decode`: `before=37412093 after=37394240 ratio=0.9995 geomean=0.9996 worst=1.0000 (syn_noise)`), and alternating A/B timing (`python3 research/ab.py ab <A> src/cm.c measure kodim01 -n9`) shows no decode regression at 1 thread (`-t1`: `A: size=1865760 enc min=588.4 med=602.4 dec min=562.4 med=572.3`, `B: size=1865760 enc min=587.4 med=602.7 dec min=561.5 med=575.6`, `B/A: enc 0.998 dec 0.998`) or 3 threads (`-t3`: `A: size=1865760 enc min=187.4 med=209.4 dec min=198.8 med=223.8`, `B: size=1865760 enc min=184.3 med=217.8 dec min=193.9 med=205.3`, `B/A: enc 0.983 dec 0.975`).

> **Verified:** 2026-10-25 | §1 | 3be1e7978083b65fe6f757ad0399b8a0081eed78
> **Review:** `docs/reviews/d03-t01-s1.json`
## 2. Decode Straight into the BMP

> **Started:** 2026-10-25T15:03:50Z

`rf_decode` writes packed RGB and `rf_bmp_store` copies it into a bottom-up BGR image: a second 4.72 MB buffer and a full pass on `measure/image.bmp`.

- [ ] Give `rf_cm_decode` an output pointer and row stride, writing each sample at `54 + stride * (h - 1 - y) + 3 * x + (2 - rgb_index)` with `stride = (3 * w + 3) & ~3`, and read G back from the same place (method 1 reads G at the same pixel for R and B). Done when: `decode` writes a BMP byte-identical to today's for every correctness case. Cheaper substitute: converting in place after decoding, which keeps the pass. **Corrected 2026-10-25:** when `stride == 0` (used by `rf_decode` for `rf_decode_image`, `rf_inspect`, and `research/bench.c`), `rf_cm_decode` continues to write packed top-down RGB at `((size_t)y * w + x) * 3 + rgb_index` via the same row-pointer/step parameterization (`row0`, `row_step`, `r_off`).
- [ ] Build the BMP header around the decoded pixels in `src/main.c`, keeping `rf_decode`'s packed RGB output for `inspect` and the tests. Done when: `decode --time` eliminates the second 4.72 MB image buffer (`4,720,056` bytes on `measure/image.bmp`), reducing live memory at the BMP write stage by 4.72 MB (from `9,440,166` to `4,720,110` bytes) and overall `decode --time` `alloc_peak` on `measure/image.bmp` from `9,440,166` to `9,161,018` bytes (`9.16 MB`, `<= 9.2 MB` per `Goal` and `Outcome`: input file `1,409,036` + BMP `4,720,110` + CM models `3,031,872`). **Corrected 2026-10-25:** clarified that removing the `4.72 MB` `img.rgb` allocation removes `4.72 MB` from the BMP conversion stage (`9.44 MB -> 4.72 MB`), making the overall process `alloc_peak` on `measure/image.bmp` `9.16 MB` (`file + BMP + models`), which matches the file's `Goal` ("falls from 10.57 MB to about 9.1 MB on decode"), `Outcome` ("at most 9.2 MB"), and `docs/research.md` section 6.3 ("about 9.1 MB for decode (input file, BMP, models)").
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
