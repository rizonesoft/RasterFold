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
format is `.fold` version 3 (with full backward-compatibility decoding of
`.fold` version 2 files). Method 0 is raw RGB, method 1 ("CM") is the
context-mixing coder in `src/cm.c` (the JPEG XL weighted predictor corrected by
a 12-tap sign-sign LMS stage, in version 3 replaced by a gated modular gradient
where that has been more than twice as accurate on the channel (section 4.11),
seven context models mixed by a logistic mixer in version 3 and five in version 2,
16-node context rows in version 3 and 32-node rows in version 2, and up to three
warm-started horizontal strips per channel G, R-G, B-G in version 3 (`S = 1` for
`h < 768` and `S = 3` for `h >= 768`, sections 4.13 and 5.8),
running up to nine strip threads in parallel), and methods 2 and 3 ("palette" and
"extended palette") are the palette index coders in `src/pal.c` for images with at
most 256 distinct colours. The Python oracle `tests/rfi_ref.py` implements the same
format independently and stays byte-identical with the C encoder.

`python tests/measure_final.py` (exit 0, PASS) on `measure/image.bmp`:

| | RasterFold | JPEG XL e7 |
|---|---:|---:|
| bytes | 1,400,388 | 1,470,703 |
| decode wall | 229 ms (9 threads, codec 137 ms) | 467 ms (ffmpeg default threads) |
| decode peak working set | 16.4 MB | 51.3 MB |

JPEG XL effort 9 gives 1,440,564 bytes on the same image (`0.9721` ratio), which
is still larger. `benchmarks/results.md` (from `python tests/benchmark.py`) has
the encode times and the one-thread comparisons (`python research/vsjxl.py -n5`
records default-thread encode `0.153x` on Windows and `0.129x` on Linux, both
`<= 0.300x` of JPEG XL e7). On the 10-photo test pack
(`testdata/photos/`), default-thread decode takes `1.48` s (`1,478,922` us) in `tests/benchmark.py -n 3`
(vs lossless WebP `1.64` s / `1,644,275` us, `0.8994` total ratio and `1.0037` sequential per-image geomean where `tests/benchmark.py` runs codecs sequentially per image, and JPEG XL e7 `2.49` s / `2,490,472` us,
`0.6089` geomean vs WebP `0.6066` geomean) while alternating per-image CLI runs (`python research/vsjxl.py --webp-photos -n3`) take `0.96..1.00` s on Windows
(vs lossless WebP `0.97..1.03` s, `0.971..0.982` total ratio and `0.967..0.971` geomean) and `0.91..0.94` s on Linux
(vs lossless WebP `1.88..1.94` s, `0.482..0.486` total ratio and `0.489..0.490` geomean).

Compression on the research corpus (`python research/ab.py size SET -v`), as
RasterFold bytes / JPEG XL e7 bytes (and JPEG XL e9 in parentheses):

| set | images | total | geometric mean | worst |
|---|---:|---:|---:|---|
| photo (measure + Kodak + imagecompression.info crops) | 39 | 0.9639 (0.9756 e9) | 0.9622 (0.9775 e9) | kodim17 0.9934 (ic_nightshot_iso_1600 1.0097 e9) |
| kodak | 24 | 0.9729 (0.9849 e9) | 0.9726 (0.9851 e9) | kodim17 0.9934 (kodim17 1.0050 e9) |
| ic | 14 | 0.9602 (0.9712 e9) | 0.9453 (0.9650 e9) | ic_nightshot_iso_1600 0.9904 (1.0097 e9) |
| tp (test pack) | 10 | 0.9666 (0.9790 e9) | 0.9625 (0.9764 e9) | tp_nightshot 0.9897 (1.0096 e9) |
| synth | 11 | 0.8901 (0.9041 e9) | 0.3275 (0.6246 e9) | syn_noise 0.9241 (syn_odd 1.1538 e9) |

All 39 of 39 photographs are smaller than JPEG XL e7 (`worst=kodim17` at `382,299` vs
`384,823`, `0.9934`), and 35 of 39 photographs (`9 of 10` on `tp`) are below JPEG
XL e9 with no photograph more than `1.0%` larger than e9 (`e9_worst = 1.0097` on
`ic_nightshot_iso_1600` at `1,852,542` vs `1,834,783`, and `1.0096` on
`tp_nightshot_iso_1600_crop` at `1,199,268` vs `1,187,909`), with `photo` total
`30,634,873` bytes (`-233,381` bytes = `-0.756%` vs Milestone 6's `30,868,254`,
and `0 of 39` larger than Milestone 6; worst vs Milestone 6 is `ic_artificial` at
`105,262` vs `105,266`, `-0.004%`, and best is `kodim03` at `320,826` vs `326,738`,
`-1.809%`) and `tp` total `6,598,954` bytes (`-57,886` bytes = `-0.870%` vs
Milestone 6's `6,656,840`, `0 of 10` larger, worst `tp_nightshot_iso_1600_crop`
`-0.498%`). On the generated images, with methods 2 and
3 (`D01 T01 §2` to `§4`), the 7-model CM coder (`D01 T02 §1`, `D01 T02 §7`,
`D01 T03 §1`), learned gated modular prediction (`D01 T04 §8` v2, ported in
`D01 T04 §10`), warm-started horizontal strips with 16-node context rows
(`D02 T02 §3`, ported in `D01 T03 §3`), and the Milestone 7 four-part CM refinement
(`D01 T05 §3`, ported in `D01 T05 §4`), all 11 `synth` images are smaller than JPEG XL e7 and 10 of 11
are below JPEG XL e9: `syn_gradient` (`119` vs `2,886` e7, `616` e9, and `180`
WebP, down from `4,227` at Milestone 4 and `4,521` at Milestone 1),
`syn_stripes` (`113` vs `1,689` e7, `271` e9, and `150` WebP, down from `368`),
`syn_solid` (`61` vs `125` e7 and `120` e9), `syn_checker` (`138` vs `316` e7
and `210` e9, down from `5,709`), `syn_pixel` (`52` vs `66` e7, `53` e9, and
`58` WebP, down from `599`), `syn_ui` (`1,410` vs `2,316` e7 and `2,195` e9),
`syn_text` (`1,447` vs `2,164` e7 and `2,038` e9), `syn_diagram` (`1,452` vs
`2,160` e7 and `2,049` e9), `syn_wide` (`133` vs `1,906` e7, `212` e9, and `202`
WebP, down from `281`), `syn_odd` (`75` vs `132` e7 and `65` e9, `1.1538` of e9,
down from `153`), and `syn_noise` (`196,640` vs `212,784` e7 and `215,205` e9),
giving `synth` total `201,640` vs `226,544` bytes e7 (`0.8901`, geometric mean
`0.3275`) and `223,034` bytes e9 (`0.9041`, geometric mean `0.6246`, down from
`206,229` at Milestone 4, `206,566` at Milestone 3, and `214,773` at Milestone
1), and `201,248` bytes for lossless WebP (`1.0019` total, `1.0007` geometric
mean).

Milestone 8 (Beat WebP on generated images, `D05 T01 §8`) was measured on
2026-11-09 and missed: `synth` total `201,640` vs WebP `201,248` (`1.0019`,
bar below), geometric mean `1.0007` (bar below), and `syn_checker` (`138` vs
`82`), `syn_text` (`1,447` vs `1,198`), and `syn_diagram` (`1,452` vs `1,228`)
over the per-image bar (within 1.1 times WebP or at most 32 bytes larger),
from `python3 scripts/windows.py research/ab.py size src/cm.c synth -v -t1`.
Under the missed-milestone policy (`D99 T01 §4`) three follow-up experiments
are filed: coding the palette itself (`D01 T06 §4`), above-copy runs
(`D01 T06 §5`), and a neighbour-candidate colour cache (`D01 T06 §6`). The
gate stays unstamped until a follow-up reaches its bar and the gate is
re-measured. All three missed on 2026-11-09, and round 4 under the capped
policy (`D99 T01 §8`), a recent-colour cache in the palette index stream
(`D01 T06 §7`), missed on 2026-11-11 (`synth` `201,640` -> `201,732`).

**Held-out validation** (`D00 T03 §2`, 2026-11-09, `python3 scripts/windows.py research/ab.py size src/cm.c heldout -v -t1`, per-image sizes grouped by the frozen manifest's kind in `research/heldout.json`): photographs (30) total `33,307,797` bytes, `0.9412` of JPEG XL e9 and `0.9345` of e7, geometric mean `0.9324` against e9, 30 of 30 below e9 and 30 of 30 at or below e7, worst `ho_p06` at `0.9822` of e9: Milestone 4's photograph targets (total ratios and win counts) and Milestone 7's photograph targets (win counts scaled proportionally to 30 images, the 1.0% worst cap applied directly) all hold on images the codec was never tuned on. Screenshots (10) total `3,326,125` bytes against lossless WebP's `711,772` (`4.6730`), geometric mean `4.6534` against WebP, and every screenshot is over Milestone 8's per-image allowance (worst `ho_s10`, `402,076` against `78,482`, `5.1232`): Milestone 8 does not generalise to browser screenshots of rendered documentation, many-colour text images that neither the palette methods nor the CM coder prices near WebP. Reported, not tuned against: the held-out set stays evaluation-only, and the screenshot miss joins the Milestone 8 gate's parked operator decision.

Speed: `research/bench.c` measures about 450 CPU cycles per sample on one
thread (G, R, and B each count as one sample) on the i5-11600K for the 7-model
version 3 CM coder with 16-node context rows and 16-bit SIMD counter updates,
about 155 ms decode on `measure/image.bmp` with three threads, and about 115 ms
decode with nine threads across `S = 3` warm-started horizontal strips. Profile
the current split between modelling and decisions (2.4, item 3) before choosing
a speed idea.

Memory, from `rasterfold.exe decode --time` on `measure/image.bmp` (`S = 3`,
`1,410,485` bytes): `alloc_peak` is 10.29 MB (`10,291,371` bytes at `--threads 9`,
`10,291,275` bytes at `--threads 3` and `--threads 6`, and `10,291,243` bytes at
`--threads 1` on Windows; `10,292,203`, `10,291,483`, `10,291,483`, and `10,291,243`
bytes on Linux), giving codec working memory (`alloc_peak - file_size`) of
`8,880,886` bytes (`8.88 MB`) at `--threads 9` on Windows (`8,881,718` bytes on
Linux, `+4.01%` / `+4.02% <= +5.00%` over Milestone 5's single-strip `8,538,454`
bytes): the input file (1.41 MB), the output BMP buffer (4.72 MB), and the three
concurrent 7-model strip allocations (`3 * 1.39 MB = 4.16 MB`, down from `3.82 MB`
for a single strip before 16-node context rows and exact reachable model row
counts). The process peak working set is 16.25..16.37 MB on Windows and
12.23..12.37 MB on Linux.

**Milestone 1 (`D05 T01 §1`, Locked baseline) reached 2026-10-25:** `pwsh scripts/check-all.ps1 -AllTargets` exits 0 (`check-all: 29/29 passed`); `build.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40199974368`), `plan.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40199974415`), and `pages.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40199974421`) pass on `main` (`f5e7e8a8ba4cbb131b00cf3f0c4a30143c8e548c`, and `8aeee4c707ff593c12df8ab095c9c9c117712037` at runs `40198760222`, `40198760291`, `40198760263`, `35eb5c4d11766d01026604bc788ab4663d9de073` at runs `40197473962`, `40197473971`, `40197473951`, and `91b43d439f0c19b4a42d0ac9fe62a63d4f125ba4` at runs `40195950978`, `40195950906`, `40195950947`); `benchmarks/corpus.csv` and `benchmarks/results.md` report JPEG XL e7, JPEG XL e9, and lossless WebP (`photo` 39 images: `30,865,211` bytes, `0.9712` total and `0.9728` geomean of e7, `0.9829` total and `0.9883` geomean of e9 with `28 of 39` photos below e9, `0.9041` total and `0.8932` geomean of WebP; `tp` 10 images: `6,651,583` bytes, `0.9743` total and `0.9722` geomean of e7, `0.9868` total and `0.9862` geomean of e9, `0.9027` total and `0.8811` geomean of WebP; `synth` 11 images: `214,773` bytes, `0.9480` total and `1.1986` geomean of e7, `0.9630` total and `2.2860` geomean of e9, `1.0672` total and `3.6626` geomean of WebP); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028`, `decode_wall_us=295854`, `decode_peak_ws=16576512`, `decode_codec_us=183840` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=459807`, `decode_peak_ws=51281920`); `tests/benchmark.py measure -n 3` (committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `rasterfold` `1,409,028` bytes, encode `308,760` us / `0.194x` `jxl`, decode `319,315` us / `0.647x` `jxl`, `encode_peak=16699392`, `decode_peak=16580608`, `rasterfold-1t` encode `640,604` us / `0.402x` `jxl`, decode `601,611` us / `1.220x` `jxl`, `encode_peak=16625664`, `decode_peak=16461824`, `jxl` `1,470,703` bytes, encode `1,595,197` us, decode `493,267` us, `jxl-1t` decode `721,246` us, `jxl-e9` `1,440,564` bytes, encode `10,492,083` us, decode `491,123` us, `webp` `1,588,708` bytes, encode `23,548,198` us, decode `219,895` us); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=10569996` (`10.57 MB`, `codec_us=183820`, `total_us=240767`, `peak_working_set=16576512`) at 3 threads and `alloc_peak=10569964` (`10.57 MB`, `codec_us=488047`, `total_us=557786`, `peak_working_set=16465920`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` at 3 threads records `alloc_peak=10570204`, `10.57 MB`, `codec_us=196505`, `total_us=205643`); `research/vsjxl.py -n5` records `rf_enc` median `292.8` ms (min `271.6`), `rf_dec` median `287.7` ms (min `284.0`), `jxl_enc` median `1582.9` ms (min `1549.6`), `jxl_dec` median `480.8` ms (min `465.2`), decode ratio `0.598`, encode ratio `0.185`; and `research/ab.py ab src/cm.c src/cm.c measure -n5` records `-t1` `size=1409028`, `enc min=482.7..484.1` ms (`med=496.4..499.6`), `dec min=453.5..467.2` ms (`med=481.7..498.2`) and `-t3` `enc min=152.2..168.6` ms (`med=177.9..187.6`), `dec min=167.3..177.9` ms (`med=185.2..193.2`).

**Milestone 2 (`D05 T01 §2`, Same bytes, leaner and harder) reached 2026-10-25:** against Milestone 1's gate stamp (`fbf1036de2cfd0eeaee1a55399563e2d2200328c`), `python3 scripts/windows.py research/ab.py compat fbf1036de2cfd0eeaee1a55399563e2d2200328c` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=93f6f8c6b0345f0eaf8b9556c80d0576fb3785a2b8e063a2236183b7124a278c` with `tools/llvm-mingw/bin/clang.exe`, `32f00ed24c19a6b27e82a79687478f0a5940e8d2fec6bc15de7c8e3c5e242384` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=4d7b788c8f0beba523c7385289c9dc0ac5c7112a6ef2a37de14274a4b61b118c`) and `build/research/cm_m1.c` (adapting `rf_cm_decode` with an unused `size_t stride` `(void)stride;` parameter for `research/bench.c` compatibility) and verifies that every file across all 55 distinct corpus images (60 set memberships in `research/ab.py` `SETS` order: `photo` `n=39` `total_bytes=30865211` `fold_stream_sha256=53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a`, `tp` `n=10` `total_bytes=6651583` `fold_stream_sha256=5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378`, `tp-only` `n=5` `total_bytes=4471965` `fold_stream_sha256=4af26c9b1489151130635001bdef108355e76cb5116e962404977b1e7b5c673a`, `synth` `n=11` `total_bytes=214773` `fold_stream_sha256=5a843b95ae5ea82510d9be819c5dd23ca51fd0b7d9620d2617c975824a31779f`) is full-file SHA-256 byte-identical at `--threads 1` and `--threads 3` (`identical=True`), every Milestone 1 `.fold` file decodes back to exact pixels in the candidate (`m1_decoded=True`), `KAT_FOLD` (`sha256=b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32`) is unchanged, and `tests/rfi_ref.py` agrees (`python3 tests/test_correctness.py` `37 passed, 0 failed`, including `truncated-abort` refusing 4096x4096 headers with 20-byte and 27-byte CM payloads in under a second at 1 and 3 threads); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=9161050` (`9.16 MB <= 9.2 MB`, `codec_us=219883`, `total_us=295720`, `peak_working_set=15233024`, vs `10,569,996` / `10.57 MB` and `peak_working_set=16576512` at Milestone 1) at 3 threads and `alloc_peak=9161018` (`9.16 MB <= 9.2 MB`, `codec_us=466779`, `total_us=526303`, `peak_working_set=15118336`, vs `10,569,964` / `10.57 MB` and `peak_working_set=16465920` at Milestone 1) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` records `alloc_peak=9161258`, `9.16 MB`, `codec_us=196256`, `total_us=206833`, `peak_working_set=11354112` at 3 threads and `alloc_peak=9161018`, `9.16 MB`, `codec_us=528858`, `total_us=538694`, `peak_working_set=10960896` at `--threads 1`); alternating CLI timing (`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe`) records `--threads 1` decode min ratio `0.903` (`464.6/514.3` ms, med `0.889`, `474.7/533.7` ms) and `0.870` (`454.8/523.0` ms, med `0.864`, `460.7/533.3` ms) and encode min ratio `0.865` (`456.5/527.9` ms) and `0.883` (`465.1/526.8` ms), and `--threads 3` decode min ratio `0.894` (`196.7/220.0` ms, med `0.894`, `202.3/226.2` ms) and `0.854` (`198.5/232.5` ms, med `0.860`, `210.0/244.1` ms) and encode min ratio `0.954` (`194.0/203.2` ms) and `0.872` (`178.2/204.5` ms) (`research/vsjxl.py -n5` against JPEG XL e7 records `rf_enc` median `285.7` ms min `237.4`, `rf_dec` median `264.9` ms min `252.4`, `jxl_enc` median `1619.5` ms min `1577.3`, `jxl_dec` median `700.1` ms min `691.6`, decode ratio `0.378`, encode ratio `0.176`); alternating `cm.c` timing (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|3> -t<1|3>`) records `-t1` `measure` `dec=0.936` (`417.3/445.8` ms, `enc=0.886`, `419.7/473.5` ms), `photo` `dec min_tot=0.913` (`9493.7/10397.8` ms, `geomean=0.896`, `enc min_tot=0.852`, `geomean=0.843`), `tp` `dec min_tot=0.893` (`1826.1/2045.4` ms, `geomean=0.893`, `enc min_tot=0.843`, `geomean=0.847`), `tp-only` `dec min_tot=0.946` (`1140.8/1205.9` ms, `geomean=0.943`, `enc min_tot=0.889`, `geomean=0.886`), and `synth` `dec min_tot=0.888` (`590.8/665.0` ms, `geomean=0.916`, `enc min_tot=0.831`, `geomean=0.847`), and `-t3` `measure` `dec=0.902` (`152.2/168.8` ms, `enc=0.799`), `photo` `dec min_tot=0.888` (`3914.0/4410.0` ms, `geomean=0.881`, `enc min_tot=0.854`, `geomean=0.876`), `tp` `dec min_tot=0.907` (`708.2/780.8` ms, `geomean=0.890`, `enc min_tot=0.866`, `geomean=0.869`), `tp-only` `dec min_tot=0.821` (`506.2/616.3` ms, `geomean=0.814`, `enc min_tot=0.825`, `geomean=0.812`), and `synth` `dec min_tot=0.813` (`193.8/238.4` ms, `geomean=0.832`, `enc min_tot=0.855`, `geomean=0.878`), with `-n7` repeat confirmation on the `-n3` single-image scheduling-jitter outliers (`kodim17` and `syn_odd` at `-t1`: `dec=0.927`, `geomean=0.922`, `worst=kodim17(0.927)`, `enc=0.896`, `geomean=0.906`, `worst=syn_odd(0.917)`; `kodim04`, `kodim08`, and `tp_nightshot_iso_1600_crop` at `-t3`: `dec=0.908`, `geomean=0.902`, `worst=tp_nightshot_iso_1600_crop(0.920)`, `enc=0.922`, `geomean=0.920`, `worst=kodim04(0.949)`); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028`, `decode_wall_us=425906`, `decode_peak_ws=15233024`, `decode_codec_us=242547` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=530331`, `decode_peak_ws=50794496`); `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1,409,028` bytes, encode `313,424` us / `0.155x` `jxl`, decode `357,151` us / `0.632x` `jxl`, `encode_peak=16224256`, `decode_peak=15237120`, `rasterfold-1t` encode `599,359` us / `0.297x` `jxl`, decode `670,151` us / `1.187x` `jxl`, `encode_peak=16134144`, `decode_peak=15122432`, `photos` `rasterfold` `6,651,583` bytes, encode `0.252x` `jxl`, decode `0.781x` `jxl`, `encode memory=0.202x` `jxl`, `decode memory=0.299x` `jxl`, `synthetic` `rasterfold` `214,773` bytes, encode `0.703x` `jxl`, decode `0.924x` `jxl`, `encode memory=0.268x` `jxl`, `decode memory=0.304x` `jxl`); `python3 tests/fuzz_decode.py 300 20261025` and `python3 scripts/windows.py tests/fuzz_decode.py 300 20261025` exit 0 (`300 mutated files: 290 rejected, 10 still valid, no crash, hang or mismatch`); `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`); and `build.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40243867586`), `plan.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40243867645`, including `actions/jekyll-build-pages` HTML build and `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40243867488`) pass on `main` (`64c746f982231111831a13fb2da4c054fe21ac81`).

**Milestone 3 (`D05 T01 §3`, Few-colour images solved) reached 2026-10-26:** Method 2 (`D01 T01 §2`) and Method 3 (`D01 T01 §4`) are shipped in `src/pal.c`, `src/fold.c`, `tests/rfi_ref.py`, and `spec/FOLD.md`; `python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (committed in `benchmarks/corpus.csv`) report `photo` (`n=39`) `total=30865211` (`ratio=0.9712`, `geomean=0.9728` of e7; `e9_ratio=0.9829`, `e9_geomean=0.9883`; `webp_ratio=0.9041`, `webp_geomean=0.8932`), `tp` (`n=10`) `total=6651583` (`ratio=0.9743`, `geomean=0.9722` of e7; `e9_ratio=0.9868`, `e9_geomean=0.9862`; `webp_ratio=0.9027`, `webp_geomean=0.8811`), and `tp-only` (`n=5`) `total=4471965` (`ratio=0.9688`, `geomean=0.9584` of e7; `e9_ratio=0.9826`, `e9_geomean=0.9762`; `webp_ratio=0.9108`, `webp_geomean=0.8781`), all 100% unchanged from Milestone 2 (`7c3f7c72f7b4100821039d34ae5ee2313d9a82ba`), while `synth` (`n=11`) drops from `214,773` bytes (`0.9480` total / `1.1986` geomean of e7, `0.9630` total / `2.2860` geomean of e9) to `206,566` bytes (`0.9118` total / `0.5883` geomean of e7, `0.9262` total / `1.1220` geomean of e9, `1.0264` total / `1.7976` geomean of WebP) with `syn_checker=138` (`< 210` e9 and `< 316` e7, down from `5,709`) and `syn_pixel=52` (`< 53` e9, `< 66` e7, and `< 58` WebP, down from `599`) alongside `syn_solid=61`, `syn_ui=1410`, `syn_text=1447`, and `syn_diagram=1452`; against Milestone 2's gate stamp (`7c3f7c72f7b4100821039d34ae5ee2313d9a82ba`), `python3 scripts/windows.py research/ab.py compat 7c3f7c72f7b4100821039d34ae5ee2313d9a82ba --allow-diff synth` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=214a26e36582dcfc3e9da56200d9fa27ac610eb0123be77171bf25e845be9c32` with `tools/llvm-mingw/bin/clang.exe`, `0441c577f63abfbdc2f2d008a0f48033c3ff9ea5575ebc6b635464b58cecaf6b` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=d1b9f815cd088114632045d2dcba3b3c1cf07f5b5268748e445a8e26dee68017`), `build/research/cm_m1.c`, and `build/research/bench_m1.exe` (`bench_m1_exe_sha256=31cd99d489051a9e62527b414ae61e1b28ff3ddd7e52368c84ea44fb02929163` with `tools/llvm-mingw/bin/clang.exe`, `72b985fd2190e5cb34207f0f1d81db5822fb853196d81728cfa85c06722624ad` with `tools/llvm-mingw-linux/bin/clang`) and verifies `bench_m1_verified=True` across all 55 distinct corpus images, full-file `.fold` SHA-256 byte identity (`identical=True`) on `photo` (`30,865,211` bytes, `fold_stream_sha256=53289c0f674c2fec2a1775ab23d2c184d38282d2c03e3ae10f3e407bb1042f2a`), `tp` (`6,651,583` bytes, `fold_stream_sha256=5cb51c373fae35d3ec8e17e286917dd9507967b0f25b623ceac5221aa0519378`), and `tp-only` (`4,471,965` bytes, `fold_stream_sha256=4af26c9b1489151130635001bdef108355e76cb5116e962404977b1e7b5c673a`), `--threads 1` vs `--threads 3` determinism across all 55 corpus images including `synth` (`total_bytes=206566`, `m1_total_bytes=214773`, `fold_stream_sha256=470ec1484b293aa58aac7b46f08c8cd3f253d3801c154f5d615f6f121ee265dd`, `m1_fold_stream_sha256=5a843b95ae5ea82510d9be819c5dd23ca51fd0b7d9620d2617c975824a31779f`, `identical=False (6/11 changed)`), and exact-pixel decoding of all 55 Milestone 2 `.fold` files (`m1_decoded=True`) and all 55 candidate `.fold` files (`cand_decoded=True`) at `--threads 1` and `--threads 3` (`python3 tests/test_correctness.py` `39 passed, 0 failed`, `python3 tests/test_research_refs.py` `Ran 12 tests OK`); the 20,000-mutation multi-method fuzz campaign (`python3 tests/fuzz_decode.py 5000 1..4` in `docs/fuzz.md`: `20,000` mutations across `methods 0:1336, 1:13324, 2:2672, 3:2668`, `19,061` rejected, `939` valid, `0` crashes, `0` hangs, `0` mismatches) plus `python3 tests/fuzz_decode.py 300 1` on Linux and Windows (`300 mutated files (methods 0:20, 1:200, 2:40, 3:40): 291 rejected, 9 still valid, no crash, hang or mismatch`) pass cleanly; `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=9161050` (`9.16 MB <= 9.2 MB`, `codec_us=157279`, `total_us=224497`, `peak_working_set=15241216`) at 3 threads and `alloc_peak=9161018` (`9.16 MB <= 9.2 MB`, `codec_us=398916`, `total_us=459500`, `peak_working_set=15126528`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` records `alloc_peak=9161258`, `9.16 MB`, `codec_us=138700`, `total_us=146583`, `peak_working_set=11354112` at 3 threads and `alloc_peak=9161018`, `9.16 MB`, `codec_us=409009`, `total_us=416867`, `peak_working_set=10964992` at `--threads 1`), identical to Milestone 2; alternating CLI timing (`python3 scripts/windows.py research/vsjxl.py -n5 --threads <1|3> --exe build/research/vs/rasterfold-m1.exe --alt-exe bin/windows-x64/bin/rasterfold.exe`) records `--threads 1` `decode ratio (min) 0.996 (median) 0.952 encode ratio (min) 1.021 (median) 1.025` (`alt_dec min=483.7/base_dec min=485.6` ms, `alt_enc min=495.5/base_enc min=485.3` ms) and `--threads 3` `decode ratio (min) 0.920 (median) 0.964 encode ratio (min) 1.002 (median) 0.954` (`alt_dec min=187.7/base_dec min=204.2` ms, `alt_enc min=180.7/base_enc min=180.3` ms), while `research/vsjxl.py -n5` against JPEG XL e7 records `rf_enc` median `256.8` ms min `235.3`, `rf_dec` median `268.0` ms min `237.3`, `jxl_enc` median `1546.3` ms min `1532.1`, `jxl_dec` median `713.1` ms min `705.4`, decode ratio `0.376`, encode ratio `0.166`; alternating container timing (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|7|9> -t<1|3>`) records `-t1` `measure` `B/A: enc 1.015 dec 0.983` (`dec=0.983`, `412.4/419.7` ms, `enc=1.015`, `411.8/405.9` ms), `photo` `B/A: enc 1.010 dec 1.012` (`dec min_tot=0.961`, `9399.5/9777.1` ms, `geomean=0.979`, `enc min_tot=0.959`, `9264.4/9659.7` ms, `geomean=0.973`), `tp` `B/A: enc 0.964 dec 0.939` (`dec min_tot=0.947`, `1785.7/1884.9` ms, `geomean=0.960`, `enc min_tot=0.970`, `1810.6/1866.3` ms, `geomean=0.976`), `tp-only` `B/A: enc 1.003 dec 1.002` (`dec min_tot=1.003`, `1118.7/1115.3` ms, `geomean=1.003`, `enc min_tot=1.007`, `1124.5/1117.0` ms, `geomean=1.006`), and `synth` `B/A: enc 0.416 dec 0.307` (`dec min_tot=0.308`, `150.4/487.6` ms, `geomean=0.181`, `enc min_tot=0.419`, `212.8/508.0` ms, `geomean=0.274`; `-n7` `B/A: enc 0.417 dec 0.311`, `dec min_tot=0.310`, `geomean=0.183`, `enc min_tot=0.423`, `geomean=0.278`), and `-t3` `measure` (`-n9`) `B/A: enc 1.006 dec 1.021` (`dec=1.021`, `128.9/126.3` ms, `enc=1.006`, `124.4/123.7` ms), `photo` `B/A: enc 0.972 dec 1.000` (`dec min_tot=1.015`, `3580.7/3528.6` ms, `geomean=1.007`, `enc min_tot=0.995`, `3453.5/3471.6` ms, `geomean=0.987`), `tp` `B/A: enc 1.013 dec 0.952` (`dec min_tot=0.974`, `585.2/600.6` ms, `geomean=0.983`, `enc min_tot=1.022`, `567.6/555.5` ms, `geomean=1.030`), `tp-only` `B/A: enc 0.962 dec 0.931` (`dec min_tot=0.976`, `368.7/377.8` ms, `geomean=0.974`, `enc min_tot=0.940`, `357.3/380.1` ms, `geomean=0.950`), and `synth` `B/A: enc 0.524 dec 0.402` (`dec min_tot=0.414`, `68.3/164.9` ms, `geomean=0.325`, `enc min_tot=0.526`, `86.1/163.8` ms, `geomean=0.417`; `-n7` `B/A: enc 0.548 dec 0.380`, `dec min_tot=0.382`, `geomean=0.319`, `enc min_tot=0.558`, `geomean=0.426`), with `-n7` repeat confirmation on the single-image photographic outliers (`kodim23` and `ic_artificial` at `-t1`: `dec=1.009`, `geomean=1.010`, `worst=kodim23(1.012)`, `enc=1.015`, `geomean=1.017`, `worst=kodim23(1.024)`; `kodim18`, `ic_artificial`, `tp_flower_foveon_crop`, `kodim05`, and `tp_nightshot_iso_1600_crop` at `-t3`: `dec=0.961`, `geomean=0.956`, `worst=tp_flower_foveon_crop(1.019)`, `enc=0.984`, `geomean=0.994`, plus `kodim05 -n7 -t3` `dec=0.985`, `enc=1.006`, all `<= 1.024 <= 1.030`); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028`, `decode_wall_us=315674`, `decode_peak_ws=15241216`, `decode_codec_us=157560` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=523708`, `decode_peak_ws=50937856`); and `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1,409,028` bytes, encode `301,768` us / `0.199x` `jxl`, decode `274,526` us / `0.537x` `jxl`, `encode_peak=16240640`, `decode_peak=15237120`, `rasterfold-1t` encode `539,373` us / `0.356x` `jxl`, decode `528,374` us / `1.034x` `jxl`, `encode_peak=16158720`, `decode_peak=15130624`, `photos` `rasterfold` `6,651,583` bytes, encode `0.260x` `jxl`, decode `0.702x` `jxl`, `encode memory=0.202x` `jxl`, `decode memory=0.299x` `jxl`, `synthetic` `rasterfold` `206,566` bytes / `0.588x` `jxl`, encode `0.625x` `jxl`, decode `0.702x` `jxl`, `encode memory=0.266x` `jxl`, `decode memory=0.258x` `jxl`); `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`); and `build.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40292372179`), `plan.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40292372177`, including `actions/jekyll-build-pages` HTML build and `drift-check.py --site-dir build/site-raw`), and `pages.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40292372223`) pass on `main` (`14bc33f8c16e51494414884a95a9f5d3cd47ecf6`).

**Milestone 4 (`D05 T01 §4`, Format version 3: beat JPEG XL e9 on photographs) reached 2026-10-29:** Format Version 3 (`D01 T02 §1`, `D01 T02 §7`, `D01 T03 §1`, with `NMODEL 7` adding neighbour models `k5` and `k6` in `src/cm.c`, `src/fold.c`, `tests/rfi_ref.py`, and `spec/FOLD.md`, while preserving full decoding of format version 2 `.fold` files via `NMODEL_V2 5`) is shipped under the 7-model envelope authorized in `D99 T01 §4`; `python3 research/ab.py size photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (committed in `benchmarks/corpus.csv`) report `photo` (`n=39`) `total=30812732` (`ratio=0.9695 <= 0.970`, `geomean=0.9698`, `worst=1.0000` (`kodim17`) of e7; `e9_ratio=0.9813 <= 0.982`, `e9_geomean=0.9853`, `e9_worst=1.0152` (`kodim16`) of e9; `webp_ratio=0.9026`, `webp_geomean=0.8905`; `measure=1405087`, `0.9554` of e7, `0.9754` of e9, `0.8844` of WebP; `31 of 39` photos below e9, `38 of 39` at or below e7 (`kodim17` `384,835` vs e7 `384,823`, `1.00003`), and `39 of 39` smaller than Milestone 3's `30,865,211`), `tp` (`n=10`) `total=6641390` (`ratio=0.9728 <= 0.973`, `geomean=0.9700`, `worst=0.9928` (`tp_nightshot_iso_1600_crop`) of e7; `e9_ratio=0.9853 <= 0.986`, `e9_geomean=0.9840`, `e9_worst=1.0128` (`tp_nightshot_iso_1600_crop`) of e9; `webp_ratio=0.9013`, `webp_geomean=0.8791`; `7 of 10` below e9, `10 of 10` below e7, and `10 of 10` smaller than Milestone 3's `6,651,583`), `tp-only` (`n=5`) `total=4467731` (`ratio=0.9679`, `geomean=0.9570` of e7; `e9_ratio=0.9817`, `e9_geomean=0.9749` of e9; `webp_ratio=0.9099`, `webp_geomean=0.8769`; `5 of 5` smaller than Milestone 3's `4,471,965`), and `synth` (`n=11`) `total=206229` (`ratio=0.9103`, `geomean=0.5760` of e7; `e9_ratio=0.9247`, `e9_geomean=1.0986` of e9; `webp_ratio=1.0248`, `webp_geomean=1.7601`, down from Milestone 3's `206,566`, with 4 Method 1 `synth` images smaller (`syn_stripes`, `syn_gradient`, `syn_odd`, `syn_wide`), the 6 palette `synth` images unchanged, and the raw (method 0) `syn_noise` unchanged at `196,640` bytes); against Milestone 3's gate stamp (`b181fb1edef63b42d29350a8b24096307a8596ee`), `python3 scripts/windows.py research/ab.py compat b181fb1edef63b42d29350a8b24096307a8596ee --allow-diff photo,tp,tp-only,synth` builds `build/research/vs/rasterfold-m1.exe` (`m1_exe_sha256=1d0456af74651fb81c4d74ce68ef9a14ce1c48392bc16a1c427797716720d1d0` with `tools/llvm-mingw/bin/clang.exe`, `7f7e36a92a4b3c1b6d96bf82701ebbe51cf5e73afb9fa469477b153aeb363ad6` with `tools/llvm-mingw-linux/bin/clang`; `cand_exe_sha256=5fdbda282a15f8239774faef849421688160f9f788087f5d957e456e394bdf19`), `build/research/cm_m1.c`, and `build/research/bench_m1.exe` (`bench_m1_exe_sha256=4b4dadb9a9b1278a911a005a1c7f428afe11ea3f57692dce0878cee99444991d` with `tools/llvm-mingw/bin/clang.exe`, `b3fe8bb4fb24b6c30e36353e997b0c8e19a161235cb4b57f252eef22834c431f` with `tools/llvm-mingw-linux/bin/clang`) and verifies `bench_m1_verified=True` across all 55 distinct corpus images (`photo` `fold_stream_sha256=a8c6930300afbfab908117f45f9f90f4c0dae0672fc5e2d7d67bcb0134482ce8`, `tp` `fold_stream_sha256=ba1c9f903c340ff32edbd1af6cf092762ce5713f221a2c1fcad529d064fff3d1`, `tp-only` `fold_stream_sha256=80493965c183205199b195fcd1496401c7530771e1b08e85dd3bdfad4cd1c6e9`, `synth` `fold_stream_sha256=d1423b01b9ba91790ff67c8a4b107d4e824efe962481fa04062fddc36dbb232f`), `--threads 1` vs `--threads 3` determinism across all 55 corpus images, and exact-pixel decoding of all 55 Milestone 3 (version 2) `.fold` files (`m1_decoded=True`), all 55 candidate (version 3) `.fold` files (`cand_decoded=True`), and all 55 version 2 freeze files in `build/research/fold-v2/` at `--threads 1` and `--threads 3` (`python3 tests/test_correctness.py` `39 passed, 0 failed`, `python3 tests/test_research_refs.py` `Ran 12 tests OK`, `python3 tests/fuzz_decode.py 300 1` on Linux and Windows `300 mutated files (methods 0:20, 1:200, 2:40, 3:40): 284 rejected, 16 still valid, no crash, hang or mismatch`); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=9943541` (`9.94 MB <= 10.0 MB`, `codec_us=272794`, `total_us=364962`, `peak_working_set=16089088`) at 3 threads and `alloc_peak=9943509` (`9.94 MB <= 10.0 MB`, `codec_us=631439`, `total_us=703329`, `peak_working_set=15978496`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` records `alloc_peak=9943749`, `9.94 MB`, `codec_us=265880`, `total_us=285015`, `peak_working_set=18546688` at 3 threads and `alloc_peak=9943509`, `9.94 MB`, `codec_us=832886`, `total_us=850717`, `peak_working_set=18546688` at `--threads 1`, adding `2 * 2048 * 32 * 2 * 3 = 786,432` bytes for the `k5`/`k6` counter tables minus `3,941` bytes on the smaller version 3 `measure` input file over Milestone 3's 5-model `9,161,050` bytes); two runs of `python3 scripts/windows.py research/vsjxl.py -n5 --threads 3` against JPEG XL e7 record `rf_enc` median `347.4` ms min `313.5`, `rf_dec` median `307.0` ms min `300.4`, `jxl_enc` median `832.2` ms min `663.2`, `jxl_dec` median `566.9` ms min `513.0`, `decode ratio (median) 0.541 encode ratio 0.417` (`min(rf_dec) / min(jxl_dec) = 300.4 / 513.0 = 0.586 <= 0.6`) and `rf_enc` median `413.9` ms min `300.0`, `rf_dec` median `361.9` ms min `311.4`, `jxl_enc` median `881.6` ms min `677.4`, `jxl_dec` median `616.4` ms min `548.2`, `decode ratio (median) 0.587 encode ratio 0.469` (`min(rf_dec) / min(jxl_dec) = 311.4 / 548.2 = 0.568 <= 0.6`), while `python3 scripts/windows.py research/vsjxl.py -n5` records `rf_enc` median `388.1` ms min `322.4`, `rf_dec` median `386.1` ms min `345.4`, `jxl_enc` median `2023.3` ms min `1762.2`, `jxl_dec` median `835.9` ms min `750.6`, `decode ratio (median) 0.462 encode ratio 0.192` (`min(rf_dec) / min(jxl_dec) = 345.4 / 750.6 = 0.460 <= 0.6`); alternating container timing (`python3 scripts/windows.py research/ab.py ab -v build/research/cm_m1.c src/cm.c <set> -n<5|7|9> -t<1|3>`) against Milestone 3's 5-model `cm_m1.c` records `-t1` `measure` (`-n9`) `B/A: enc 1.131 dec 1.088` (`dec=1.088`, `493.5/453.4` ms, `enc=1.131`, `510.7/451.6` ms), `photo` (`-n5`) `B/A: enc 1.204 dec 1.159` (`dec min_tot=1.192`, `11115.2/9325.4` ms, `geomean=1.211`, `enc min_tot=1.215`, `11291.9/9294.4` ms, `geomean=1.235`), `tp` (`-n7`) `B/A: enc 1.063 dec 1.073` (`dec min_tot=1.089`, `2133.8/1959.3` ms, `geomean=1.089`, `enc min_tot=1.084`, `2146.2/1980.4` ms, `geomean=1.091`), `tp-only` (`-n5`) `B/A: enc 1.190 dec 1.176` (`dec min_tot=1.187`, `1402.8/1181.4` ms, `geomean=1.173`, `enc min_tot=1.183`, `1389.4/1174.0` ms, `geomean=1.179`), and `synth` (`-n5`) `B/A: enc 1.253 dec 1.215` (`dec min_tot=1.230`, `237.5/193.1` ms, `geomean=1.134`, `enc min_tot=1.262`, `315.0/249.6` ms, `geomean=1.175`), and `-t3` `measure` (`-n9`) `B/A: enc 1.038 dec 1.076` (`dec=1.076`, `173.6/161.4` ms, `enc=1.038`, `187.7/180.9` ms), `photo` (`-n5`) `B/A: enc 1.199 dec 1.227` (`dec min_tot=1.200`, `4947.9/4121.8` ms, `geomean=1.201`, `enc min_tot=1.171`, `4623.9/3947.5` ms, `geomean=1.168`), `tp` (`-n5`) `B/A: enc 1.144 dec 1.215` (`dec min_tot=1.198`, `731.9/610.9` ms, `geomean=1.190`, `enc min_tot=1.094`, `687.9/628.6` ms, `geomean=1.094`), `tp-only` (`-n9`) `B/A: enc 0.970 dec 1.014` (`dec min_tot=0.945`, `539.0/570.4` ms, `geomean=0.938`, `enc min_tot=0.979`, `542.1/553.8` ms, `geomean=0.950`), and `synth` (`-n7`) `B/A: enc 0.965 dec 1.131` (`dec min_tot=1.053`, `85.3/81.0` ms, `geomean=1.013`, `enc min_tot=0.973`, `103.9/106.8` ms, `geomean=0.975`), with `-n7` repeat confirmation on the photographic single-image outliers (`kodim07`, `kodim08`, `kodim13`, `kodim23` at `-t1`: `dec=1.073`, `geomean=1.110`, `worst=kodim07(1.245)`, `enc=1.132`, `geomean=1.172`, all photographic images `<= 1.245 <= 1.270`, while tiny 65x63 `syn_odd` reflects the fixed `~0.7` ms cost of zeroing `786 KB` of `k5`/`k6` counter tables on `1.3..1.4` ms base time; `kodim06`, `ic_leaves_iso_1600`, `tp_deer_crop`, `syn_gradient`, `syn_stripes` at `-t3`: `dec=1.054`, `geomean=1.075`, `worst=kodim06(1.266)`, plus `tp_deer_crop -n7 -t3` `dec=1.035`, `enc=1.113`, all confirmed `<= 1.266 <= 1.270`); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1405087`, `decode_wall_us=530249`, `decode_peak_ws=16089088`, `decode_codec_us=283140` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=641234`, `decode_peak_ws=50892800`); and `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1,405,087` bytes / `0.955x` `jxl`, encode `397,167` us / `0.243x` `jxl`, decode `418,628` us / `0.816x` `jxl`, `encode_peak=17002496` / `0.182x` `jxl`, `decode_peak=16093184` / `0.315x` `jxl`, `rasterfold-1t` encode `1,039,277` us / `0.635x` `jxl`, decode `1,126,239` us / `2.195x` `jxl`, `encode_peak=17022976`, `decode_peak=15978496`, `photos` `rasterfold` `6,641,390` bytes / `0.970x` `jxl`, encode `0.277x` `jxl`, decode `0.856x` `jxl`, `encode memory=0.217x` `jxl`, `decode memory=0.323x` `jxl`, `synthetic` `rasterfold` `206,229` bytes / `0.576x` `jxl`, encode `0.693x` `jxl`, decode `0.935x` `jxl`, `encode memory=0.280x` `jxl`, `decode memory=0.267x` `jxl`); `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`).

**Milestone 5 (`D05 T01 §5`, Exact repeats) reached 2026-11-05:** Learned gated modular prediction (`D01 T04 §8`'s variant `v2` (`g4`, context off, margin 2), ported to `src/cm.c`, `tests/rfi_ref.py`, `spec/FOLD.md`, and `tests/test_correctness.py` in `D01 T04 §10`) is shipped in Format Version 3 under the envelope authorized in `D99 T01 §4`; `python3 research/ab.py size src/cm.c photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (committed in `benchmarks/corpus.csv`) report `synth` (`n=11`) `total=201634` (`ratio=0.8900`, `geomean=0.3264`, `worst=0.9241` (`syn_noise`) of e7; `e9_ratio=0.9041`, `e9_geomean=0.6225`, `e9_worst=1.2000` (`syn_odd`) of e9; `webp_ratio=1.0019`, `webp_geomean=0.9974`, `webp_worst=1.6829` (`syn_checker`) of WebP, down from Milestone 4's `206,229` bytes / `e9_geomean=1.0986` / `webp_geomean=1.7601`, with `syn_gradient=119 < 616` e9 vs Milestone 4's `4,227`, `syn_stripes=108 < 271` e9 vs Milestone 4's `368`, `syn_ui=1410 < 2195` e9, `syn_text=1447 < 2038` e9, `syn_odd=78` vs Milestone 4's `153`, `syn_wide=129` vs Milestone 4's `281`, and every `synth` image below JPEG XL e7), `photo` (`n=39`) `total=30795260` (`ratio=0.9690`, `geomean=0.9691`, `worst=1.0000` (`kodim17`) of e7; `e9_ratio=0.9807`, `e9_geomean=0.9846`, `e9_worst=1.0152` (`kodim16`) of e9; `webp_ratio=0.9021`, `webp_geomean=0.8898`; `measure=1405118`, `0.9554` of e7, `0.9754` of e9, `0.8844` of WebP; `-17,472` bytes = `-0.0567%` below Milestone 4's `30,812,732`, `0 of 39` photographs more than `+0.1%` larger than Milestone 4 with worst `kodim03` `325,824` vs `325,720` = `+104` bytes = `+0.0319%` and best `kodim20` `319,213` vs `322,418` = `-3,205` bytes = `-0.994%`, `31 of 39` below e9, and `38 of 39` at or below e7), `tp` (`n=10`) `total=6640275` (`ratio=0.9727`, `geomean=0.9698`, `worst=0.9926` (`tp_nightshot_iso_1600_crop`) of e7; `e9_ratio=0.9852`, `e9_geomean=0.9838`, `e9_worst=1.0126` (`tp_nightshot_iso_1600_crop`) of e9; `webp_ratio=0.9011`, `webp_geomean=0.8789`; `-1,115` bytes = `-0.0168% <= +0.1%` below Milestone 4's `6,641,390`, `7 of 10` below e9, and `10 of 10` below e7), and `tp-only` (`n=5`) `total=4467232` (`ratio=0.9678`, `geomean=0.9569` of e7; `e9_ratio=0.9816`, `e9_geomean=0.9748` of e9; `webp_ratio=0.9098`, `webp_geomean=0.8768`; `-499` bytes = `-0.0112%` below Milestone 4's `4,467,731`); all 55 format version 2 `.fold` files in `build/research/fold-v2/` decode to exact source pixels at `--threads 1` and `--threads 3` on both `bin/linux-x64/bin/rasterfold` and `bin/windows-x64/bin/rasterfold.exe` (`55/55 exact at --threads 1 and --threads 3`), while Milestone 4's superseded version 3 `KAT_FOLD3` (`91` bytes from `0c1ec08`) is cleanly refused with exit code `4` (`rasterfold: invalid fold: cm stream`); pooled alternating timing (`python3 build/d05-t01-s5/abpool.py run <log> -t<1|3> <rounds> <set>` against Milestone 4's `build/research/cm_m4.c` from `0c1ec08ab66b2eca8d8ac66a1bce83c9e25cffe8`, with `uptime` 1/5/15-min loads `0.34..1.14 / 0.46..1.04 / 0.70..1.01`) records two disjoint agreeing pools per set and thread count: `measure -t1` (`[0:5]` `enc=1.057` `515.4/487.4` ms, `dec=1.076` `520.7/483.7` ms, `geomean enc=1.057 dec=1.076`; `[5:10]` `enc=1.071` `524.9/490.3` ms, `dec=1.054` `518.9/492.3` ms, `geomean enc=1.071 dec=1.054`), `measure -t3` (`[5:10]` `enc=0.992` `150.0/151.2` ms, `dec=0.980` `164.6/167.9` ms, `geomean enc=0.992 dec=0.980`; `[10:15]` `enc=1.026` `145.2/141.5` ms, `dec=1.038` `158.4/152.6` ms, `geomean enc=1.026 dec=1.038`; standalone `-n7` `enc=1.058` `157.8/149.2` ms, `dec=1.042` `164.7/158.1` ms), `photo -t1` (`[0:5]` `enc=1.030` `9656.5/9370.8` ms, `dec=1.055` `9764.6/9257.6` ms, `geomean enc=1.029 dec=1.052`, `worst>=16384px enc=ic_artificial(1.105) dec=ic_artificial(1.118)`; `[5:10]` `enc=1.030` `9635.5/9352.3` ms, `dec=1.048` `9726.0/9282.5` ms, `geomean enc=1.029 dec=1.047`, `worst>=16384px enc=ic_artificial(1.091) dec=ic_artificial(1.118)`), `photo -t3` (`[0:5]` `enc=1.012` `3132.5/3096.7` ms, `dec=1.014` `3295.2/3250.5` ms, `geomean enc=1.004 dec=1.015`, `worst>=16384px enc=kodim09(1.111) dec=kodim11(1.093)`; `[5:10]` `enc=1.020` `3176.0/3112.3` ms, `dec=1.027` `3361.9/3272.2` ms, `geomean enc=1.009 dec=1.024`, `worst>=16384px enc=ic_nightshot_iso_1600(1.123) dec=ic_spider_web(1.088)`), `tp -t1` (`[0:5]` `enc=1.040` `2106.5/2024.9` ms, `dec=1.053` `2135.0/2028.2` ms, `geomean enc=1.037 dec=1.051`, `worst>=16384px enc=tp_flower_foveon_crop(1.078) dec=tp_spider_web_crop(1.099)`; `[5:10]` `enc=1.032` `2135.9/2070.5` ms, `dec=1.038` `2154.3/2075.1` ms, `geomean enc=1.033 dec=1.039`, `worst>=16384px enc=tp_flower_foveon_crop(1.120) dec=tp_flower_foveon_crop(1.087)`), `tp -t3` (`[0:5]` `enc=1.004` `708.2/705.7` ms, `dec=0.996` `722.0/724.9` ms, `geomean enc=1.008 dec=0.994`, `worst>=16384px enc=tp_spider_web_crop(1.048) dec=kodim01(1.019)`; `[5:10]` `enc=1.010` `716.4/709.1` ms, `dec=0.998` `729.5/731.3` ms, `geomean enc=1.003 dec=1.007`, `worst>=16384px enc=tp_fireworks_crop(1.123) dec=kodim01(1.090)`), `tp-only -t1` (`[0:5]` `enc=1.046` `1396.9/1334.9` ms, `dec=1.070` `1404.7/1313.4` ms, `geomean enc=1.050 dec=1.075`, `worst>=16384px enc=tp_spider_web_crop(1.080) dec=tp_spider_web_crop(1.117)`; `[5:10]` `enc=1.042` `1385.1/1329.0` ms, `dec=1.059` `1394.5/1316.5` ms, `geomean enc=1.048 dec=1.063`, `worst>=16384px enc=tp_spider_web_crop(1.077) dec=tp_spider_web_crop(1.085)`), `tp-only -t3` (`[0:5]` `enc=1.037` `445.2/429.4` ms, `dec=1.014` `460.0/453.5` ms, `geomean enc=1.043 dec=1.013`, `worst>=16384px enc=tp_spider_web_crop(1.095) dec=tp_deer_crop(1.058)`; `[5:10]` `enc=1.027` `443.3/431.8` ms, `dec=1.018` `463.6/455.5` ms, `geomean enc=1.033 dec=1.018`, `worst>=16384px enc=tp_spider_web_crop(1.086) dec=tp_fireworks_crop(1.045)`), `synth -t1` (`[0:5]` `enc=1.112` `266.4/239.6` ms, `dec=1.117` `192.2/172.1` ms, `geomean enc=1.105 dec=1.030`, `worst>=16384px enc=syn_ui(1.548) dec=syn_wide(1.154)`, `small<16384px syn_odd enc +0.00ms dec +0.00ms`; `[5:10]` `enc=1.094` `264.8/242.0` ms, `dec=1.101` `190.9/173.4` ms, `geomean enc=1.085 dec=1.029`, `worst>=16384px enc=syn_ui(1.535) dec=syn_stripes(1.144)`, `small<16384px syn_odd enc +0.00ms dec +0.00ms`; standalone `syn_ui -n7 -t1` `enc=1.566` `24.9/15.9` ms, `dec=1.049` `6.4/6.1` ms, where `syn_ui` encodes as Method 3 (`1,410` bytes, `cap=1,378`) and `run_pipelined` checks `tot > m->cap` every 32 rows (`(y & 31u) == 0u`), so Milestone 4 crosses `1,378` at `y=64` (`65` rows) while Milestone 5 compresses `syn_ui`'s top gradient bar better and stays under `1,378` at `y=64`, encoding to the next check at `y=96` (`97` rows, `97/65 = 1.492x` rows) before aborting in favour of Method 3), and `synth -t3` (`[0:5]` `enc=1.036` `89.9/86.8` ms, `dec=1.023` `72.1/70.5` ms, `geomean enc=1.031 dec=0.999`, `worst>=16384px enc=syn_diagram(1.111) dec=syn_wide(1.054)`, `small<16384px syn_odd enc +0.00ms dec +0.00ms`; `[5:10]` `enc=1.030` `91.7/89.0` ms, `dec=1.031` `73.0/70.8` ms, `geomean enc=1.022 dec=1.012`, `worst>=16384px enc=syn_text(1.115) dec=syn_stripes(1.071)`, `small<16384px syn_odd enc -0.10ms dec -0.10ms`), with every set total `<= 1.117 <= 1.15x` and every image `>= 16,384` pixels `<= 1.566 <= 1.60x` (`<= 1.154x` on all other 53 images `>= 16,384` pixels); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` (`1,405,118` bytes vs Milestone 4's `1,405,087`, `+31` bytes) records `alloc_peak=9943572` (`codec_us=205005`, `total_us=255045`, `peak_working_set=16080896`) at 3 threads and `alloc_peak=9943540` (`codec_us=522543`, `total_us=593318`, `peak_working_set=15978496`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` records `alloc_peak=9943780`, `codec_us=183761`, `total_us=191974`, `peak_working_set=12439552` at 3 threads and `alloc_peak=9943540`, `codec_us=531273`, `total_us=538456`, `peak_working_set=12025856` at `--threads 1`, each `+31` bytes over Milestone 4's `9,943,541` / `9,943,509` on Windows and `9,943,749` / `9,943,509` on Linux, with codec working memory unchanged); `python3 tests/test_correctness.py` and `python3 scripts/windows.py tests/test_correctness.py` print `40 passed, 0 failed`; `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1405118 decode_wall_us=303666 decode_peak_ws=16089088 decode_codec_us=178370 pixels=True` vs `jxl bytes=1470703 decode_wall_us=446414 decode_peak_ws=51949568`); and `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `measure` `rasterfold` `1,405,118` bytes / `0.955x` `jxl`, encode `337,290` us / `0.232x` `jxl`, decode `343,785` us / `0.756x` `jxl`, `encode_peak=17084416` / `0.183x` `jxl`, `decode_peak=16089088` / `0.309x` `jxl`, `rasterfold-1t` encode `652,397` us / `0.449x` `jxl`, decode `677,068` us / `1.489x` `jxl`, `encode_peak=17006592`, `decode_peak=15978496`, `photos` `rasterfold` `6,640,275` bytes / `0.970x` `jxl`, encode `0.283x` `jxl`, decode `0.839x` `jxl`, `encode memory=0.217x` `jxl`, `decode memory=0.318x` `jxl`, `synthetic` `rasterfold` `201,634` bytes / `0.326x` `jxl`, encode `0.812x` `jxl`, decode `0.976x` `jxl`, `encode memory=0.281x` `jxl`, `decode memory=0.267x` `jxl`); `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`).

**Milestone 6 (`D05 T01 §6`, Faster than WebP to decode) reached 2026-11-06:** Warm-started horizontal strips (`S = (h >= 768 ? 3 : (h >= 512 ? 2 : 1))` with decoder acceptance of `S in {1, 2, 3, 4}`, `NODES = 16`, reachable model row counts `MROWS_C` and `WSEL_C = {32, 96, 96}`, single-block `cm_alloc_strips`, and 16-bit SIMD counter updates with 64-bit-clamped `int64_t dot` in `code_bit7`, designed and measured in `D02 T02 §3` and ported in `D01 T03 §3` after `D02 T02 §1`/`D01 T03 §2` rejected cold-start strips) are shipped in Format Version 3; `python3 scripts/windows.py tests/benchmark.py -n 3` (all sets: `measure`, `photos`, `synthetic`, committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `photos` default-thread `rasterfold` decode wall time total `1,297,941` us / `1.30` s vs lossless `webp` `1,497,906` us / `1.50` s, `0.8665x` total and `0.8988x` geometric mean `< 1.000x`, and `0.568x` `jxl` geometric mean vs `webp` `0.632x` `jxl`, `encode_us=1,316,258` / `0.201x` `jxl`, `encode_peak=14,512,128` / `0.210x` `jxl`, `decode_peak=13,840,384` / `0.308x` `jxl`, `rasterfold-1t` `encode_us=2,626,204` / `0.391x` `jxl`, `decode_us=2,698,498` / `1.155x` `jxl`; `measure` `rasterfold` `1,410,485` bytes / `0.959x` `jxl`, `encode_us=230,234` / `0.165x` `jxl`, `decode_us=242,564` / `0.565x` `jxl`, `encode_peak=17,498,112` / `0.188x` `jxl`, `decode_peak=16,412,672` / `0.315x` `jxl`, `rasterfold-1t` `encode_us=565,242` / `0.404x` `jxl`, `decode_us=587,298` / `1.368x` `jxl`; `synthetic` `rasterfold` `201,638` bytes / `0.328x` `jxl`, `encode_us=778,201` / `0.507x` `jxl`, `decode_us=714,993` / `0.629x` `jxl` vs `webp` `906,313` us / `0.808x` `jxl`, `encode_peak=11,403,264` / `0.231x` `jxl`, `decode_peak=8,658,944` / `0.241x` `jxl`), and alternating 3-repeat `testdata/photos/` CLI decode wall-time runs (`python3 build/d01-t03-s3/bench_photos_webp.py`) record `windows-x64` `--threads 9` `rf_decode_us=1,273,789` (`1.27` s) vs `webp_decode_us=1,431,836` (`1.43` s, `ratio=0.890`, `geomean=0.938`) and default threads `rf_decode_us=1,270,325` (`1.27` s) vs `webp_decode_us=1,408,615` (`1.41` s, `ratio=0.902`, `geomean=0.940`), and `linux-x64` `--threads 9` `rf_decode_us=630,744` (`0.63` s) vs `webp_decode_us=1,663,143` (`1.66` s, `ratio=0.379`, `geomean=0.383`) and default threads `rf_decode_us=658,696` (`0.66` s) vs `webp_decode_us=1,617,791` (`1.62` s, `ratio=0.407`, `geomean=0.411`); `python3 research/ab.py size src/cm.c photo -v`, `tp -v`, `tp-only -v`, and `synth -v` (committed in `benchmarks/corpus.csv`) report `photo` (`n=39`) `total=30868254` (`+72,994` B = `+0.2370% <= +0.300%` over Milestone 5's `30,795,260`, `0 of 39` photographs more than `+0.5%` larger with worst `kodim24` `434,031 -> 435,918` = `+0.4348%` and best `ic_artificial` `105,420 -> 105,266` = `-0.1461%`; `ratio=0.9712`, `geomean=0.9719`, `worst=1.0037` (`kodim17`) of e7; `e9_ratio=0.9830`, `e9_geomean=0.9874`, `e9_worst=1.0186` (`kodim16`) of e9; `webp_ratio=0.9042`, `webp_geomean=0.8924`; `measure=1410485`, `0.9591` of e7, `0.9791` of e9, `0.8878` of WebP), `tp` (`n=10`) `total=6656840` (`+16,565` B = `+0.2495% <= +0.300%` over Milestone 5's `6,640,275`, `0 of 10` photographs more than `+0.5%` larger with worst `tp_flower_foveon_crop` `516,250 -> 518,444` = `+0.4250%`; `ratio=0.9751`, `geomean=0.9726`, `worst=0.9946` (`tp_nightshot_iso_1600_crop`) of e7; `e9_ratio=0.9876`, `e9_geomean=0.9866`, `e9_worst=1.0146` (`tp_nightshot_iso_1600_crop`) of e9; `webp_ratio=0.9034`, `webp_geomean=0.8815`), `tp-only` (`n=5`) `total=4476915` (`+9,683` B = `+0.2168% <= +0.300%` over Milestone 5's `4,467,232`; `ratio=0.9699`, `geomean=0.9594` of e7; `e9_ratio=0.9837`, `e9_geomean=0.9773` of e9; `webp_ratio=0.9118`, `webp_geomean=0.8791`), and `synth` (`n=11`) `total=201638` (`+4` B = `+0.0020% <= +0.300%` over Milestone 5's `201,634`; `ratio=0.8901`, `geomean=0.3275`, `worst=0.9241` (`syn_noise`) of e7; `e9_ratio=0.9041`, `e9_geomean=0.6247`, `e9_worst=1.2154` (`syn_odd`) of e9; `webp_ratio=1.0019`, `webp_geomean=1.0008`, `webp_worst=1.6829` (`syn_checker`), with `syn_gradient=120 < 616` e9, `syn_stripes=109 < 271` e9, `syn_ui=1410 < 2195` e9, `syn_text=1447 < 2038` e9, `syn_checker=138 < 210` e9, `syn_pixel=52 < 53` e9); all 55 format version 2 `.fold` files in `build/research/fold-v2/` decode to exact source pixels at `--threads 1, 3, 6, 9` on both `bin/linux-x64/bin/rasterfold` and `bin/windows-x64/bin/rasterfold.exe`, while Milestone 5's superseded single-strip version 3 `KAT_FOLD3` (`79` bytes from `b4d6b8b:tests/test_correctness.py`, `payload[0]=19`) is cleanly refused with exit code `4` (`rasterfold: invalid fold: cm strips`) across `--threads 1, 3, 6, 9`; pooled alternating timing (`python3 research/abpool.py run <log> build/research/cm_m5.c src/cm.c -t<1|3> 5 <set>` against Milestone 5's `build/research/cm_m5.c` `sha256=4183692c4518b6c9e27edea2b4d09a0cb3c37cb83e1a03713e6fdb8e1fb18999`, with `uptime` 1/5/15-min loads `0.20..1.29 / 0.35..0.94 / 1.04..2.56` in `build/d05-t01-s6/pool-load.log`) records two disjoint agreeing 5-round pools (`[0:5]` and `[5:10]`) per set and thread count with `over bar: 0 []` across all 20 pool reports: `measure -t1` (`[0:5]` `enc=0.934` `460.0/492.6` ms, `dec=0.940` `469.2/499.0` ms, `geomean enc=0.934 dec=0.940`; `[5:10]` `enc=0.941` `462.9/491.7` ms, `dec=0.938` `473.5/505.0` ms, `geomean enc=0.941 dec=0.938`), `measure -t3` (`[0:5]` `enc=1.072` `158.3/147.7` ms, `dec=0.951` `154.4/162.3` ms, `geomean enc=1.072 dec=0.951`; `[5:10]` `enc=1.063` `155.9/146.6` ms, `dec=1.004` `156.8/156.2` ms, `geomean enc=1.063 dec=1.004`), `photo -t1` (`[0:5]` `enc=0.925` `8723.6/9428.7` ms, `dec=0.924` `8829.2/9553.2` ms, `geomean enc=0.929 dec=0.927`, `worst>=16384px enc=ic_spider_web(0.972) dec=ic_spider_web(0.971)`; `[5:10]` `enc=0.928` `8753.5/9435.6` ms, `dec=0.923` `8851.8/9588.8` ms, `geomean enc=0.931 dec=0.924`, `worst>=16384px enc=ic_spider_web(0.973) dec=ic_artificial(0.984)`), `photo -t3` (`[0:5]` `enc=1.010` `3119.9/3088.9` ms, `dec=0.975` `3135.6/3216.0` ms, `geomean enc=1.020 dec=0.982`, `worst>=16384px enc=ic_spider_web(1.109) dec=kodim14(1.173)`; `[5:10]` `enc=1.032` `3185.2/3086.2` ms, `dec=0.975` `3138.5/3219.4` ms, `geomean enc=1.036 dec=0.980`, `worst>=16384px enc=kodim07(1.153) dec=kodim03(1.093)`), `tp -t1` (`[0:5]` `enc=0.925` `1855.4/2005.2` ms, `dec=0.922` `1878.2/2037.6` ms, `geomean enc=0.928 dec=0.924`, `worst>=16384px enc=tp_spider_web_crop(0.959) dec=tp_spider_web_crop(0.977)`; `[5:10]` `enc=0.924` `1869.8/2022.7` ms, `dec=0.919` `1888.5/2055.8` ms, `geomean enc=0.927 dec=0.920`, `worst>=16384px enc=tp_spider_web_crop(0.954) dec=tp_spider_web_crop(0.964)`), `tp -t3` (`[0:5]` `enc=1.017` `673.6/662.2` ms, `dec=0.967` `668.3/690.9` ms, `geomean enc=1.019 dec=0.967`, `worst>=16384px enc=tp_spider_web_crop(1.050) dec=kodim23(0.995)`; `[5:10]` `enc=1.013` `671.4/662.8` ms, `dec=0.960` `667.5/695.6` ms, `geomean enc=1.019 dec=0.966`, `worst>=16384px enc=kodim03(1.051) dec=tp_flower_foveon_crop(0.991)`), `tp-only -t1` (`[0:5]` `enc=0.927` `1259.6/1359.5` ms, `dec=0.923` `1273.0/1378.7` ms, `geomean enc=0.932 dec=0.930`, `worst>=16384px enc=tp_flower_foveon_crop(0.960) dec=tp_spider_web_crop(0.967)`; `[5:10]` `enc=0.925` `1253.6/1354.7` ms, `dec=0.916` `1262.5/1379.0` ms, `geomean enc=0.931 dec=0.921`, `worst>=16384px enc=tp_spider_web_crop(0.958) dec=tp_spider_web_crop(0.958)`), `tp-only -t3` (`[0:5]` `enc=1.014` `430.1/424.0` ms, `dec=0.985` `428.7/435.2` ms, `geomean enc=1.025 dec=0.985`, `worst>=16384px enc=tp_flower_foveon_crop(1.146) dec=tp_deer_crop(0.999)`; `[5:10]` `enc=0.998` `426.5/427.5` ms, `dec=0.970` `425.2/438.3` ms, `geomean enc=1.004 dec=0.975`, `worst>=16384px enc=tp_flower_foveon_crop(1.066) dec=tp_spider_web_crop(1.017)`), `synth -t1` (`[0:5]` `enc=0.906` `235.1/259.5` ms, `dec=0.970` `179.8/185.3` ms, `geomean enc=0.911 dec=0.957`, `worst>=16384px enc=syn_pixel(1.500) dec=syn_solid(1.000)`, `small<16384px syn_odd enc -0.30ms dec -0.40ms`; `[5:10]` `enc=0.915` `237.7/259.7` ms, `dec=0.974` `180.2/185.1` ms, `geomean enc=0.881 dec=0.954`, `worst>=16384px enc=syn_pixel(1.000) dec=syn_ui(1.017)`, `small<16384px syn_odd enc -0.30ms dec -0.40ms`), and `synth -t3` (`[0:5]` `enc=0.937` `84.9/90.6` ms, `dec=0.986` `71.1/72.1` ms, `geomean enc=0.804 dec=0.954`, `worst>=16384px enc=syn_stripes(1.096) dec=syn_text(1.032)`, `small<16384px syn_odd enc -0.40ms dec -0.40ms`; `[5:10]` `enc=0.940` `84.5/89.9` ms, `dec=0.987` `70.9/71.8` ms, `geomean enc=0.798 dec=0.957`, `worst>=16384px enc=syn_gradient(1.093) dec=syn_gradient(1.046)`, `small<16384px syn_odd enc -0.50ms dec -0.40ms`), with every set total `<= 1.072 <= 1.10x` (`<= 0.987 <= 1.20x` on `synth`) and every image `>= 16,384` pixels `<= 1.500 <= 1.50x` (`<= 1.173x` on all photographic images, and `syn_odd` `-0.30..-0.50` ms `<= +1.0` ms), plus 9-thread (`-t9`) `photo` `dec=0.617 (2544.7/4121.3ms) geomean=0.649 enc=0.731 (2713.0/3710.4ms) geomean=0.751`; `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` (`1,410,485` bytes vs Milestone 5's `1,405,118`) records `alloc_peak=10291243` (`codec_mem=8880758`, `+4.01% <= +5.00%`, `codec_us=486679`, `total_us=549080`, `peak_ws=16257024`) at `-t 1`, `alloc_peak=10291275` (`codec_mem=8880790`, `+4.01%`, `codec_us=193078`, `total_us=256691`, `peak_ws=16347136`) at `-t 3`, `alloc_peak=10291275` (`codec_mem=8880790`, `+4.01%`, `codec_us=236911`, `total_us=301392`, `peak_ws=16343040`) at `-t 6`, and `alloc_peak=10291371` (`codec_mem=8880886 <= 8,965,376`, `+4.01% <= +5.00%` over Milestone 5's `8,538,454`, `codec_us=123192`, `total_us=185391`, `peak_ws=16379904`) at `-t 9` (`bin/linux-x64/bin/rasterfold decode --time` records `alloc_peak=10291243` / `codec_mem=8880758` at `-t 1`, `alloc_peak=10291483` / `codec_mem=8880998` at `-t 3` and `-t 6`, and `alloc_peak=10292203` / `codec_mem=8881718 <= 8,965,595`, `+4.02% <= +5.00%` over Milestone 5's `8,538,662`, `codec_us=123868`, `total_us=136342`, `peak_ws=13471744` at `-t 9`); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1410485 decode_wall_us=235156 decode_peak_ws=16379904 decode_codec_us=129750 pixels=True` vs `jxl bytes=1470703 decode_wall_us=435235 decode_peak_ws=52097024`); and `python3 tests/test_correctness.py` (`41 passed, 0 failed` on Linux and Windows), `python3 tests/test_api.py` (`Ran 9 tests ... OK`), `python3 tests/test_artifacts.py` (`Ran 15 tests ... OK`), `python3 tests/fuzz_decode.py 300 1` on Linux and Windows (`300 mutated files: 287 rejected, 13 still valid, no crash, hang or mismatch`), `python3 scripts/build-docs.py --stage build/site-source`, `python3 -m unittest discover -s tests -p test_docs.py -v`, and `python3 scripts/check-all.py --all-targets` exit 0 (`check-all: 29/29 passed`).

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
RasterFold uses up to nine threads by default (across channels and strips); `tests/benchmark.py` also reports
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
| ceiling of encoder-chosen parameters over LMS step {6, 10, 16} and WP maxima (13, 12, 12, 12) and (16, 10, 10, 10) (4.5, `D01 T02 §6`) | per-image best of the four candidates sums to `30,859,686`, only 0.018% below the baseline `photo` total `30,865,211` (winners: baseline 9, LMS 6: 15, LMS 16: 11, WP alt 4); the per-channel bound (smallest stream per channel) sums to `30,857,198`, 0.026% below; and the exact mixed-assignment oracle (all 64 per-channel assignments encoded, best per image) sums to `30,855,916`, 0.030% below; variant totals: LMS 6 `30,864,012`, LMS 16 `30,869,437`, WP alt `30,891,641` | not worth a header byte: both bounds miss the 0.3% bar by an order of magnitude, so `D01 T05 §2` will strike its items |
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
| 7 models: 5 plus k5/k6 neighbour contexts (4.2, `D01 T02 §1`) | integer codec, 7-model: `photo` -0.170% (`30,865,211` -> `30,812,732`, `0.9712` -> `0.9695` of e7, `0.9829` -> `0.9813` of e9; geomean `0.9728` -> `0.9698`; worst kodim17 `1.0037` -> `1.0000`); `tp` -0.153% (`6,651,583` -> `6,641,390`; geomean `0.9722` -> `0.9700`; worst tp_nightshot_iso_1600_crop `0.9931` -> `0.9928`); `synth` `206,566` -> `206,229` (geomean `0.5883` -> `0.5760`; worst syn_gradient `1.5665` -> `1.4647`). k5-only: `photo` `30,836,880` (`0.9820` of e9; geomean `0.9711`; worst kodim17 `1.0016`), `tp` `6,645,737` (`0.9710`; worst `0.9926`), `synth` `206,344` (`0.5810`; worst `1.4972`). k6-only: `photo` `30,826,654` (`0.9817` of e9; geomean `0.9708`; worst kodim17 `1.0012`), `tp` `6,643,903` (`0.9706`; worst `0.9931`), `synth` `206,317` (`0.5790`; worst `1.4924`). decode ~1.24x src (`ab -n5` B/A enc `1.239` / dec `1.217` -t1, enc `1.260` / dec `1.267` -t3); scratch-CLI three-thread decode vs JPEG XL on `measure/image.bmp`: min `0.578`/`0.571` (both 3-thread), `0.400` (JXL 1-thread), all below the 0.6 bar | accepted: `research/variants/models-6-7.patch`, ported by `D01 T03 §1` |
| accepted variants combined for version 3 (§1 to §6) (`D01 T02 §7`) | one part only (the 7-model change; §2 to §6 accepted nothing): `photo` -0.170% (`30,812,732`, `0.9695` of e7), `tp` -0.153% (`6,641,390`), `synth` -0.163% (`206,229`); decode ~1.25x baseline at -t1 and ~1.33x at -t3 (`ab -n5` B/A dec, repeated until two runs agreed); combined file byte-identical to §1's measured variant, so its JPEG XL decode evidence carries | accepted: `research/variants/version-3.patch`, ported by `D01 T03 §1` |
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
| match model as an eighth method 1 model (`D01 T04 §2`, 2026-10-30): per channel a causal int16 plane, a 2^16-entry hash of 8-sample runs ending in earlier rows (multiplicative hash, top 16 bits, a recorded default), a pointer followed while it predicts, context `(clamp(match - pred, -15, 15) + 15) * 16 + min(len, 15)` (240 with no match), `NMODEL 8`, `WSTRIDE 16`; baseline `src/cm.c` at the `D01 T03 §1` stamp `207073e` | `synth` `206,229` -> `205,363` (geomean `0.5760` -> `0.5696` of e7): `syn_gradient` `4,227` -> `3,326` (bar `< 616` e9), `syn_stripes` `368` -> `381` (bar `< 271`), `syn_wide` `281` -> `301`, `syn_odd` `153` -> `155`, `syn_ui` `1,410` and `syn_text` `1,447` unchanged (palette method 3, which a method 1 model cannot reach; bars `< 2,195` and `< 2,038` already held); `photo` -0.149% (`30,812,732` -> `30,766,711`; `0.9695` -> `0.9681` of e7, geomean `0.9698` -> `0.9666`; every image smaller, ic_artificial -7.58%, smallest gain kodim05 -0.055%); `tp` -0.121% (`6,641,390` -> `6,633,374`; geomean `0.9700` -> `0.9687`); 55/55 corpus images byte-identical at -t1 and -t3 and decoded 1 -> 3 and 3 -> 1; `ab -n5 measure kodim01` B/A enc `2.131` / dec `1.618` -t1, enc `1.683` / dec `1.537` -t3 (unoptimized prototype: scalar 9-slot dot product, a 6-byte-per-pixel plane plus 768 KiB of hash tables); full summary lines (`python3 research/ab.py size build/research/try.c <set> -v`): `synth` `total=205363 jxl=226544 ratio=0.9065 geomean=0.5696 worst=1.1742 (syn_odd) e9=223034 e9_ratio=0.9208 e9_geomean=1.0864 e9_worst=5.3994 (syn_gradient) webp=201248 webp_ratio=1.0204 webp_geomean=1.7405 webp_worst=18.4778 (syn_gradient)`; `photo` `total=30766711 jxl=31782058 ratio=0.9681 geomean=0.9666 worst=0.9989 (kodim17) e9=31400714 e9_ratio=0.9798 e9_geomean=0.9820 e9_worst=1.0131 (kodim16) webp=34138176 webp_ratio=0.9012 webp_geomean=0.8875 webp_worst=1.0093 (ic_deer) measure=1402408 (0.9536 e9=0.9735 webp=0.8827)`; `tp` `total=6633374 jxl=6826895 ratio=0.9717 geomean=0.9687 worst=0.9921 (tp_nightshot_iso_1600_crop) e9=6740249 e9_ratio=0.9841 e9_geomean=0.9826 e9_worst=1.0121 (tp_nightshot_iso_1600_crop) webp=7368948 webp_ratio=0.9002 webp_geomean=0.8779 webp_worst=1.0064 (tp_deer_crop)` | rejected: `syn_gradient` and `syn_stripes` miss the bar, so no `research/variants/match-model.patch` and `D01 T04 §3` strikes its items; the uniform photo gain is a separate lead, backlog `B-002` |
| exact-plane flag in method 1 (`D01 T04 §4`, 2026-10-30): per channel `gp` the modular gradient on raw samples, gate `x > 0 && y > 0` with `gp` exact at W and N, one unmixed flag decision before the residual, 64 direct counters (run, `ex(NE)`, `ex(NW)`) or 16 (run), rate `RATE[n]` or a fixed shift of 5, counters in unused nodes 30 and 31 of model 0, hits skip residual coding and residual learning; prototype `build/research/try.c`, scratch CLIs from `build/d01-t04-s4/mkcli.sh`; baseline `src/cm.c` at `207073e` | `python3 research/ab.py size build/research/try.c synth -v` (and `photo -v`) with each variant's `-D` flag: (64, RATE) `synth` `206,229` -> `202,396`, `syn_gradient` `4,227` -> `368` (bar `< 616` met), `syn_stripes` `368` -> `357` (bar `< 271` missed), `syn_wide` `281` -> `375`, `syn_odd` `153` -> `96`, `photo` `30,812,732` -> `30,958,665` (+0.474%, 25 of 39 images more than 0.1% worse: `ic_artificial` +17.90%, `ic_spider_web` +5.54%, `measure` +2.25%); (64, fixed) `syn_stripes` `451`, `syn_gradient` `442`, `photo` `30,964,709`; (16, RATE) `syn_stripes` `355`, `syn_gradient` `366`, `photo` `30,982,672`; (16, fixed) `syn_stripes` `440`, `syn_gradient` `432`, `photo` `30,988,986`; `syn_ui` `1,410` and `syn_text` `1,447` unchanged in all four (palette-coded); one-thread and three-thread files identical and round-tripping 55/55 for every variant; no variant eligible, so no timing, memory, or `tp` run | rejected. Measured: `inspect` on `syn_stripes` streams (bytes, baseline -> (64, RATE)) G 17 -> 54, R 214 -> 164, B 93 -> 95, so the flag made the free G plane dearer and barely moved B. Inferred, not separated by any measurement: a direct counter updated by `floor` settles at `c = 65474` under `RATE[60]` (`p = 4092`, `build/d01-t04-s4/counter.py`), and one such counter over the `syn_stripes` channel's roughly 228,400 gated hits (640 by 360) costs about 41 bytes ideal, matching G's 37-byte rise, so the flag is roughly a quarter of R's 164 bytes and nearly half of B's 95; the rest is column 0, which the gate excludes and `gp` would predict exactly, and coder overhead. Also inferred: on photos a hit freezes the residual counters and mixer, and an unmixed flag cannot weigh its own evidence against the models, so every gated miss pays twice. |
| gated modular prediction in method 1 (`D01 T04 §6`, 2026-10-30): per channel `gp` the modular gradient on raw samples (`W + N - NW` inside, `2N - NN` in column 0, `2W - WW` in row 0), a gate on `gp` exact at the causal neighbours (`g2`: W and N, or their column-0 and row-0 forms; `g4`: also NW and NE, or NN), `gp` replacing the prediction inside the gate with no new decision, the residual coder and every model learning as today, optionally the gate bit as a mixer weight-set offset (`wsel + 4`); prototype `build/research/try.c` from `build/d01-t04-s6/mktry.py`; baseline `src/cm.c` at `207073e` | `python3 research/ab.py size build/research/try.c synth -v` (and `photo -v`) with each variant's `-D` flags: (`g2`, on) `synth` `206,229` -> `201,610`, `syn_gradient` `4,227` -> `111` (bar `< 616` met), `syn_stripes` `368` -> `103` (bar `< 271` met), `syn_wide` `281` -> `124`, `syn_odd` `153` -> `72`, `photo` `30,812,732` -> `31,002,196` (+0.615%, 33 of 39 images more than 0.1% worse, `ic_spider_web` +4.51%); (`g4`, on) `syn_gradient` `118`, `syn_stripes` `106`, `photo` `30,812,123` (-0.002%, but 11 images more than 0.1% worse: `ic_artificial` +1.17%, `ic_spider_web` +0.90%, `measure` +0.60%, `ic_flower_foveon` +0.30%; `kodim20` -0.97%); (`g2`, off) `111`, `103`, `31,076,336` (+0.856%); (`g4`, off) `118`, `106`, `30,827,515` (+0.048%, 10 images worse, `ic_artificial` +3.59%); `syn_ui` `1,410` and `syn_text` `1,447` unchanged; `syn_stripes` streams (G, R, B bytes) `17, 214, 93` -> `17, 22, 20` (`g2`, on), `syn_gradient` `129, 1,624, 2,430` -> `22, 23, 22`; one-thread and three-thread files identical and round-tripping 55/55 per variant, and 6/6 boundary fixtures with gate counts matching an independent count; no variant eligible, so no timing, memory, or `tp` run | rejected on the per-image `photo` bar alone: every variant clears both `synth` bars by a wide margin, and (`g4`, on) leaves the `photo` total flat, but 11 images still lose more than 0.1%, up to 1.2%. The cause is a hypothesis, not measured: a gate that is exact at four neighbours is exact only about half the time it opens on `measure` (54%) and `ic_spider_web` (49%), and on gated misses `gp` errs more than MED in the planning probe (`D01 T04 §6`; MED stands in for the shipped prediction); that does not explain `ic_artificial`, the worst, which is 99.1% exact when gated. Measured: the weight-set offset saves 0.24% of `photo` with `g2` and 0.05% with `g4` and nothing on `synth` (the gate is open almost everywhere there, so the offset only relabels slots), and `g4` against `g2` saves 0.61% (offset on) and 0.80% (offset off). Carried forward: the gate must learn when `gp` is reliable, not only where it was exact. |
| learned gated modular prediction in method 1 (`D01 T04 §8`, 2026-10-30): §6's `gp`, column-0 and row-0 forms and gates, plus per channel two running absolute-error sums over gated samples, `Eg` for `gp` and `Et` for today's prediction (modular distance, each capped at 32, both halved when either reaches 4,096), optionally one pair per `qE` band; inside the gate `gp` is used only when `M * Eg < Et` (`M` 1 or 2), and the choice is the weight-set offset; prototype `build/research/try.c` from `build/d01-t04-s8/mktry.py`; baseline `src/cm.c` at `207073e` | `python3 research/ab.py size build/research/try.c synth -v` (and `photo -v`) with each variant's `-D` flags: v1 (`g4`, off, `M` 1) `syn_gradient` `119`, `syn_stripes` `108`, `photo` `30,786,997` (-0.084%, `ic_artificial` +1.694% and `measure` +0.113% over the 0.1% bar); v2 (`g4`, off, `M` 2) `119`, `108`, `30,795,260` (-0.057%, no image over the bar, worst `kodim03` +0.032%); v3 (`g4`, `qE`, `M` 2) `127`, `117`, `30,796,521` (-0.053%, `ic_artificial` +0.750%); v4 (`g2`, off, `M` 2) `112`, `105`, `30,807,923` (-0.016%, `ic_artificial` +0.743%); `syn_ui` `1,410` and `syn_text` `1,447` in all four; one-thread and three-thread files identical and round-tripping 55/55 per variant, 7/7 boundary fixtures with gate counts matching Python, and an independent selector replay with 0 disagreements; v2 timed (alternating, per-image minima pooled by `build/d01-t04-s8/abpool.py` on a loaded shared machine): `synth` `-t1` totals enc 1.094 and dec 1.136 over rounds 1 to 5, 1.080 and 1.127 over the disjoint rounds 6 to 20; `photo` `-t1` dec total 1.019 and 1.044 in the two disjoint pools (geomean 1.025 and 1.047), and in every pool some image of at least 16,384 pixels above 1.03 at both thread counts; decode `alloc_peak` `9,943,780` and `9,943,540` against `9,943,749` and `9,943,509`, 31 bytes over, the size difference of its `measure` file; no `tp` run | rejected: v2, the only variant that clears every size bar, misses the timing bar on the `synth` `-t1` set totals by 8% to 14% in two disjoint, agreeing pools, and the memory bar as written by 31 bytes, so there is no candidate, no `research/variants/learned-gradient.patch`, and `D01 T04 §9` strikes its items. Measured: the margin decides most choices (`margin2_decides=2,177,014` of the selector replay's v2 samples) and the error-sum comparison removes the per-image `photo` losses §6 had. Inferred, not separated by a profile: the cost is the per-sample sum update and compare, paid wherever the gate opens, which is almost every sample of the synthetic planes. Carried forward: the selection works for size; a port needs it at no measurable time cost. |
| colour-cache context in version 3, causal predicted-byte form (`D01 T06 §2`, 2026-11-09): per channel and strip a 32-sample recent-value cache (timestamp per byte value, refreshed per coded sample, strip-reset), hit = the context prediction byte `pred` was coded within the last 32 samples of the channel, hit replaces the low bit of the `k6` context (`k6 = (k6 & ~1) | hit`), cache updated in `cm_code` identically on encode and decode; prototype `build/research/try.c`, baseline `src/cm.c` at `b22dec2` | `python3 scripts/windows.py research/ab.py size <source> <set> -v -t1`: `synth` candidate total `201640` = baseline `201640`, per-image candidate/baseline geometric mean `1.0000` (all 11 encoder-selected sizes identical; `syn_ui` `1410`, `syn_text` `1447`, `syn_diagram` `1452` unchanged, palette method 3); `photo` `30634873` -> `30634088` (`0.99997`, -785 bytes); `tp` `6598954` -> `6598371` (`0.99991`) | rejected: the `synth` bar `GM <= 0.90` is missed by the full margin; the measured fact is only that no encoder-selected synth size moved. Why is a hypothesis (the generated set is palette-dominated at selection time, and cache hits may be rare on the CM-coded images); neither was measured directly. This rejects the causal predicted-byte variant only, not colour caching in general; the neighbour-candidate form is a separate experiment. Not ported: no `src/cm.c`, oracle, spec, or KAT change. The prototype was corrected and remeasured after review found unseen entries counted as hits at strip start (hit now requires a valid timestamp within the last 32 coded samples); the corrected figures above are the remeasured ones. |
| copy-run token for palette method 3 (`D01 T06 §3`, 2026-11-09): left-copy runs in `build/research/try_pal.c` (copy of `src/pal.c`), after a D1 hit in flat patterns (`pat & 31 == 31`) a run flag (per-pattern adaptive counter) plus a unary skipped-pixel count (per-position adaptive counters) for runs of at least 8, `count == 1` exempt, decoder fills the skipped indices | `python3 scripts/windows.py research/ab.py size build/research/try_pal.c synth -v -t1`: the six palette-coded `synth` images total `4560` -> `7220` bytes (+58.3%; `syn_checker` `138` -> `506`, `syn_ui` `1410` -> `2175`, `syn_text` `1447` -> `2204`, `syn_diagram` `1452` -> `2222`, `syn_solid` `61` and `syn_pixel` `52` unchanged), against the acceptance total of at most `3648`; an earlier variant offering the flag after every D1 hit measured the same direction (`7226`) | rejected: this token's flag plus length code measured far more than the coding it replaced. Inference, not measured directly: `syn_checker`'s whole index stream is 94 bytes for 262,144 pixels (section 4.13), which suggests the per-hit decisions the token skips are already near-zero cost under the adaptive D1 counters, leaving nothing for an explicit length code to save. This rejects only the measured variant (left-copy, flat-pattern gate, threshold 8, unary length); above-copy tokens and other length codes are untested. Not ported: no `src/pal.c`, oracle, spec, or KAT change. |
| palette coding for method 3 (`D01 T06 §4`, 2026-11-09): entry 0 stored raw, then palette triples coded as per-channel delta bytes from the previous entry, each byte 8 bits MSB-first under per-channel adaptive binary trees with the palette coder's update rule, in its own range-coded stream replacing the raw palette field; prototype `build/research/try_pal.c` (copy of `src/pal.c`) | `python3 scripts/windows.py research/ab.py size build/research/try_pal.c synth -v -t1`: the six palette-coded `synth` images total `4560` -> `4858` bytes (+6.5%; `syn_text` `1447` -> `1544`, `syn_diagram` `1452` -> `1541`, `syn_ui` `1410` -> `1497`, `syn_checker` `138` -> `146`, `syn_pixel` `52` -> `60`, `syn_solid` `61` -> `70`), against the acceptance total of at most `4104`; `photo` per-image sizes identical on all 39 images (total `30634873`) | rejected: coding the palette costs more than the raw triples it replaces. Measured fact: every palette image grew, including 2-colour `syn_checker`, so the overhead is in the coded representation itself, not in any one image class. Hypothesis, not measured directly: first-seen palette order makes the delta bytes near-uniform, so they cost about 8 bits each plus the length field and stream flush. This rejects delta-byte tree coding of the palette in first-seen order; sorted-palette remapping and per-channel plane coding remain untested. Not ported: no `src/pal.c`, oracle, spec, or KAT change. |
| above-copy run token for palette method 3 (`D01 T06 §5`, 2026-11-09): after a D1 hit on rows below the first, a run flag on the D1 run counter `pat * 2 + 1`, offset in -2..2 by successive elimination under four adaptive counters (terminal offset by elimination), unary `runlen - 8` (runlen includes the D1 pixel) under per-position counters capped at 31, skipped pixels fill the neighbour rows and update no D1 counters; prototype `build/research/try_pal.c` (copy of `src/pal.c`, index plane precomputed) | `python3 scripts/windows.py research/ab.py size build/research/try_pal.c synth -v -t1`: the six palette-coded `synth` images total `4560` -> `9973` bytes (+118.7%; `syn_checker` `138` -> `3259`, `syn_ui` `1410` -> `2175`, `syn_text` `1447` -> `2204`, `syn_diagram` `1452` -> `2222`, `syn_solid` `61` and `syn_pixel` `52` unchanged), against the acceptance total of at most `4104` and `syn_checker` at most `114`; `photo` per-image sizes identical on all 39 images (total `30634873`) | rejected: the token's flag, offset, and length cost far more than the per-pixel decisions they skip, consistent with section 3's left-copy result. Hypothesis, not measured directly (whole-file sizes carry no token attribution): on `syn_checker` the above-copy runs are long, so the unary length code itself becomes a major cost. This rejects the measured variant (D1-hit routing, shared run counter, threshold 8, unary length, successive-elimination offset); a non-unary length code or a different offer point remains untested. Not ported: no `src/pal.c`, oracle, spec, or KAT change. |
| neighbour-candidate colour cache in version 3 (`D01 T06 §6`, 2026-11-09): at each pixel with `y >= 1`, `x >= 1`, `x + 1 < w` in the strip, a hit flag on the G stream (dedicated adaptive counter) and, on a hit, a 2-bit index (two adaptive counters) naming the first matching candidate of N, NE, NW, and the clamped above-row gradient `2N - NE`; hit samples run full model updates with the candidate colour and emit no residual bits (ghost pass); counters reset per strip; prototype `build/research/try.c` (copy of `src/cm.c`), one-thread pipelined driver | `python3 scripts/windows.py research/ab.py size build/research/try.c <set> -v -t1`: `synth` candidate/baseline geometric mean `1.0298` (total `201640` -> `201682`; `syn_stripes` `113` -> `123`, `syn_gradient` `119` -> `133`, `syn_wide` `133` -> `151`, eight sizes identical), `photo` `30634873` -> `30972555` (`1.0110`, bar at most `1.0005`), `tp` `6598954` -> `6681672` | rejected: the hit flag and index cost more than the ghosted residuals save, on generated images and on photographs alike. Hypothesis, not measured directly (whole-file sizes carry no hit/index attribution): the version 3 models already price exact neighbour repeats near zero, leaving an explicit colour-hit token nothing to buy. This rejects the above-row candidate set (N, NE, NW, clamped `2N - NE`), the G-stream flag, and the ghost scheme as measured; W-inclusive candidate sets remain untested but would first have to overcome the same flag cost. Not ported: no `src/cm.c`, oracle, spec, or KAT change. |
| recent-colour cache for palette method 3 (`D01 T06 §7`, 2026-11-11): a 32-slot direct-mapped table of palette indices (slot `((r * 31 + g) * 31 + b) mod 32`, empty at image start, set after every pixel); where D1, D2 and D3 have all missed, a hit flag under a per-pattern adaptive counter (128 counters) and on a hit the slot number as five decisions through a 31-node adaptive tree, in place of the 8-decision index tree; images with more than two colours only; prototype `build/research/try_pal.c` (copy of `src/pal.c`), baseline `src/cm.c` at `de6833a` | `python3 scripts/windows.py research/ab.py size build/research/try_pal.c <set> -v -t1`: the six palette-coded `synth` images total `4560` -> `4652` bytes (+2.0%; `syn_ui` `1410` -> `1438`, `syn_text` `1447` -> `1479`, `syn_diagram` `1452` -> `1484`, `syn_solid` `61`, `syn_checker` `138` and `syn_pixel` `52` unchanged), `synth` total `201640` -> `201732` (`webp_ratio` `1.0024`, `webp_geomean` `1.0065`), against the bar of a total below `201248`, a geometric mean below `1.0000`, `syn_checker` at most `114`, `syn_text` at most `1317` and `syn_diagram` at most `1350`; `photo` per-image sizes identical on all 39 images (`30634873`) and `tp` on all 10 (`6598954`) | rejected: the cache-token variant increases size on every image it reaches. Hypotheses, not measured directly (whole-file sizes carry no token attribution): the hit flag may cost more on the pixels that reach the index tree than a hit saves, five slot decisions need not cost fewer bits than eight index decisions under trained counters, hits also remove samples that would have trained the index tree, and 240 to 251 colours hashed into 32 slots may overwrite each other before reuse. `syn_checker` cannot move because two-colour images never reach the token. This rejects the measured variant (32 slots, this hash, update on every pixel, token after D3, per-pattern flag counter); a larger table, a move-to-front list, or a hit context other than the neighbour pattern remains untested. Not ported: no `src/pal.c`, oracle, spec, or KAT change. |

### 3.3 Mixing and counters

| experiment | result | decision |
|---|---:|---|
| APM/SSE stage after the mixer (context qE, or qE and the G residual) | -0.005% photo | rejected |
| int32 mixer, scale 2^16, floor update instead of rounded | +0.06% sub | rounding kept |
| int32 mixer, scale 2^14, clamped weights, floor update | +0.32% to +0.92% sub | rejected |
| int16 mixer, floor update | +1.30% sub | rejected |
| int16 mixer, scale 2^12, rounded saturating update | +0.09% sub against the int32 mixer (five-model era) | speed unmeasured (5.4, `D02 T02 §2`) |
| int16 SIMD mixer measured (`D02 T02 §2`, 2026-11-09): seven-model int16 weights at scale 2^12 (init 1229, dot `>> 12`, update `(err * stretch + 32768) >> 16` with saturating add) in `build/research/try.c` against current `src/cm.c` | `photo` 30,634,873 -> 30,634,064 (-0.003%; JPEG XL-referenced geomean 0.9622 -> 0.9614, worst 0.9934 -> 0.9919, both kodim17), `tp` 6,598,954 -> 6,598,160 (-0.012%; geomean 0.9625 -> 0.9621, worst 0.9897 -> 0.9893, both tp_nightshot_iso_1600_crop), `synth` 201,640 -> 201,640 (identical; geomean 0.3275, worst 0.9241 syn_noise); all sets round-trip in the harness | decode B/A 1.006 and 1.029 in two alternating `ab -n5 -t1` passes on `measure kodim01` at host load 1.15 to 5.63, and 0.944 and 1.010 in two further passes at load 1.77 to 3.49 (bar `<= 0.950`); 0.953 and 0.981 at `-t3`; encode 0.924 to 0.963 at `-t1` and 0.884 and 1.049 at `-t3` (the 1.049 is a three-thread encode regression, retained here as the protocol requires). Rejected: no consistent one-thread decode gain (four loaded-host passes span 0.944 to 1.029, centred on neutral), and the idle-host repetition on 2026-11-10 (variant reconstructed in `build/research/try-int16.c`, aggregate `photo` size matching the recorded 30,634,064; host load 0.29 to 0.54) gave one-thread decode B/A minima 1.015 and 1.028 and 1.045 at `-t3`, so the 5% bar is not met on idle evidence either; the aggregate timings cannot attribute the result to any single component, and no causal claim is made. No patch saved and no port filed |
| mixer learning rate 2x / 0.5x | -0.02% / +0.08% photo | unchanged |
| initial mixer weight 0.2 / 0.4 instead of 0.3 | +0.03% / +0.08% photo | unchanged |
| counters 12-bit probability plus 4-bit count per node | +0.07% sub | superseded |
| counters 16-bit, rate from one visit count per context row | +0.08% sub, decode 20% and encode 15% faster | kept |
| counters with a fixed shift of 4 or 5 | +0.37% / +0.13% sub | rejected |
| 13-bit probability plus 3-bit count, shift schedule | +0.10% sub, synthetic geometric mean +10% | rejected |
| visit-count limit 30 / 120 instead of 60 | +0.08% photo / +0.05% sub | 60 kept |
| two adaptation speeds per context: fast counter (rate 1/16) in node+32 beside the slow counter, mixer learns the split (4.3, `D01 T02 §2`) | model 0 fast: `photo` +0.025% (`30,865,211` -> `30,872,973`), `tp` +0.039%, `synth` -0.033%; models 0+1 fast: `photo` +0.008% (`30,867,640`), `tp` +0.021%, `synth` +0.010%; all five fast (WSTRIDE 16): `photo` +0.186% (`30,922,501`), `tp` +0.234%, `synth` -0.024%; decode slower everywhere (`ab -n5` B/A dec: f0 `1.054` -t1 / `1.078` -t3; f01 `1.103` / `1.352`; fall `1.214` / `1.340`) | rejected: no stage gains on photo, so no winner, no patch, no `vsjxl.py` run |
| second mixer weight set chosen by `k2 % 128`, outputs averaged before `SQUASH` (4.4, `D01 T02 §3`) | `photo` +0.030% (`30,865,211` -> `30,874,474`; geomean `0.9728` -> `0.9731`; worst kodim17 `1.0037` -> `1.0044`), `tp` +0.036% (`6,653,991`), `synth` -0.052% (`206,459`); decode slower (`ab -n5` B/A dec `1.130`/`1.098` -t1 and `1.099`/`1.150` -t3 across two agreeing runs) against an expected -0.2% to -0.6% | rejected: size rejection (no patch, no `vsjxl.py` run) |
| small-image learning-cost starts (4.6, `D01 T02 §4`) | energy-shaped model-1 zero-flag counter start: `kodak` -0.001% (`9,989,104` -> `9,988,989`), `photo` -0.000%, `tp` +0.000%, `synth` -0.001%; worst `photo` image +0.0016%; kodim17 -0.0021%, kodim09 -0.0010%. Mixer starts 0.45/0.25: `kodak` +0.057%, `photo` +0.042%, `tp` +0.049%, `synth` +0.012%; worst image ic_artificial +0.2934%. Decode timing across repeated runs agrees at ~1.00 -t1 / ~0.94 -t3 for s1 and ~1.01 / ~0.9 for s2: no measurable decode cost (init-only changes) | rejected: neither reaches the 0.1% `kodak` bar, so no winner and no patch |
| exact-repeat context `eq * 16 + qE` (4.7, `D01 T02 §5`) | as sixth model: `synth` geomean +0.48% (`0.5883` -> `0.5911`), `photo` -0.038% (`30,853,565`), `tp` -0.043%; replacing k0/k1/k2/k3/k4 in turn: `synth` geomean +2.86% / +0.36% / -0.05% / +6.9% / +4.8%, `photo` +0.28% / +0.17% / +0.098% / +0.42% / +0.22%, every replacement worse on `tp` | rejected: bar needs `synth` geomean at least 2% lower; best variant is forty times short, so no winner and no patch |

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
| whole-codec `-march=x86-64-v3` build (`D02 T01 §3`, 2026-11-09): pinned clang, release sources and flags, only `-march=x86-64-v2` changed to `x86-64-v3` (Linux `build/v3/rasterfold-v3` sha256 `ba247b028226...` against release `bin/linux-x64/bin/rasterfold` sha256 `c99784fc35bd...`, both rebuilt from current source; identical `.fold` bytes on `measure/image.bmp`) | encode minimum ratios 1.014 and 0.944, medians 1.144 and 0.947 (two `-n5 --threads 1` passes on an observed idle host, load 0.71 to 1.87 on 10 cores), 0.899 minimum and 0.872 median at 3 threads | decode minimum ratios 1.041 and 1.014 at 1 thread (bar `<= 0.950`; medians 0.994 and 1.017), 1.053 minimum and 1.019 median at 3 threads -- neither one-thread minimum cleared the bar on the idle host, confirming the loaded-host preliminary result (0.975 and 1.013 at load 5.2 to 6.7). No runtime dispatch is built. Release stays x86-64-v2 |
| profile-guided optimization (`-fprofile-instr-use`, 5.1, `D02 T01 §1`) | 0.962 to 0.988 (`measure`), 0.998 (`photo`), 0.978 (`tp`), 1.012 (`synth`) | 0.991 to 1.079 (`measure`), 1.009 (`photo`), 1.004 (`tp`), 0.995 (`synth`) -- below 5% bar; default release left unprofiled |
| link-time optimization (`-flto`, 5.2, `D02 T01 §2`) | 0.968 to 0.986 (`measure`), 0.996 (`photo`), 0.991 (`tp`), 0.939 (`synth`) | 0.905 to 0.924 (`measure`), 1.008 (`photo`), 1.019 (`tp`), 0.937 (`synth`) -- fails corpus `<= 0.970` bar on `photo` and held-out `tp` (and 3-thread `tp` is `1.086` dec / `1.267` enc); default release left non-LTO |
| hot-loop vectorization (`_mm_madd_epi16` mixer update, per-sample `rdec_tail`, `row_ctx_t` interior specialization, `qcount3` + 128-bit `perr` stores, 5.7, `D02 T01 §4`) | 0.847 (`measure` `ab.py`), 0.890 to 0.955 (`measure` `vsjxl.py`), 0.884 (`photo`), 0.895 (`tp`), 0.908 (`tp-only`), 0.817 (`synth`) | 0.927 (`measure` `ab.py`), 0.926 to 0.940 (`measure` `vsjxl.py`), 0.935 (`photo`), 0.928 (`tp`), 0.939 (`tp-only`), 0.910 (`synth`) -- kept (3-thread decode 0.995 `measure` `ab.py`, 0.789 to 0.890 `vsjxl.py`, 0.915 `photo`, 0.914 `tp`, 0.924 `tp-only`, 0.833 `synth`) |
| independent horizontal strips (`S` strips of rows `floor(s*H/S)` to `floor((s+1)*H/S) - 1`, `research/strips.py`, 5.3, `D02 T02 §1`) | size cost on current `src/cm.c` (`m5`, `photo=30,795,260`, `tp=6,640,275`, `synth=201,634`): `S=4` `photo` `adj_sum=31,095,902` (`+0.976%`, geomean `1.012037`, worst `kodim07` `+1.912%`), `raw_sum=31,099,607` (`+0.988%`, geomean `1.012229`, worst `kodim07` `+1.938%`), `H<=768` (`n=24`) `adj_sum=10,096,033` (`+1.462%`, `raw_sum` `+1.485%`), `tp` `adj_sum=6,715,331` (`+1.130%`, geomean `1.012874`, worst `kodim05` `+1.673%`), `raw_sum=6,716,281` (`+1.145%`, geomean `1.013052`, worst `kodim05` `+1.693%`), `synth` `adj_sum=203,398` (`+0.875%`, `raw_sum=203,778` `+1.063%`, `non_cm=7`; pure-CM all 11 `OK`: `whole=209,131` `adj_sum=214,175` `+2.412%`, `raw_sum=215,220` `+2.912%`; pure-CM 10 non-noise: `whole=9,917` `adj_sum=13,020` `+31.290%`, `raw_sum=13,970` `+40.869%`); `S=2` `photo` `adj_sum=30,914,915` (`+0.389%`, `raw_sum=30,916,124` `+0.392%`), `tp` `adj_sum=6,668,567` (`+0.426%`, `raw_sum=6,668,877` `+0.431%`), `synth` `adj_sum=202,231` (`+0.296%`, `raw_sum=202,355` `+0.358%`, `non_cm=7`; pure-CM all 11 `OK`: `adj_sum=211,111` `+0.947%`, `raw_sum=211,452` `+1.110%`; pure-CM 10 non-noise: `adj_sum=11,121` `+12.141%`, `raw_sum=11,431` `+15.267%`); pre-gated-gradient 7-model `cm_m4.c` (`fb27c31` / `0c1ec08`): `S=4` `photo` `+0.980%` `adj_sum` / `+0.992%` `raw_sum`, `tp` `+1.145%` / `+1.160%`, `S=2` `photo` `+0.390%` / `+0.394%`, `tp` `+0.431%` / `+0.436%`; 5-model `cm_m1.c` (`b181fb1`): `S=4` `photo` `+0.885%` / `+0.897%`, `tp` `+1.025%` / `+1.039%`, `S=2` `photo` `+0.357%` / `+0.361%`, `tp` `+0.391%` / `+0.396%` | rejected: `S=4` exceeds the `<= +0.300%` bar by 3.25x to 3.82x on the 7-model baselines (`m5` and `m4`) and by 2.95x to 3.42x on the 5-model baseline (`m1`) (and even `S=2` exceeds `+0.300%`), so `D01 T03 §2` takes its rejection path |
| small-image model-state setup (`D02 T03 §1`): (a) biased counter storage (`p ^ 0x8000u` in `code_bit7`/`code_bit5`, zero-filled `ch->ctr`, `try-a.c`) and (b) first-touch row bitmap (`uint64_t row_init[NMODEL][NROWS / 64]` zeroed in `cm_alloc`, 64-byte SIMD row init on first touch in `cm_model`, `try-b.c`) | `syn_odd` (65x63) min `cyc/sample` (180 reps, 6 alternating `-r30` calls): baseline A-vs-A `-t1` `388` vs `405` (`1.0438`), `-t3` `298` vs `300` (`1.0067`); (a) `-t1` `0.8575` (`343/400`), `-t3` `0.6667` (`202/303`); (b) `-t1` `0.8520` (`334/392`), `-t3` `0.6755` (`204/302`). Set totals (`ab -v -n5`, `B/A` / `min_tot`, with 3-run confirmation repeats on `synth`): (a) `photo` `-t1` `1.061` / `1.058`, `-t3` `1.074` / `1.069`, `tp` `-t1` `1.045` / `1.055`, `-t3` `1.042` / `1.084`, `synth` `-t1` `[1.023, 1.015, 1.011]` / `[1.019, 1.011, 1.016]`, `-t3` `[1.033, 0.903, 0.988]` / `[0.994, 0.933, 0.984]`; (b) `photo` `-t1` `1.049` / `1.041`, `-t3` `1.032` / `1.060`, `tp` `-t1` `1.023` / `1.041`, `-t3` `1.075` / `1.058`, `synth` `-t1` `[1.027, 1.014, 1.022]` / `[1.024, 1.022, 1.022]`, `-t3` `[1.008, 0.978, 0.971]` / `[1.046, 1.028, 1.030]` (all sizes unchanged: `30,795,260` / `6,640,275` / `201,634`) | `syn_odd` min `cyc/sample`: baseline A-vs-A `-t1` `403` vs `400` (`0.9926`), `-t3` `314` vs `312` (`0.9936`); (a) `-t1` `0.8178` (`350/428`), `-t3` `0.6699` (`207/309`); (b) `-t1` `0.8478` (`351/414`), `-t3` `0.6944` (`209/301`). Set totals (`ab -v -n5`, `B/A` / `min_tot`, with 3-run confirmation repeats on `synth`): (a) `photo` `-t1` `1.048` / `1.043`, `-t3` `1.017` / `1.029`, `tp` `-t1` `1.033` / `1.034`, `-t3` `1.070` / `1.030`, `synth` `-t1` `[0.988, 0.968, 0.987]` / `[0.992, 0.977, 0.988]`, `-t3` `[1.005, 0.891, 0.973]` / `[0.987, 0.992, 0.975]`; (b) `photo` `-t1` `1.070` / `1.062`, `-t3` `1.055` / `1.047`, `tp` `-t1` `1.030` / `1.053`, `-t3` `1.065` / `1.076`, `synth` `-t1` `[1.059, 1.032, 1.057]` / `[1.058, 1.040, 1.057]`, `-t3` `[1.053, 1.036, 0.957]` / `[1.058, 1.052, 1.012]` -- rejected: both clear `syn_odd <= 0.900`, but per-bit XOR in (a) and per-sample bitmap checks in (b) slow down `photo` and `tp` by 1.7% to 8.4% (exceeding `<= 1.005`) to save ~0.3 ms of one-time `cm_alloc` setup |
| warm-started horizontal strips, 16-node context rows, exact reachable model row counts, single-block strip allocation, and branchless 16-bit SIMD counter updates (5.8, `D02 T02 §3`, `D01 T03 §3`) | sizes (`ab.py size`): `photo` `30,868,254` (`+72,994` = `+0.237% <= +0.300%`, geomean `1.002863`, `0 of 39 > +0.5%`, worst `kodim24` `+0.435%`), `tp` `6,656,840` (`+16,565` = `+0.249% <= +0.300%`, geomean `1.002895`, `0 of 10 > +0.5%`, worst `tp_flower_foveon_crop` `+0.425%`), `tp-only` `4,476,915` (`+9,683` = `+0.217%`), `synth` `201,638` (`+4` = `+0.002%`, `syn_gradient=120 < 616`, `syn_stripes=109 < 271`). Two-pool `-t1`/`-t3` encode totals (`abpool.py`): `measure` `-t1` `0.917 / 0.917`, `-t3` `0.968 / 1.032`, `synth` `-t1` `0.899 / 0.904`, `-t3` `0.956 / 0.930`, `tp-only` `-t1` `0.912 / 0.919`, `-t3` `0.999 / 1.025`, `tp` `-t1` `0.919 / 0.903`, `-t3` `1.043 / 1.044`, `photo` `-t1` `0.914 / 0.913`, `-t3` `1.053 / 1.078` | `testdata/photos/` (`n=10`, alternating `repeats=3`): `windows-x64` `--threads 9` `1,270,062 us` (`1.27 s`) vs WebP `1,431,200 us` (`1.43 s`, `ratio=0.887`, `geomean=0.937`), default threads `1,284,773 us` (`1.28 s`) vs WebP `1,414,107 us` (`1.41 s`, `ratio=0.909`, `geomean=0.950`); `linux-x64` `--threads 9` `629,757 us` (`0.63 s`) vs WebP `1,588,709 us` (`1.59 s`, `ratio=0.396`, `geomean=0.399`), default threads `626,151 us` (`0.63 s`) vs WebP `1,600,523 us` (`1.60 s`, `ratio=0.391`, `geomean=0.394`). `measure/image.bmp` (`S=3`, `1,410,485` bytes) `-t 9` `alloc_peak=10,291,371` (`codec_mem=8,880,886 <= 8,965,376`, `+4.01% <= +5.00%`, `codec_us=130,352`, `total_us=182,961`, `peak_ws=13,873,152` on Windows; `alloc_peak=10,292,203`, `codec_mem=8,881,718`, `codec_us=124,352`, `total_us=131,942` on Linux). Two-pool `-t1`/`-t3` decode totals: `measure` `-t1` `0.931 / 0.908`, `-t3` `0.983 / 0.942`, `synth` `-t1` `0.943 / 0.945`, `-t3` `0.958 / 0.967`, `tp-only` `-t1` `0.910 / 0.910`, `-t3` `0.980 / 1.025`, `tp` `-t1` `0.907 / 0.899`, `-t3` `0.959 / 0.990`, `photo` `-t1` `0.913 / 0.906`, `-t3` `0.994 / 0.954` (`over bar: 0 []`) -- accepted (`research/variants/warm-strips.patch`) |

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

### 4.5 Encoder-chosen parameters and why the losing photos lose

**Owner:** `D01 T02 §6` (the ceiling measurement), `D01 T05 §1` (the per-channel and per-decision-kind breakdown of the losing photographs), and `D01 T05 §2` (the encoder search, filed only if the ceiling clears 0.3%).

**Idea.** Adapt to each image without changing decoder cost: the encoder tries a
few parameter sets per channel and signals the best in a header byte (JPEG XL's
encoder adapts per image at effort 7 by learning a context tree).

**Candidates.** The LMS step (6, 10, or 16); the WP sub-predictor weight maxima
(`{13, 12, 12, 12}` today, or `{16, 10, 10, 10}`, trusting `W + NE - N` more);
the colour transform for channel 2 (`B - G` or `B - (R + G) / 2`). Encode time
grows by the number of candidates; encode takes 0.15 of JPEG XL's time, so two
or three candidates fit.

**Ceiling (`D01 T02 §6`, 2026-10-27): rejected.** Over `photo` (`n = 39`), the per-image best of the four candidates (`src/cm.c` baseline with LMS step `10` and WP maxima `{13, 12, 12, 12}`, `try-c6.c` LMS step `6` at `30,864,012`, `try-c16.c` LMS step `16` at `30,869,437`, and `try-cw.c` WP maxima `{16, 10, 10, 10}` at `30,891,641` vs baseline `30,865,211`, computed by `research/ceiling.py`) sums to `30,859,686` bytes (`-5,525` bytes = `-0.018%`, winners: baseline `9`, LMS 6: `15`, LMS 16: `11`, WP alt `4`), the per-channel bound (smallest stream per channel) sums to `30,857,198` bytes (`-8,013` bytes = `-0.026%`), and the exact 64-assignment mixed per-channel oracle sums to `30,855,916` bytes (`-9,295` bytes = `-0.030%`). All three bounds miss the `0.3%` (`0.300%`) bar by an order of magnitude, so `D01 T05 §2` takes its rejection path.

**Losing photographs at Milestone 6 (`D01 T05 §1`, 2026-11-06).** At the Milestone 6 codec (`b3eb341`, format version 3 with 7 context models, learned gated modular prediction, and warm-started horizontal strips), `python3 research/ab.py size src/cm.c photo -v` measures `total=30868254` (`0.9830` of JPEG XL e9 `31,400,714`, geomean `0.9881`, `27 of 39` photographs below e9, `12` above e9, `7` above `1.0100` of e9, worst `kodim16` `1.0186`), and `python3 research/ab.py size src/cm.c tp -v` measures `total=6656840` (`0.9876` of JPEG XL e9 `6,740,249`, geomean `0.9866`, `7 of 10` test-pack photographs below e9, `3` above e9, `2` above `1.0100` of e9, worst `tp_nightshot_iso_1600_crop` `1.0146`). Across both sets (`kodim01` belongs to both `photo` and `tp`), 14 distinct photographs are above JPEG XL e9:

| Set | Image | Dimensions (`S`) | RasterFold (`.fold`) | JPEG XL e9 | Diff vs e9 | Ratio vs e9 | vs `1.0100` cap |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: | ---: |
| `photo` | `kodim16` | `768x512` (`S=2`) | `371,397` | `364,606` | `+6,791` | `1.0186` | `+3,145` (`> 1.01`) |
| `photo` | `kodim17` | `512x768` (`S=3`) | `386,263` | `379,664` | `+6,599` | `1.0174` | `+2,802` (`> 1.01`) |
| `photo` | `ic_nightshot_iso_1600` | `1200x1200` (`S=3`) | `1,861,309` | `1,834,973` | `+26,336` | `1.0144` | `+7,986` (`> 1.01`) |
| `tp` | `tp_nightshot_iso_1600_crop` | `960x960` (`S=3`) | `1,205,267` | `1,187,904` | `+17,363` | `1.0146` | `+5,484` (`> 1.01`) |
| `photo` | `kodim12` | `768x512` (`S=2`) | `356,496` | `351,572` | `+4,924` | `1.0140` | `+1,408` (`> 1.01`) |
| `photo` | `ic_flower_foveon` | `1512x1008` (`S=3`) | `773,339` | `763,241` | `+10,098` | `1.0132` | `+2,466` (`> 1.01`) |
| `tp` | `tp_flower_foveon_crop` | `1200x800` (`S=3`) | `518,444` | `512,356` | `+6,088` | `1.0119` | `+964` (`> 1.01`) |
| `photo` | `kodim09` | `512x768` (`S=3`) | `381,080` | `376,729` | `+4,351` | `1.0115` | `+584` (`> 1.01`) |
| `photo` | `kodim10` | `512x768` (`S=3`) | `389,124` | `384,869` | `+4,255` | `1.0111` | `+406` (`> 1.01`) |
| `photo`, `tp` | `kodim01` | `768x512` (`S=2`) | `456,861` | `453,425` | `+3,436` | `1.0076` | `-1,098` (`<= 1.01`) |
| `photo` | `kodim06` | `768x512` (`S=2`) | `420,817` | `418,487` | `+2,330` | `1.0056` | `-1,855` (`<= 1.01`) |
| `photo` | `kodim21` | `768x512` (`S=2`) | `436,841` | `434,536` | `+2,305` | `1.0053` | `-2,040` (`<= 1.01`) |
| `photo` | `kodim08` | `768x512` (`S=2`) | `502,137` | `499,839` | `+2,298` | `1.0046` | `-2,700` (`<= 1.01`) |
| `photo` | `kodim24` | `768x512` (`S=2`) | `435,918` | `434,637` | `+1,281` | `1.0029` | `-3,065` (`<= 1.01`) |

**Decision-cost attribution rule (`research/bench.c --breakdown`).** Because range-coder output bytes are delayed through `cache` and `pending` and flushed outside `renc_bit` (`src/rc.h`), counting emitted output bytes per `renc_bit` call would misattribute earlier decisions to whichever later decision triggers `renc_shift`. Instead, `--breakdown` compiles an instrumented second pass (`bd_cm_encode_inner`) from `RF_BENCH_CM_PATH` (`../src/cm.c` by default) that wraps `renc_init` to bind each strip's `renc` state to its channel `c in {0, 1, 2}` (`G`, `R-G`, `B-G`) and wraps `renc_bit(rc, bit, p1)` to accumulate the exact sub-interval code length `bits = -log2((double)nr / (double)rc->range)` (`bytes = bits / 8.0`), where `bound = (rc->range >> 12) * p1` and `nr = bit ? bound : (rc->range - bound)` (`src/rc.h:35`), into four decision categories: `zero` (`node == 1`), `sign` (`node == 2`), `exp` (`3 <= node <= 8`), and `mant` (`9 <= node <= 15` adaptive mantissa bits plus `code_raw` raw bits at `p1 = 2048`). After verifying that `bd_cm_encode_inner` writes a payload byte-identical to `rf_encode`'s `.fold` payload, `run_breakdown` sums each channel's exact flushed stream bytes across all `S` strips from the `.fold` stream directory (`stream_G + stream_R-G + stream_B-G == payload_sz - payload_hdr`), reports `flush = stream - dec` (`~ +4.4` bytes per range-coder stream from the 5-byte `renc_flush` trailer and integer interval truncation), and reports `hdr = 32 + 1 + 12*S` (`45`, `57`, or `69` bytes for `S = 1, 2, 3`) so `stream_ALL + hdr == fold_sz` holds identically on every CM image.

**Per-channel and per-decision-kind breakdown across all 14 losers and 6 winning reference photos.** Running `bench_m6_bd.exe -r1 -t1 --breakdown` across all 14 losing photographs and 6 winning reference photographs (`measure` `0.9791` of e9, `kodim03` `0.9854`, `kodim20` `0.9613`, `kodim23` `0.9776`, `ic_cathedral` `0.9690`, `ic_artificial` `0.6285`) gives:

| Image (status vs e9) | `G` stream (`%`) | `R-G` stream (`%`) | `B-G` stream (`%`) | `ALL` stream | `zero` (`%` of `dec`) | `sign` (`%`) | `exp` (`%`) | `mant` (`%`) | `flush` / `hdr` / `fold` |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `kodim16` (`1.0186` lose) | `190,101` (`51.2%`) | `85,907` (`23.1%`) | `95,332` (`25.7%`) | `371,340` | `103,943.6` (`28.0%`) | `83,537.1` (`22.5%`) | `121,636.1` (`32.8%`) | `62,196.4` (`16.8%`) | `+26.7` / `57` / `371,397` |
| `kodim17` (`1.0174` lose) | `191,196` (`49.5%`) | `94,377` (`24.4%`) | `100,621` (`26.1%`) | `386,194` | `101,742.3` (`26.3%`) | `89,716.8` (`23.2%`) | `130,024.7` (`33.7%`) | `64,670.8` (`16.7%`) | `+39.4` / `69` / `386,263` |
| `ic_nightshot_iso_1600` (`1.0144` lose) | `572,724` (`30.8%`) | `606,115` (`32.6%`) | `682,401` (`36.7%`) | `1,861,240` | `262,092.1` (`14.1%`) | `375,806.3` (`20.2%`) | `725,373.8` (`39.0%`) | `497,927.3` (`26.8%`) | `+40.4` / `69` / `1,861,309` |
| `tp_nightshot_iso_1600_crop` (`1.0146` lose) | `371,314` (`30.8%`) | `393,784` (`32.7%`) | `440,100` (`36.5%`) | `1,205,198` | `166,962.2` (`13.9%`) | `241,822.8` (`20.1%`) | `470,186.6` (`39.0%`) | `326,185.4` (`27.1%`) | `+41.0` / `69` / `1,205,267` |
| `kodim12` (`1.0140` lose) | `174,984` (`49.1%`) | `88,927` (`24.9%`) | `92,528` (`26.0%`) | `356,439` | `103,720.5` (`29.1%`) | `80,966.4` (`22.7%`) | `116,856.0` (`32.8%`) | `54,869.2` (`15.4%`) | `+26.9` / `57` / `356,496` |
| `ic_flower_foveon` (`1.0132` lose) | `229,709` (`29.7%`) | `297,095` (`38.4%`) | `246,466` (`31.9%`) | `773,270` | `359,137.0` (`46.4%`) | `170,797.2` (`22.1%`) | `179,444.5` (`23.2%`) | `63,851.5` (`8.3%`) | `+39.7` / `69` / `773,339` |
| `tp_flower_foveon_crop` (`1.0119` lose) | `158,298` (`30.5%`) | `193,244` (`37.3%`) | `166,833` (`32.2%`) | `518,375` | `233,331.0` (`45.0%`) | `117,222.5` (`22.6%`) | `123,640.0` (`23.9%`) | `44,142.2` (`8.5%`) | `+39.3` / `69` / `518,444` |
| `kodim09` (`1.0115` lose) | `184,954` (`48.5%`) | `96,753` (`25.4%`) | `99,304` (`26.1%`) | `381,011` | `103,276.3` (`27.1%`) | `89,596.0` (`23.5%`) | `128,129.0` (`33.6%`) | `59,970.1` (`15.7%`) | `+39.6` / `69` / `381,080` |
| `kodim10` (`1.0111` lose) | `185,104` (`47.6%`) | `99,149` (`25.5%`) | `104,802` (`26.9%`) | `389,055` | `104,262.3` (`26.8%`) | `91,039.9` (`23.4%`) | `131,358.9` (`33.8%)` | `62,352.5` (`16.0%`) | `+41.5` / `69` / `389,124` |
| `kodim01` (`1.0076` lose) | `248,932` (`54.5%`) | `104,567` (`22.9%`) | `103,305` (`22.6%`) | `456,804` | `95,896.4` (`21.0%`) | `96,806.7` (`21.2%`) | `156,677.8` (`34.3%`) | `107,396.2` (`23.5%`) | `+27.0` / `57` / `456,861` |
| `kodim06` (`1.0056` lose) | `211,327` (`50.2%`) | `96,129` (`22.8%`) | `113,304` (`26.9%`) | `420,760` | `95,727.1` (`22.8%`) | `92,007.9` (`21.9%`) | `144,551.8` (`34.4%`) | `88,446.8` (`21.0%`) | `+26.4` / `57` / `420,817` |
| `kodim21` (`1.0053` lose) | `213,332` (`48.8%`) | `102,436` (`23.5%`) | `121,016` (`27.7%`) | `436,784` | `101,363.0` (`23.2%`) | `98,706.2` (`22.6%`) | `149,664.0` (`34.3%`) | `87,023.7` (`19.9%`) | `+27.1` / `57` / `436,841` |
| `kodim08` (`1.0046` lose) | `249,442` (`49.7%`) | `123,129` (`24.5%`) | `129,509` (`25.8%`) | `502,080` | `93,875.0` (`18.7%`) | `103,280.3` (`20.6%`) | `180,833.6` (`36.0%`) | `124,065.1` (`24.7%`) | `+26.0` / `57` / `502,137` |
| `kodim24` (`1.0029` lose) | `210,857` (`48.4%`) | `107,029` (`24.6%`) | `117,975` (`27.1%`) | `435,861` | `96,123.6` (`22.1%`) | `90,422.1` (`20.7%`) | `150,288.2` (`34.5%`) | `98,999.6` (`22.7%`) | `+27.5` / `57` / `435,918` |
| `measure` (`0.9791` win) | `531,398` (`37.7%`) | `436,468` (`30.9%`) | `442,550` (`31.4%`) | `1,410,416` | `474,238.9` (`33.6%`) | `276,716.8` (`19.6%`) | `397,118.6` (`28.2%`) | `262,299.7` (`18.6%`) | `+42.0` / `69` / `1,410,485` |
| `kodim03` (`0.9854` win) | `157,093` (`48.1%`) | `81,182` (`24.9%`) | `88,406` (`27.1%`) | `326,681` | `105,344.7` (`32.2%`) | `72,367.8` (`22.2%`) | `101,901.7` (`31.2%`) | `47,039.4` (`14.4%`) | `+27.4` / `57` / `326,738` |
| `kodim20` (`0.9613` win) | `132,061` (`41.2%`) | `61,119` (`19.1%`) | `127,031` (`39.7%`) | `320,211` | `66,828.5` (`20.9%`) | `69,216.3` (`21.6%`) | `114,476.0` (`35.8%`) | `69,663.4` (`21.8%`) | `+26.8` / `57` / `320,268` |
| `kodim23` (`0.9776` win) | `164,378` (`44.4%`) | `101,557` (`27.4%`) | `104,133` (`28.1%`) | `370,068` | `110,075.2` (`29.7%`) | `87,631.7` (`23.7%`) | `121,526.6` (`32.8%`) | `50,807.2` (`13.7%`) | `+27.3` / `57` / `370,125` |
| `ic_cathedral` (`0.9690` win) | `482,106` (`30.8%`) | `527,780` (`33.8%`) | `553,287` (`35.4%`) | `1,563,173` | `323,663.4` (`20.7%`) | `357,509.8` (`22.9%`) | `583,798.5` (`37.3%`) | `298,161.5` (`19.1%`) | `+39.8` / `69` / `1,563,242` |
| `ic_artificial` (`0.6285` win) | `44,569` (`42.4%`) | `32,344` (`30.7%`) | `28,284` (`26.9%`) | `105,197` | `31,274.8` (`29.7%`) | `11,946.7` (`11.4%`) | `31,840.9` (`30.3%`) | `30,094.8` (`28.6%`) | `+39.8` / `69` / `105,266` |

**Normalized byte-share observations and unverified mechanism hypotheses.** The `--breakdown` instrumentation measures coding costs by channel (`stream_c / stream_ALL`) and decision kind (`kind / dec`); it does not directly measure per-node empirical bit-1 frequencies, counter oversaturation, residual cross-correlations, or strip-boundary adaptation loss. Comparing the normalized byte shares between the 14 losing photographs and the 6 winning reference photographs, together with static inspection of `src/cm.c`, motivates four unverified hypotheses for the experiments in section 4.12 (`D01 T05 §3` and `D01 T05 §4`):

1. **Hypothesis 1 -- counter initialization priors, initial adaptation step, and strip boundary warm-up on medium-height (`512 <= h <= 960`) and noisy photographs:** In the breakdown table, `exp` consumes `32.8%` to `39.0%` of decision bytes on the 14 losers vs `28.2%` to `31.2%` on `measure` and `kodim03`, and every portrait Kodak loser (`kodim09`, `kodim10`, `kodim17`, `512x768`, `S = 3`, `256` rows/strip), every landscape Kodak loser (`kodim01`, `kodim06`, `kodim08`, `kodim12`, `kodim16`, `kodim21`, `kodim24`, `768x512`, `S = 2`, `256` rows/strip), `tp_flower_foveon_crop` (`1024x768`, `S = 3`, `256` rows/strip), and `ic_flower_foveon` (`1280x960`, `S = 3`, `320` rows/strip) are split into strips below section 5.3's `>= 512` rows/strip threshold (`choose_strips(h)` currently uses `h >= 768 ? 3 : (h >= 512 ? 2 : 1)`), while `chan_copy_warm` halves the learned LMS weights (`dst->lms[k] = src->lms[k] >> 1`) and caps the copied row visit count at `CAP_VISIT = 6` at each strip boundary. In `src/cm.c`, `chan_init_cold` initializes all 15 decision nodes `c[1..15]` to `32,768` (`p = 0.5`) and `RATE[0]` steps by `2/3` (`(65536 * 2) / (2 * n + 3)` for `n = 0..60`). We hypothesize (unverified by `--breakdown` alone) that lower initial bit-1 priors (`< 0.5`) on high unary-exponent nodes (`nodes 3..8`) and adaptive mantissa nodes (`nodes 9..15`), a damped first-visit adaptation schedule (`2/7` instead of `2/3`), single-strip coding below `h = 768` (`h >= 768 ? 3 : 1`), and unhalved warm LMS state with `CAP_VISIT = 24` will reduce cold-start and strip-boundary coding cost on medium-height and high-ISO photographs while keeping `S = 3` (`9` threads) on all `h >= 768` photographs.
2. **Hypothesis 2 -- chroma (`R-G` and `B-G`) stream share and cross-channel conditioning:** In the breakdown table, chroma dominates `ic_flower_foveon` (`R-G` `38.4%`, `B-G` `31.9%`, chroma `70.3%` of stream bytes, with `zero` taking `46.4%` of decision bytes), `tp_flower_foveon_crop` (`R-G` `37.3%`, `B-G` `32.2%`, chroma `69.5%`), and `ic_nightshot_iso_1600` (`R-G` `32.6%`, `B-G` `36.7%`, chroma `69.2%`, with `B-G` alone `109,677` bytes larger than `G`). In `src/cm.c`, `k2`, `k4`, `k5`, and `wsel` on `c == 2` (`B - G`) condition on `gq = q9(eg)` (luma residual) or omit residual signs in `k4`/`k5` rather than conditioning on `rq = q9(er_r)` (the just-coded `R - G` residual at the same pixel), and `c == 2`'s `lms[6]` (`fer`, the `R - G` residual tap) starts at `0`. We hypothesize that conditioning `c == 2` on `rq` and initializing `lms[6] > 0` on `c == 2` will reduce `B - G` coding cost.
3. **Hypothesis 3 -- modulo-2048 hash-table wraparound collisions in `k0`, `k3`, and `k6`:** Static inspection of `cm_model` in `src/cm.c` shows that `k0` and `k3` pack a 4-tuple of 7-level quantized differences (`0..6`) with radix-7 weights `(343, 49, 7, 1)` into `2,048` rows (`(qdot * 3 + band) & 2047` for `k0`, leaving `682` distinct `qdot` slots before `* 3`, and `qdot & 2047` for `k3` where `7^4 = 2,401 > 2,048`), and `k6` multiplies `qdot in [0, 342]` by `16` (`343 * 16 = 5,488`) before `& 2047`, folding opposite-sign large residuals (`q = 6` vs `q = 0`) onto the same context row. We hypothesize that reducing these index wraparound collisions will lower `exp` and `sign` costs on textured photographs.
4. **Hypothesis 4 -- high-frequency noise amplification in weighted predictor `wpred` (`pr[0..3]`):** In the breakdown table, `ic_nightshot_iso_1600` (`exp + mant = 65.8%` of decision bytes, `1,223,301` bytes) and `tp_nightshot_iso_1600_crop` (`exp + mant = 66.1%`, `796,372` bytes) spend two thirds of their bit budget in `exp` and `mant`. In `src/cm.c`, `pr[0] = w8 + ne8 - n8` applies full unit gain (`+1`) to the diagonal difference `NE - N`, and `pr[1..3]` apply `16/32`, `10/32`, and `7/32` neighbour-error corrections. We hypothesize that damping the diagonal term in `pr[0]` and the error-feedback gains in `pr[1..3]` will reduce residual magnitudes on noisy photographs.

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

### 4.9 Exact modular planes

**Owner:** `D01 T04 §4` (measured), `D01 T04 §5` (the port).

Milestone 5's two missing images are exact linear planes modulo 256 in every channel (`tests/test_correctness.py`: `syn_gradient` R = `3x mod 256`, G = `5y mod 256`, B = `(x + 2y) mod 256`; `syn_stripes` one colour per row, R = `17y mod 256`, G = 40, B = `(200 - 3y) mod 256`). Their cost is not a per-sample floor but the wraparounds and row starts: `rasterfold inspect` on today's files (2026-10-30) gives `syn_gradient` streams G 129, R 1,624, B 2,430 bytes and `syn_stripes` G 17, R 214, B 93 bytes, so the constant-step G planes are almost free while each wrap of R and B costs bits, because method 1 predicts the unwrapped value and clamps. A modular gradient on a channel's own sample values, `gp = (W + N - NW) mod 256` inside the image, `(2N - NN) mod 256` in column 0 from row 2, and `(2W - WW) mod 256` in row 0 from column 2, is exact on every such plane, wraps included. A planning probe on 2026-10-30 (raw RGB planes, all three channels) found `gp` exact on 100% of defined samples of `syn_gradient`, `syn_stripes`, `syn_wide`, and `syn_odd`; gated on `gp` having been exact at W and at N, the gate opens on 99.6% of `syn_gradient`'s samples and 99.6% of `syn_stripes`' with every gated sample exact, on 1.6% of `kodim01`'s with 58% exact, on 32.5% of `kodim20`'s (its flat sky) with 97.9% exact, on 88.8% of `ic_artificial`'s with 98.8% exact, and on none of `syn_noise`'s.

The design is a flag, not a model. Inside the gate one binary decision says whether the sample equals `gp`; when it does, the value is `gp` and the residual is not coded, so a gated hit costs one well-predicted decision instead of the residual's three or more. Outside the gate, and after a miss, the residual is coded exactly as today. The flag's probability comes from a direct counter (not mixed) chosen by the length of the current run of gated hits and whether `gp` was exact at NE and NW. Everything is per channel, so the three channel threads and the one-thread pipeline stay byte-identical. Prior art: PNG's filters predict modulo 256 (`Sub`, `Up`, `Average`, `Paeth`, ISO/IEC 15948 section 9), lossless WebP adds its predicted residuals modulo 256 per component (RFC 9649, the predictor transform of the lossless format), JPEG XL's `Gradient` predictor clamps `W + N - NW` to the range of W and N (ISO/IEC 18181-1, modular predictor 5) and so cannot follow a wrap, and JPEG-LS enters a run mode when the local gradients are zero, coding run lengths instead of residuals (ITU-T T.87, section A.7). The flag borrows the run-mode idea, gated on exactness of a modular predictor instead of on flatness.

Non-goals: no change to the WP, LMS, or context models; no backward references or colour cache (`D01 T06`); no eighth model (the alternative-prediction context is follow-up 2 if this misses, the shape `D01 T04 §2` measured for the match model).

**Result (`D01 T04 §4`, 2026-10-30): rejected.** All four variants of the frozen grid miss `syn_stripes < 271` (`357`, `451`, `355`, `440` bytes) and the `photo` bar (+0.474% to +0.572%, 25 of 39 images more than 0.1% worse, `ic_artificial` up to +24%), while `syn_gradient` falls from `4,227` to `366`-`442`, under e9's `616` in every variant. The section 3.2 row gives the figures and the inferred causes (the flag's own cost is estimated from the counter rule; no run separated flag bytes from residual bytes): a direct counter's `floor` ceiling (`p = 4092`, about 41 bytes a channel on `syn_stripes`), column 0 outside the gate, and a hit that freezes residual learning; and the design's premise that a hit replaces "the residual's three or more" decisions was wrong, because a zero residual is itself one decision (node 1), so the flag saves nothing where the residual path already predicts well. What carries forward is the evidence that `gp` is the right predictor for wrapped planes; follow-up 2 feeds it to the mixer as model context, covering column 0, instead of escaping around the mixer.

The summary lines `python3 research/ab.py size build/research/try.c <set> -v` printed on 2026-10-30 (geometric means and worst images are ratios against JPEG XL `-e 7`, `e9`, and lossless WebP as `research/ab.py` reports them):

```text
# baseline src/cm.c at 207073e
synth total=206229 jxl=226544 ratio=0.9103 geomean=0.5760 worst=1.4647 (syn_gradient) e9=223034 e9_ratio=0.9247 e9_geomean=1.0986 e9_worst=6.8620 (syn_gradient) webp=201248 webp_ratio=1.0248 webp_geomean=1.7601 webp_worst=23.4833 (syn_gradient)
photo total=30812732 jxl=31782058 ratio=0.9695 geomean=0.9698 worst=1.0000 (kodim17) e9=31400714 e9_ratio=0.9813 e9_geomean=0.9853 e9_worst=1.0152 (kodim16) webp=34138176 webp_ratio=0.9026 webp_geomean=0.8905 webp_worst=1.0100 (ic_deer) measure=1405087 (0.9554 e9=0.9754 webp=0.8844)
# (64 counters, RATE) no -D
synth total=202396 jxl=226544 ratio=0.8934 geomean=0.4527 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9075 e9_geomean=0.8635 e9_worst=1.7689 (syn_wide) webp=201248 webp_ratio=1.0057 webp_geomean=1.3834 webp_worst=2.3800 (syn_stripes)
photo total=30958665 jxl=31782058 ratio=0.9741 geomean=0.9780 worst=1.0074 (ic_artificial) e9=31400714 e9_ratio=0.9859 e9_geomean=0.9936 e9_worst=1.0941 (ic_artificial) webp=34138176 webp_ratio=0.9069 webp_geomean=0.8980 webp_worst=1.0848 (ic_artificial) measure=1436665 (0.9769 e9=0.9973 webp=0.9043)
# (64 counters, fixed rate) -DEP_FIXED
synth total=202753 jxl=226544 ratio=0.8950 geomean=0.5156 worst=1.7500 (syn_odd) e9=223034 e9_ratio=0.9091 e9_geomean=0.9833 e9_worst=3.5538 (syn_odd) webp=201248 webp_ratio=1.0075 webp_geomean=1.5754 webp_worst=3.8500 (syn_odd)
photo total=30964709 jxl=31782058 ratio=0.9743 geomean=0.9786 worst=1.0252 (ic_artificial) e9=31400714 e9_ratio=0.9861 e9_geomean=0.9942 e9_worst=1.1135 (ic_artificial) webp=34138176 webp_ratio=0.9070 webp_geomean=0.8985 webp_worst=1.1041 (ic_artificial) measure=1437022 (0.9771 e9=0.9975 webp=0.9045)
# (16 counters, RATE) -DEP_CTX16
synth total=202388 jxl=226544 ratio=0.8934 geomean=0.4512 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9074 e9_geomean=0.8606 e9_worst=1.7594 (syn_wide) webp=201248 webp_ratio=1.0057 webp_geomean=1.3787 webp_worst=2.3667 (syn_stripes)
photo total=30982672 jxl=31782058 ratio=0.9748 geomean=0.9795 worst=1.0390 (ic_artificial) e9=31400714 e9_ratio=0.9867 e9_geomean=0.9951 e9_worst=1.1285 (ic_artificial) webp=34138176 webp_ratio=0.9076 webp_geomean=0.8994 webp_worst=1.1189 (ic_artificial) measure=1436816 (0.9770 e9=0.9974 webp=0.9044)
# (16 counters, fixed rate) -DEP_CTX16 -DEP_FIXED
synth total=202711 jxl=226544 ratio=0.8948 geomean=0.5102 worst=1.6818 (syn_odd) e9=223034 e9_ratio=0.9089 e9_geomean=0.9730 e9_worst=3.4154 (syn_odd) webp=201248 webp_ratio=1.0073 webp_geomean=1.5590 webp_worst=3.7000 (syn_odd)
photo total=30988986 jxl=31782058 ratio=0.9750 geomean=0.9802 worst=1.0595 (ic_artificial) e9=31400714 e9_ratio=0.9869 e9_geomean=0.9958 e9_worst=1.1508 (ic_artificial) webp=34138176 webp_ratio=0.9078 webp_geomean=0.8999 webp_worst=1.1410 (ic_artificial) measure=1437177 (0.9772 e9=0.9976 webp=0.9046)
```


### 4.10 Gated modular prediction

**Owner:** `D01 T04 §6` (measured), `D01 T04 §7` (the port).

Follow-up 2 of Milestone 5's missed-milestone policy (`D99 T01 §4`). Section 4.9's flag proved the target right and the mechanism wrong: the modular gradient `gp` predicts `syn_gradient` and `syn_stripes` exactly, but the flag missed `syn_stripes < 271` and cost `photo` +0.47% to +0.57% (section 3.2, "exact-plane flag in method 1"). Three causes are carried forward: column 0 lay outside the gate, so every row start paid a full residual; a hit skipped the residual coder, so its counters and the mixer stopped learning exactly where the image was predictable; and a direct counter with no mixer could not weigh its own record against the neighbourhood.

The design removes the decision instead of tuning it. Inside a gate that `gp` was exact at the causal neighbours (now also defined in column 0 from `2N - NN` and in row 0 from `2W - WW`), `gp` replaces the prediction, so a hit is the residual coder's own zero decision, mixed and learning as today, and a miss is one residual with no flag. The gate state selects a weight set from the four unused slots of each `qE` group (`wsel + 4`), so the mixer learns the gate's reliability per error-energy band with no memory growth. A planning probe (2026-10-30, quoted in `D01 T04 §6`) found the two-neighbour gate open on 99.9% or more of every exact-plane image with every gated sample exact, and on photos open on 1.9% to 89% of samples (`kodim20` 32.5%, `ic_artificial` 88.9%) at 45% to 99% exactness; the four-neighbour gate halves to quarters the openings on `kodim01`, `kodim05`, `measure` and `ic_spider_web` but barely changes them on `kodim20` (31.5%) and `ic_artificial` (86.2%), and lifts exactness above 90% on the natural photos, while `measure` and `ic_spider_web` stay near 50%. On gated misses the mean `|v - gp|` is above the mean `|v - MED(W, N, NW)|` on every photo probed; MED is the probe's stand-in, not the shipped prediction (WP, the LMS correction and the channel base), so the photo bar is the question.

Non-goals: no new decision, counter, or model row; no change to WP, LMS, or the context models' inputs; no backward references or colour cache (`D01 T06`).

Prior art: the PNG Paeth and WebP lossless predictors choose among neighbour predictors per image or block, not per sample; JPEG XL's modular mode selects a gradient predictor per tree leaf; JPEG-LS switches into run mode on a flat-neighbourhood gate. This gate is per sample and learned only through the mixer's weight set.

**Result (`D01 T04 §6`, 2026-10-30): rejected.** All four variants clear `syn_gradient < 616` (111 to 118 bytes) and `syn_stripes < 271` (103 to 106), so the exact-plane cost is gone (`syn_stripes` R and B streams 214 and 93 bytes -> 22 and 20). Every variant misses the per-image `photo` bar: the best, (`g4`, on), leaves the total at -0.002% but makes 11 of 39 images more than 0.1% larger (`ic_artificial` +1.17%, `ic_spider_web` +0.90%, `measure` +0.60%). The cause is not established by these runs. A hypothesis for `measure` and `ic_spider_web` is that the four-neighbour gate is exact only about half the time it opens there (planning probe); it does not explain `ic_artificial`, the worst regression, where the gated samples are 99.1% exact, so a second hypothesis is that replacing the prediction across 86% of that image changes what the shared residual contexts and models learn from it. Follow-up 3 keeps `gp`, its column-0 and row-0 forms, the `g4` gate, and the weight-set offset, and adds what this lacked: a per-sample choice that uses `gp` only where it has recently been the better prediction, learned identically by encoder and decoder from errors already known. Summary lines (`python3 research/ab.py size <src> <set> -v`):

```text
# baseline src/cm.c at 207073e
synth total=206229 jxl=226544 ratio=0.9103 geomean=0.5760 worst=1.4647 (syn_gradient) e9=223034 e9_ratio=0.9247 e9_geomean=1.0986 e9_worst=6.8620 (syn_gradient) webp=201248 webp_ratio=1.0248 webp_geomean=1.7601 webp_worst=23.4833 (syn_gradient)
photo total=30812732 jxl=31782058 ratio=0.9695 geomean=0.9698 worst=1.0000 (kodim17) e9=31400714 e9_ratio=0.9813 e9_geomean=0.9853 e9_worst=1.0152 (kodim16) webp=34138176 webp_ratio=0.9026 webp_geomean=0.8905 webp_worst=1.0100 (ic_deer) measure=1405087 (0.9554 e9=0.9754 webp=0.8844)
# (g2, on) no -D
synth total=201610 jxl=226544 ratio=0.8899 geomean=0.3194 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9039 e9_geomean=0.6093 e9_worst=1.1077 (syn_odd) webp=201248 webp_ratio=1.0018 webp_geomean=0.9761 webp_worst=1.6829 (syn_checker)
photo total=31002196 jxl=31782058 ratio=0.9755 geomean=0.9780 worst=1.0110 (kodim17) e9=31400714 e9_ratio=0.9873 e9_geomean=0.9936 e9_worst=1.0322 (ic_flower_foveon) webp=34138176 webp_ratio=0.9081 webp_geomean=0.8980 webp_worst=1.0099 (ic_deer) measure=1429459 (0.9720 e9=0.9923 webp=0.8998)
# (g4, on) -DGP_4N
synth total=201628 jxl=226544 ratio=0.8900 geomean=0.3247 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9040 e9_geomean=0.6194 e9_worst=1.1846 (syn_odd) webp=201248 webp_ratio=1.0019 webp_geomean=0.9923 webp_worst=1.6829 (syn_checker)
photo total=30812123 jxl=31782058 ratio=0.9695 geomean=0.9701 worst=1.0007 (kodim17) e9=31400714 e9_ratio=0.9813 e9_geomean=0.9856 e9_worst=1.0166 (kodim16) webp=34138176 webp_ratio=0.9026 webp_geomean=0.8907 webp_worst=1.0099 (ic_deer) measure=1413462 (0.9611 e9=0.9812 webp=0.8897)
# (g2, off) -DGP_NOCTX
synth total=201610 jxl=226544 ratio=0.8899 geomean=0.3194 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9039 e9_geomean=0.6093 e9_worst=1.1077 (syn_odd) webp=201248 webp_ratio=1.0018 webp_geomean=0.9761 webp_worst=1.6829 (syn_checker)
photo total=31076336 jxl=31782058 ratio=0.9778 geomean=0.9813 worst=1.0131 (kodim17) e9=31400714 e9_ratio=0.9897 e9_geomean=0.9970 e9_worst=1.0469 (ic_flower_foveon) webp=34138176 webp_ratio=0.9103 webp_geomean=0.9010 webp_worst=1.0098 (ic_deer) measure=1438246 (0.9779 e9=0.9984 webp=0.9053)
# (g4, off) -DGP_4N -DGP_NOCTX
synth total=201628 jxl=226544 ratio=0.8900 geomean=0.3247 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9040 e9_geomean=0.6194 e9_worst=1.1846 (syn_odd) webp=201248 webp_ratio=1.0019 webp_geomean=0.9923 webp_worst=1.6829 (syn_checker)
photo total=30827515 jxl=31782058 ratio=0.9700 geomean=0.9710 worst=1.0007 (kodim17) e9=31400714 e9_ratio=0.9817 e9_geomean=0.9865 e9_worst=1.0166 (kodim16) webp=34138176 webp_ratio=0.9030 webp_geomean=0.8916 webp_worst=1.0098 (ic_deer) measure=1416501 (0.9631 e9=0.9833 webp=0.8916)
```

### 4.11 Learned gated modular prediction

**Owner:** `D01 T04 §8` (measured), `D01 T04 §9` (the port under the old bars, struck), `D01 T04 §10` (the port).

Follow-up 3, the last, of Milestone 5's missed-milestone policy (`D99 T01 §4`). Section 4.10's gate proved the exact-plane cost removable (`syn_gradient` 111 to 118 bytes, `syn_stripes` 103 to 106) but replaced the prediction wherever the gate opened, which made 10 to 33 of 39 photographs more than 0.1% larger (section 3.2, "gated modular prediction in method 1"). The gate says only that `gp` was exact at the neighbours; it never compares `gp` with the prediction it displaces, so on photographs it displaced a prediction that was often as good or better.

The design adds that comparison and nothing else. Each channel keeps two running absolute-error sums over gated samples, one for `gp` and one for today's prediction, each sample's error capped at 32 and both sums halved when either reaches 4,096, optionally one pair per error-energy band `qE`. Inside the gate `gp` is used only when its sum times a margin (1 or 2) is below today's; ties and the empty start keep today's prediction, so a channel where `gp` is never clearly better codes exactly as today. On an exact plane `gp`'s sum stays 0 while today's grows from the first gated sample, so the choice flips to `gp` at once. Encoder and decoder update the sums from samples already coded, so nothing is signalled, and the sums live in the stack-held coder state, so nothing is allocated. §6's weight-set offset now marks the choice rather than the gate.

Non-goals: no new decision bit, counter row, or model row; no change to WP, LMS, or the context models' inputs; no backward references or colour cache (`D01 T06`).

Prior art: CALIC and JPEG-LS switch predictors on local gradient evidence; JPEG XL's weighted predictor weighs its sub-predictors by their recent errors at the neighbours; lpaq and paq8 mix or select models by their running success. This choice is a two-way selection by running error, learned per channel and optionally per error band.

**Result (`D01 T04 §8`, 2026-10-30): rejected.** All four variants clear `syn_gradient < 616` (112 to 127 bytes) and `syn_stripes < 271` (105 to 117). The error-sum choice does what section 4.10 lacked on size: v2 (`g4`, context off, margin 2) keeps every photograph within 0.1% of the baseline, which no variant of section 4.10 did, (worst `kodim03` +0.032%) while cutting the `photo` total by 0.057%. v1, v3 and v4 each leave `ic_artificial` 0.74% to 1.69% larger. v2 then misses the timing bar: on the `synth` set at one thread, encode and decode totals are 8% to 9% and 13% to 14% slower in two disjoint pools of alternating runs, and `photo` decode at one thread is 2% to 4% slower with single images up to 25%. Its decode `alloc_peak` on `measure/image.bmp` is 31 bytes over the baseline, exactly the growth of that file, which the frozen bar counts. No candidate exists, so `tp` was not run, no patch exists, and nothing is ported. Follow-up 3 was the last under `D99 T01 §4`, so Milestone 5 goes to the operator. Summary lines (`python3 research/ab.py size build/research/try.c <set> -v`; the baseline lines are section 4.10's):

**Ported (`D01 T04 §10`, 2026-10-30).** The operator relaxed Milestone 5's timing and memory rule for v2 (`D99 T01 §4`: encode and decode time at most 1.15x Milestone 4's on every set total and 1.5x on any image of at least 16,384 pixels, codec working memory unchanged with the input file's growth not counted), so v2 is now part of format version 3 (`spec/FOLD.md`, "Gated gradient (version 3)"), with `KAT_FOLD3` regenerated and a `gated-gradient` correctness case whose eight boundary fixtures reach every selector boundary and fail all eight listed mutations. The port's files equal the prototype's on all 55 corpus images and the fixtures, and its sizes are §8's: `syn_gradient` 119, `syn_stripes` 108, `photo` 30,795,260 (-0.057%, worst `kodim03` +0.032%), `tp` 6,640,275 against 6,641,390 (-0.017%). Timing and memory are measured by the Milestone 5 gate (`D05 T01 §5`).

```text
# v1 (g4, off, M 1) no -D
synth total=201634 jxl=226544 ratio=0.8900 geomean=0.3264 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9041 e9_geomean=0.6225 e9_worst=1.2000 (syn_odd) webp=201248 webp_ratio=1.0019 webp_geomean=0.9974 webp_worst=1.6829 (syn_checker)
photo total=30786997 jxl=31782058 ratio=0.9687 geomean=0.9694 worst=1.0002 (kodim17) e9=31400714 e9_ratio=0.9805 e9_geomean=0.9849 e9_worst=1.0153 (kodim16) webp=34138176 webp_ratio=0.9018 webp_geomean=0.8901 webp_worst=1.0099 (ic_deer) measure=1406679 (0.9565 e9=0.9765 webp=0.8854)
# v2 (g4, off, M 2) -DGS_M2
synth total=201634 jxl=226544 ratio=0.8900 geomean=0.3264 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9041 e9_geomean=0.6225 e9_worst=1.2000 (syn_odd) webp=201248 webp_ratio=1.0019 webp_geomean=0.9974 webp_worst=1.6829 (syn_checker)
photo total=30795260 jxl=31782058 ratio=0.9690 geomean=0.9691 worst=1.0000 (kodim17) e9=31400714 e9_ratio=0.9807 e9_geomean=0.9846 e9_worst=1.0152 (kodim16) webp=34138176 webp_ratio=0.9021 webp_geomean=0.8898 webp_worst=1.0098 (ic_deer) measure=1405118 (0.9554 e9=0.9754 webp=0.8844)
# v3 (g4, qE, M 2) -DGS_QE -DGS_M2
synth total=201667 jxl=226544 ratio=0.8902 geomean=0.3355 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9042 e9_geomean=0.6399 e9_worst=1.3231 (syn_odd) webp=201248 webp_ratio=1.0021 webp_geomean=1.0252 webp_worst=1.6829 (syn_checker)
photo total=30796521 jxl=31782058 ratio=0.9690 geomean=0.9694 worst=1.0001 (kodim17) e9=31400714 e9_ratio=0.9808 e9_geomean=0.9848 e9_worst=1.0152 (kodim16) webp=34138176 webp_ratio=0.9021 webp_geomean=0.8900 webp_worst=1.0098 (ic_deer) measure=1405355 (0.9556 e9=0.9756 webp=0.8846)
# v4 (g2, off, M 2) -DGS_2N -DGS_M2
synth total=201615 jxl=226544 ratio=0.8900 geomean=0.3209 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9040 e9_geomean=0.6121 e9_worst=1.1231 (syn_odd) webp=201248 webp_ratio=1.0018 webp_geomean=0.9806 webp_worst=1.6829 (syn_checker)
photo total=30807923 jxl=31782058 ratio=0.9693 geomean=0.9697 worst=1.0000 (kodim17) e9=31400714 e9_ratio=0.9811 e9_geomean=0.9851 e9_worst=1.0152 (kodim16) webp=34138176 webp_ratio=0.9024 webp_geomean=0.8903 webp_worst=1.0100 (ic_deer) measure=1405118 (0.9554 e9=0.9754 webp=0.8844)
```

### 4.12 Strip-adaptation priors, collision-reduced contexts, and noise-damped prediction

**Owner:** `D01 T05 §3` (measured), `D01 T05 §4` (the port into format version 3).

Section 4.5's `--breakdown` table on the 14 losing and 6 winning photographs at Milestone 6 (`D01 T05 §1`), combined with static inspection of `src/cm.c`, motivated four candidate hypotheses across medium-height Kodak and Foveon photographs and high-ISO nightshots (`ic_nightshot_iso_1600`, `tp_nightshot_iso_1600_crop`), while `D01 T02 §6` ruled out encoder-chosen parameters (`-0.018% < 0.3%`). Rather than adding a new pass, context model, or allocation, this candidate design combines four zero-hot-loop-overhead changes inside format version 3 (`src/cm.c`) to be tested empirically in `D01 T05 §3`:

1. **Candidate counter priors (`chan_init_cold`) and damped initial adaptation schedule (`RATE[0..72]`):**
   - `chan_init_cold` initializes the 16-node context row (`c[0..15]`) in version 3 (`nmodel == 7`) to `c0_v = (0, 32768, 32768, 30000, 28000, 24000, 18000, 12000)` and `c1_v = (6000, 29000, 29000, 29000, 29000, 30000, 30000, 30000)` instead of `32,768` across all `c[1..15]`, testing lower initial bit-1 priors (`< 0.5`) on unary exponent nodes `3..8` and adaptive mantissa nodes `9..15` from the first sample in a row.
   - `RATE[n]` is extended from `n = 0..60` to `n = 0..72` (`RATE[73]`) with `RATE[n] = (uint32_t)((65536 * 2) / (2 * n + 7))` and `cp[k][0] = n + (n < 72)`. When the linear predictor is active (`s->predv == s->base`), `rate[k] = RATE[n]` (`2/7` on the first visit, settling to `2/151` at `n = 72`), damping first-visit counter overshoot on sparse context rows in noisy images; when gated modular prediction is active (`s->predv != s->base`) and `n >= 2`, `rate[k] = RATE[n - 2]` (`2 / (2n + 3)`), preserving fast tracking on sharp synthetic edges (`ic_artificial`).
2. **Strip-height threshold (`choose_strips`) and unhalved warm LMS state (`chan_copy_warm`):**
   - `choose_strips(h)` uses `h >= 768 ? 3 : 1` (instead of `h >= 768 ? 3 : (h >= 512 ? 2 : 1)`), so the 18 landscape `768x512` Kodak photographs (`h = 512 < 768`, where `S = 2` split each image into 256-row halves) are coded as a single continuous strip without strip-boundary resets, while all `h >= 768` photographs (`measure/image.bmp` `1672x941`, all 14 `ic_*` `1280x960` photographs, all 5 `tp_*_crop` `1024x768` test-pack crops, and the 6 portrait `512x768` Kodak photographs) keep `S = 3` (`9` threads, with `wr = h / 6u > 96u ? h / 6u : 96u` unchanged).
   - `chan_copy_warm` copies `dst->lms[k] = src->lms[k]` without halving (`>> 1`), raises the copied row visit-count cap from `CAP_VISIT = 6` to `CAP_VISIT = 24`, and `cm_alloc_strips` initializes `ch->lms[6] = 20000` on channel `c == 2` (`B - G`'s `fer` cross-channel tap for the `R - G` residual).
3. **Collision-reduced context indexing and `B - G` (`c == 2`) cross-channel conditioning (`cm_model`):**
   - `scl = (E >> 2) + 3` (instead of `+ 2`), slightly widening the local quantization scale on low-energy textures.
   - `k0` uses coprime strides `(195, 31, 5, 1) * 3 + band` (`6 * 195 + 6 * 31 + 6 * 5 + 6 = 1,392`, `* 3 + 2 = 4,178`, eliminating radix-7 power-of-two aliasing), `k3` uses `(289, 43, 7, 1)` (`6 * 289 + 6 * 43 + 6 * 7 + 6 = 2,040 < 2,048`, eliminating the `7^4 = 2,401 > 2,048` wraparound collision entirely), and `k6` uses `(qdot * 6u + ((qE * 3) >> 3)) & 2047u` (`342 * 6 + 5 = 2,057`, eliminating the `343 * 16 = 5,488` `2.68x` wraparound).
   - On chroma channels (`c >= 1`), `k2`, `k4`, `k5`, and `wsel` condition `c == 2` (`B - G`) on `rq = q9(er_r)` (the just-coded `R - G` residual at the same pixel) instead of `gq` alone: `k2 = (q2d * 3u + ((c == 2 ? rq : gq) < 3 ? 0 : (c == 2 ? rq : gq) > 5 ? 2 : 1)) & 2047u`, `k4` and `k5` replace `qE` on `c >= 1` with `((qE >> 1) * 2 + ((c == 2 ? rq : gq) != 4))` (keeping `MROWS_C` at `1,296` rows), and `wsel` on `c == 2` tests `(rq == 4 && gq == 4) ? 0 : 1 + (iabs(er_r) > 3)`.
4. **High-frequency noise damping in `wpred` (`pr[0..3]`):**
   - `pr[0] = w8 + (((ne8 - n8) * 3) >> 3)` (damping the diagonal `NE - N` difference from `1.0` to `3/8 = 0.375`, reducing `Var(pr[0])` under independent noise from `3.0 * sigma^2` to `1.28 * sigma^2`), `pr[1] = n8 - (((sumWN + teNE) * 12) >> 5)`, `pr[2] = w8 - (((sumWN + teNW) * 8) >> 5)`, and `pr[3] = n8 - (((teNW + teN + teNE) * 5) >> 5)`.

**Expected effect and stop condition.** Tested in `D01 T05 §3` (`build/research/try.c`) against Milestone 6 (`src/cm.c`): `photo` total must drop by at least `0.5%` (`<= 30,713,912`), at least `34 of 39` photographs in `photo` and `8 of 10` photographs in `tp` must be smaller than JPEG XL e9, no photograph in `photo` or `tp` may be more than `1.0%` larger than JPEG XL e9 (`<= 1.0100`), no single photograph in `photo` or `tp` may regress by more than `+0.5%` vs Milestone 6, and two agreeing sequential alternating `research/ab.py ab -n9` timing runs on an idle machine must keep both `B/A dec` (total minimum across runs) and `min_tot dec` (sum of per-image minima across runs) within `1.03x` of Milestone 6 on `measure` (`1672x941`, `S = 3`) at `-t1`/`-t3` and within `1.05x` on representative `S = 3` corpus photographs (`ic_nightshot_iso_1600`, `ic_cathedral`, `ic_deer`, `1280x960`) at `-t1`/`-t3`/`-t9`. Once ported into `src/cm.c` in `D01 T05 §4` (and gated in `D05 T01 §7`), encode time on `measure/image.bmp` (`python3 research/vsjxl.py -n5`, where RasterFold runs at default threads against `ffmpeg -threads 1` per section 4.5, with single-threaded `python3 research/vsjxl.py -n5 --threads 1` also measured) must stay at most `0.3` of JPEG XL e7, and multi-threaded decode on `testdata/photos/` (`n = 10`, `--threads 9` and default threads via `python3 research/vsjxl.py --webp-photos`) must remain faster than lossless WebP (`< 1.00x`).

**Measured (`D01 T05 §3`, 2026-11-07 -- accepted for `D01 T05 §4`).** Prototyped in `build/research/try.c` against Milestone 6 `src/cm.c` (`b3eb341`):
- `python3 research/ab.py size build/research/try.c photo -v`: `total=30634873 jxl=31782058 ratio=0.9639 geomean=0.9622 worst=0.9934 (kodim17) e9=31400714 e9_ratio=0.9756 e9_geomean=0.9775 e9_worst=1.0097 (ic_nightshot_iso_1600) webp=34138176 webp_ratio=0.8974 webp_geomean=0.8834 webp_worst=1.0044 (ic_deer) measure=1400388 (0.9522 e9=0.9721 webp=0.8815)` vs Milestone 6 `total=30868254` (`-233,381` B = `-0.7561% <= -0.500%`, clearing `<= 30,713,912`). `35 of 39` photographs beat JPEG XL e9 (clearing `>= 34 of 39`; the 4 above e9 are `kodim09` `376,715 / 376,471 = 1.0006`, `kodim16` `365,559 / 364,606 = 1.0026`, `kodim17` `382,280 / 380,388 = 1.0050`, and `ic_nightshot_iso_1600` `1,852,542 / 1,834,783 = 1.0097`), `0 of 39` are above `1.0100` of e9 (`e9_worst = 1.0097 <= 1.0100`), `39 of 39` beat JPEG XL e7 (`worst = 0.9934` on `kodim17`), and `39 of 39` are strictly smaller than Milestone 6 (`0 of 39` above `+0.5%`, worst `ic_artificial` `105,266 -> 105,262` = `-4` B = `-0.0038%`, best `kodim03` `326,738 -> 320,826` = `-5,912` B = `-1.8094%`, `kodim07` `-6,038` B = `-1.6921%`, `kodim12` `-5,954` B = `-1.6701%`).
- `python3 research/ab.py size build/research/try.c tp -v`: `total=6598954 jxl=6826895 ratio=0.9666 geomean=0.9625 worst=0.9897 (tp_nightshot_iso_1600_crop) e9=6740249 e9_ratio=0.9790 e9_geomean=0.9764 e9_worst=1.0096 (tp_nightshot_iso_1600_crop) webp=7368948 webp_ratio=0.8955 webp_geomean=0.8723 webp_worst=1.0014 (tp_deer_crop)` vs Milestone 6 `total=6656840` (`-57,886` B = `-0.8696%`). `9 of 10` test-pack photographs beat JPEG XL e9 (clearing `>= 8 of 10`; only `tp_nightshot_iso_1600_crop` `1,199,268 / 1,187,904 = 1.0096` is above e9, flipping `kodim01` `450,829 < 454,757` and `tp_flower_foveon_crop` `511,404 < 512,571` below e9), `0 of 10` are above `1.0100` of e9 (`e9_worst = 1.0096 <= 1.0100`), and `10 of 10` are strictly smaller than Milestone 6 (worst `tp_nightshot_iso_1600_crop` `-5,999` B = `-0.4977%`, best `kodim03` `-5,912` B = `-1.8094%`).
- `python3 research/ab.py size build/research/try.c synth -v`: `total=201640 jxl=226544 ratio=0.8901 geomean=0.3275 worst=0.9241 (syn_noise) e9=223034 e9_ratio=0.9041 e9_geomean=0.6246 e9_worst=1.1538 (syn_odd) webp=201248 webp_ratio=1.0019 webp_geomean=1.0007 webp_worst=1.6829 (syn_checker)` vs Milestone 6 `total=201638` (`+2` B = `+0.0010%`, with `syn_gradient=119 < 616` e9, `syn_stripes=113 < 271` e9, `syn_ui=1410 < 2195` e9, `syn_text=1447 < 2038` e9, `syn_checker=138 < 210` e9, `syn_pixel=52 < 53` e9, `syn_odd=75` vs Milestone 6 `79`).
- Alternating A/B timing (`python3 research/ab.py ab src/cm.c build/research/try.c -n9 ...`, run sequentially on an idle machine after replacing earlier concurrent `-n5` runs that had contended for CPU cores): on `measure` (`1672x941`, `S = 3`), `-t1` Run 1 `B/A: enc 0.986 dec 1.026` (`579.7 / 587.8` ms enc, `579.5 / 564.7` ms dec) and Run 2 `B/A: enc 0.993 dec 1.013` (`528.3 / 532.0` ms enc, `529.6 / 522.9` ms dec), and `-t3` Run 1 `B/A: enc 1.105 dec 1.020` (`267.6 / 242.1` ms enc, `249.7 / 244.8` ms dec) and Run 2 `B/A: enc 0.983 dec 0.933` (`267.5 / 272.1` ms enc, `245.3 / 263.0` ms dec), all `<= 1.03x` decode; on `ic_nightshot_iso_1600 ic_cathedral ic_deer` (`1280x960`, `S = 3`, `-v`), `-t1` Run 1 `B/A: enc 1.002 dec 0.975` (`min_tot (n=3): dec=0.975 (1609.0/1649.7ms) geomean=0.977 worst=ic_nightshot_iso_1600(0.999) | enc=1.002 (1579.3/1575.5ms) geomean=0.995 worst=ic_deer(1.092)`) and Run 2 `B/A: enc 1.044 dec 0.954` (`min_tot (n=3): dec=0.970 (1636.5/1687.4ms) geomean=0.969 worst=ic_deer(0.977) | enc=1.019 (1617.2/1586.5ms) geomean=1.019 worst=ic_deer(1.046)`), `-t3` Run 1 `B/A: enc 1.007 dec 0.979` (`min_tot (n=3): dec=1.022 (747.8/731.5ms) geomean=1.022 worst=ic_deer(1.053) | enc=1.039 (818.2/787.5ms) geomean=1.041 worst=ic_nightshot_iso_1600(1.118)`) and Run 2 `B/A: enc 1.092 dec 1.035` (`min_tot (n=3): dec=0.999 (772.2/773.3ms) geomean=0.997 worst=ic_cathedral(1.040) | enc=1.082 (834.9/771.9ms) geomean=1.078 worst=ic_deer(1.142)`), and `-t9` Run 1 `B/A: enc 1.001 dec 1.009` (`min_tot (n=3): dec=0.997 (609.9/611.9ms) geomean=0.995 worst=ic_deer(1.016) | enc=0.991 (646.1/651.8ms) geomean=0.992 worst=ic_cathedral(1.050)`) and Run 2 `B/A: enc 1.008 dec 0.972` (`min_tot (n=3): dec=0.990 (583.5/589.1ms) geomean=0.990 worst=ic_nightshot_iso_1600(1.064) | enc=1.029 (631.8/613.7ms) geomean=1.032 worst=ic_nightshot_iso_1600(1.120)`), with every `B/A dec` and `min_tot dec` `<= 1.05x`. Accepted for port in `D01 T05 §4`.

### 4.13 Where WebP still wins on generated images

**Owner:** `D01 T06 §1` (measured 2026-11-09 at codec revision `b22dec2`, codec sources unchanged since `abd8039`).

Measured with `python3 scripts/windows.py research/ab.py size src/cm.c synth -v` (`-t1`, Windows ffmpeg reference sizes in `research/jxl_refs.json`): `total=201640 webp=201248 webp_ratio=1.0019 webp_geomean=1.0007 webp_worst=1.6829 (syn_checker)`. Four of the eleven `synth` images are above 1.1 times WebP. Attribution is method-appropriate: palette files are attributed structurally from the container (`spec/FOLD.md`: 32-byte header, then `2 + 3 * count + 4` palette field bytes and the index stream), and the one CM file uses `bench.exe --breakdown` (per channel and decision kind, payload verified byte-identical). WebP mechanism claims are hypotheses: only whole-file WebP sizes are measured, not WebP-internal attribution.

| Image | Method | RasterFold | WebP | Ratio | Attribution | Target |
| --- | --- | ---: | ---: | ---: | --- | --- |
| `syn_checker` (512x512, 2 colours) | 3 (extended palette) | 138 | 82 | 1.6829 | header 32, palette field 12, index stream 94 (68% of file); left/above index repeat 93.8% | §3 copy runs: the index stream codes 16-pixel runs one pixel at a time |
| `syn_text` (800x600, 251 colours) | 3 | 1447 | 1198 | 1.2078 | header 32, palette field 759 (753 raw palette bytes, 52% of file), index stream 656; left repeat 99.1%, above 97.1% | §3 for the index stream; palette bytes are the larger half (see below) |
| `syn_diagram` (900x600, 251 colours) | 3 | 1452 | 1228 | 1.1824 | header 32, palette field 759 (753 palette bytes), index stream 661; left repeat 99.2%, above 97.1% | §3 for the index stream; palette bytes are the larger half (see below) |
| `syn_odd` (65x63, 4095 colours) | 1 (CM) | 75 | 60 | 1.2500 | header 45 of 75 (60%), stream 30 (G 11, R-G 12, B-G 7; decision bytes 15.8: zero 8.2 (52.2%), sign 1.8 (11.5%), exp 3.8 (24.4%), mant 1.9 (11.9%), flush +14.2; per channel G dec 6.3, R-G dec 7.3, B-G dec 2.2; from `bench.exe -r1 -t1 --breakdown build/research/ppm/syn_odd.ppm`) | neither: fixed header overhead on a 4095-pixel image; the +15 bytes are inside the Milestone 8 gate's 32-byte allowance |

Two findings route the next two sections. First, §3 (copy runs for palette images) targets the index streams of `syn_checker`, `syn_text`, and `syn_diagram`: with left/above repeats above 93%, a run or copy token is the leading hypothesis for those bytes (repeat counts are pixel counts, not decision costs, so the saving is unproven until §3's prototype measures it); `syn_checker`'s whole WebP file (82) is smaller than this index stream plus header (126), so no palette-only improvement can reach WebP there. Second, the raw palette is the single largest component on `syn_text` and `syn_diagram` (753 of 1447 and 1452 bytes), and the same pattern sits just under the bar on `syn_ui` (720 palette bytes of 1410, ratio 1.0475). Hypothesis: WebP's colour cache and colour-indexing coding avoid paying 3 raw bytes per colour. §2 (a colour-cache model in version 3) is the CM-side answer to that hypothesis and must be measured on these many-colour screen images, not only on the set average; if §2 and §3 both land and the palette bytes still dominate, coding the palette itself (delta or entropy coding of the triples) is the follow-up experiment, filed when those results are in. `syn_noise` (ratio 0.9998) and the remaining `synth` images are at or below the 1.1 bar and need no target; `syn_solid` (61 vs 56, 1.0893) still loses to WebP outright and passes only through the bar and the gate's 32-byte allowance.

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

**Result (`D02 T02 §1`, 2026-11-05): rejected.** Measured via `research/strips.py` across the current 7-model + learned gated modular prediction `src/cm.c` (`m5`, `photo=30,795,260`, `tp=6,640,275`, `synth=201,634`), the pre-gated-gradient 7-model `cm_m4.c` (`fb27c317d4cae74e7d5aadd38789e3fbc68fc80c` / `0c1ec08ab66b2eca8d8ac66a1bce83c9e25cffe8`, `photo=30,812,732`, `tp=6,641,390`, `synth=206,229`), and the 5-model `cm_m1.c` (`b181fb1edef63b42d29350a8b24096307a8596ee`, `photo=30,865,211`, `tp=6,651,583`, `synth=206,566`). At `S = 4`, single-container payload-adjusted size (`adj_sum = raw_sum - (S - 1) * 32 + 1` per CM-coded image) grows by `+0.976%` (`raw_sum` `+0.988%`) on `photo` (`30,795,260 -> 31,095,902 / 31,099,607`, geomean `1.012037 / 1.012229`, worst `kodim07` `+1.912% / +1.938%`) and `+1.130%` (`raw_sum` `+1.145%`) on held-out `tp` (`6,640,275 -> 6,715,331 / 6,716,281`, geomean `1.012874 / 1.013052`, worst `kodim05` `+1.673% / +1.693%`) on `m5` (`+0.980% / +0.992%` `photo` and `+1.145% / +1.160%` `tp` on `m4`; `+0.885% / +0.897%` `photo` and `+1.025% / +1.039%` `tp` on `m1`), exceeding the `<= +0.300%` bar by 3.25x to 3.82x on the 7-model baselines (`m5` and `m4`) and by 2.95x to 3.42x on the 5-model baseline (`m1`). Even `S = 2` costs `+0.389% / +0.392%` on `photo` and `+0.426% / +0.431%` on `tp` (`m5`), and on the 24 `photo` images with `H <= 768` (`9,950,590` bytes on `m5`), `S = 4` costs `+1.462% / +1.485%` while the remaining 15 larger images (`H > 768`, `20,844,670` bytes) still pay `+155,199` bytes (`+0.745%` of `H > 768`, or `+0.504%` of all `photo` even if `H <= 768` stays at `S = 1`). `D01 T03 §2` therefore takes its rejection path.

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

### 5.8 Warm-started horizontal strips, 16-node context rows, and SIMD counter updates

**Owner:** `D02 T02 §3` and `D01 T03 §3`.

Cold-start independent horizontal strips (`D02 T02 §1`, section 5.3) cost `+0.389%` on `photo` at `S = 2` (`8 of 39` photographs above `+0.5%`, worst `kodim07` `+0.823%`) and `+0.976%` at `S = 4` (`35 of 39` above `+0.5%`), missing the `<= +0.300%` strip size bar because every strip `s >= 1` starts with cold `ctr` and `wt` tables and re-learns local edge, gradient, and mixer statistics from scratch. Three changes eliminate the cold-start tax while cutting per-strip CM model-state memory (`-63.5%` per strip, so 3 concurrent strips fit within `+4.01%` total codec working memory `alloc_peak - file_size` on `measure/image.bmp`) and per-decision CPU time enough for multi-strip decode to beat lossless WebP on the 12-thread development machine:

1. **Per-channel warm-started horizontal strips (`S = 1` for `h < 512`, `S = 2` for `512 <= h < 768`, `S = 3` for `h >= 768`, balanced cascaded split with `w_rows = (S == 2 ? 68 : max(64, h / 10))` and `warm_rows = (S == 2 ? 68 : max(96, h / 6))`):** strip `s` (`0 <= s < S`) codes its slice of rows with reset neighbour state at its top boundary (`y_local = y - y_s`). As soon as channel `c` of strip `s` (`s + 1 < S`) finishes row `warm_rows - 1`, it copies its adapted `(ctr, wt, lms, eg, et)` state into channel `c` of strip `s + 1` with context-row visit counts `ctr[r * NODES]` capped at `CAP_VISIT = 6` (`RATE[6] = 4369`) via `_mm_min_epu16` and `lms` halved (`lms[k] >> 1`), publishes `warm_ready = 1`, and wakes channel `c` of strip `s + 1` from OS thread suspension (`rf_thread_resume`), keeping waiting strips at 0% CPU until their warm state is ready. Giving strip `s` height `q + (s < r_rem ? 1 : 0) + (S - 1 - s) * w_rows` (where `rem = h - (S * (S - 1) / 2) * w_rows`, falling back to `floor(s * h / S)..floor((s + 1) * h / S)` when `S == 1 || h <= total_extra + S * 32`) balances the remaining row work across all `S` strips once cascaded warm-up completes.
2. **32-byte context rows (`NODES = 16`), hoisted mantissa decisions, exact reachable model row counts, and single-block strip allocation:** pooling rare large-residual decision nodes in `code_residual` (unary exponent `3 + min(k, 5)` for `k = 0..6`, first adaptive mantissa bit `8 + min(e, 4)` for `e = 1..7`, and second adaptive mantissa bit `e == 2 ? 13 + (got & 1) : 15` for `e = 2..7`, with the two adaptive mantissa decisions hoisted before the raw-bit loop `for (int b = e - 3; b >= 0; b--)`) fits every context row into `NODES = 16` `uint16_t` entries (`32` bytes, two rows per 64-byte cache line with zero cache-line straddling) and eliminates loop unrolling of `code_bit7` across the 14 raw-bit iterations. Sizing direct-indexed models to their exact reachable ranges (`MROWS_C[3][7]`: `{2048, 16, 729, 2048, 1296, 1296, 2048}` = `9,481` rows on channel 0, `{2048, 144, 2048, 2048, 1296, 1296, 2048}` = `10,928` rows on channel 1, and `{2048, 1296, 2048, 2048, 1296, 1296, 2048}` = `12,080` rows on channel 2, `32,489` total rows vs `43,008`), sizing `WSEL_C = {32, 96, 96}`, packing `pos_t` to `16` bytes (`uint16_t perr[4]; int16_t err, fe; int8_t er, ex;`), reducing `XROWS` from `32` to `4`, and allocating `cm_t[S]` plus all `S` strip buffers in a single `rf_alloc` call (`cm_alloc_strips`) shrinks per-strip CM model-state allocation on `measure/image.bmp` (`1672x941`, `S = 3`, where `alloc_peak - file_size` also includes the `4,720,104`-byte output BMP buffer) from `3,818,350` bytes (`1` strip in Milestone 5) to `1,386,901` bytes per strip (`4,160,654` bytes across all `S = 3` concurrent strips, `-63.7%` per strip and `+9.0%` across 3 strips, so total codec working memory `alloc_peak - file_size` is `8,880,886` bytes at `--threads 9`, `+4.01% <= +5.00%` over Milestone 5's `8,538,454` bytes).
3. **Branchless 16-bit SIMD counter update and 64-bit-clamped mixer dot product in `code_bit7`:** storing `uint16_t RATE16[61]` and 16-byte aligned `uint16_t rate[8]` in `stage_t` allows `code_bit7` to update all 7 16-bit counters in parallel with `_mm_mulhi_epu16` and `_mm_mullo_epi16` (`xv = cv ^ mask`, `hi = mulhi_epu16(xv, rv)`, `adj = cmpeq_epi16(mullo_epi16(xv, rv), 0) & not_mask`, `uv16 = (cv + (hi ^ not_mask)) - adj`, verified bit-identical to `v + (((target - (int64_t)v) * (int64_t)rate) >> 16)` across all `65536 * 61 * 2` inputs), while retaining 64-bit `int64_t dot` accumulation and clamping `dot >> 16` to `[-2047, 2047]` in 64-bit before casting to `int` so full `int32_t` mixer weights never wrap on 32-bit overflow.

**Result (`D02 T02 §3` and `D01 T03 §3`, 2026-11-06): accepted and ported into format version 3.** Exhaustive SIMD counter-update and 64-bit mixer dot equivalence (`test_simd_eq`: `7,995,392` counter cases, `0 mismatches`; `1,000,008` full-`int32_t` dot/weight-update cases including `w = 200000, s = 2047` and `INT32_MIN`/`INT32_MAX`, `0 mismatches`, verified in both native `-march=x86-64-v2` and `-DRF_PORTABLE_SIMD -fsanitize=undefined -fsanitize-trap=undefined` builds) and failure/early-abort injection (`test_strip_fail`: `28` thread-creation failure injection masks across the 9-thread path, 2-thread chain fallback, and `run_one_strip` fallback, `33` early-abort/header cases, `live_threads=0`, `live_alloc=0`, verified in both native and `-DRF_PORTABLE_SIMD -fsanitize=undefined -fsanitize-trap=undefined` builds, and carried into `tests/test_artifacts.py`'s `test_cm_thread_failure_injection_and_portable_simd`) pass. Across the corpus (`python3 research/ab.py size src/cm.c <set> -v`), `photo` (`n = 39`) is `30,868,254` bytes (`+72,994` bytes = `+0.237% <= +0.300%` over Milestone 5's `30,795,260`, geomean `1.002863`, `0 of 39` images above `+0.5%`, worst `kodim24` `434,031 -> 435,918` = `+0.435%`, best `ic_artificial` `105,420 -> 105,266` = `-0.146%`), held-out `tp` (`n = 10`) is `6,656,840` bytes (`+16,565` bytes = `+0.249% <= +0.300%` over Milestone 5's `6,640,275`, geomean `1.002895`, `0 of 10` images above `+0.5%`, worst `tp_flower_foveon_crop` `516,250 -> 518,444` = `+0.425%`, best `tp_deer_crop` `1,497,468 -> 1,499,031` = `+0.104%`), `tp-only` (`n = 5`) is `4,476,915` bytes (`+9,683` bytes = `+0.217%`), and `synth` (`n = 11`) is `201,638` bytes (`+4` bytes = `+0.002%`, with `syn_gradient=120 < 616` e9, `syn_stripes=109 < 271` e9, `syn_ui=1410 < 2195` e9, `syn_text=1447 < 2038` e9). All 55 corpus images produce byte-identical `.fold` files across `--threads 1, 3, 6, 9` and round-trip losslessly, and all 55 `fold-v2` files decode pixel-identically across `--threads 1, 3, 6, 9`. On the shipped production binaries (`bin/windows-x64/bin/rasterfold.exe` and `bin/linux-x64/bin/rasterfold`), `measure/image.bmp` (`1672x941`, `S = 3`, `.fold` size `1,410,485` bytes) `decode --time` records `alloc_peak=10,291,243` (`codec_mem=8,880,758`, `codec_us=476,355`, `total_us=531,680`, `peak_ws=16,252,928`) at `-t 1`, `alloc_peak=10,291,275` (`codec_mem=8,880,790`, `codec_us=213,418`, `total_us=267,783`, `peak_ws=16,343,040`) at `-t 3`, `alloc_peak=10,291,275` (`codec_mem=8,880,790`, `codec_us=192,252`, `total_us=246,733`, `peak_ws=16,338,944`) at `-t 6`, and `alloc_peak=10,291,371` (`codec_mem=8,880,886 <= 8,965,376`, `+4.01% <= +5.00%`, `codec_us=122,737`, `total_us=173,838`, `peak_ws=16,371,712`) at `-t 9` on `windows-x64`, and `alloc_peak=10,291,243` (`codec_mem=8,880,758`, `codec_us=472,360`, `total_us=492,067`, `peak_ws=12,234,752`) at `-t 1`, `alloc_peak=10,291,483` (`codec_mem=8,880,998`, `codec_us=178,088`, `total_us=187,693`, `peak_ws=12,365,824`) at `-t 3`, `alloc_peak=10,291,483` (`codec_mem=8,880,998`, `codec_us=172,964`, `total_us=183,090`, `peak_ws=12,365,824`) at `-t 6`, and `alloc_peak=10,292,203` (`codec_mem=8,881,718 <= 8,965,376`, `+4.02% <= +5.00%`, `codec_us=146,561`, `total_us=154,676`, `peak_ws=12,361,728`) at `-t 9` on `linux-x64`. On `testdata/photos/` (`n = 10`, alternating `repeats = 3`), the shipped production `windows-x64` CLI decodes all 10 photos in `1,285,847 us` (`1.29 s`) vs lossless WebP's `1,440,165 us` (`1.44 s`, `ratio=0.893`, `geomean=0.927`) at `--threads 9` and `1,270,966 us` (`1.27 s`) vs WebP's `1,453,416 us` (`1.45 s`, `ratio=0.874`, `geomean=0.926`) at default threads, and the shipped production `linux-x64` CLI decodes in `679,606 us` (`0.68 s`) vs WebP's `1,639,217 us` (`1.64 s`, `ratio=0.415`, `geomean=0.415`) at `--threads 9` and `683,392 us` (`0.68 s`) vs WebP's `1,620,566 us` (`1.62 s`, `ratio=0.422`, `geomean=0.419`) at default threads. Alternating `ab.py ab -v build/research/cm_m5.c src/cm.c <set> -n5 -t<1|3|6|9>` on the shipped `src/cm.c` records `measure` `-t1` `enc 0.915 dec 0.916`, `-t3` `enc 1.099 dec 0.970`, `-t6` `enc 1.070 dec 1.025`, `-t9` `enc 0.875 dec 0.729`; `synth` `-t1` `min_tot dec=0.936 geomean=0.944 enc=0.889 geomean=0.841`, `-t3` `min_tot dec=0.961 geomean=0.936 enc=0.910 geomean=0.758`, `-t6` `min_tot dec=0.961 geomean=0.948 enc=0.961 geomean=0.811`, `-t9` `min_tot dec=0.982 geomean=0.948 enc=0.943 geomean=0.815`; `tp-only` `-t1` `min_tot dec=0.916 geomean=0.919 enc=0.906 geomean=0.910`, `-t3` `min_tot dec=0.990 geomean=0.992 enc=0.977 geomean=0.982`, `-t6` `min_tot dec=0.988 geomean=0.994 enc=0.970 geomean=0.976`, `-t9` `min_tot dec=0.772 geomean=0.777 enc=0.796 geomean=0.808`; `tp` `-t1` `min_tot dec=0.893 geomean=0.895 enc=0.911 geomean=0.912`, `-t3` `min_tot dec=0.978 geomean=0.979 enc=1.010 geomean=1.020`, `-t6` `min_tot dec=0.869 geomean=0.802 enc=0.906 geomean=0.867`, `-t9` `min_tot dec=0.716 geomean=0.711 enc=0.814 geomean=0.823`; and `photo` `-t1` `min_tot dec=0.904 geomean=0.908 enc=0.911 geomean=0.914`, `-t3` `min_tot dec=0.916 geomean=0.956 enc=0.972 geomean=0.999`, `-t6` `min_tot dec=0.846 geomean=0.792 enc=0.911 geomean=0.863`, `-t9` `min_tot dec=0.617 (2544.7/4121.3ms) geomean=0.649 enc=0.731 (2713.0/3710.4ms) geomean=0.751`.

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
   match model do, for shifted copies of earlier rows. Measured 2026-10-27
   (`D01 T04 §1`, `python3 research/repeats.py synth` and `photo` on raw RGB
   planes; shifted-N is the per-channel best share of samples equal to the
   sample one row above at shifts -8..8, pooled over channels; match>=8 is
   retrospective coverage by earlier-row matches of 8 or more samples):
   `syn_ui` 97.67% / 99.46%, `syn_text` 97.22% / 99.30%, `syn_gradient` 66.67%
   (R 100% at shift 0, G 0%, B 100% at shift +2) / 82.25%, `syn_stripes`
   33.33% / 52.65%, `syn_noise` 0.43% / 0.00%. Photographs hold far less:
   pooled shifted-N shares of 5.33% to 21.43% and match>=8 coverage of 0.17%
   to 11.93% across 23 of the 24 Kodak images (`kodim20`, with its flat sky
   backdrop, reaches 39.89% / 34.39%), with flat or synthetic-like `photo`
   outliers (`ic_artificial` 92.70% / 96.76%, `ic_nightshot_iso_100` 38.90% /
   53.75%, `ic_flower_foveon` 38.66% / 46.58%, `measure` 32.87% / 47.36%).
   Exact repeats are a generated-content phenomenon; on photographs the match
   model can only be a small win, which is what `D01 T04 §2`'s bar (photo total
   no more than 0.05% worse) guards. Measured 2026-10-30 (`D01 T04 §2`, section
   3.2): the eighth-model match model gained 0.149% on `photo` but missed the
   `syn_gradient` and `syn_stripes` bars and doubled encode time, so it was
   rejected.

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

Each milestone keeps everything the one before it reached and adds one measurable step. Milestones 2 to 4 held
the original rule: no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak
memory (Milestone 4 under its 7-model Format Version 3 envelope, `D99 T01 §4`), and every older file still decoding.
On 2026-10-30 the operator replaced it (`D99 T01 §4`): Milestone 5 carries its own envelope for learned gated modular
prediction, and from milestone 6 the rule is a cost envelope: the `photo` and `tp` totals no more than 0.1% larger and
no photograph more than 0.5% larger; encode and decode time at most 1.10x on the `measure`, `photo`, `tp`, and
`tp-only` totals and 1.20x on the `synth` total, and at most 1.5x on any image of at least 16,384 pixels (smaller
images at most 1 ms slower), from pooled alternating runs whose disjoint pools agree; codec working memory (decode
`alloc_peak` less the input file) at most 5% higher; and every older file still decoding. Milestones 1 to 10 are measured against JPEG XL and WebP; from
milestone 11 RasterFold is measured against its own previous release. Targets past milestone 10 are estimates:
each is re-derived from the previous milestone's measured results when its planning section runs. The gates live
in `todo/05-milestones/`; this table is generated from `research/milestones.py`.

### Milestones 1 to 10: Beat JPEG XL e9 and WebP, then Release 1.0

| # | milestone | target | gate |
|---|---|---|---|
| 1 | Locked baseline | `pwsh scripts/check-all.ps1` green with every gate `PASS`; the `plan-gates` and `build` workflows green on the last push; `benchmarks/results.md` and `benchmarks/corpus.csv` report JPEG XL e7, JPEG XL e9, and lossless WebP; the HTML docs build and `python scripts/drift-check.py` exits 0. Baseline on 2026-09-29: photos 0.971 of JPEG XL e7 and 0.983 of e9, 28 of 39 photos below e9 | `D05 T01 §1` (reached 2026-10-25) |
| 2 | Same bytes, leaner and harder | every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second | `D05 T01 §2` (reached 2026-10-25) |
| 3 | Few-colour images solved | method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean | `D05 T01 §3` (reached 2026-10-26) |
| 4 | Format version 3: beat JPEG XL e9 on photographs | format version 3 with the accepted model changes (`D01 T02 §1`, `D01 T02 §7`, `D01 T03 §1`; operator decision 2026-10-29 under `D99 T01 §4` after `D01 T02 §2` to `§6` missed): photo total at most 0.982 of JPEG XL e9 (and at most 0.970 of JPEG XL e7), test pack at most 0.986 of JPEG XL e9 (and at most 0.973 of e7), at least 31 of 39 photos below e9 (and 38 of 39 at or below e7), three-thread decode at most 0.6 of JPEG XL's, 7-model decode `alloc_peak` at most 10.0 MB (+786 KB for the `k5`/`k6` counter tables over Milestone 3's 5-model 9.16 MB), and 7-model encode/decode time at most 1.27x Milestone 3's 5-model baseline on every set total and every image (images under 16K pixels allowed a fixed +1 ms instead of the ratio) | `D05 T01 §4` (reached 2026-10-29) |
| 5 | Exact repeats | learned gated modular prediction in version 3 (`D01 T04 §8`'s variant v2, ported in `D01 T04 §10`; operator decision 2026-10-30 under `D99 T01 §4` after `D01 T04 §2` to `§9` missed): `syn_gradient`, `syn_stripes`, `syn_ui`, and `syn_text` below JPEG XL e9 (616, 271, 2195, and 2038 bytes); the photo total no larger than milestone 4's and no photograph more than 0.1% larger | `D05 T01 §5` (reached 2026-11-05) |
| 6 | Faster than WebP to decode | horizontal strips in version 3: photo decode wall time below lossless WebP's on the 12-thread development machine; strip size cost at most 0.3% | `D05 T01 §6` (reached 2026-11-06) |
| 7 | Photographs beat JPEG XL e9 | at least 34 of 39 photos and 8 of 10 test-pack photos below JPEG XL e9, and no photo or test-pack photo more than 1.0% larger than e9 (milestone 4: 31 of 39, 7 of 10, worst `kodim16` 1.52%); encode at most 0.3 of JPEG XL e7's time (`python research/vsjxl.py -n5`) | `D05 T01 §7` (reached 2026-11-07) |
| 8 | Beat WebP on generated images | the generated set's total and geometric mean below lossless WebP's, and every generated image within 1.1 times WebP's size or at most 32 bytes larger, whichever allows more | `D05 T01 §8` |
| 9 | Holds on images never tuned on | the set-total and geometric-mean ratios that milestones 4, 7, and 8 reached against JPEG XL e9 and lossless WebP hold on the held-out set (`python research/ab.py size heldout`) within 1% of each tuned figure, without any retuning; the per-image counts are reported beside them, not gated | `D05 T01 §9` |
| 10 | RasterFold 1.0 | `rasterfold-v1.0.0` released: format version 3 frozen; Windows/Linux x64 CLI and shared/static C libraries; BMP, PNG, and PPM in the CLI; the full HTML docs published; 100,000 fuzz mutations clean; smaller than JPEG XL e9 and lossless WebP on every set total, and faster and leaner than JPEG XL e7 on the `tests/benchmark.py` set totals for encode, decode, and peak memory, each codec at its default thread count | `D05 T01 §10` |

### Milestones 11 to 20: RasterFold Beats RasterFold (2.0)

| # | milestone | target | gate |
|---|---|---|---|
| 11 | SIMD decisions | one-thread decode at least 10% faster than 1.0, bytes within 0.1% | `D05 T02 §2` |
| 12 | Two-layer mixing | photo total at least 0.4% smaller than milestone 11 | `D05 T02 §4` |
| 13 | Stronger prediction | photo total at least 0.5% smaller than milestone 12 | `D05 T02 §6` |
| 14 | Adaptive colour transform | photo total at least 0.3% smaller than milestone 13, decode time within the no-regression envelope | `D05 T02 §8` |
| 15 | Effort levels | a fast level decoding at least twice as fast as the default and still smaller than JPEG XL e7; the default unchanged | `D05 T02 §10` |
| 16 | Max-effort encoder search | the max level at least 1% smaller than the default, its decode time within the no-regression envelope of the default's | `D05 T02 §12` |
| 17 | Trained starting state | images of Kodak size and below at least 0.5% smaller, confirmed on the held-out set | `D05 T02 §14` |
| 18 | Streaming | peak memory on a 24-megapixel image at most one third of milestone 17's | `D05 T02 §16` |
| 19 | Many-core scaling | decode throughput on 16 or more threads at least twice milestone 18's, size within 0.2% | `D05 T02 §18` |
| 20 | RasterFold 2.0 | `rasterfold-v2.0.0`: against 1.0, the default at least 1.5% smaller, the max level at least 2.5% smaller, one-thread decode at least 1.2 times faster, memory at most one third | `D05 T02 §20` |

### Milestones 21 to 30: Reach (3.0)

| # | milestone | target | gate |
|---|---|---|---|
| 21 | Run mode for flat regions | generated images decode at least twice as fast and photos at least 5% faster, sizes no worse | `D05 T03 §2` |
| 22 | Verified hashed context tables | decode at least 10% faster with codec memory within the no-regression envelope, size within 0.1% | `D05 T03 §4` |
| 23 | Higher-precision coder | generated-image geometric mean at least 10% smaller, photos unchanged | `D05 T03 §6` |
| 24 | Learned predictor at the max level | the max level at least 1% smaller on photos, fast and default byte-identical, confirmed on the held-out set | `D05 T03 §8` |
| 25 | Texture-gated models | the default level at least 0.4% smaller, decode time within the no-regression envelope | `D05 T03 §10` |
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
