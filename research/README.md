# Research tools

Tools for changing the codec with numbers instead of guesses. Data goes to
`build/research/` (ignored by git); the tools live here.

| command | what it does |
|---|---|
| `python research/corpus.py` | builds `build/research/ppm/` (55 images: measure, 24 Kodak, 14 imagecompression.info crops, 5 test-pack crops, 11 generated) and `jxl_refs.json` (JPEG XL e7 sizes) |
| `python research/ab.py size [file.c] SET [-v]` | exact `.fold` sizes of a `cm.c` variant on a set, against JPEG XL: total, geometric mean, worst image |
| `python research/ab.py bench [file.c] [-r3] [-t1\|-t3] IMAGES` | encode and decode time of a `cm.c` variant, verified round trip |
| `python research/ab.py ab [-v] A.c B.c [-n5] [-t1] IMAGES` | alternates two variants and compares their minimum times (with `-v`, also reports set `min_tot`, per-image `geomean`, and `worst`) |
| `python research/prof.py [N]` | line-level profile from `bench.exe -p` samples |
| `python research/vsjxl.py [image.bmp] [-n5]` | process wall times of `bin/windows-x64/bin/rasterfold.exe` against ffmpeg libjxl |

Sets for `ab.py`: `sub` (8 mixed photos), `kodak`, `ic`, `tp` (test pack),
`tp-only` (5 exclusive test-pack crops), `photo` (measure + kodak + ic), `synth`,
`all`. A name without a set is one
image from `build/research/ppm`.

`ab.py` compiles `bench.c` + `benchsys.c` + the given `cm.c` with the local
Clang. Without a file argument it uses `src/cm.c`. `bench.c` times with thread
cycle counters at high priority for one thread (`-t1`), and with the wall clock
for three threads (`-t3`), so a busy machine distorts the numbers less. Still,
compare variants with `ab`, which alternates them, rather than across separate
runs.

To try a model change: copy `src/cm.c` to `build/research/x.c`, edit, run
`python research/ab.py size build/research/x.c photo` and
`python research/ab.py ab src/cm.c build/research/x.c -n5 measure kodim01`.
A change that alters the bitstream must also change `tests/rfi_ref.py` and
`spec/FOLD.md`, and the known-answer file in `tests/test_correctness.py`.
