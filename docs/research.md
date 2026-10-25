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
format is `.fold` version 2. Method 0 is raw RGB and method 1 ("CM") is the
context-mixing coder in `src/cm.c`: the JPEG XL weighted predictor corrected by
a 12-tap sign-sign LMS stage, five context models mixed by a logistic mixer,
and one binary range coder per channel (G, R-G, B-G), with one thread per
channel. The Python oracle `tests/rfi_ref.py` implements the same format
independently and stays byte-identical with the C encoder.

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
| synth | 11 | 0.9480 | 1.1986 | syn_checker 18.07 |

Only kodim17 (+0.37%) and kodim09 (+0.19%) are larger than JPEG XL among the
photographs. On the generated images, stripes, solid fills, UI, text, diagram,
and noise are smaller. `syn_checker` (16-pixel cells) is 18.1 times larger,
`syn_pixel` (1-pixel checker) 9.1 times, `syn_gradient` 1.57 times, and
`syn_odd` (a 65x63 gradient) 1.23 times. JPEG XL has a palette transform for the
checkers; the palette design (4.1) addresses them. Lossless WebP is smaller than
both codecs on most generated images, and on `syn_gradient` it takes 180 bytes
where RasterFold takes 4,521 (its backward references copy shifted rows; the
match model of `D01 T04` answers it).

Speed: `research/bench.c` measures about 440 CPU cycles per sample on one
thread (G, R, and B each count as one sample) on the i5-11600K, and about 190
wall-clock cycles per sample with three threads, one per channel. Profile the
current split between modelling and decisions (2.4, item 3) before choosing a
speed idea.

Memory, from `rasterfold.exe decode --time` on `measure/image.bmp`:
`alloc_peak` is 10.57 MB: the input file (1.41 MB), the RGB image (4.72 MB),
the three channel models (2.95 MB), and the decoder's padded stream copy
(1.41 MB). The later RGB-to-BMP step holds two 4.72 MB images. The process peak
working set is 17.3 MB. Encode `alloc_peak` is 17.19 MB: RGB (4.72), the output
file buffer (4.72), three stream buffers (4.72), and the models (2.95).

**Milestone 1 (`D05 T01 §1`, Locked baseline) reached 2026-10-25:** `pwsh scripts/check-all.ps1 -AllTargets` exits 0 (`check-all: 29/29 passed`); `build.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40195950978`), `plan.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40195950906`), and `pages.yml` (`https://github.com/rizonesoft/RasterFold/actions/runs/40195950947`) pass on `main` (`91b43d439f0c19b4a42d0ac9fe62a63d4f125ba4`); `benchmarks/corpus.csv` and `benchmarks/results.md` report JPEG XL e7, JPEG XL e9, and lossless WebP (`photo` 39 images: `30,865,211` bytes, `0.9712` total and `0.9728` geomean of e7, `0.9829` total and `0.9883` geomean of e9 with `28 of 39` photos below e9, `0.9041` total and `0.8932` geomean of WebP; `tp` 10 images: `6,651,583` bytes, `0.9743` total and `0.9722` geomean of e7, `0.9868` total and `0.9862` geomean of e9, `0.9027` total and `0.8811` geomean of WebP; `synth` 11 images: `214,773` bytes, `0.9480` total and `1.1986` geomean of e7, `0.9630` total and `2.2860` geomean of e9, `1.0672` total and `3.6626` geomean of WebP); `python3 scripts/windows.py tests/measure_final.py` exits 0 (`PASS`, `bytes=1409028`, `decode_wall_us=295854`, `decode_peak_ws=16576512`, `decode_codec_us=183840` vs JPEG XL e7 `bytes=1470703`, `decode_wall_us=459807`, `decode_peak_ws=51281920`); `tests/benchmark.py measure -n 3` (committed in `benchmarks/results.csv` and `benchmarks/results.md`) exits 0 (`failures: 0`, `rasterfold` `1,409,028` bytes, encode `308,760` us / `0.194x` `jxl`, decode `319,315` us / `0.647x` `jxl`, `encode_peak=16699392`, `decode_peak=16580608`, `rasterfold-1t` encode `640,604` us / `0.402x` `jxl`, decode `601,611` us / `1.220x` `jxl`, `encode_peak=16625664`, `decode_peak=16461824`, `jxl` `1,470,703` bytes, encode `1,595,197` us, decode `493,267` us, `jxl-1t` decode `721,246` us, `jxl-e9` `1,440,564` bytes, encode `10,492,083` us, decode `491,123` us, `webp` `1,588,708` bytes, encode `23,548,198` us, decode `219,895` us); `bin/windows-x64/bin/rasterfold.exe decode --time` on `measure/image.bmp` records `alloc_peak=10569996` (`10.57 MB`, `codec_us=183820`, `total_us=240767`, `peak_working_set=16576512`) at 3 threads and `alloc_peak=10569964` (`10.57 MB`, `codec_us=488047`, `total_us=557786`, `peak_working_set=16465920`) at `--threads 1` (`bin/linux-x64/bin/rasterfold decode --time` at 3 threads records `alloc_peak=10570204`, `10.57 MB`, `codec_us=196505`, `total_us=205643`); `research/vsjxl.py -n5` records `rf_enc` median `292.8` ms (min `271.6`), `rf_dec` median `287.7` ms (min `284.0`), `jxl_enc` median `1582.9` ms (min `1549.6`), `jxl_dec` median `480.8` ms (min `465.2`), decode ratio `0.598`, encode ratio `0.185`; and `research/ab.py ab src/cm.c src/cm.c measure -n5` records `-t1` `size=1409028`, `enc min=482.7..484.1` ms (`med=496.4..499.6`), `dec min=453.5..467.2` ms (`med=481.7..498.2`) and `-t3` `enc min=152.2..168.6` ms (`med=177.9..187.6`), `dec min=167.3..177.9` ms (`med=185.2..193.2`).

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
print `33 passed, 0 failed`, or more once cases are added.

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

Assembly against C (the old version 1 assembly, rebuilt with ml64, against the
current C): CRC-32 slicing-by-8 in C is 4.2x faster than the assembly CRC; the C
BMP loader takes 0.61 to 0.66 of the assembly time. No assembly remains, and
reintroducing any needs an A/B run (2.4) that shows a gain.

---

## 4. Compression designs

### 4.1 Palette method for images with few colours (method 2)

**Owner:** `D01 T01 §1` to `§3`.

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
the C runtime with the normal entry point; the release build stays
runtime-free. The profile comes from encoding and decoding `measure/image.bmp`
and the ten test-pack photos at one and three threads
(`LLVM_PROFILE_FILE=build\pgo\%p.profraw`, merged with `llvm-profdata merge`),
and must be reproducible from a clean checkout. `ab.py` builds its own harness,
so the executables are compared with `research/vsjxl.py` or alternating
`rasterfold.exe` runs.

**Expected effect.** 5% to 15% faster (estimate), from branch layout.

### 5.2 Link-time optimization

**Owner:** `D02 T01 §2`.

`-flto` in `cflags` and `ldflags` of `build.ninja`, kept only if faster and the
executable still imports nothing but `KERNEL32.dll` and `SHELL32.dll`
(`tools\llvm-mingw\bin\llvm-objdump.exe -p Bin\rasterfold.exe`).

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

A 32768x1024 header with a 20-byte CM payload makes the decoder run all 100
million samples before it rejects the file. Once a channel's read pointer
reaches `end + 2`, the file will be rejected anyway. After each row,
`run_channel` and `run_pipelined` can check `ch->dec.in >= ch->dec.lim`, set a
shared abort flag and the channel's `bad`, and publish `done = h`, so no channel
waits forever on another's progress counter.

### 5.6 AVX2 path with runtime dispatch

**Owner:** `D02 T01 §3`.

Building the whole codec with `-march=x86-64-v3` is measured first; only if
that is at least 5% faster is `src/cm.c` compiled twice (v2 and v3, entry points
renamed with `-D`), with a CPUID and `XGETBV` check in `main.c` choosing the v3
path when the CPU and the operating system support AVX2. The output must be
byte-identical.

---

## 6. Memory designs

**Owner:** `D03 T01 §1` to `§3`.

### 6.1 Decoder: drop the padded stream copy (-1.41 MB on `measure`)

`rf_cm_decode` copies the three streams into a buffer with 8 zero bytes after
each, so `rdec_bit` can load two bytes past a stream's end without a bounds
check. The copy is unnecessary: the decoder never uses a byte past `end` in a
stream it accepts (`rdec_bit` loads `in[0]` and `in[1]` but shifts out what it
does not consume); for G and R the two bytes after `end` are the next stream's;
only B's lie past the end of the file. `rf_read_file` can allocate `size + 8`
zeroed bytes and `rdec_init` point at the payload directly.

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
| 2 | Same bytes, leaner and harder | every file byte-identical to milestone 1 (`KAT_FOLD` unchanged, the oracle agrees); decode `alloc_peak` at most 9.2 MB on `measure/image.bmp` (10.57 MB at milestone 1); one-thread decode at least 5% faster; a 4096x4096 header with a 20-byte payload refused in under a second | `D05 T01 §2` |
| 3 | Few-colour images solved | method 2 shipped; `syn_checker` below 210 bytes and `syn_pixel` below 53 bytes (JPEG XL e9's sizes); photo sizes unchanged; 20,000 fuzz mutations over every method clean | `D05 T01 §3` |
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
