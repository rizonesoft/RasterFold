# RasterFold research notes

Status date: 2026-09-29.

This is the research record of the codec: where it stands against lossless JPEG
XL, how size and speed are measured, every experiment that was run and what it
showed, and the designs of the ideas not yet built. The work itself is planned
and tracked in [`todo/`](../todo/README.md): each open idea here names the TODO
section that owns it, and that section carries the checklist, the test
checkpoint, and the stop condition. The format is specified in
[`spec/FOLD.md`](../spec/FOLD.md); the tools referenced below are described in
[`research/README.md`](../research/README.md).

Every number here was measured, with the command that produced it; a number
that is an estimate says so. A finished TODO experiment adds its result to
section 3, whether it succeeded or not.

| section | contents |
|---|---|
| 1 | Where the codec stands: size, speed, and memory against JPEG XL |
| 2 | The measurement protocol every change follows |
| 3 | Already tried: every experiment and its measured result |
| 4 | Compression designs |
| 5 | Speed designs |
| 6 | Memory designs |
| 7 | Robustness and format scope |
| 8 | Prior art |
| 9 | Roadmap: milestones 1 to 60 |

**Codec and format.** FOLD is the file format: the `.fold` bitstream,
specified in `spec/FOLD.md`, versioned, and frozen per version once released.
RasterFold is the reference codec that reads and writes it: the encoder and
decoder, the `rasterfold.exe` command-line tool, and (from version 1.0) the
`rasterfold.dll` library with its C API. The Python oracle `tests/rfi_ref.py`
is a second, independent decoder and encoder of the same format. A change to the
codec that keeps the bytes (speed, memory) is not a format change; a change to
what the encoder writes is.

---

## 1. Where things stand

The codec is C with no C runtime, built by `build.ps1` with the pinned
toolchain in `tools/` (Clang 23, LLD, Ninja; pins in `toolchain.json`). The
format is `.fold` version 2. Method 0 is raw RGB, method 1 ("CM") is the
context-mixing coder in `src/cm.c` (the JPEG XL weighted predictor corrected by
a 12-tap sign-sign LMS stage, five context models mixed by a logistic mixer,
and one binary range coder per channel G, R-G, B-G, with one thread per
channel), and methods 2 and 3 ("palette" and "extended palette") are the palette
index coders in `src/pal.c` for images with at most 256 distinct colours. The
Python oracle `tests/rfi_ref.py` implements the same format independently and
stays byte-identical with the C encoder.

`python tests/measure_final.py` (exit 0, PASS) on `measure/image.bmp`:

| | RasterFold | JPEG XL e7 |
|---|---:|---:|
| bytes | 1,409,028 | 1,470,703 |
| decode wall | 261 ms (3 threads) | 434 ms (ffmpeg default threads) |
| decode peak working set | 17.3 MB | 53.6 MB |

JPEG XL effort 9 gives 1,440,564 bytes on the same image, which is still
larger. `benchmarks/results.md` (from `python tests/benchmark.py`) has the encode
times and the one-thread comparisons: encode takes 0.15 of JPEG XL's time, and
single-thread decode 0.71 to 0.80 of single-thread JPEG XL's.

Compression on the research corpus (`python research/ab.py size SET -v`), as
RasterFold bytes / JPEG XL e7 bytes:

| set | images | total | geometric mean | worst |
|---|---:|---:|---:|---|
| photo (measure + Kodak + imagecompression.info crops) | 39 | 0.9712 | 0.9728 | kodim17 1.0037 |
| kodak | 24 | 0.9859 | 0.9860 | kodim17 1.0037 |
| ic | 14 | 0.9647 | 0.9517 | ic_nightshot_iso_1600 0.9942 |
| tp (test pack) | 10 | 0.9743 | 0.9722 | tp_nightshot 0.9931 |
| synth | 11 | 0.9118 | 0.5883 | syn_gradient 1.5665 |

Only kodim17 (+0.37%) and kodim09 (+0.19%) are larger than JPEG XL among the
photographs. On the generated images, with methods 2 and 3 (`D01 T01 §2` to `§4`),
stripes, solid fills, 16-pixel checker (`syn_checker`, 138 vs 316 bytes e7 and
210 bytes e9, down from 5,709), 1-pixel checker (`syn_pixel`, 52 vs 66 bytes e7,
53 bytes e9, and 58 bytes WebP, down from 599), UI (1,410 vs 2,316 e7 and 2,195
e9), text (1,447 vs 2,164 e7 and 2,038 e9), diagram (1,452 vs 2,160 e7 and 2,049
e9), wide, and noise are smaller than JPEG XL e7 (`synth` total `206,566` vs
`226,544` bytes, geometric mean `0.5883`, down from `214,773` and `1.1986`).
`syn_gradient` is 1.57 times JPEG XL e7 (4,521 vs 2,886 bytes) and `syn_odd`
(a 65x63 gradient) is 1.23 times (163 vs 132 bytes). Lossless WebP is smaller
than both codecs on most generated images, and on `syn_gradient` it takes 180
bytes where RasterFold takes 4,521 (its backward references copy shifted rows;
the match model of `D01 T04` answers it).

Speed: `research/bench.c` measures about 440 CPU cycles per sample on one
thread (G, R, and B each count as one sample) on the i5-11600K, and about 190
wall-clock cycles per sample with three threads, one per channel. Profile the
current split between modelling and decisions (2.4, item 3) before choosing a
speed idea.

Memory, from `rasterfold.exe decode --time` on `measure/image.bmp`:
`alloc_peak` is 9.16 MB (`9,161,050` bytes at 3 threads and `9,161,018` bytes
at `--threads 1`, down from 10.57 MB at Milestone 1 after `D03 T01 §1` dropped
the 1.41 MB padded stream copy and `D03 T01 §2` decoded straight into the
output BMP buffer): the input file (1.41 MB), the output BMP buffer (4.72 MB),
and the three channel models (2.95 MB). The process peak working set is
15.23 MB on Windows and 11.35 MB on Linux (down from 16.58 MB at Milestone 1).
Encode `alloc_peak` is 12.46 MB (`12,469,480` bytes, down from 17.19 MB after
`D03 T01 §3` encoded channel streams in place inside the output buffer): the
input RGB image (4.72 MB), the output buffer holding the three channel stream
slices in place (4.72 MB), and the three channel models (2.95 MB).

**Milestone 1 (`D05 T01 §1`, Locked baseline) reached 2026-10-25:** `pwsh scripts/check-all.ps1 -AllTargets` exits 0 (`check-all: 29/29 passed`); `build.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40199974368`), `plan.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40199974415`), and `pages.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40199974421`) pass on `main` (`f5e7e8a8ba4cbb131b00cf3f0c4a30143c8e548c`, and `8aeee4c707ff593c12df8ab095c9c9c117712037` at runs `40198760222`, `40198760291`, `40198760263`, `35eb5c4d11766d01026604bc788ab4663d9de073` at runs `40197473962`, `40197473971`, `40197473951`, and `91b43d439f0c19b4a42d0ac9fe62a63d4f125ba4` at runs `40195950978`, `40195950906`, `40195950947`); `benchmarks/corpus.csv` and `benchmarks/results.md` report JPEG XL e7, JPEG XL e9, and lossless WebP (`photo` 39 images: `30,865,211` bytes, `0.9712` total and `0.9728` geomean of e7, `0.9829` total and `0.9883` geomean of e9 with `28 of 39` photos below e9, `0.9041` total and `0.8932` geomean of WebP; `tp` 10 images: `6,651,583` bytes, `0.9743` total and `0.9722` geomean of e7, `0.9868` total and `0.9862` geomean of e9, `0.9027` total and `0.8811` geomean of WebP; `synth` 11 images: `214,773` bytes, `0.9480` total and `1.1986` geomean of e7, `0.9630` total and `2.2860` geomean of e9, `1.0672` total and `3.6626` geomean of WebP); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028`, `decode_wall_us=295854`, `decode_peak_ws=16576512`, `decode_codec_us=183840` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=459807`, `decode_peak_ws=51281920`); `tests/benchmark.py measure -n 3` (committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `rasterfold` `1,409,028` bytes, encode `308,760` us / `0.194x` `jxl`, decode `319,315` us / `0.647x` `jxl`, `encode_peak=16699392`, `decode_peak=16580608`, `rasterfold-1t` encode `640,604` us / `0.402x` `jxl`, decode `601,611` us / `1.220x` `jxl`, `encode_peak=16625664`, `decode_peak=16461824`, `jxl` `1,470,703` bytes, encode `1,595,197` us, decode `493,267` us, `jxl-1t` decode `721,246` us, `jxl-e9` `1,440,564` bytes, encode `10,492,083` us, decode `491,123` us, `webp` `1,588,708` bytes, encode `23,548,198` us, decode `219,895` us); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=10569996` (`10.57 MB`, `codec_us=183820`, `total_us=240767`, `peak_working_set=16576512`) at 3 threads and `alloc_peak=10569964` (`10.57 MB`, `codec_us=488047`, `total_us=557786`, `peak_working_set=16465920`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` at 3 threads records `alloc_peak=10570204`, `10.57 MB`, `codec_us=196505`, `total_us=205643`); `research/vsjxl.py -n5` records `rf_enc` median `292.8` ms (min `271.6`), `rf_dec` median `287.7` ms (min `284.0`), `jxl_enc` median `1582.9` ms (min `1549.6`), `jxl_dec` median `480.8` ms (min `465.2`), decode ratio `0.598`, encode ratio `0.185`; and `research/ab.py ab src/cm.c src/cm.c measure -n5` records `-t1` `size=1409028`, `enc min=482.7..484.1` ms (`med=496.4..499.6`), `dec min=453.5..467.2` ms (`med=481.7..498.2`) and `-t3` `enc min=152.2..168.6` ms (`med=177.9..187.6`), `dec min=167.3..177.9` ms (`med=185.2..193.2`).

**Milestone 2 (`D05 T01 §2`, Same bytes, leaner and harder) reached 2026-10-25:** against Milestone 1's gate stamp (`fbf1036de2cfd0eeaee1a55399563e2d2200328c`), `python3 scripts/windows.py research/ab.py compat fbf1036de2cfd0eeaee1a55399563e2d2200328c` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=93f6f8c6b0345f0eaf8b9556c80d0576fb3785a2b8e063a2236183b7124a278c` with `tools/llvm-mingw/bin/clang.exe`, `32f00ed24c19a6b27e82a79687478f0a5940e8d2fec6bc15de7c8e3c5e242384` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=4d7b788c8f0beba523c7385289c9dc0ac5c7112a6ef2a37de14274a4b61b118c`) and `build/research/cm_m1.c` (adapting `rf_cm_decode` with an unused `size_t stride` `(void)stride;` parameter for `research/bench.c` compatibility) and verifies that every file across all 55 distinct corpus images (60 set memberships in `research/ab.py` `SETS` order: `photo` `n=39` `total_bytes=30865211` `fold_stream_sha256=53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a`, `tp` `n=10` `total_bytes=6651583` `fold_stream_sha256=5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378`, `tp-only` `n=5` `total_bytes=4471965` `fold_stream_sha256=4af26c9b1489151130635001bdef108355e76cb5116e962404977b1e7b5c673a`, `synth` `n=11` `total_bytes=214773` `fold_stream_sha256=5a843b95ae5ea82510d9be819c5dd23ca51fd0b7d9620d2617c975824a31779f`) is full-file SHA-256 byte-identical at `--threads 1` and `--threads 3` (`identical=True`), every Milestone 1 `.fold` file decodes back to exact pixels in the candidate (`m1_decoded=True`), `KAT_FOLD` (`sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`) is unchanged, and `tests/rfi_ref.py` agrees (`python3 tests/test_correctness.py` `37 passed, 0 failed`, including `truncated-abort` refusing 4096x4096 headers with 20-byte and 27-byte CM payloads in under a second at 1 and 3 threads); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=9161050` (`9.16 MB <= 9.2 MB`, `codec_us=219883`, `total_us=295720`, `peak_working_set=15233024`, vs `10,569,996` / `10.57 MB` and `peak_working_set=16576512` at Milestone 1) at 3 threads and `alloc_peak=9161018` (`9.16 MB <= 9.2 MB`, `codec_us=466779`, `total_us=526303`, `peak_working_set=15118336`, vs `10,569,964` / `10.57 MB` and `peak_working_set=16465920` at Milestone 1) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` records `alloc_peak=9161258`, `9.16 MB`, `codec_us=196256`, `total_us=206833`, `peak_working_set=11354112` at 3 threads and `alloc_peak=9161018`, `9.16 MB`, `codec_us=528858`, `total_us=538694`, `peak_working_set=10960896` at `--threads 1`); alternating CLI timing (`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe`) records `--threads 1` decode min ratio `0.903` (`464.6/514.3` ms, med `0.889`, `474.7/533.7` ms) and `0.870` (`454.8/523.0` ms, med `0.864`, `460.7/533.3` ms) and encode min ratio `0.865` (`456.5/527.9` ms) and `0.883` (`465.1/526.8` ms), and `--threads 3` decode min ratio `0.894` (`196.7/220.0` ms, med `0.894`, `202.3/226.2` ms) and `0.854` (`198.5/232.5` ms, med `0.860`, `210.0/244.1` ms) and encode min ratio `0.954` (`194.0/203.2` ms) and `0.872` (`178.2/204.5` ms) (`research/vsjxl.py -n5` against JPEG XL e7 records `rf_enc` median `285.7` ms min `237.4`, `rf_dec` median `264.9` ms min `252.4`, `jxl_enc` median `1619.5` ms min `1577.3`, `jxl_dec` median `700.1` ms min `691.6`, decode ratio `0.378`, encode ratio `0.176`); alternating `cm.c` timing (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|3> -t<1|3>`) records `-t1` `measure` `dec=0.936` (`417.3/445.8` ms, `enc=0.886`, `419.7/473.5` ms), `photo` `dec min_tot=0.913` (`9493.7/10397.8` ms, `geomean=0.896`, `enc min_tot=0.852`, `geomean=0.843`), `tp` `dec min_tot=0.893` (`1826.1/2045.4` ms, `geomean=0.893`, `enc min_tot=0.843`, `geomean=0.847`), `tp-only` `dec min_tot=0.946` (`1140.8/1205.9` ms, `geomean=0.943`, `enc min_tot=0.889`, `geomean=0.886`), and `synth` `dec min_tot=0.888` (`590.8/665.0` ms, `geomean=0.916`, `enc min_tot=0.831`, `geomean=0.847`), and `-t3` `measure` `dec=0.902` (`152.2/168.8` ms, `enc=0.799`), `photo` `dec min_tot=0.888` (`3914.0/4410.0` ms, `geomean=0.881`, `enc min_tot=0.854`, `geomean=0.876`), `tp` `dec min_tot=0.907` (`708.2/780.8` ms, `geomean=0.890`, `enc min_tot=0.866`, `geomean=0.869`), `tp-only` `dec min_tot=0.821` (`506.2/616.3` ms, `geomean=0.814`, `enc min_tot=0.825`, `geomean=0.812`), and `synth` `dec min_tot=0.813` (`193.8/238.4` ms, `geomean=0.832`, `enc min_tot=0.855`, `geomean=0.878`), with `-n7` repeat confirmation on the `-n3` single-image scheduling-jitter outliers (`kodim17` and `syn_odd` at `-t1`: `dec=0.927`, `geomean=0.922`, `worst=kodim17(0.927)`, `enc=0.896`, `geomean=0.906`, `worst=syn_odd(0.917)`; `kodim04`, `kodim08`, and `tp_nightshot_iso_1600_crop` at `-t3`: `dec=0.908`, `geomean=0.902`, `worst=tp_nightshot_iso_1600_crop(0.920)`, `enc=0.922`, `geomean=0.920`, `worst=kodim04(0.949)`); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028`, `decode_wall_us=425906`, `decode_peak_ws=15233024`, `decode_codec_us=242547` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=530331`, `decode_peak_ws=50794496`); `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1,409,028` bytes, encode `313,424` us / `0.155x` `jxl`, decode `357,151` us / `0.632x` `jxl`, `encode_peak=16224256`, `decode_peak=15237120`, `rasterfold-1t` encode `599,359` us / `0.297x` `jxl`, decode `670,151` us / `1.187x` `jxl`, `encode_peak=16134144`, `decode_peak=15122432`, `photos` `rasterfold` `6,651,583` bytes, encode `0.252x` `jxl`, decode `0.781x` `jxl`, `encode memory=0.202x` `jxl`, `decode memory=0.299x` `jxl`, `synthetic` `rasterfold` `214,773` bytes, encode `0.703x` `jxl`, decode `0.924x` `jxl`, `encode memory=0.268x` `jxl`, `decode memory=0.304x` `jxl`); `python3 tests/fuzz_decode.py 300 20261025` and `python3 scripts/windows.py tests/fuzz_decode.py 300 20261025` exit 0 (`300 mutated files: 290 rejected, 10 still valid, no crash, hang or mismatch`); `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`); and `build.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40243867586`), `plan.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40243867645`, including `actions/jekyll-build-pages` HTML build and `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40243867488`) pass on `main` (`64c746f982231111831a13fb2da4c054fe21ac81`).

**Milestone 3 (`D05 T01 §3`, Few-colour images solved) reached 2026-10-26:** Method 2 (`D01 T01 §2`) and Method 3 (`D01 T01 §4`) are shipped in `src/pal.c`, `src/fold.c`, `tests/rfi_ref.py`, and `spec/FOLD.md`; `python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (committed in `benchmarks/corpus.csv`) report `photo` (`n=39`) `total=30865211` (`ratio=0.9712`, `geomean=0.9728` of e7; `e9_ratio=0.9829`, `e9_geomean=0.9883`; `webp_ratio=0.9041`, `webp_geomean=0.8932`), `tp` (`n=10`) `total=6651583` (`ratio=0.9743`, `geomean=0.9722` of e7; `e9_ratio=0.9868`, `e9_geomean=0.9862`; `webp_ratio=0.9027`, `webp_geomean=0.8811`), and `tp-only` (`n=5`) `total=4471965` (`ratio=0.9688`, `geomean=0.9584` of e7; `e9_ratio=0.9826`, `e9_geomean=0.9762`; `webp_ratio=0.9108`, `webp_geomean=0.8781`), all 100% unchanged from Milestone 2 (`7c3f7c72f7b4100821039d34ae5ee2313d9a82ba`), while `synth` (`n=11`) drops from `214,773` bytes (`0.9480` total / `1.1986` geomean of e7, `0.9630` total / `2.2860` geomean of e9) to `206,566` bytes (`0.9118` total / `0.5883` geomean of e7, `0.9262` total / `1.1220` geomean of e9, `1.0264` total / `1.7976` geomean of WebP) with `syn_checker=138` (`< 210` e9 and `< 316` e7, down from `5,709`) and `syn_pixel=52` (`< 53` e9, `< 66` e7, and `< 58` WebP, down from `599`) alongside `syn_solid=61`, `syn_ui=1410`, `syn_text=1447`, and `syn_diagram=1452`; against Milestone 2's gate stamp (`7c3f7c72f7b4100821039d34ae5ee2313d9a82ba`), `python3 scripts/windows.py research/ab.py compat 7c3f7c72f7b4100821039d34ae5ee2313d9a82ba --allow-diff synth` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=214a26e36582dcfc3e9da56200d9fa27ac610eb0123be77171bf25e845be9c32` with `tools/llvm-mingw/bin/clang.exe`, `0441c577f63abfbdc2f2d008a0f48033c3ff9ea5575ebc6b635464b58cecaf6b` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=d1b9f815cd088114632045d2dcba3b3c1cf07f5b5268748e445a8e26dee68017`), `build/research/cm_m1.c`, and `build/research/bench_m1.exe` (`bench_m1_exe_sha256=31cd99d489051a9e62527b414ae61e1b28ff3ddd7e52368c84ea44fb02929163` with `tools/llvm-mingw/bin/clang.exe`, `72b985fd2190e5cb34207f0f1d81db5822fb853196d81728cfa85c06722624ad` with `tools/llvm-mingw-linux/bin/clang`) and verifies `bench_m1_verified=True` across all 55 distinct corpus images, full-file `.fold` SHA-256 byte identity (`identical=True`) on `photo` (`30,865,211` bytes, `fold_stream_sha256=53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a`), `tp` (`6,651,583` bytes, `fold_stream_sha256=5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378`), and `tp-only` (`4,471,965` bytes, `fold_stream_sha256=4af26c9b1489151130635001bdef108355e76cb5116e962404977b1e7b5c673a`), `--threads 1` vs `--threads 3` determinism across all 55 corpus images including `synth` (`total_bytes=206566`, `m1_total_bytes=214773`, `fold_stream_sha256=470ec1484b293aa58aac7b46f08c8cd3f253d3801c154f5d615f6f121ee265dd`, `m1_fold_stream_sha256=5a843b95ae5ea82510d9be819c5dd23ca51fd0b7d9620d2617c975824a31779f`, `identical=False (6/11 changed)`), and exact-pixel decoding of all 55 Milestone 2 `.fold` files (`m1_decoded=True`) and all 55 candidate `.fold` files (`cand_decoded=True`) at `--threads 1` and `--threads 3` (`python3 tests/test_correctness.py` `39 passed, 0 failed`, `python3 tests/test_research_refs.py` `Ran 12 tests OK`); the 20,000-mutation multi-method fuzz campaign (`python3 tests/fuzz_decode.py 5000 1..4` in `docs/fuzz.md`: `20,000` mutations across `methods 0:1336, 1:13324, 2:2672, 3:2668`, `19,061` rejected, `939` valid, `0` crashes, `0` hangs, `0` mismatches) plus `python3 tests/fuzz_decode.py 300 1` on Linux and Windows (`300 mutated files (methods 0:20, 1:200, 2:40, 3:40): 291 rejected, 9 still valid, no crash, hang or mismatch`) pass cleanly; `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=9161050` (`9.16 MB <= 9.2 MB`, `codec_us=157279`, `total_us=224497`, `peak_working_set=15241216`) at 3 threads and `alloc_peak=9161018` (`9.16 MB <= 9.2 MB`, `codec_us=398916`, `total_us=459500`, `peak_working_set=15126528`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` records `alloc_peak=9161258`, `9.16 MB`, `codec_us=138700`, `total_us=146583`, `peak_working_set=11354112` at 3 threads and `alloc_peak=9161018`, `9.16 MB`, `codec_us=409009`, `total_us=416867`, `peak_working_set=10964992` at `--threads 1`), identical to Milestone 2; alternating CLI timing (`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe`) records `--threads 1` `decode ratio (min) 0.996 (median) 0.952 encode ratio (min) 1.021 (median) 1.025` (`alt_dec min=483.7/base_dec min=485.6` ms, `alt_enc min=495.5/base_enc min=485.3` ms) and `--threads 3` `decode ratio (min) 0.920 (median) 0.964 encode ratio (min) 1.002 (median) 0.954` (`alt_dec min=187.7/base_dec min=204.2` ms, `alt_enc min=180.7/base_enc min=180.3` ms), while `research/vsjxl.py -n5` against JPEG XL e7 records `rf_enc` median `256.8` ms min `235.3`, `rf_dec` median `268.0` ms min `237.3`, `jxl_enc` median `1546.3` ms min `1532.1`, `jxl_dec` median `713.1` ms min `705.4`, decode ratio `0.376`, encode ratio `0.166`; alternating container timing (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|7|9> -t<1|3>`) records `-t1` `measure` `B/A: enc 1.015 dec 0.983` (`dec=0.983`, `412.4/419.7` ms, `enc=1.015`, `411.8/405.9` ms), `photo` `B/A: enc 1.010 dec 1.012` (`dec min_tot=0.961`, `9399.5/9777.1` ms, `geomean=0.979`, `enc min_tot=0.959`, `9264.4/9659.7` ms, `geomean=0.973`), `tp` `B/A: enc 0.964 dec 0.939` (`dec min_tot=0.947`, `1785.7/1884.9` ms, `geomean=0.960`, `enc min_tot=0.970`, `1810.6/1866.3` ms, `geomean=0.976`), `tp-only` `B/A: enc 1.003 dec 1.002` (`dec min_tot=1.003`, `1118.7/1115.3` ms, `geomean=1.003`, `enc min_tot=1.007`, `1124.5/1117.0` ms, `geomean=1.006`), and `synth` `B/A: enc 0.416 dec 0.307` (`dec min_tot=0.308`, `150.4/487.6` ms, `geomean=0.181`, `enc min_tot=0.419`, `212.8/508.0` ms, `geomean=0.274`; `-n7` `B/A: enc 0.417 dec 0.311`, `dec min_tot=0.310`, `geomean=0.183`, `enc min_tot=0.423`, `geomean=0.278`), and `-t3` `measure` (`-n9`) `B/A: enc 1.006 dec 1.021` (`dec=1.021`, `128.9/126.3` ms, `enc=1.006`, `124.4/123.7` ms), `photo` `B/A: enc 0.972 dec 1.000` (`dec min_tot=1.015`, `3580.7/3528.6` ms, `geomean=1.007`, `enc min_tot=0.995`, `3453.5/3471.6` ms, `geomean=0.987`), `tp` `B/A: enc 1.013 dec 0.952` (`dec min_tot=0.974`, `585.2/600.6` ms, `geomean=0.983`, `enc min_tot=1.022`, `567.6/555.5` ms, `geomean=1.030`), `tp-only` `B/A: enc 0.962 dec 0.931` (`dec min_tot=0.976`, `368.7/377.8` ms, `geomean=0.974`, `enc min_tot=0.940`, `357.3/380.1` ms, `geomean=0.950`), and `synth` `B/A: enc 0.524 dec 0.402` (`dec min_tot=0.414`, `68.3/164.9` ms, `geomean=0.325`, `enc min_tot=0.526`, `86.1/163.8` ms, `geomean=0.417`; `-n7` `B/A: enc 0.548 dec 0.380`, `dec min_tot=0.382`, `geomean=0.319`, `enc min_tot=0.558`, `geomean=0.426`), with `-n7` repeat confirmation on the single-image photographic outliers (`kodim23` and `ic_artificial` at `-t1`: `dec=1.009`, `geomean=1.010`, `worst=kodim23(1.012)`, `enc=1.015`, `geomean=1.017`, `worst=kodim23(1.024)`; `kodim18`, `ic_artificial`, `tp_flower_foveon_crop`, `kodim05`, and `tp_nightshot_iso_1600_crop` at `-t3`: `dec=0.961`, `geomean=0.956`, `worst=tp_flower_foveon_crop(1.019)`, `enc=0.984`, `geomean=0.994`, plus `kodim05 -n7 -t3` `dec=0.985`, `enc=1.006`, all `<= 1.024 <= 1.030`); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028`, `decode_wall_us=315674`, `decode_peak_ws=15241216`, `decode_codec_us=157560` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=523708`, `decode_peak_ws=50937856`); and `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1,409,028` bytes, encode `301,768` us / `0.199x` `jxl`, decode `274,526` us / `0.537x` `jxl`, `encode_peak=16240640`, `decode_peak=15237120`, `rasterfold-1t` encode `539,373` us / `0.356x` `jxl`, decode `528,374` us / `1.034x` `jxl`, `encode_peak=16158720`, `decode_peak=15130624`, `photos` `rasterfold` `6,651,583` bytes, encode `0.260x` `jxl`, decode `0.702x` `jxl`, `encode memory=0.202x` `jxl`, `decode memory=0.299x` `jxl`, `synthetic` `rasterfold` `206,566` bytes / `0.588x` `jxl`, encode `0.625x` `jxl`, decode `0.702x` `jxl`, `encode memory=0.266x` `jxl`, `decode memory=0.258x` `jxl`); `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`); and `build.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40292372179`), `plan.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40292372177`, including `actions/jekyll-build-pages` HTML build and `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40292372223`) pass on `main` (`14bc33f8c16e51494414884a95a9f5d3cd47ecf6`).

---

## 2. Measurement protocol

### 2.1 Build and test

```
powershell -ExecutionPolicy Bypass -File build.ps1
python tests\test_correctness.py
python tests\fuzz_decode.py 300
python tests\measure_final.py
```

All four must pass before a change is finished; `pwsh scripts/check-all.ps1`
runs the first three with every plan and tool gate. `test_correctness.py` must
print `37 passed, 0 failed`, or more once cases are added.

### 2.2 A bitstream change touches four places

Any change to what the encoder writes (a context, a constant, the binarization,
the coder, the header) changes all of these in the same commit:

1. `src/cm.c`, plus `src/fold.c` for the container.
2. `tests/rfi_ref.py`, written from the spec text rather than translated from
   the C line by line; a translated oracle repeats the C's bugs.
3. `spec/FOLD.md`.
4. The known-answer file `KAT_FOLD` in `tests/test_correctness.py`, regenerated
   with the oracle:

   ```
   python -c "import sys; sys.path.insert(0,'tests'); import rfi_ref as rf, test_correctness as t; print(rf.encode_image(t.KAT_W, t.KAT_H, t.KAT_RGB).hex())"
   ```

   Byte 16 of the new file (the method) must still be 1; if it became 0, the
   test no longer covers the CM path and needs another small smooth image.

A new coding method gets a new method number (2, 3, ...), and old methods keep
decoding. A change to method 1 itself needs a new format version: `RF_VERSION`
in `src/rf.h` (also the fourth magic byte), `MAGIC` and `VERSION` in
`tests/rfi_ref.py`, the spec title, and the version rejection test in
`tests/test_correctness.py`. The `.fold` bitstream is a frozen behavior in the
TODO system: a section that changes it carries a freeze check, and a new format
version needs the operator's approval (`D99 T01 §3`).

### 2.3 Measuring compression

1. Build the corpus once with `python research/corpus.py`. It downloads about
   360 MB into `build/research/` (ignored by git) and needs ffmpeg with libjxl
   on the PATH.
2. Prototype in a copy: `src/cm.c` to `build/research/try.c`. `research/ab.py`
   compiles any `cm.c` variant that keeps the `rf_cm_encode` / `rf_cm_decode`
   interface.
3. Run `python research/ab.py size build/research/try.c photo`, then `synth`
   and `tp`, and compare with `python research/ab.py size photo` (which uses
   `src/cm.c`). Report `total`, `geomean`, and `worst`. The `size` mode also
   decodes and checks every image.
4. A compression change is kept only if one of these holds:
   - the photo `total` improves by at least 0.1% and `worst` gets no worse by
     more than 0.1%;
   - a whole image class, such as the synthetic set, improves clearly and the
     photo total gets no worse by more than 0.05%.
5. Nothing is judged on `measure/image.bmp` alone, and no rule, constant, or
   branch looks at the dimensions, hash, file name, or colour statistics of a
   particular image. Every rule is a general statement about pixels that the
   decoder can repeat.
6. Parameters are tuned on the `photo` set and confirmed on `tp`, which is not
   used for tuning. A change that improves `photo` while `tp` gets worse is
   overfitted.

### 2.4 Measuring speed

The development machine often runs other programs, so a single run can vary by
±40%.

1. `python research/ab.py ab src/cm.c build/research/try.c -n5 measure kodim01`
   alternates the two builds five times on one thread and prints the minimum
   and median times. Repeat with `-t3`. A minimum-time ratio below 0.97 is a
   real speed-up; between 0.97 and 1.03 is noise.
2. Process wall times against JPEG XL: `python research/vsjxl.py -n5`.
3. Where the time goes: `python research/ab.py bench -r1 measure` builds
   `build/research/bench.exe`; in `build/research`, run
   `bench.exe -p -r3 ppm\measure.ppm`, then `python ..\..\research\prof.py 40`.
   The first list is the per-function split and the second names the hot lines
   of `src/cm.c`. The first touch of a context row shows up on the line that
   loads the visit count (`uint32_t n = cp[k][0];` in `cm_code`): a cache miss,
   not arithmetic.

### 2.5 Honest reporting

Thread counts are stated on both sides. `tests/measure_final.py` encodes JPEG
XL with `-threads 1` but decodes it with ffmpeg's default threads, while
RasterFold uses three threads by default; `tests/benchmark.py` also reports
`rasterfold-1t` and `jxl-1t`, and both are quoted. Regressions and failures are
reported with the wins.

### 2.6 Measurement hygiene

1. When a result is close, `tests/measure_final.py` runs three times: the
   machine's background load moves decode walls by tens of milliseconds.
2. After a deliberate change, `benchmarks/results.md` and
   `benchmarks/results.csv` are committed with the change.
3. Every speed claim states the thread counts, the image, the command, and
   whether the machine was idle (`Idle machine (timing)` marks such sections).
4. Size is also compared with JPEG XL effort 9:
   `ffmpeg -i in.bmp -c:v libjxl -distance 0 -effort 9 -threads 1 out.jxl`
   gives 1,440,564 bytes on `measure/image.bmp`.
5. A general win is claimed only after the full `photo`, `tp`, and `synth` sets,
   with the worst image of each, not only the totals.

---

## 3. Already tried (do not repeat unchanged)

The numbers are total-size changes on the 8-image `sub` set (measure, kodim01,
kodim13, kodim23, ic_deer, ic_fireworks, ic_nightshot_iso_1600,
ic_artificial) or on the 39-image `photo` set, against the design of that
moment. Negative is smaller, which is better. Early rows come from
floating-point research harnesses; later rows from the integer codec. An
experiment is repeated only when its plan says what differs this time.

### 3.1 Prediction

| experiment | result | decision |
|---|---:|---|
| MED instead of the weighted predictor | +0.70% sub, +1.71% measure | WP kept |
| WP plus a per-context bias correction (mean bias) | +0.48% sub | rejected |
| WP plus a per-context bias correction (moving average, rate 1/32 or 1/16) | +0.91% / +1.33% sub | rejected |
| sign-sign LMS on the final errors (10 inputs) | -0.69% to -0.78% sub | kept |
| normalized LMS instead of sign-sign | -0.46% sub (sign-sign: -0.49% at the same step) | sign-sign kept (integer, cheaper) |
| LMS step 6 or 14 instead of 10 | -0.01% / +0.01% photo | 10 kept |
| LMS inputs 8(NN-N) and 8(NNE-NE) added (10 to 12 inputs) | -0.15% photo, worst image better | kept |
| LMS inputs from the four WP sub-predictions | -0.04% photo | not worth it |
| chroma prediction plus a learned multiple of the G error | -0.31% sub total, +1.1% geometric mean | replaced by the LMS inputs `FG`, `FR` |
| colour transform B-(R+G)/2 instead of B-G | +0.43% sub | B-G kept |
| B without the R residual (only G) | +0.41% sub | rejected |

### 3.2 Contexts and binarization

| experiment | result | decision |
|---|---:|---|
| 3 models instead of 5 | +0.81% sub | rejected |
| 4 models instead of 5 (three of the five dropped in turn) | +0.16% to +0.24% sub | rejected |
| 7 models: 5 plus two more neighbour contexts (4.2) | -0.22% sub | not tried in the integer codec yet (`D01 T02 §1`) |
| half-octave error-energy quantizer | +0.07% sub | rejected |
| fractional part of the prediction as a context | +0.03% to +0.23% sub | rejected |
| 8-level binary tree over the folded residual instead of the exponent code | -0.09% to -0.13% sub, but 8.0 decisions per sample instead of 3.2 | exponent code kept for speed |
| low mantissa bits raw, split k from the error energy | +0.86% to +1.65% sub | rejected |
| sign coded by one counter, not mixed | +1.03% to +1.80% sub | rejected: the mixer must see the sign |
| mantissa coded by one counter, not mixed | +0.28% to +1.15% sub | rejected |
| context rows capped at 2048 per model (instead of unbounded; 4-model design) | +0.12% sub | kept: bounded memory |
| context rows capped at 1024 per model (4-model design) | +0.35% sub | rejected |
| context rows capped at 256 per model | +1.8% on measure + kodim01 | rejected |
| method 2 palette coder (first-seen <= 256 colours, 5-bit neighbour equality pattern, D1/D2/D3 + 8-bit literal tree, 4.1, `D01 T01 §2`, `§3`) | -3.55% `synth` total (`214,773` -> `207,146` bytes, `0.9480` -> `0.9144` of e7, geomean `1.1986` -> `0.7081` of e7; `syn_checker` `5,709` -> `297`, `syn_pixel` `599` -> `90`, `syn_ui` `2,107` -> `1,560`, `syn_text` `2,118` -> `1,540`, `syn_diagram` `2,134` -> `1,553`; `photo` `30,865,211` and `tp` `6,651,583` unchanged) | kept; `syn_pixel` (`90` vs `66` e7 / `53` e9) and `syn_checker` (`297` vs `210` e9) follow-up filed as `D01 T01 §4` |
| method 3 extended palette coder (7-bit neighbour pattern adding `WW == a` and `NN == b`, fallback candidate `e` in `(d, c, ww)`, two-colour deduction after `D1 = 0`, minimum-1 counter step, 4.1, `D01 T01 §4`) | -3.82% `synth` total vs M2 (`214,773` -> `206,566` bytes, `-580` vs method 2; `0.9118` of e7, geomean `0.5883` of e7 and `1.1220` of e9; `syn_checker` `297` -> `138` `< 210` e9, `syn_pixel` `90` -> `52` `< 53` e9 and `< 58` WebP, `syn_ui` `1,560` -> `1,410`, `syn_text` `1,540` -> `1,447`, `syn_diagram` `1,553` -> `1,452`; `photo` `30,865,211` and `tp` `6,651,583` unchanged) | kept (`RF_METHOD_PAL2 = 3u`; method 2 files continue to decode) |

### 3.3 Mixing and counters

| experiment | result | decision |
|---|---:|---|
| APM/SSE stage after the mixer (context qE, or qE and the G residual) | -0.005% photo | rejected |
| int32 mixer, scale 2^16, floor update instead of rounded | +0.06% sub | rounding kept |
| int32 mixer, scale 2^14, clamped weights, floor update | +0.32% to +0.92% sub | rejected |
| int16 mixer, floor update | +1.30% sub | rejected |
| int16 mixer, scale 2^12, rounded saturating update | +0.09% sub against the int32 mixer | speed unmeasured (5.4, `D02 T02 §2`) |
| mixer learning rate 2x / 0.5x | -0.02% / +0.08% photo | unchanged |
| initial mixer weight 0.2 / 0.4 instead of 0.3 | +0.03% / +0.08% photo | unchanged |
| counters 12-bit probability plus 4-bit count per node | +0.07% sub | superseded |
| counters 16-bit, rate from one visit count per context row | +0.08% sub, decode 20% and encode 15% faster | kept |
| counters with a fixed shift of 4 or 5 | +0.37% / +0.13% sub | rejected |
| 13-bit probability plus 3-bit count, shift schedule | +0.10% sub, synthetic geometric mean +10% | rejected |
| visit-count limit 30 / 120 instead of 60 | +0.08% photo / +0.05% sub | 60 kept |

### 3.4 Speed

Minimum-time ratios from alternating A/B runs on one thread:

| change | encode | decode |
|---|---:|---:|
| branch-free decoder renormalization, rounded unclamped mixer, SSE4.1 LMS | -5% | -4% |
| one visit count per context row instead of per node | -15% | -20% |
| SSE threshold-count quantizers, WP weight lookup table | -28% | -26% |
| split each sample into a model phase and a coding phase | no measurable change | no measurable change |
| prefetch the five context rows in the model phase | -15% | -2% |
| pipeline G(x), R(x-1), B(x-2) on one thread (against sequential) | no measurable change | no measurable change |
| one thread per channel | 3.3x faster | 3.2x faster |
| profile-guided optimization (`-fprofile-instr-use`, 5.1, `D02 T01 §1`) | 0.962 to 0.988 (`measure`), 0.998 (`photo`), 0.978 (`tp`), 1.012 (`synth`) | 0.991 to 1.079 (`measure`), 1.009 (`photo`), 1.004 (`tp`), 0.995 (`synth`) -- below 5% bar; default release left unprofiled |
| link-time optimization (`-flto`, 5.2, `D02 T01 §2`) | 0.968 to 0.986 (`measure`), 0.996 (`photo`), 0.991 (`tp`), 0.939 (`synth`) | 0.905 to 0.924 (`measure`), 1.008 (`photo`), 1.019 (`tp`), 0.937 (`synth`) -- fails corpus `<= 0.970` bar on `photo` and held-out `tp` (and 3-thread `tp` is `1.086` dec / `1.267` enc); default release left non-LTO |
| hot-loop vectorization (`_mm_madd_epi16` mixer update, per-sample `rdec_tail`, `row_ctx_t` interior specialization, `qcount3` + 128-bit `perr` stores, 5.7, `D02 T01 §4`) | 0.847 (`measure` `ab.py`), 0.890 to 0.955 (`measure` `vsjxl.py`), 0.884 (`photo`), 0.895 (`tp`), 0.908 (`tp-only`), 0.817 (`synth`) | 0.927 (`measure` `ab.py`), 0.926 to 0.940 (`measure` `vsjxl.py`), 0.935 (`photo`), 0.928 (`tp`), 0.939 (`tp-only`), 0.910 (`synth`) -- kept (3-thread decode 0.995 `measure` `ab.py`, 0.789 to 0.890 `vsjxl.py`, 0.915 `photo`, 0.914 `tp`, 0.924 `tp-only`, 0.833 `synth`) |

Assembly against C (the old version 1 assembly, rebuilt with ml64, against the
current C): CRC-32 slicing-by-8 in C is 4.2x faster than the assembly CRC; the C
BMP loader takes 0.61 to 0.66 of the assembly time. No assembly remains, and
reintroducing any needs an A/B run (2.4) that shows a gain.

---

## 4. Compression designs

### 4.1 Palette method for images with few colours (method 2)

**Owner:** `D01 T01 §1` to `§4`.

**Problem.** `syn_checker` is 18.07 times JPEG XL's size and `syn_pixel` 9.08
times; both have two colours. The weighted predictor mispredicts every cell
edge, while JPEG XL and WebP turn such images into palette indices with
near-certain contexts.

**Design.** Method 2 applies when the image has at most 256 distinct RGB
colours. Payload:

```
offset  size       field
0       2          count - 1        (count = number of colours, 1..256)
2       3*count    palette: R, G, B of entry 0, then entry 1, ...
2+3c    4          size of the index stream (at least 5)
6+3c    ...        index stream: one binary range coder, the same coder as method 1
```

The palette is in first-seen raster order: entry 0 is the colour of pixel
(0, 0), and each new colour gets the next index. The decoder rejects a palette
with a repeated colour, because the encoder never writes one.

Index plane: `I(x, y)` is the palette index of pixel `(x, y)`. Neighbours use
method 1's rules (spec section "Neighbours") applied to `I` instead of `P`: at
`y == 0`, `N = NW = NE = W`, and at `x == 0`, `W = NW = N`; `W` of the very
first pixel is 0. The context of pixel `(x, y)`:

```
a = W, b = N, c = NW, d = NE
pattern = (a==b) | (a==c)<<1 | (a==d)<<2 | (b==c)<<3 | (b==d)<<4      # 0..31
hit     = 1 if the previous pixel in raster order was coded "equal to a" (decision 1 = 1), else 0
          (0 for the first pixel of the image)
```

Each decision uses one adaptive counter with method 1's update rule (16-bit
probabilities, a per-context visit count `n` that stops at 60, `RATE[n]`, and
`p >> 4` as the 12-bit coding probability). There is no mixer in the first
version.

```
D1: I == a ?                          counter D1[pattern*2 + hit]      (64 counters)
if not:
  D2 (only if b != a): I == b ?       counter D2[pattern]             (32)
  if not:
    D3 (only if d != a and d != b): I == d ?   counter D3[pattern]    (32)
    if not:
      literal: 8 bits of I, most significant first, through a binary tree
      node = 1; for bit in 7..0: code bit with counter T[node]; node = node*2 + bit
      (255 counters, T[1..255])
```

The decoded literal must be less than `count` and must not equal any candidate
already excluded (`a`, and `b` or `d` when their decision was coded "not
equal"); otherwise the file is invalid. For `count <= 2` the literal is still 8
bits; the counters learn that the high bits are always 0, which costs almost
nothing.

**Implementation notes.** The range coder moves into a shared `src/rc.h` first,
unchanged, so both methods use one coder. Colour counting uses an
open-addressing hash of 1024 `uint32_t` keys (`0xFFFFFFFF` empty, key
`R<<16 | G<<8 | B`, hash `(key * 0x9E3779B1) >> 22`, linear probing) and stops
at the 257th colour. The encoder codes both methods for images with at most 256
colours and keeps the smaller (ties: method 1); such images are rare among
photographs, so encode time is unaffected there. Measuring method 2 with
`research/ab.py` needs `research/bench.c` to call the container (`rf_encode` /
`rf_decode`) instead of `rf_cm_encode` / `rf_cm_decode`.

**Expected effect.** `syn_checker` and `syn_pixel` shrink to tens of bytes,
below JPEG XL; UI, text, and diagram images shrink further; photographs are
untouched (more than 256 colours). If `syn_pixel` stays above JPEG XL's 66
bytes, `WW == a` and `NN == b` become two more pattern bits (pattern 0..127).
In `D01 T01 §4`, method 3 (`RF_METHOD_PAL2 = 3u`) adds `WW == a` (`ww = I(x-2, y)`
for `x >= 2`, else `a`) and `NN == b` (`nn = a` at `y == 0`, `nn = b` at
`y == 1`, else `I(x, y-2)`) to form a 7-bit pattern (`0..127`), selects the
third candidate `e` as the first index in `(d, c, ww)` distinct from `a` and `b`
(or `a` if none), deduces `I = 1 - a` with zero extra bits after `D1 = 0` when
`count == 2`, and uses a minimum step of 1 on counter updates, shrinking
`syn_checker` from 297 to 138 bytes (below JPEG XL e9's 210 bytes) and
`syn_pixel` from 90 to 52 bytes (below JPEG XL e9's 53 bytes and WebP's 58
bytes) while keeping method 2 (`RF_METHOD_PAL = 2u`) decoding unchanged.

### 4.2 A sixth and seventh context model

**Owner:** `D01 T02 §1`.

**Evidence.** In the floating-point research harness two more contexts took the
`sub` total from 8,246,827 to 8,228,829 bytes (-0.22%) and improved the worst
image. They have not been tried in the integer codec.

**Design.** As the research harness defined them (`pred` and `scl` as in
`cm_model`):

```
k5 = (q9(NE - pred) * 9 + q9(NW - pred)) * 16 + qE
k6 = ((qn7(WW - pred) * 7 + qn7(NN - pred)) * 7 + qn7(NNE - pred)) * 16 + qE
```

`qn7` is `k0`'s 7-level scaled quantizer: 3 plus or minus the number of
thresholds `scl, 3*scl, 7*scl` that `6 * |d|` reaches. `WW` is `P(x-2, y)` if
`x > 1`, otherwise `W`; `cm_model` does not load it yet. `WSTRIDE` is 8, so seven
weights plus the bias still fit one weight row.

**Expected effect.** -0.1% to -0.25% on `photo` (estimate), for 10% to 20%
slower decisions (estimate: two more table rows per sample, two more
multiply-adds and counter updates per decision).

### 4.3 Two adaptation speeds per context

**Owner:** `D01 T02 §2`.

**Idea.** The counter rate falls to 1/61.5 once a row has been visited 60
times. A second, fast counter per node (fixed rate 1/16) reacts to local
change, and the mixer learns when to trust each: the two-rate state of lpaq and
paq8 (section 8).

**Design.** `NODES` doubles to 64: slot `node` stays the adaptive counter, slot
`node + 32` is the fast counter, starting at 32768 and updated by
`c += ((target - c) * 4096) >> 16`. Counter memory per channel grows from 640 KB
to 1.25 MB. The fast counters start on model 0 only (one more mixer input), then
models 0 and 1, then all five (`WSTRIDE` 16).

**Expected effect.** Unknown; an estimate of -0.2% to -0.5%, for one more
counter load and update and one more mixer term per decision per fast counter.

### 4.4 A second weight set chosen by a different context

**Owner:** `D01 T02 §3`.

**Idea.** The mixer weights depend only on `wsel` (the error energy and the G
residual class). Paq-style coders average two mixers whose weight sets are
selected by different contexts.

**Design.** A second weight table the size of `wt`, its row chosen by
`wsel2 = k2 % 128` (the JPEG-LS gradient context), a second dot product in
`code_bit`, `x = (x1 + x2) >> 1` before `SQUASH`, and both rows updated with the
same error.

**Expected effect.** An estimate of -0.2% to -0.6%, for about 15% slower
decisions.

### 4.5 Encoder-chosen parameters

**Owner:** `D01 T02 §6` (the ceiling measurement); the encoder search is filed
only if the ceiling clears 0.3%.

**Idea.** Adapt to each image without changing decoder cost: the encoder tries a
few parameter sets per channel and signals the best in a header byte (JPEG XL's
encoder adapts per image at effort 7 by learning a context tree).

**Candidates.** The LMS step (6, 10, or 16); the WP sub-predictor weight maxima
(`{13, 12, 12, 12}` today, or `{16, 10, 10, 10}`, trusting `W + NE - N` more);
the colour transform for channel 2 (`B - G` or `B - (R + G) / 2`). Encode time
grows by the number of candidates; encode takes 0.15 of JPEG XL's time, so two
or three candidates fit.

**Ceiling.** The sum over images of the best fixed candidate per image bounds
the gain; it is computed from `ab.py size -v` outputs before anything is built.

### 4.6 Learning cost on small images

**Owner:** `D01 T02 §4`.

**Problem.** kodim17 (+0.37%) and kodim09 (+0.19%), both 768x512, are the only
photographs larger than JPEG XL. Small images spend a larger share of their
samples learning from counters that start at 1/2.

**Candidates.** Initialize model 1's zero-flag counter (node 1) from its row's
energy, `qE = k1 / 81`, as `p = 65535 - 4096 * qE` clamped to `[8192, 57344]`;
start model 0's mixer weight at 0.45 (29491) and the others at 0.25 (16384); as
a last resort, share the exponent nodes' counters across neighbouring energy
bands for the first samples.

**Expected effect.** 0.1% to 0.4% on small images (estimate), nothing on large
ones.

### 4.7 Exact-repeat context for screenshots with many colours

**Owner:** `D01 T02 §5`.

**Problem.** Anti-aliased screenshots and diagrams with more than 256 colours
still contain long exact runs that method 2 cannot reach.

**Design.** A context of neighbour equalities over this channel's plane values,
as a sixth model or replacing the model whose removal costs least:

```
eq = (W==N) | (N==NE)<<1 | (W==NW)<<2 | (NW==N)<<3 | (W==WW)<<4 | (N==NN)<<5
k  = eq * 16 + qE
```

**Expected effect.** Better synthetic results and a small photo change.

### 4.8 Format version 3

**Owner:** `D01 T02 §7` (the accepted ideas measured together), `D01 T03 §1`
(the port).

Gains measured one at a time do not simply add: two models can capture the same
redundancy. The accepted ideas of 4.2 to 4.7 are combined and measured once,
then ported together as one new format version, so users see one format change
rather than one per experiment. Version 2 files keep decoding.

---

## 5. Speed designs

### 5.1 Profile-guided optimization

**Owner:** `D02 T01 §1`.

The hot paths (`code_bit`, `code_residual`, `cm_model`) are branchy; a profile
tells Clang which way the branches go. `-nostdlib` removes the profile runtime,
so the instrumented build is a separate target that links
`tools\llvm-mingw\lib\clang\23\lib\windows\libclang_rt.profile-x86_64.a` and
the C runtime with a hosted `main` entry point (`#ifdef RF_PGO_GEN`) that
returns normally so the runtime flushes `.profraw`; the release build stays
runtime-free. The profile comes from encoding and decoding the `photo` tuning
set (or deterministic non-held-out training images when the downloaded corpus is
absent, keeping held-out `tp` and `measure/image.bmp` out of training) at one
and three threads (`LLVM_PROFILE_FILE=build\pgo\%p.profraw`, merged with
`llvm-profdata merge`), and must be reproducible from a clean checkout. `ab.py`
builds its own harness, so the executables are compared with `research/vsjxl.py`
(extended with `--exe`, `--alt-exe`, and `--threads`) and corpus timing across
`photo`, `tp`, and `synth`.

**Expected effect.** 5% to 15% faster (estimate), from branch layout. Measured in `D02 T01 §1` (2026-10-25): one-thread decode ratio is `0.991` to `1.079` min (`1.005` to `1.034` median) on `measure/image.bmp`, `1.009` on `photo` (`n=39`, geomean `1.020`), `1.004` on held-out `tp` (`n=10`, geomean `1.003`), and `0.995` on `synth` (`n=11`, geomean `0.997`), missing the 5% (`<= 0.950`) bar; `--pgo-gen`, `scripts/pgo.ps1`, `scripts/pgo.py`, and `--pgo-use` are retained as opt-in tooling while default release builds stay unprofiled.

### 5.2 Link-time optimization

**Owner:** `D02 T01 §2`.

`-flto` (`--lto` in `scripts/build.py` and `research/vsjxl.py`), compiling CLI
and shared-library objects into `build/<target>/lto/*.o` with `-flto` while
keeping `librasterfold.a` archive objects as native machine code so non-LTO
static consumers link cleanly, and kept as the default only if faster (`<= 0.970`
one-thread decode ratio across `measure/image.bmp`, `photo`, and held-out `tp`
with no `> 1.020` encode or 3-thread regression) while `bin/windows-x64/bin/rasterfold.exe`
still imports only `KERNEL32.dll` and `SHELL32.dll` and `bin/windows-x64/bin/rasterfold.dll`
imports only `KERNEL32.dll`.

**Measured effect.** Measured in `D02 T01 §2` (2026-10-25): on `measure/image.bmp`, one-thread decode min ratio is `0.905` to `0.924` (`0.985` to `1.044` median) and encode min ratio is `0.968` to `0.986` (`0.907` median), while 3-thread decode min ratio is `1.033` to `1.042` (`0.868` to `1.073` median) and encode min ratio is `1.041` to `1.098` (`0.847` to `0.959` median); across the corpus (`research/vsjxl.py --corpus all -n 3 --threads <1|3> --lto`), one-thread decode `min_tot` is `1.008` on `photo` (`n=39`, geomean `0.981`, encode `0.996` / geomean `0.989`), `1.019` on held-out `tp` (`n=10`, geomean `0.999`, encode `0.991` / geomean `0.981`), and `0.937` on `synth` (`n=11`, geomean `0.944`, encode `0.939` / geomean `0.950`), and 3-thread `tp` regresses to `1.086` decode (`geomean 1.073`) and `1.267` encode (`geomean 1.239`). Because `photo` and held-out `tp` miss the `<= 0.970` bar, `--lto` is retained as a tested opt-in flag in `scripts/build.py` and `research/vsjxl.py` while default release builds stay non-LTO.

### 5.3 More than three threads: horizontal strips

**Owner:** `D02 T02 §1` (the size cost), `D01 T03 §2` (the port).

**Problem.** Three threads leave most of a 12-thread CPU idle; this is the
largest remaining speed lever.

**Design.** The encoder splits the image into `S` horizontal strips of rows
`floor(s*H/S)` to `floor((s+1)*H/S) - 1`. Each strip is coded as a whole image
of that height, with fresh model state and its own three streams; the first row
of a strip uses the `y == 0` neighbour rules, so strips are independent. The
payload becomes `S`, then `3*S` stream sizes, then the streams in strip order.
The encoder chooses `S = clamp(H / 512, 1, 4)` from the height only, never from
the thread count, so the file stays identical for any thread count. The decoder
runs up to three threads per strip; memory grows by one model set (2.95 MB) per
concurrent strip.

**Cost.** Each strip pays the learning cost again. It is measured without
writing strip code: code each strip as a separate image and add up the sizes.

**Expected effect.** About 2.5x faster decode on `measure/image.bmp` with 12
threads (estimate), for 0.2% to 0.5% larger files (estimate).

### 5.4 SIMD mixer with int16 weights

**Owner:** `D02 T02 §2`.

int16 weights at scale 2^12 with a rounded, saturating update cost 0.09% in
size (3.3). The five stretch values and the bias input (64) become int16 lanes:
the dot product is `_mm_madd_epi16` plus a horizontal add, and the update is a
multiply, a rounding add, a shift, and a saturating add. The mixer is about a
third of each decision's time, which bounds the gain. It is a format change.

### 5.5 Stop decoding a stream that has run out

**Owner:** `D04 T01 §1`.

A 32768x1024 header with a minimal 27-byte CM payload (12 framing bytes plus
three 5-byte streams; a 20-byte payload is rejected before `run()` by `sum != n`)
makes the decoder run all 100 million samples before it rejects the file. Once a
channel's read pointer reaches `end + 2`, the file will be rejected anyway.
After each row, `run_channel` and `run_pipelined` can check
`ch->dec.in >= ch->dec.lim`, set a shared abort flag and the channel's `bad`,
and publish `done = h`, so no channel waits forever on another's progress
counter.

### 5.6 AVX2 path with runtime dispatch

**Owner:** `D02 T01 §3`.

Building the whole codec with `-march=x86-64-v3` is measured first; only if
that is at least 5% faster is `src/cm.c` compiled twice (v2 and v3, entry points
renamed with `-D`), with a CPUID and `XGETBV` check in `main.c` choosing the v3
path when the CPU and the operating system support AVX2. The output must be
byte-identical.

### 5.7 Hot-loop vectorization and row-context specialization in `src/cm.c`

**Owner:** `D02 T01 §4`.

After `D02 T01 §1` (PGO) and `D02 T01 §2` (LTO) missed their corpus bars, one-thread
decode of `src/cm.c` remained within ~3% of Milestone 1. Profiling `src/cm.c` on
`measure/image.bmp` (`research/prof.py`) shows four non-bitstream-changing hot-loop
bottlenecks that can be removed on the portable/SSE4.1 (`x86-64-v2`) path while keeping
every `.fold` file byte-identical:

1. **Mixer weight update (`WUPD`) and counter aliasing in `code_bit`:** `s0..s4`, `64`,
   and `err = (bit << 12) - p` all fit in `int16_t` (`[-2047, 2047]` and `[-4095, 4095]`),
   so `((s * err + 4096) >> 13)` is computed for four lanes at a time by a single
   `_mm_madd_epi16` (`pmaddwd`) on `(s_k, 1)` and `(err, 4096)` followed by
   `_mm_srai_epi32(..., 13)`. Preloading `v0..v4 = *c0..*c4`, `rate[0..4]`, and `wv0, wv1`
   into registers before `renc_bit` / `rdec_bit` avoids reloading or serializing stores
   across `c0..c4` and `w[0..5]`.
2. **Per-sample `rdec_tail` check in `cm_code`:** one sample (`code_residual`) makes at
   most 16 binary decisions and advances `d->in` by at most 32 bytes (with a 2-byte
   lookahead in `rdec_bit`). Widening `rdec.tail` from 16 to 128 bytes (copying the last
   48 stream bytes plus 80 zero bytes, `lim = in + n - 40`, with streams of `rem <= 48`
   starting directly in `d->tail` in `rdec_init`) allows moving
   `if (d->in >= d->lim) rdec_tail(d)` out of `rdec_bit` to once per sample immediately
   after `code_residual` in `cm_code`. Running `rdec_tail` immediately after
   `code_residual` preserves the exact post-sample and end-of-row invariant of `rdec_bit`:
   any sample that enters the last 40 bytes of `in` transitions into `d->tail` (with
   `lim = d->tail + 50 = d->end + 2`) before `cm_code` returns, so the end-of-row
   exhaustion checks (`d->in >= d->lim`) in `run_pipelined` and `run_channel` trigger
   if and only if the stream has actually run past `d->end + 2`.
3. **Row-context precomputation and interior specialization:** precomputing `rgb_row`,
   `xs_row`, `r0`, `r1`, `r2`, `cur`, `prv`, and `c_off` once per row `y` in `row_ctx_t`
   removes per-pixel `% 3`, `% XROWS`, and `y * stride` addressing, and specializing
   `cm_model` on compile-time `interior = 1` for `y >= 2 && 2 <= x < w - 1` eliminates 15
   per-pixel boundary branches across the row interior.
4. **3-threshold quantizer (`qcount3`) and 128-bit `perr` stores:** replacing
   `_mm_add_epi32(_mm_sign_epi32(_mm_sub_epi32(zero, m), sgn), mid)` with
   `_mm_sub_epi32(mid, _mm_sign_epi32(m, sgn))`, dropping the dummy fourth threshold
   `0x7FFFFFFF` on `k0` and `k3` (`qcount3`), and writing `pc->perr` and `pa->perr` in
   `cm_code` with 128-bit `_mm_storeu_si128` eliminates the four-scalar-store to
   128-bit-load forwarding stall when the next pixel loads `pN->perr`.

**Measured result (2026-10-25, `D02 T01 §4`, against Milestone 1 `fbf1036de2cfd0eeaee1a55399563e2d2200328c`).**
Across all 55 distinct corpus images (60 set memberships: `photo` `n=39` `30,865,211` bytes `53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a`, held-out `tp` `n=10` `6,651,583` bytes `5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378` including `tp-only` `n=5` `4,471,965` bytes, `synth` `n=11` `214,773` bytes `5a843b95ae5ea82510d9be819c5dd23ca51fd0b7d9620d2617c975824a31779f`), every encoded `.fold` file at `--threads 1` and `--threads 3` is byte-identical between Milestone 1 (`32f00ed24c19a6b27e82a79687478f0a5940e8d2fec6bc15de7c8e3c5e242384`) and the candidate (`7e4f7dab2f7d585eb3366032f8e4a7c6bcc57af1957b5a48094f3b8cf7c15d6a`). On an observed idle machine, `python3 scripts/windows.py research/ab.py ab build/research/cm_m1.c src/cm.c measure -n5` gives `-t1` decode `0.927` min (`423.8 / 457.0 ms`, median `469.6 / 505.6 ms` = `0.929`) and encode `0.847` min (`409.4 / 483.6 ms`, median `433.2 / 497.9 ms` = `0.870`), and `-t3` decode `0.995` min (`140.6 / 141.3 ms`, median `154.9 / 198.3 ms` = `0.781`) and encode `0.856` min (`141.5 / 165.3 ms`, median `164.4 / 179.6 ms` = `0.915`). Alternating CLI runs via `python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe` on `measure/image.bmp` give `--threads 1` decode ratio `0.926` min (`454.6 / 491.2 ms`) / `0.965` median (`509.7 / 528.5 ms`) and encode ratio `0.955` min (`473.4 / 495.8 ms`) / `0.909` median (`499.0 / 548.9 ms`) on pass 1, and `--threads 1` decode ratio `0.940` min (`450.9 / 479.7 ms`) / `0.964` median (`468.2 / 485.4 ms`) and encode ratio `0.890` min (`447.9 / 503.4 ms`) / `0.890` median (`458.7 / 515.6 ms`) on pass 2; at `--threads 3`, pass 1 gives decode ratio `0.789` min (`157.7 / 199.9 ms`) / `0.940` median (`195.3 / 207.7 ms`) and encode ratio `0.998` min (`161.2 / 161.5 ms`) / `0.873` median (`176.1 / 201.6 ms`), and pass 2 gives decode ratio `0.890` min (`171.2 / 192.4 ms`) / `0.896` median (`174.3 / 194.6 ms`) and encode ratio `0.901` min (`154.5 / 171.5 ms`) / `0.882` median (`186.9 / 212.0 ms`). Across the corpus (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <photo|tp|tp-only|synth> -n3 -t<1|3>`), `photo` (`n=39`) gives `-t1` decode `B/A=0.937` (`8243.5 / 8798.9 ms`), `min_tot=0.935` (`7991.8 / 8548.2 ms`), `geomean=0.931`, `worst=ic_fireworks(1.004)` and encode `B/A=0.891` (`8212.3 / 9220.1 ms`), `min_tot=0.884` (`8011.2 / 9060.7 ms`), `geomean=0.883`, `worst=kodim19(0.938)`, and `-t3` decode `B/A=0.906` (`4481.7 / 4946.9 ms`), `min_tot=0.915` (`4257.0 / 4650.2 ms`), `geomean=0.890`, `worst=ic_fireworks(1.103)` and encode `B/A=0.867` (`4200.0 / 4843.8 ms`), `min_tot=0.908` (`4035.8 / 4443.8 ms`), `geomean=0.872`, `worst=ic_leaves_iso_1600(1.104)`; held-out `tp` (`n=10`) gives `-t1` decode `B/A=0.935` (`1763.9 / 1887.5 ms`), `min_tot=0.928` (`1726.9 / 1860.7 ms`), `geomean=0.937`, `worst=kodim13(1.014)` and encode `B/A=0.917` (`1803.2 / 1966.4 ms`), `min_tot=0.895` (`1734.1 / 1937.2 ms`), `geomean=0.892`, `worst=tp_nightshot_iso_1600_crop(0.921)`, and `-t3` decode `B/A=0.935` (`665.9 / 711.9 ms`), `min_tot=0.914` (`636.1 / 695.6 ms`), `geomean=0.907`, `worst=tp_deer_crop(1.120)` and encode `B/A=0.914` (`630.7 / 689.8 ms`), `min_tot=0.883` (`579.3 / 656.4 ms`), `geomean=0.887`, `worst=kodim05(0.977)`; exclusive held-out `tp-only` (`n=5`) gives `-t1` decode `B/A=0.980` (`1237.7 / 1263.2 ms`), `min_tot=0.939` (`1169.9 / 1246.3 ms`), `geomean=0.935`, `worst=tp_fireworks_crop(1.002)` and encode `B/A=0.913` (`1218.9 / 1335.4 ms`), `min_tot=0.908` (`1199.4 / 1320.9 ms`), `geomean=0.912`, `worst=tp_spider_web_crop(0.964)`, and `-t3` decode `B/A=0.851` (`489.1 / 575.0 ms`), `min_tot=0.924` (`489.1 / 529.3 ms`), `geomean=0.914`, `worst=tp_fireworks_crop(1.104)` and encode `B/A=0.884` (`491.5 / 556.0 ms`), `min_tot=0.869` (`483.4 / 556.0 ms`), `geomean=0.864`, `worst=tp_fireworks_crop(0.897)`; `synth` (`n=11`) gives `-t1` decode `B/A=0.932` (`580.9 / 623.1 ms`), `min_tot=0.910` (`556.9 / 612.1 ms`), `geomean=0.943`, `worst=syn_pixel(1.152)` and encode `B/A=0.837` (`545.7 / 652.1 ms`), `min_tot=0.817` (`525.3 / 643.3 ms`), `geomean=0.858`, `worst=syn_odd(1.083)`, and `-t3` decode `B/A=0.883` (`235.9 / 267.3 ms`), `min_tot=0.833` (`215.4 / 258.6 ms`), `geomean=0.820`, `worst=syn_wide(1.092)` and encode `B/A=0.825` (`209.3 / 253.6 ms`), `min_tot=0.844` (`202.9 / 240.4 ms`), `geomean=0.821`, `worst=syn_checker(1.025)`. Kept.

---

## 6. Memory designs

**Owner:** `D03 T01 §1` to `§3`.

### 6.1 Decoder: drop the padded stream copy (-1.41 MB on `measure`)

`rf_cm_decode` copies the three streams into a buffer with 8 zero bytes after
each, so `rdec_bit` can load two bytes past a stream's end without a bounds
check. The copy is unnecessary: the decoder never uses a byte past `end` in a
stream it accepts (`rdec_bit` loads `in[0]` and `in[1]` but shifts out what it
does not consume); `rf_read_file` allocates `size + 8` zeroed bytes, `rdec_init`
points at the payload directly, and `rdec` switches to a 16-byte zero-padded
in-struct tail buffer for the last 4 bytes of each stream so borrowed `size`-byte
C API inputs (`rf_decode_image`, `rf_inspect`) never read past `bytes + size`.

### 6.2 Decoder: write the BMP directly (-4.72 MB)

`rf_decode` writes packed RGB, and `rf_bmp_store` builds a second bottom-up BGR
image. `rf_cm_decode` can take an output pointer and row stride and write each
sample at `54 + stride * (h - 1 - y) + 3 * x + (2 - rgb_index)`, with
`stride = (3 * w + 3) & ~3`. The decoder reads G back at the same pixel, so it
uses the same addressing; `rf_decode`'s packed RGB output stays for `inspect`
and the tests.

### 6.3 Encoder: code straight into the output buffer (-4.72 MB)

`rf_encode` allocates the output file (32 + 3wh bytes) and `rf_cm_encode` three
more stream buffers of `wh + 64` bytes. Stream `c` can use the region at offset
`32 + 12 + c * (wh + 64)` of the output buffer (grown by 256 bytes), with streams
R and B moved down behind G after coding. A stream that overflows its region
makes the result raw, as today.

After 6.1 to 6.3, `alloc_peak` falls from 10.6 to about 9.1 MB for decode
(input file, BMP, models) and from 17.2 to about 12.4 MB for encode (RGB,
output, models) on `measure/image.bmp`. The working set is measured with
`--time`, because freed heap memory is not always returned.

---

## 7. Robustness and format scope

1. **Longer fuzzing** (`D04 T01 §2`): `python tests/fuzz_decode.py` over seeds
   that reach every method, including larger images that reach more model
   states; 20,000 mutations with their seeds on record.
2. **Grayscale images** (backlog `B-005`): when R = G = B everywhere, channels R
   and B cost almost nothing in bytes but two thirds of the single-thread
   modelling time. A flag saying "R = B = G" would code G only and copy it,
   saving CPU time but not wall time on three threads.
3. **Alpha and 16-bit samples** (backlog `B-001`): out of scope for the
   BMP-only tool. Alpha would be a fourth channel of method 1 after B, with its
   own models and stream.
4. **Exactly repeated content** (`D01 T04`, milestone 5): a match model predicting from
   the longest earlier match, as lossless WebP's backward references and lpaq's
   match model do, for shifted copies of earlier rows.

---

## 8. Prior art

The designs above build on these published codecs and papers:

- **JPEG XL** (libjxl, modular mode): the self-correcting weighted predictor
  RasterFold uses unchanged, the palette transform (4.1), and per-image context
  trees (4.5).
- **lpaq and paq8** (Matt Mahoney): logistic mixing of context models, adaptive
  counters with visit-count rates, two-rate counters (4.3), selectable mixer
  weight sets (4.4), APM/SSE (tried, 3.3), and match models (`D01 T04`).
- **CALIC** (Wu and Memon): context-based bias cancellation (tried, 3.1) and
  gradient-adjusted prediction.
- **JPEG-LS / LOCO-I** (CharLS): the MED predictor (tried, 3.1) and gradient
  contexts (`k2`).
- **LZMA** (Igor Pavlov): the binary range coder with 12-bit probabilities and
  carry propagation used by every method.
- **Lossless WebP** (libwebp): the colour cache and 2D backward references that
  make it small on synthetic images (`D01 T04`, `D01 T06`, milestone 8).

---

## 9. Roadmap: milestones 1 to 60

<!-- BEGIN milestones (research/milestones.py) -->

Each milestone keeps everything the one before it reached and adds one measurable step: no photo size more than
0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still
decoding. Milestones 1 to 10 are measured against JPEG XL and WebP; from milestone 11 RasterFold is measured
against its own previous release. Targets past milestone 10 are estimates: each is re-derived from the previous
milestone's measured results when its planning section runs. The gates live in `todo/05-milestones/`; this
table is generated from `research/milestones.py`.

### Milestones 1 to 10: Beat JPEG XL e9 and WebP, then Release 1.0

| # | milestone | target | gate |
|---|---|---|---|
| 1 | Locked baseline | `pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9 | `D05 T01 §1` (reached 2026-10-25) |
| 2 | Same bytes, leaner and harder | every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second | `D05 T01 §2` (reached 2026-10-25) |
| 3 | Few-colour images solved | method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean | `D05 T01 §3` (reached 2026-10-26) |
| 4 | Format version 3: beat JPEG XL e9 on photographs | format version 3 with the accepted model changes: photo total at most 0.975 of JPEG XL e9, test pack at most 0.980, at least 35 of 39 photos below e9, three-thread decode at most 0.6 of JPEG XL's | `D05 T01 §4` |
| 5 | Exact repeats | a match model in version 3: `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` below JPEG XL e9 (616, 271, 2195, and 2038 bytes); photos no worse than milestone 4 | `D05 T01 §5` |
| 6 | Faster than WebP to decode | horizontal strips in version 3: photo decode wall time below lossless WebP's on the 12-thread development machine; strip size cost at most 0.3% | `D05 T01 §6` |
| 7 | Every photograph beats JPEG XL e9 | 39 of 39 photos and 10 of 10 test-pack photos below JPEG XL e9; encode at most 0.3 of JPEG XL e7's time | `D05 T01 §7` |
| 8 | Beat WebP on generated images | the generated set's total and geometric mean below lossless WebP's, and every generated image within 1.1 times WebP's size | `D05 T01 §8` |
| 9 | Holds on images never tuned on | milestones 4, 7, and 8 hold on the held-out set (`python research/ab.py size heldout`) without any retuning | `D05 T01 §9` |
| 10 | RasterFold 1.0 | `rasterfold-v1.0.0` released: format version 3 frozen; Windows/Linux x64 and Android ARM64/x64 CLI, shared/static C libraries and the complete native converter; BMP, PNG, and PPM in the CLI; the full HTML docs published; 100,000 fuzz mutations clean; smaller than JPEG XL e9 and lossless WebP on every set, and faster and leaner than JPEG XL on encode, decode, and memory | `D05 T01 §10` |

### Milestones 11 to 20: RasterFold Beats RasterFold (2.0)

| # | milestone | target | gate |
|---|---|---|---|
| 11 | SIMD decisions | one-thread decode at least 10% faster than 1.0, bytes within 0.1% | `D05 T02 §2` |
| 12 | Two-layer mixing | photo total at least 0.4% smaller than milestone 11 | `D05 T02 §4` |
| 13 | Stronger prediction | photo total at least 0.5% smaller than milestone 12 | `D05 T02 §6` |
| 14 | Adaptive colour transform | photo total at least 0.3% smaller than milestone 13, decode cost unchanged | `D05 T02 §8` |
| 15 | Effort levels | a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged | `D05 T02 §10` |
| 16 | Max-effort encoder search | the max level at least 1% smaller than the default at the same decode cost | `D05 T02 §12` |
| 17 | Trained starting state | images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set | `D05 T02 §14` |
| 18 | Streaming | peak memory on a 24-megapixel image at most one third of milestone 17's | `D05 T02 §16` |
| 19 | Many-core scaling | decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2% | `D05 T02 §18` |
| 20 | RasterFold 2.0 | `rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third | `D05 T02 §20` |

### Milestones 21 to 30: Reach (3.0)

| # | milestone | target | gate |
|---|---|---|---|
| 21 | Run mode for flat regions | generated images decode at least twice as fast and photos at least 5% faster, sizes no worse | `D05 T03 §2` |
| 22 | Verified hashed context tables | decode at least 10% faster at equal memory, size within 0.1% | `D05 T03 §4` |
| 23 | Higher-precision coder | generated-image geometric mean at least 10% smaller, photos unchanged | `D05 T03 §6` |
| 24 | Learned predictor at the max level | the max level at least 1% smaller on photos, fast and default byte-identical, confirmed on the held-out set | `D05 T03 §8` |
| 25 | Texture-gated models | the default level at least 0.4% smaller at equal decode time | `D05 T03 §10` |
| 26 | Random-access decode | decoding a 1/16 region at least 8 times faster than a full decode, size within 0.1% | `D05 T03 §12` |
| 27 | Portable and byte-exact | Linux, macOS, and Windows on ARM64 builds with identical bytes on every platform | `D05 T03 §14` |
| 28 | Alpha, 16-bit, and metadata | alpha, 16-bit samples, and ICC and EXIF preserved, 8-bit RGB files byte-identical to milestone 27, the 16-bit set below JPEG XL e9 | `D05 T03 §16` |
| 29 | Coverage-proven decoder | libFuzzer branch coverage of the decoder at least 95%, no crash in 10^9 executions, hostile-input decode time at most twice honest time | `D05 T03 §18` |
| 30 | RasterFold 3.0 | `rasterfold-v3.0.0`: against 2.0, generated images at least 10% smaller, photos at the max level at least 1% smaller, decode at least 1.3 times faster, with random access, portability, alpha, and 16-bit | `D05 T03 §20` |

### Milestones 31 to 40: Beyond Single Still Files (4.0)

| # | milestone | target | gate |
|---|---|---|---|
| 31 | Language bindings and a streaming API | Python, Rust, and C# bindings over the 1.x C API and a streaming callback API, each with byte-exact round trips and API overhead at most 1% | `D05 T04 §2` |
| 32 | More formats | TIFF, 16-bit PNG, and PAM in and out with exact pixels | `D05 T04 §4` |
| 33 | Lossless JPEG recompression | JPEG files stored at least 2% smaller than JPEG XL's JPEG recompression, the original JPEG rebuilt bit for bit | `D05 T04 §6` |
| 34 | Image sequences | bursts and screen recordings smaller than lossless animated JPEG XL and animated WebP | `D05 T04 §8` |
| 35 | Gigapixel images | the size limit raised from 32 megapixels to 4 gigapixels with peak memory proportional to width | `D05 T04 §10` |
| 36 | Browser decoding | decoding in a browser at most twice native time, from a WebAssembly module under 200 KB | `D05 T04 §12` |
| 37 | Host plugins | open and save with exact pixels in Pillow, ffmpeg, libvips, and GIMP | `D05 T04 §14` |
| 38 | Second implementation | an independent Rust decoder, written from the spec alone, byte-identical on the conformance suite | `D05 T04 §16` |
| 39 | Automatic effort | at least 90% of the max level's gain at no more than 1.5 times the default encode time | `D05 T04 §18` |
| 40 | RasterFold 4.0 | `rasterfold-v4.0.0`: bindings, more formats, JPEG recompression, sequences, gigapixel images, browser decoding, and plugins, with no regression on 3.0 files | `D05 T04 §20` |

### Milestones 41 to 50: Efficiency and Trust (5.0)

| # | milestone | target | gate |
|---|---|---|---|
| 41 | GPU encode search | max-level encode at least 10 times faster on a GPU, bytes identical to the CPU search | `D05 T05 §2` |
| 42 | Energy per megapixel | decode energy per megapixel at least 20% lower than milestone 41's | `D05 T05 §4` |
| 43 | Phone-class ARM | decode per GHz on ARM64 within 1.2 times x86-64, and 4K frames at the fast level in real time | `D05 T05 §6` |
| 44 | Proven decoder bounds | bounded model checking proves no out-of-bounds access or overflow in the decoder for every input up to a stated size | `D05 T05 §8` |
| 45 | Reproducible, signed releases | the same source builds a byte-identical executable on any machine; releases are signed with a software bill of materials | `D05 T05 §10` |
| 46 | Collection archives | a library of similar photos at least 3% smaller than encoding each file alone | `D05 T05 §12` |
| 47 | Tiny decoder profile | a profile decodable in at most 256 KB of working memory by a decoder of at most 20 KB of code | `D05 T05 §14` |
| 48 | Recognised format | detected by libmagic and the major image libraries without our plugins; media type registered | `D05 T05 §16` |
| 49 | Third-party benchmarks | public benchmark results by others match our measurements within their noise | `D05 T05 §18` |
| 50 | RasterFold 5.0 | `rasterfold-v5.0.0`: lower energy, a verified decoder, reproducible signed builds, ARM parity, archives, and the tiny profile | `D05 T05 §20` |

### Milestones 51 to 60: New Data and Long-Term Robustness (6.0)

| # | milestone | target | gate |
|---|---|---|---|
| 51 | Learned model at the default level | the default photo size at least 2% smaller at no more than 1.2 times decode time | `D05 T06 §2` |
| 52 | Transmitted priors | images under 1 megapixel at least 1% smaller | `D05 T06 §4` |
| 53 | Camera raw | raw sensor data smaller than DNG's lossless JPEG and than JPEG XL on the same raw files | `D05 T06 §6` |
| 54 | Medical and scientific imagery | 16-bit grayscale and multi-channel images smaller than JPEG-LS and JPEG XL on public medical and satellite sets | `D05 T06 §8` |
| 55 | Error resilience | a file with a damaged byte still yields every undamaged tile | `D05 T06 §10` |
| 56 | Progressive decoding | a usable preview from the first 5% of the bytes, lossless at the end, for at most 1% size cost | `D05 T06 §12` |
| 57 | Lossless edit deltas | an edited copy of an image stored in at most 10% of a full file | `D05 T06 §14` |
| 58 | Hardware decoder | an FPGA decoder of the fast or tiny profile at 100 megapixels per second or more, byte-exact with the software | `D05 T06 §16` |
| 59 | Twenty-year compatibility | every file written by every release since version 2 decodes in the newest release, proven by a permanent compatibility corpus | `D05 T06 §18` |
| 60 | RasterFold 6.0 | `rasterfold-v6.0.0`: the learned default model, raw and scientific data, resilience, progressive decoding, edit deltas, and the hardware reference | `D05 T06 §20` |

<!-- END milestones -->
