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
