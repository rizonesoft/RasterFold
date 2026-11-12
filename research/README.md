# Research tools

Tools for changing the codec with numbers instead of guesses. Data goes to
`build/research/` (ignored by git); the tools live here.

| command | what it does |
|---|---|
| `python research/corpus.py` | builds `build/research/ppm/` (95 images: measure, 24 Kodak, 14 imagecompression.info crops, 5 test-pack crops, 11 generated, and the 40-image held-out set of 30 Wikimedia Commons photographs and 10 locally rendered screenshots frozen in `research/heldout.json` (screenshot recovery restores the committed frozen PNGs in `research/heldout-assets/`; rendering for authoring downloads the pinned Chrome for Testing build recorded in the manifest and verifies its version); `--verify-heldout` checks membership, dimensions, and hashes without regenerating) and `jxl_refs.json` (JPEG XL e7 and e9 and lossless WebP sizes) |
| `python research/ab.py size [file.c] SET [-v]` | exact `.fold` sizes of a `cm.c` or `pal.c` variant on a set, against JPEG XL and WebP: total, geometric mean, worst image |
| `python research/ab.py bench [file.c] [-r3] [-t1\|-t3] IMAGES` | encode and decode time of a `cm.c` or `pal.c` variant through `rf_encode`/`rf_decode`, verified round trip (`bench.exe --breakdown` also attributes CM bytes per channel `G`, `R-G`, `B-G` and decision kind `zero`, `sign`, `exp`, `mant`) |
| `python research/ab.py ab [-v] A.c B.c [-n5] [-t1] IMAGES` | alternates two variants and compares their minimum times (with `-v`, also reports set `min_tot`, per-image `geomean`, and `worst`) |
| `python research/abpool.py run <jsonl> <base_cm> <cand_cm> -t<N> <rounds> SET` | appends SHA-256-tagged alternating A/B runs to `<jsonl>`, and `abpool.py report <jsonl> [lo:hi]` evaluates pooled per-image minima against the set-total and per-image bars |
| `python research/ab.py compat [REF] [--allow-diff SET]` | builds baseline `REF` (`build/research/vs/rasterfold-m1.exe`, `build/research/cm_m1.c`, and `build/research/bench_m1.exe`) and verifies 55-image `.fold` byte identity (except any permitted `--allow-diff SET`), `bench_m1` size identity, and baseline/candidate decode at `-t1` and `-t3` |
| `python research/prof.py [N]` | line-level profile from `bench.exe -p` samples |
| `python research/vsjxl.py [image.bmp] [-n5]` | process wall times of `bin/windows-x64/bin/rasterfold.exe` against ffmpeg libjxl (`--webp-photos` times `testdata/photos/` decode against lossless WebP; `--threads {1,3,6,9}` sets the thread count) |

Sets for `ab.py`: `sub` (8 mixed photos), `kodak`, `ic`, `tp` (test pack), `heldout` (the 40 held-out images; never used to choose a parameter),
`tp-only` (5 exclusive test-pack crops), `photo` (measure + kodak + ic), `synth`,
`all`. A name without a set is one
image from `build/research/ppm`.

`ab.py` compiles `bench.c` + `benchsys.c` + `src/fold.c` + `src/cm.c` + `src/pal.c` + `src/bmp.c` + `src/crc32.c` (replacing `src/cm.c` or `src/pal.c` when a variant `.c` file is given, or linking against `build/research/m1_src/` when compiling `build/research/cm_m1.c`) with the local
Clang. Without a file argument it uses `src/cm.c` and `src/pal.c`. `bench.c` times `rf_encode` and `rf_decode` with thread
cycle counters at high priority for one thread (`-t1`), and with the wall clock
for three threads (`-t3`), so a busy machine distorts the numbers less. Still,
compare variants with `ab`, which alternates them, rather than across separate
runs.

Three things keep a timing comparison honest (each found on `syn_checker` at the Milestone 8 gate, 2026-11-12). `ab` and `abpool.py` build both variants without the byte-breakdown instrumentation (`-DRF_NO_BREAKDOWN=1`), because a historical baseline is always built that way and the instrumented harness is a larger executable whose cold three-thread encode of a small image is about 0.8 ms slower; only `bench` keeps the instrumentation, for `--breakdown`. `bench.c` prints times to a microsecond, because at 0.1 ms a 0.14 ms and a 0.16 ms encode print as 0.1 and 0.2 and read as a ratio of 2. And one process times every image it is given in order, so a one-time cost (the CM coder builds its tables on first use) lands on whichever image first reaches it: when a change makes an earlier image skip the CM attempt, the next image's row grows by that cost while the set total does not. Confirm a single-image outlier by timing that image alone (`ab ... -n7 <image>`).

To try a model change: copy `src/cm.c` to `build/research/x.c`, edit, run
`python research/ab.py size build/research/x.c photo` and
`python research/ab.py ab src/cm.c build/research/x.c -n5 measure kodim01`.
A change that alters the bitstream must also change `tests/rfi_ref.py` and
`spec/FOLD.md`, and the known-answer file in `tests/test_correctness.py`.
