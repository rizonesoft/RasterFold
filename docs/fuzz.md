# Fuzz Campaign Record (`D04 T01 §2`)

Recorded 2026-10-26 with `tests/fuzz_decode.py` against `bin/linux-x64/bin/rasterfold` (`version 2`) and the independent Python oracle `tests/rfi_ref.py`.

## Seed Set (`15` seeds covering methods `0`, `1`, `2`, and `3`)

Every seed has `w >= 36`, `h >= 34 > XROWS = 32` (crossing the 32-row channel backpressure boundary in `src/cm.c`), and `w * h > 1200` pixels (exceeding the previous `40x30`, `17x9`, `64x3`, `32x24`, and `16x8` seeds).

| Index | Seed Name | Method | Dimensions | `.fold` Bytes | Description |
| :---: | --------- | :----: | :--------: | ------------: | ----------- |
| 0 | `noise_64x48` | `0` (`RAW`) | `64x48` | 9,248 | High-entropy pseudorandom RGB (`random.Random(101)`) |
| 1 | `kodim01_48x36` | `1` (`CM`) | `48x36` | 2,238 | Crop `(120, 80, 48, 36)` of `testdata/photos/kodim01.png` |
| 2 | `pal2_2col_64x48` | `2` (`PAL`) | `64x48` | 121 | 2-colour checkerboard encoded with Method 2 (`pal_encode(..., method=2)`) |
| 3 | `pal3_200col_64x48` | `3` (`PAL2`) | `64x48` | 3,311 | 200-colour pattern encoded with Method 3 (`encode_image`) |
| 4 | `pal2_200col_64x48` | `2` (`PAL`) | `64x48` | 3,308 | 200-colour pattern encoded with Method 2 (`pal_encode(..., method=2)`) |
| 5 | `pal3_2col_64x48` | `3` (`PAL2`) | `64x48` | 65 | 2-colour checkerboard encoded with Method 3 (`encode_image`) |
| 6 | `kodim03_48x36` | `1` (`CM`) | `48x36` | 2,536 | Crop `(160, 120, 48, 36)` of `testdata/photos/kodim03.png` |
| 7 | `kodim05_64x40` | `1` (`CM`) | `64x40` | 3,722 | Crop `(180, 140, 64, 40)` of `testdata/photos/kodim05.png` |
| 8 | `kodim13_36x34` | `1` (`CM`) | `36x34` | 1,968 | Crop `(200, 140, 36, 34)` of `testdata/photos/kodim13.png` |
| 9 | `kodim23_36x34` | `1` (`CM`) | `36x34` | 1,653 | Crop `(200, 160, 36, 34)` of `testdata/photos/kodim23.png` |
| 10 | `deer_crop_36x34` | `1` (`CM`) | `36x34` | 2,707 | Crop `(220, 160, 36, 34)` of `testdata/photos/deer_crop.png` |
| 11 | `fireworks_crop_36x34` | `1` (`CM`) | `36x34` | 1,982 | Crop `(140, 200, 36, 34)` of `testdata/photos/fireworks_crop.png` |
| 12 | `flower_foveon_crop_36x34` | `1` (`CM`) | `36x34` | 1,323 | Crop `(240, 180, 36, 34)` of `testdata/photos/flower_foveon_crop.png` |
| 13 | `nightshot_iso_1600_crop_36x34` | `1` (`CM`) | `36x34` | 2,129 | Crop `(150, 220, 36, 34)` of `testdata/photos/nightshot_iso_1600_crop.png` |
| 14 | `spider_web_crop_36x34` | `1` (`CM`) | `36x34` | 896 | Crop `(160, 240, 36, 34)` of `testdata/photos/spider_web_crop.png` |

## Mutation Operators and Verification

- **Method `0` (`RAW`)**: payload bit flips, payload truncation, payload extension, header `payload_size` vs byte-length mismatch, header `(width, height)` perturbation, and random byte replacements, with `payload_crc32` and `header_crc32` recomputed so mutations reach the method decoder.
- **Method `1` (`CM`)**: stream bit flips, single-stream truncation, single-stream extension, cross-stream size-table shifts, header `(width, height)` perturbation, and random stream byte replacements, with `payload_size`, `payload_crc32`, and `header_crc32` recomputed.
- **Methods `2` (`PAL`) and `3` (`PAL2`)**: palette/stream bit flips, index-stream truncation, index-stream extension, `count_minus_one` or `stream_size` field corruption, header `(width, height)` perturbation, and random index-stream byte replacements, with `payload_size`, `payload_crc32`, and `header_crc32` recomputed.
- **Verification (`verify_mutation`)**: alternates `--threads 1` and `--threads 3` on every mutation, enforces a 60-second timeout, verifies that exit code is `0` or `4`, verifies that neither `.bmp` nor `.bmp.partial` remains on exit `4` (and no `.bmp.partial` remains on exit `0`), and checks bidirectional agreement with `rf.decode_image(blob)` on both acceptance (`returncode == 0` requires identical `(w, h, rgb)`) and refusal (`returncode == 4` requires `rf.FormatError`).

## 10-Mutation Smoke Check (`seed = 1`)

Command:

```text
python3 tests/fuzz_decode.py 10 1
python3 scripts/windows.py tests/fuzz_decode.py 10 1
```

Output:

```text
10 mutated files (methods 0:1, 1:5, 2:2, 3:2): 10 rejected, 0 still valid, no crash, hang or mismatch
```

## 20,000-Mutation Campaign (`4 x 5,000` mutations across seeds `1..4`)

Commands and summary lines:

```text
$ python3 tests/fuzz_decode.py 5000 1
5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4769 rejected, 231 still valid, no crash, hang or mismatch

$ python3 tests/fuzz_decode.py 5000 2
5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4751 rejected, 249 still valid, no crash, hang or mismatch

$ python3 tests/fuzz_decode.py 5000 3
5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4757 rejected, 243 still valid, no crash, hang or mismatch

$ python3 tests/fuzz_decode.py 5000 4
5000 mutated files (methods 0:334, 1:3331, 2:668, 3:667): 4784 rejected, 216 still valid, no crash, hang or mismatch
```

Aggregate across `20,000` mutated files (`methods 0:1336, 1:13324, 2:2672, 3:2668`): `19,061` rejected with exit code `4`, `939` accepted with exit code `0` and identical pixels to `tests/rfi_ref.py`, `0` crashes, `0` hangs, and `0` oracle mismatches.

## Method 4 addendum (`D01 T06 §8`, 2026-11-12)

The encoder writes method 4 (compact palette) since `D01 T06 §8`, so the seed set grew from 15 to 17: `pal4_200col_64x48` (index 4) and `pal4_2col_64x48` (index 7) are the two palette images as `encode_image` writes them, and the two method 3 seeds are now built explicitly with `pal_encode(..., method=3)`, as the method 2 seeds always were. The table above records the 2026-10-26 campaign and is not rewritten.

- **Method `4` (`PAL3`)**: palette-field and stream bit flips, index-stream truncation and extension (the stream has no size field, so both change `payload_size` only), corruption of the count byte or of one of the three 4-bit Rice parameters (to 0, 8, 9 or 15), one flipped bit inside the Rice-coded entries, the field's last bit (padding when the field is not byte-aligned), header `(width, height)` perturbation, and random index-stream byte replacements, with the checksums recomputed.

```text
$ python3 tests/fuzz_decode.py 2000 20261111
2000 mutated files (methods 0:118, 1:1174, 2:236, 3:236, 4:236): 1893 rejected, 107 still valid, no crash, hang or mismatch
```

## Release 1.0.0 Campaign (`D09 T01 §4`, 2026-11-13)

`D09 T01 §4` extended `tests/fuzz_decode.py` to cover all seven `.fold` methods (`0` `RAW`, `1` `CM` across `S in {1, 2, 3, 4}` and `--threads 1, 3, 6, 9`, `2` `PAL`, `3` `PAL2`, `4` `PAL3`, `5` `SC5`, `6` `SC6` -- 21 `.fold` seeds total) plus all three supported CLI input formats (`BMP`, `PNG`, `PPM`) through `--bmp`, `--ppm`, and unified parallel `--all-formats`, checking bidirectional exit-code and pixel agreement against independent Python reference loaders (`bmp_ref_load`, Pillow + `_png_is_supported_valid` / `_png_ihdr_over_limit`, `ppm_ref_load`) and `tests/rfi_ref.py`.

Linux (`bin/linux-x64/bin/rasterfold`, `5 x 30,000 = 150,000` mutations across seeds `20261113..20261117`, with seed `20261117` confirming independent per-fixture `--threads 1, 3, 6, 9` rotation across every compact and tall `S in {1, 2, 3, 4}` CM seed while executing `bin/linux-x64/bin/rasterfold` from its native path rather than `/dev/shm`):

```text
$ python3 tests/fuzz_decode.py --all-formats 30000 20261113
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22673 rejected, 2327 valid]; BMP=1667 [835 ok, 257 unsup, 507 bad, 68 limit]; PNG=1667 [244 ok, 23 unsup, 1359 bad, 41 limit]; PPM=1666 [593 ok, 148 unsup, 718 bad, 207 limit]): no crash, hang or mismatch

$ python3 tests/fuzz_decode.py --all-formats 30000 20261114
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22768 rejected, 2232 valid]; BMP=1667 [816 ok, 281 unsup, 511 bad, 59 limit]; PNG=1667 [215 ok, 20 unsup, 1373 bad, 59 limit]; PPM=1666 [558 ok, 158 unsup, 763 bad, 187 limit]): no crash, hang or mismatch

$ python3 tests/fuzz_decode.py --all-formats 30000 20261115
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22756 rejected, 2244 valid]; BMP=1667 [848 ok, 289 unsup, 478 bad, 52 limit]; PNG=1667 [217 ok, 30 unsup, 1369 bad, 51 limit]; PPM=1666 [585 ok, 176 unsup, 746 bad, 159 limit]): no crash, hang or mismatch

$ python3 tests/fuzz_decode.py --all-formats 30000 20261116
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22847 rejected, 2153 valid]; BMP=1667 [810 ok, 276 unsup, 521 bad, 60 limit]; PNG=1667 [245 ok, 27 unsup, 1344 bad, 51 limit]; PPM=1666 [629 ok, 160 unsup, 720 bad, 157 limit]): no crash, hang or mismatch

$ python3 tests/fuzz_decode.py --all-formats 30000 20261117
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22715 rejected, 2285 valid]; BMP=1667 [793 ok, 299 unsup, 517 bad, 58 limit]; PNG=1667 [223 ok, 18 unsup, 1370 bad, 56 limit]; PPM=1666 [579 ok, 154 unsup, 755 bad, 178 limit]): no crash, hang or mismatch
```

Linux aggregate across `150,000` mutated files:
- `.fold` (`125,000` mutations; `methods 0:20000, 1:5000, 2:20000, 3:20000, 4:20000, 5:20000, 6:20000`): `113,759` rejected with exit code `4`, `11,241` accepted with exit code `0` and identical pixels to `tests/rfi_ref.py`.
- Input formats (`25,000` mutations across `BMP`, `PNG`, `PPM`): `BMP=8,335` (`4,102` accepted, `1,402` unsupported, `2,534` malformed, `297` over-limit), `PNG=8,335` (`1,144` accepted, `118` unsupported, `6,815` malformed, `258` over-limit), `PPM=8,330` (`2,944` accepted, `796` unsupported, `3,702` malformed, `888` over-limit); `0` crashes, `0` hangs, and `0` oracle mismatches.

Windows (`bin/windows-x64/bin/rasterfold.exe` via `scripts/windows.py`, `5 x 30,000 = 150,000` mutations across seeds `20261113..20261117`, with seed `20261117` confirming independent per-fixture `--threads 1, 3, 6, 9` rotation across all 21 `.fold` seeds):

```text
$ python3 scripts/windows.py tests/fuzz_decode.py --all-formats 30000 20261113
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22715 rejected, 2285 valid]; BMP=1667 [813 ok, 246 unsup, 542 bad, 66 limit]; PNG=1667 [242 ok, 21 unsup, 1351 bad, 53 limit]; PPM=1666 [582 ok, 158 unsup, 738 bad, 188 limit]): no crash, hang or mismatch

$ python3 scripts/windows.py tests/fuzz_decode.py --all-formats 30000 20261114
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22750 rejected, 2250 valid]; BMP=1667 [813 ok, 284 unsup, 510 bad, 60 limit]; PNG=1667 [235 ok, 21 unsup, 1353 bad, 58 limit]; PPM=1666 [578 ok, 180 unsup, 716 bad, 192 limit]): no crash, hang or mismatch

$ python3 scripts/windows.py tests/fuzz_decode.py --all-formats 30000 20261115
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22765 rejected, 2235 valid]; BMP=1667 [808 ok, 284 unsup, 512 bad, 63 limit]; PNG=1667 [222 ok, 30 unsup, 1359 bad, 56 limit]; PPM=1666 [599 ok, 168 unsup, 724 bad, 175 limit]): no crash, hang or mismatch

$ python3 scripts/windows.py tests/fuzz_decode.py --all-formats 30000 20261116
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22751 rejected, 2249 valid]; BMP=1667 [819 ok, 289 unsup, 488 bad, 71 limit]; PNG=1667 [227 ok, 23 unsup, 1358 bad, 59 limit]; PPM=1666 [562 ok, 168 unsup, 756 bad, 180 limit]): no crash, hang or mismatch

$ python3 scripts/windows.py tests/fuzz_decode.py --all-formats 30000 20261117
30000 mutated files across all methods and input formats (fold=25000 [methods 0:4000, 1:1000, 2:4000, 3:4000, 4:4000, 5:4000, 6:4000; 22730 rejected, 2270 valid]; BMP=1667 [822 ok, 276 unsup, 513 bad, 56 limit]; PNG=1667 [208 ok, 23 unsup, 1371 bad, 65 limit]; PPM=1666 [598 ok, 168 unsup, 729 bad, 171 limit]): no crash, hang or mismatch
```

Windows aggregate across `150,000` mutated files:
- `.fold` (`125,000` mutations; `methods 0:20000, 1:5000, 2:20000, 3:20000, 4:20000, 5:20000, 6:20000`): `113,711` rejected with exit code `4`, `11,289` accepted with exit code `0` and identical pixels to `tests/rfi_ref.py`.
- Input formats (`25,000` mutations across `BMP`, `PNG`, `PPM`): `BMP=8,335` (`4,075` accepted, `1,379` unsupported, `2,565` malformed, `316` over-limit), `PNG=8,335` (`1,134` accepted, `118` unsupported, `6,792` malformed, `291` over-limit), `PPM=8,330` (`2,919` accepted, `842` unsupported, `3,663` malformed, `906` over-limit); `0` crashes, `0` hangs, and `0` oracle mismatches.
