# Research tools

Tools for changing the codec with numbers instead of guesses. Data goes to
`build/research/` (ignored by git); the tools live here.

| command | what it does |
|---|---|
| `python research/corpus.py` | builds `build/research/ppm/` (55 images: measure, 24 Kodak, 14 imagecompression.info crops, 5 test-pack crops, 11 generated) and `jxl_refs.json` (JPEG XL e7 sizes) |
| `python research/ab.py size [file.c] SET [-v]` | exact `.fold` sizes of a `cm.c` or `pal.c` variant on a set, against JPEG XL and WebP: total, geometric mean, worst image |
| `python research/ab.py bench [file.c] [-r3] [-t1\|-t3] IMAGES` | encode and decode time of a `cm.c` or `pal.c` variant through `rf_encode`/`rf_decode`, verified round trip |
| `python research/ab.py ab [-v] A.c B.c [-n5] [-t1] IMAGES` | alternates two variants and compares their minimum times (with `-v`, also reports set `min_tot`, per-image `geomean`, and `worst`) |
| `python research/abpool.py run <jsonl> <base_cm> <cand_cm> -t<N> <rounds> SET` | appends SHA-256-tagged alternating A/B runs to `<jsonl>`, and `abpool.py report <jsonl> [lo:hi]` evaluates pooled per-image minima against the set-total and per-image bars |
| `python research/ab.py compat [REF] [--allow-diff SET]` | builds baseline `REF` (`build/research/vs/rasterfold-m1.exe`, `build/research/cm_m1.c`, and `build/research/bench_m1.exe`) and verifies 55-image `.fold` byte identity (except any permitted `--allow-diff SET`), `bench_m1` size identity, and baseline/candidate decode at `-t1` and `-t3` |
| `python research/prof.py [N]` | line-level profile from `bench.exe -p` samples |
| `python research/vsjxl.py [image.bmp] [-n5]` | process wall times of `bin/windows-x64/bin/rasterfold.exe` against ffmpeg libjxl |

Sets for `ab.py`: `sub` (8 mixed photos), `kodak`, `ic`, `tp` (test pack),
`tp-only` (5 exclusive test-pack crops), `photo` (measure + kodak + ic), `synth`,
`all`. A name without a set is one
image from `build/research/ppm`.

`ab.py` compiles `bench.c` + `benchsys.c` + `src/fold.c` + `src/cm.c` + `src/pal.c` + `src/bmp.c` + `src/crc32.c` (replacing `src/cm.c` or `src/pal.c` when a variant `.c` file is given, or linking against `build/research/m1_src/` when compiling `build/research/cm_m1.c`) with the local
Clang. Without a file argument it uses `src/cm.c` and `src/pal.c`. `bench.c` times `rf_encode` and `rf_decode` with thread
cycle counters at high priority for one thread (`-t1`), and with the wall clock
for three threads (`-t3`), so a busy machine distorts the numbers less. Still,
compare variants with `ab`, which alternates them, rather than across separate
runs.

To try a model change: copy `src/cm.c` to `build/research/x.c`, edit, run
`python research/ab.py size build/research/x.c photo` and
`python research/ab.py ab src/cm.c build/research/x.c -n5 measure kodim01`.
A change that alters the bitstream must also change `tests/rfi_ref.py` and
`spec/FOLD.md`, and the known-answer file in `tests/test_correctness.py`.
