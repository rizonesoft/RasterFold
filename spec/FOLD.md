# RasterFold image format, version 3

This document is the contract for `.fold` files and for the BMP subset the
reference tool accepts. A second implementation can decode a file from this
text alone; `tests/rfi_ref.py` is such an implementation, written from it.
The reference encoder's choice between methods is not part of the contract.

Version 3 differs from version 2 only in method 1: its context mixer has
seven models instead of five (contexts `k5` and `k6` below), 16 nodes per
context row instead of 32, a gated gradient selector, and warm-started
horizontal strips. Methods 0, 2 and 3 are identical in versions 2 and 3, and
version 3 adds methods 4, 5 and 6, which a version 2 file may not use. A
decoder reads both versions; an encoder writes version 3. Format version 3 is
final and frozen from release `rasterfold-v1.0.0`: every version 3 file written
at 1.0.0 continues to decode identically, and any later change to what version 3
writes requires a new format version. Version 2 replaced
the experimental version 1 (64×64 blocks with PackBits, palette, Rice and
binary rANS modes). Version 1 files are rejected as an unsupported version.

## Conventions

- Multi-byte integers are little-endian.
- Integer arithmetic is exact, on signed integers wide enough that nothing
  overflows unless a step says otherwise. Operators follow C11 precedence on
  32-bit or wider signed integers, except that `2^n` is mathematical
  exponentiation (`1 << n`, binding tighter than multiplicative operators) and
  `a mod m` is the non-negative remainder in `0..m-1` (at multiplicative
  precedence). `a >> n` is an arithmetic shift: the floor of `a / 2^n`, also for
  negative `a`. `a / b` appears only for non-negative operands and is truncating
  integer division.
- `clamp(v, lo, hi)` is `lo` if `v < lo`, `hi` if `v > hi`, otherwise `v`.
- `sign(v)` is -1, 0 or 1. `ilog2(v)` for `v >= 1` is the index of the highest
  set bit (`ilog2(1) = 0`).
- No field is trusted. Every size and count is checked before use.

## File layout

```
offset  size  field
0       4     magic = 52 46 49 V   ('R','F','I', V), V = 0x02 or 0x03
4       2     version = V (the same value as the magic's fourth byte)
6       2     header_size = 32
8       4     width
12      4     height
16      1     method: 0 = raw, 1 = CM, 2 = palette, 3 = extended palette,
              4 = compact palette (version 3 only),
              5 = screen content (version 3 only),
              6 = screen content v2 (version 3 only)
17      1     reserved = 0
18      2     pixel_format = 1
20      4     payload_size
24      4     payload_crc32
28      4     header_crc32
32      N     payload, N = payload_size
```

`pixel_format = 1` is 8-bit RGB with no alpha and no colour-space metadata.
Samples are the BMP byte values after the BGR-to-RGB swap.

The magic's fourth byte and the `version` field must agree and be 2 or 3;
any other combination (including `(2, 3)` and `(3, 2)`) is rejected as an
unsupported version.

```
1 <= width  <= 32768
1 <= height <= 32768
width * height <= 33554432
payload_size = file_size - 32
```

Both checksums are the zlib / PNG CRC-32 (reflected polynomial `0xEDB88320`,
initial and final XOR `0xFFFFFFFF`; `CRC-32("123456789") = 0xCBF43926`).
`payload_crc32` covers the payload. `header_crc32` covers bytes `[0, 28)`.

## Method 0, raw

The payload is the image as packed RGB, rows top to bottom, pixels left to
right, bytes R, G, B. `payload_size = width * height * 3`.

## Method 1, CM

### Payload

In version 2:

```
offset  size  field
0       4     size of stream G
4       4     size of stream R
8       4     size of stream B
12      ...   stream G, then stream R, then stream B
```

`12 + size G + size R + size B = payload_size`, and each stream is at least 5
bytes long. Stream c carries every binary decision of channel c (below) and
nothing else.

In version 3:

```
offset     size      field
0          1         S = number of horizontal strips (1..4, S <= height)
1          12 * S    3 * S 4-byte stream sizes:
                     strip 0 (G, R, B), strip 1 (G, R, B), ..., strip S-1 (G, R, B)
1 + 12 S   ...       3 * S streams in the same order:
                     strip 0 (G, R, B), strip 1 (G, R, B), ..., strip S-1 (G, R, B)
```

`1 <= S <= 4`, `S <= height`, `1 + 12 * S + sum(stream sizes) = payload_size`,
and each of the `3 * S` streams is at least 5 bytes long. Stream `(s, c)`
carries every binary decision of channel `c` in strip `s` (below) and nothing
else.

### Strips and warm start (version 3)

Version 2 files have a single strip (`S = 1`, `y0_0 = 0`, `y1_0 = height`,
`h_0 = height`).

In version 3, the reference encoder chooses `S = 3` when `height >= 768` and
`S = 1` when `height < 768` (a decoder accepts any `S` in `1..4` with
`S <= height`). The `height` image rows `0 .. height - 1` are partitioned into
`S` contiguous non-empty horizontal strips `s = 0 .. S - 1` covering image rows
`[y0_s, y1_s)` of height `h_s = y1_s - y0_s >= 1`:

```
w_rows      = 68 if S == 2 else max(64, height / 10)
total_extra = (S * (S - 1) / 2) * w_rows
if S == 1 or height <= total_extra + 32 * S:
    y0_s = (s * height) / S
    y1_s = ((s + 1) * height) / S
else:
    rem   = height - total_extra
    q     = rem / S
    r_rem = rem mod S
    h_k   = q + (1 if k < r_rem else 0) + (S - 1 - k) * w_rows    (k = 0 .. S - 1)
    y0_s  = sum_{k < s} h_k
    y1_s  = y0_s + h_s
```

Each strip `s` codes its `h_s` rows using strip-local row coordinates
`y = 0 .. h_s - 1` (corresponding to image row `y0_s + y`). Every row-dependent
rule below ("Neighbours", "Per-channel state", "LMS correction", "Gated
gradient", "Contexts") uses the strip's local row `y`: in particular, local row
`y = 0` of every strip uses the `y == 0` top-edge neighbour rule
(`N = NW = NE = NN = NNE = W`, with `W = 0` at `x = 0`), local row `y = 1` uses
`y == 1` (`NN = N`, `NNE = NE`), and each strip starts with its two-row
predictor arrays (`perr`, `err`, `er`, `fe`, `ex`) and gated-gradient sums
(`Eg`, `Et`) set to 0.

Strip 0 starts cold: for each channel `c`, all `M * 2048` model rows have visit
count `cell 0 = 0`. In version 2 (`M = 5`, `N_nodes = 32`), `cell 1 .. 31 = 32768`
and all twelve LMS weights `u_0 .. u_11 = 0`. In version 3 (`M = 7`, `N_nodes = 16`),
`cell 1 .. 15 = (32768, 32768, 30000, 28000, 24000, 18000, 12000, 6000, 29000, 29000, 29000, 29000, 30000, 30000, 30000)`,
and the twelve LMS weights start at `u_0 .. u_11 = 0` except on channel 2
(`B - G`), where `u_6 = 20000`. In both versions, all `128 * N_nodes` weight
sets have weights `0 .. M - 1 = 19661` and bias weight `M = 0`.

For `S > 1`, each strip `s < S - 1` warm-starts strip `s + 1` after coding
`warm_rows_s` local rows of strip `s`:

```
warm_rows_s = min(h_s, 68 if S == 2 else max(96, height / 6))
```

Immediately after strip `s` finishes coding its local row `y = warm_rows_s - 1`
on channel `c` (and before strip `s` codes row `y = warm_rows_s`, if any),
strip `s + 1`'s channel `c` is initialized from strip `s`'s channel `c` state
at that instant:

- For every model row `0 .. M * 2048 - 1`: `cell 0 = min(strip_s.cell 0, 24)`,
  and `cell d = strip_s.cell d` for `d = 1 .. N_nodes - 1`.
- Every weight set `(wsel, d)`, the gated-gradient accumulators `Eg` and `Et`,
  and all twelve LMS weights `u_0 .. u_11` (`u_k = strip_s.u_k` for
  `k = 0 .. 11`) are copied verbatim from `strip_s`.
- Strip `s + 1`'s two-row predictor buffers (`perr`, `err`, `er`, `fe`, `ex`)
  remain 0.

Once initialized after row `warm_rows_s - 1` of strip `s`, strip `s + 1`
evolves independently on its own streams.

### Channels and order

Each pixel has three channel samples:

```
channel 0 (G):  v0 = G           plane value p0 = G           base b0 = 0
channel 1 (R):  v1 = R           plane value p1 = R - G       base b1 = G
channel 2 (B):  v2 = B           plane value p2 = B - G       base b2 = G
```

Plane values of channels 1 and 2 lie in -255..255. Pixels are coded in raster
order, row by row top to bottom, left to right. Within a pixel the order is
G, R, B. Each channel keeps its own state (models, predictor, streams); a
channel reads another channel only at the same pixel, where R uses G and B uses
G and R, as defined below. Implementations may therefore run the channels in
any schedule that respects those same-pixel dependencies.

### Neighbours

For channel c at `(x, y)`, `P(x, y)` is the plane value `p_c` of an already
coded pixel.

```
if y == 0:
    W  = P(x-1, 0) if x > 0 else 0
    N = NW = NE = NN = NNE = W
else:
    N   = P(x, y-1)
    W   = P(x-1, y)     if x > 0          else N
    NW  = P(x-1, y-1)   if x > 0          else N
    NE  = P(x+1, y-1)   if x+1 < width    else N
    NN  = P(x, y-2)     if y > 1          else N
    NNE = P(x+1, y-2)   if y > 1 and x+1 < width else NE
WW  = P(x-2, y)     if x > 1          else W
```

`WW` is used only by context `k6` (version 3).

### Per-channel state

Each channel keeps, for two rows (`R = y mod 2`), arrays over positions
`0 .. width+1`: the four sub-predictor errors `perr[k]`, the predictor error
`err`, the residual `er` and the final error `fe`. All start at 0. Position `x`
of row `y` is written when sample `(x, y)` is coded. `cur` names row `y mod 2`
and `prv` names row `(y - 1) mod 2`; at `y = 0` the `prv` row is all zeros
except for the accumulation described in "Updates". (This is the layout of the
JPEG XL weighted predictor; `prv` of row 0 is the same array that row 1 will
overwrite as `cur`.)

Version 3 adds, at the same positions, the flag `ex` (0 or 1, starting at 0;
`ex(x, y)` below is the value written when sample `(x, y)` was coded), and two
per-channel error sums `Eg` and `Et`, unsigned 16-bit values that start at 0.

### Weighted predictor

This is the JPEG XL self-correcting predictor with its default parameters,
computed in units of 1/8.

```
pN  = position x of prv
pNE = position x+1 of prv if x+1 < width else pN
pNW = position x-1 of prv if x > 0 else pN

errw(e, m): s = max(0, ilog2(e + 1) - 5)
            return 4 + ((m * DIVL[e >> s]) >> s)
            where DIVL[i] = (2^24) / (i + 1)

wt_k = errw(perr_k[pN] + perr_k[pNE] + perr_k[pNW], m_k),  m = (13, 12, 12, 12)

teW  = err[cur, x-1] if x > 0 else 0
teN  = err[pN],  teNW = err[pNW],  teNE = err[pNE]
n8, w8, ne8 = 8N, 8W, 8NE
if version == 2:
    pr_0 = w8 + ne8 - n8
    pr_1 = n8 - (((teN + teW + teNE) * 16) >> 5)
    pr_2 = w8 - (((teN + teW + teNW) * 10) >> 5)
    pr_3 = n8 - (((teNW + teN + teNE) * 7) >> 5)
else:                                                  (version 3)
    pr_0 = w8 + (((ne8 - n8) * 3) >> 3)
    pr_1 = n8 - (((teN + teW + teNE) * 12) >> 5)
    pr_2 = w8 - (((teN + teW + teNW) * 8) >> 5)
    pr_3 = n8 - (((teNW + teN + teNE) * 5) >> 5)

S  = wt_0 + wt_1 + wt_2 + wt_3
L  = ilog2(S) - 4
wt_k = wt_k >> L                   (for each k)
S  = wt_0 + wt_1 + wt_2 + wt_3
wpred = (((S >> 1) - 1 + sum_k pr_k * wt_k) * DIVL[S - 1]) >> 24
if ((teN xor teW) or (teN xor teNW)) <= 0:     (two's complement xor / or)
    wpred = clamp(wpred, min(w8, ne8, n8), max(w8, ne8, n8))
```

### LMS correction

Twelve inputs, each clamped to -2047..2047:

```
x_0  = fe[cur, x-1]          if x > 0            else 0
x_1  = fe[prv, x]            if y > 0            else 0
x_2  = fe[prv, x-1]          if x > 0 and y > 0  else 0
x_3  = fe[prv, x+1]          if y > 0 and x+1 < width else 0
x_4  = fe[cur, x-2]          if x > 1            else 0
x_5  = FG  (channels 1, 2)   else 0
x_6  = FR  (channel 2)       else 0
x_7  = 8(N - W),  x_8 = 8(NE - N),  x_9 = 8(NW - W)
x_10 = 8(NN - N), x_11 = 8(NNE - NE)
```

`FG` and `FR` are the final errors of G and R at this pixel, clamped to
-2047..2047. The channel has twelve weights `u_k`, starting at 0 in strip 0
(except `u_6 = 20000` on channel 2 in version 3; for `S > 1`, strip `s + 1`
copies `u_0 .. u_11` from strip `s` at the warm-start handoff).

```
corr  = clamp((sum_k u_k * x_k) >> 16, -256, 256)
pred8 = wpred + corr
pred  = (pred8 + 4) >> 3
predv = clamp(pred + b_c, 0, 255)
```

### Gated gradient (version 3)

`V(x, y)` is the channel value `v_c` (not the plane value) of an already coded
pixel. The gated gradient `gp` and its gate are:

```
if x > 0 and y > 0:
    gp   = (V(x-1, y) + V(x, y-1) - V(x-1, y-1)) mod 256
    gate = ex(x-1, y) and ex(x, y-1) and ex(x-1, y-1)
           and (x+1 == width or ex(x+1, y-1))
else if x == 0 and y >= 2:
    gp   = (2 V(0, y-1) - V(0, y-2)) mod 256
    gate = ex(0, y-1) and (width == 1 or ex(1, y-1)) and ex(0, y-2)
else if y == 0 and x >= 2:
    gp   = (2 V(x-1, 0) - V(x-2, 0)) mod 256
    gate = ex(x-1, 0) and ex(x-2, 0)
else:
    gp is undefined, gate = 0

pt   = predv
use  = 1 if gate and 2 Eg < Et, else 0
pc   = gp if use else pt                                (the chosen prediction)
```

In the two-row layout, `ex(0, y-2)` is still held at position 0 of `cur` when
sample `(0, y)` is modelled. Version 2 files have `use = 0` and `pc = predv`
everywhere. Everything else (`pred`, `pred8`, the contexts) uses `pred` as
before.

### Residual

```
r = (v_c - pc) mod 256;  if r >= 128: r = r - 256           (r in -128..127)
decoder: v_c = (pc + r) mod 256
```

### Contexts

`EG` and `ER` are the residuals `r` of G and R at this pixel.

```
eW  = er[cur, x-1] if x > 0 else (er[prv, x] if y > 0 else 0)
eN  = er[prv, x]   if y > 0 else eW
eNW = er[prv, x-1] if x > 0 and y > 0 else eN
eNE = er[prv, x+1] if y > 0 and x+1 < width else eN
E   = 2|eW| + 2|eN| + |eNW| + |eNE|
qE  = 0 if E <= 0, 15 if E >= 16384, else ilog2(E) + 1
band = 0 if qE < 4, 1 if qE < 8, else 2
scl = (E >> 2) + 2  in version 2;  (E >> 2) + 3  in version 3

count(a, t...) = number of thresholds t_i with a >= t_i
q9(v)  = 4 + sign(v) * count(|v|, 1, 2, 4, 8)
q7(v)  = 3 + sign(v) * count(|v|, 1, 3, 7)
qj(v)  = 4 + sign(v) * count(|v|, 1, 3, 7, 21)
qn(v)  = 3 + sign(v) * count(6|v|, scl, 3 scl, 7 scl)
med(a, b, c) = min(a, b) if c >= max(a, b); max(a, b) if c <= min(a, b); else a + b - c
gq = q9(EG) for channels 1, 2, else 4
rq = q9(ER) for channel 2, else 4
q2d = qj(NE-N)*81 + qj(N-NW)*9 + qj(NW-W)

if version == 2:
    k0 = ((((qn(W-pred)*7 + qn(N-pred))*7 + qn(NE-pred))*7 + qn(NW-pred))*3 + band
    k1 = qE*81 + gq*9 + rq
    k2 = q2d*5 + (gq+1)/2
    k3 = ((q7(med(W,N,NW)-pred)*7 + q7(W+N-NW-pred))*7 + q7(((W+NE+1)>>1)-pred))*7 + q7(N+NE-NNE-pred)
    k4 = (q9(W-pred)*9 + q9(N-pred))*16 + qE
    wsel = qE*8 + j,  j = 3 for channel 0; for channels 1, 2 j = 0 if gq = 4,
           else 1 if |EG| <= 3, else 2
else:                                                  (version 3)
    cq   = rq for channel 2, else gq
    qe_c = (qE >> 1)*2 + (1 if cq != 4 else 0) for channels 1, 2, else qE
    k0 = (qn(W-pred)*195 + qn(N-pred)*31 + qn(NE-pred)*5 + qn(NW-pred))*3 + band
    k1 = qE for channel 0;  qE*9 + gq for channel 1;  qE*81 + gq*9 + rq for channel 2
    k2 = q2d for channel 0;  q2d*3 + (0 if cq < 3 else 2 if cq > 5 else 1) for channels 1, 2
    k3 = q7(med(W,N,NW)-pred)*289 + q7(W+N-NW-pred)*43 + q7(((W+NE+1)>>1)-pred)*7 + q7(N+NE-NNE-pred)
    k4 = (q9(W-pred)*9 + q9(N-pred))*16 + qe_c
    k5 = (q9(NE-pred)*9 + q9(NW-pred))*16 + qe_c
    k6 = ((qn(WW-pred)*7 + qn(NN-pred))*7 + qn(NNE-pred))*6 + ((qE*3) >> 3)
    wsel = qE*2 + use                                                          (channel 0)
           (qE*3 + (0 if gq = 4 else 1 if |EG| <= 3 else 2))*2 + use           (channel 1)
           (qE*3 + (0 if rq = 4 and gq = 4 else 1 if |ER| <= 3 else 2))*2 + use (channel 2)
row_m = k_m mod 2048                                   for model m = 0..M-1
```

`M`, the number of models, is 5 for version 2 files and 7 for version 3
files. `N_nodes`, the number of cells per model row and nodes per `wsel`, is
32 for version 2 files and 16 for version 3 files.

### Models

Each channel has `M` tables of 2048 rows by `N_nodes` 16-bit cells. Cell 0 of a
row is its visit count `n`, starting at 0 (or capped at 24 on warm-start
handoff). Cells `1 .. N_nodes - 1` are probabilities of a 1 bit in units of
1/65536, starting at 32768 in version 2 and at the version 3 cold vector in
"Strips and warm start (version 3)" above. Before the sample is coded, for each
model m in order 0..M-1:

```
n = cell 0 of row_m
if version == 2:
    rate_m = 65536 * 2 / (2n + 3)
    cell 0 of row_m = n + 1 if n < 60 else n
else:                                                  (version 3)
    rn = n - 2 if pc != b_c and n >= 2 else n
    rate_m = 65536 * 2 / (2 rn + 7)
    cell 0 of row_m = n + 1 if n < 72 else n
```

Each channel has `128 * N_nodes` weight sets of `M + 1` signed 32-bit weights,
all `19661` except the last (the bias weight), which starts at 0. The set for a
decision is `(wsel, node)`.

### One decision

A decision codes one bit at node `d` (`1..29` in version 2, `1..15` in version
3) with the `M` cells `c_m = cell d of row_m` and the weight set
`w = (wsel, d)`:

```
STRETCH, SQUASH: see below
s_m = STRETCH[c_m >> 4]                                  (m = 0..M-1)
x   = clamp((sum_m w_m * s_m + w_M * 64) >> 16, -2047, 2047)
p   = SQUASH(x)                                          (1..4095)
code the bit with probability p/4096 of a 1 (range coder below)
e   = 4096*bit - p
w_m = w_m + ((s_m * e + 4096) >> 13)       for m = 0..M-1, in 32-bit two's complement
w_M = w_M + ((64 * e + 4096) >> 13)        likewise
c_m = c_m + ((((65535*bit) - c_m) * rate_m) >> 16)         for m = 0..M-1
```

```
SQUASH(x) = 4095 if x > 2047; 1 if x < -2047; otherwise
            (T[i] * (128 - f) + T[i+1] * f + 64) >> 7, i = (x >> 7) + 16, f = x & 127,
T = 1 2 3 6 10 16 27 45 73 120 194 310 488 747 1101 1546 2047 2549 2994 3348 3607
    3785 3901 3975 4022 4050 4068 4079 4085 4089 4092 4093 4094
STRETCH[q] for q in 0..4095 is the smallest x in -2047..2047 with SQUASH(x) >= q,
            or 2047 when there is none.
```

### Binarization of the residual

In version 2 (`N_nodes = 32`):

```
node 1   bit r != 0                         (stop here when 0)
node 2   bit r < 0
a = |r|  (1..128), e = ilog2(a)
nodes 3 .. 3+e-1: bit 1 each;  node 3+e: bit 0, only when e < 7
mantissa m = a - (2^e), its e bits from the top:
    bit e-1 at node 10 + e
    bit e-2 at node 14 + 2e + (bit e-1)
    lower bits with probability 2048/4096, no model and no update
```

In version 3 (`N_nodes = 16`), high-exponent unary and mantissa decisions pool
into nodes `8`, `12`, and `15`:

```
node 1   bit r != 0                         (stop here when 0)
node 2   bit r < 0
a = |r|  (1..128), e = ilog2(a)
for k = 0 .. e-1: bit 1 at node (3 + min(k, 5))
if e < 7:         bit 0 at node (3 + min(e, 5))
mantissa m = a - (2^e), its e bits from the top:
    bit e-1 at node (8 + min(e, 4))
    bit e-2 at node (13 + (bit e-1) if e == 2 else 15)
    lower bits with probability 2048/4096, no model and no update
```

A decoder that assembles `a = 128` with `r > 0` rejects the file.

### Range coder

Each stream is coded by a binary range coder with 32-bit range and 12-bit
probabilities, the coder of LZMA. A bit with probability `p` (of a 1):

```
encoder:  bound = (range >> 12) * p
          if bit: range = bound  else: low = low + bound, range = range - bound
          while range < 2^24: range = range << 8 (mod 2^32), shift_low()
shift_low: if (low mod 2^32) < 0xFF000000 or low >= 2^32:
               emit (cache + (low >> 32)) mod 256, then pending - 1 bytes of
               (0xFF + (low >> 32)) mod 256; cache = (low >> 24) mod 256; pending = 0
           pending = pending + 1; low = (low mod 2^24) << 8
start:     low = 0, range = 0xFFFFFFFF, cache = 0, pending = 1
finish:    shift_low() five times
decoder:  start: range = 0xFFFFFFFF, code = the first five bytes, big-endian, mod 2^32
          bound = (range >> 12) * p
          if code < bound: bit = 1, range = bound
          else: bit = 0, code = code - bound, range = range - bound
          while range < 2^24: range = range << 8, code = (code << 8) + next byte (mod 2^32)
```

The decoder must consume exactly the bytes of its stream: a stream that needs
a byte past its end, or has bytes left after the image, is rejected.

### Updates

After the channel value `v_c` is known:

```
pv    = v_c - b_c
fe_e  = 8 pv - pred8                                     (final error)
g     = 10 if fe_e > 0, -10 if fe_e < 0, else 0
u_k   = clamp(u_k + g * sign(x_k), -65536, 65536)        (x_k after clamping)
v8    = 8 pv
err[cur, x]     = wpred - v8
perr_k[cur, x]  = (|pr_k - v8| + 3) >> 3                 (k = 0..3)
perr_k[prv, x+1] = perr_k[prv, x+1] + that value         (k = 0..3)
er[cur, x] = r,  fe[cur, x] = fe_e
```

Version 3 then updates the gated gradient state:

```
d(v, p) = min(m, 256 - m, 32)  where m = (v - p) mod 256
ex[cur, x] = 1 if gp is defined and v_c == gp, else 0
if gate:
    Eg = Eg + d(v_c, gp)
    Et = Et + d(v_c, pt)
    if Eg >= 4096 or Et >= 4096:  Eg = Eg >> 1,  Et = Et >> 1
```

The G residual and final error at this pixel are what R and B read as `EG`
and `FG`; those of R are `ER` and `FR`. `FG` and `FR` are clamped to
-2047..2047. A decoder that produced `r = 128` passes it on as -128; the file is
rejected anyway.

## Method 2, palette

### Payload

```
offset    size        field
0         2           count - 1     (count = number of colours, 1..256)
2         3 * count   palette: R, G, B of entry 0, then entry 1, ...
2 + 3c    4           size of the index stream (at least 5)
6 + 3c    ...         index stream: one binary range coder ("Range coder" above)
```

`1 <= count <= 256` (so the 16-bit `count - 1` field is in `0..255`), the
`count` palette triples `(R, G, B)` are distinct, the stream size `S` is at
least 5, and `6 + 3 * count + S = payload_size`. The reference encoder lists
colours in first-seen raster order: entry 0 is the colour of pixel `(0, 0)`,
and each new colour takes the next index.

### Index neighbours and contexts

Pixels are coded in raster order, `y = 0 .. height - 1`, `x = 0 .. width - 1`.
`I(x, y)` is the palette index (`0 .. count - 1`) of pixel `(x, y)`, and the
colour of `(x, y)` is palette entry `I(x, y)`.

For `(x, y)`, the four causal neighbour indices `a = W`, `b = N`, `c = NW`,
and `d = NE` follow the boundary rules of method 1 ("Neighbours"):

```
if y == 0:
    a = I(x-1, 0) if x > 0 else 0
    b = c = d = a
else:
    b = I(x, y-1)
    a = I(x-1, y)   if x > 0         else b
    c = I(x-1, y-1) if x > 0         else b
    d = I(x+1, y-1) if x+1 < width   else b
```

The equality pattern `pattern` (`0..31`) and previous-pixel hit flag `hit`
(`0..1`) are:

```
pattern = (a == b) | ((a == c) << 1) | ((a == d) << 2) | ((b == c) << 3) | ((b == d) << 4)
hit     = 0 for (0, 0); thereafter 1 if the previous pixel in raster order coded 1 on D1, else 0
```

### Counters and decisions

Method 2 keeps four tables of adaptive counters: `D1[0..63]`, `D2[0..31]`,
`D3[0..31]`, and `T[1..255]`. Each counter has a 16-bit probability `c`
(starting at `32768`) and a visit count `n` (starting at `0`). Coding a bit
with a counter `(c, n)` uses the range coder of method 1 ("Range coder") and
method 1's counter update rule:

```
p    = max(1, c >> 4)
rate = (65536 * 2) / (2n + 3)
n    = n + 1 if n < 60 else n
code the bit with probability p/4096 of a 1
c    = c + ((((65535 * bit) - c) * rate) >> 16)
```

At each pixel `(x, y)`:

```
D1: code bit (I == a) with counter D1[pattern * 2 + hit]
if D1 coded 1:
    I = a
else:
    if b != a:
        D2: code bit (I == b) with counter D2[pattern]
    if b != a and D2 coded 1:
        I = b
    else:
        if d != a and d != b:
            D3: code bit (I == d) with counter D3[pattern]
        if d != a and d != b and D3 coded 1:
            I = d
        else:
            node = 1
            for bit_pos in 7 down to 0:
                code bit ((I >> bit_pos) & 1) with counter T[node]
                node = node * 2 + bit
            I = node - 256
```

When the literal tree is reached, the decoded index `I` must satisfy
`I < count` and `I not in {a, b, d}` (every candidate already excluded either
by a `0` decision on `D1`, `D2`, or `D3`, or by equality with an excluded
candidate); otherwise the file is rejected. The index stream must end at
exactly `S` bytes after the last pixel.

## Method 3, extended palette

### Payload

Method 3 uses the same payload layout and framing rules as method 2 ("Method 2,
palette"): `1 <= count <= 256`, `count` distinct `(R, G, B)` triples, stream
size `S >= 5`, and `6 + 3 * count + S = payload_size`.

### Index neighbours and contexts

For `(x, y)`, the four immediate causal neighbour indices `a = W`, `b = N`,
`c = NW`, and `d = NE` follow the boundary rules of method 2. The two-step
causal neighbours `ww = WW` and `nn = NN` and the third candidate `e` are:

```
ww = I(x-2, y) if x >= 2 else a
nn = a         if y == 0 else
     b         if y == 1 else
     I(x, y-2)
e  = d         if (d != a and d != b) else
     c         if (c != a and c != b) else
     ww        if (ww != a and ww != b) else
     a
```

The 7-bit equality pattern `pattern` (`0..127`) and previous-pixel hit flag
`hit` (`0..1`) are:

```
pattern = (a == b) | ((a == c) << 1) | ((a == d) << 2) | ((b == c) << 3) | ((b == d) << 4)
        | ((ww == a) << 5) | ((nn == b) << 6)
hit     = 0 for (0, 0); thereafter 1 if the previous pixel in raster order coded 1 on D1, else 0
```

### Counters and decisions

Method 3 keeps four tables of adaptive counters: `D1[0..255]`, `D2[0..127]`,
`D3[0..127]`, and `T[1..255]`. Each counter starts at `c = 32768`, `n = 0`.
Coding a bit with a counter `(c, n)` uses the range coder of method 1 ("Range
coder") and updates `c` with a minimum step of 1 whenever `c` has not reached
its target extreme:

```
p    = max(1, c >> 4)
rate = (65536 * 2) / (2n + 3)
n    = n + 1 if n < 60 else n
code the bit with probability p/4096 of a 1
if bit == 1 and c < 65535:
    c = c + max(1, ((65535 - c) * rate) >> 16)
else if bit == 0 and c > 0:
    c = c - max(1, (c * rate) >> 16)
```

At each pixel `(x, y)`:

```
D1: code bit (I == a) with counter D1[pattern * 2 + hit]
if D1 coded 1:
    I = a
else if count == 2:
    I = 1 - a
else:
    if b != a:
        D2: code bit (I == b) with counter D2[pattern]
    if b != a and D2 coded 1:
        I = b
    else:
        if e != a and e != b:
            D3: code bit (I == e) with counter D3[pattern]
        if e != a and e != b and D3 coded 1:
            I = e
        else:
            node = 1
            for bit_pos in 7 down to 0:
                code bit ((I >> bit_pos) & 1) with counter T[node]
                node = node * 2 + bit
            I = node - 256
```

When the literal tree is reached, the decoded index `I` must satisfy
`I < count` and `I not in {a, b, e}` (every candidate already excluded either
by a `0` decision on `D1`, `D2`, or `D3`, or by equality with an excluded
candidate); otherwise the file is rejected. The index stream must end at
exactly `S` bytes after the last pixel.

## Method 4, compact palette

Method 4 is valid only in a version 3 file. It codes the same palette indices
as method 3 with a smaller palette field, no stream size field, finer
probabilities, and separate first-decision counters for the image's top row
and left column.

### Payload

```
offset    size   field
0         1      count - 1     (count = number of colours, 1..256)
1         P      palette field: a bit string, described below
1 + P     S      index stream: every remaining byte of the payload
```

`S = payload_size - 1 - P` and `S >= 5`.

The palette field is read as a sequence of bits, the most significant bit of
each byte first. A number of `n` bits is read most significant bit first.

```
kR, kG, kB: three 4-bit numbers, each in 0..8
for each entry i = 0 .. count - 1, with R(-1) = 0:
    R(i) = R(i-1) + rice(kR)
    G(i) = R(i)   + unzigzag(rice(kG))
    B(i) = G(i)   + unzigzag(rice(kB))
padding: zero bits up to the next byte boundary (none if already on one)

rice(k):      q = the number of 1 bits before the next 0 bit (that 0 bit is consumed)
              the value is (q << k) + the next k bits as a number (0 when k = 0)
unzigzag(u):  u >> 1 if u is even, otherwise -((u + 1) >> 1)
```

`P` is the number of bytes the field occupies, padding included. Every `R(i)`,
`G(i)` and `B(i)` is in `0..255`, and the entries are in strictly increasing
order of `R * 65536 + G * 256 + B`, so the colours are distinct. Entry `i` is
palette index `i`. A `rice` quotient `q` is at most 511.

### Index stream

The index stream is one binary range coder ("Range coder" under method 1),
with this difference: a bit with probability `p` (of a 1), `1 <= p <= 65535`,
is coded on a 16-bit scale with an exact split,

```
bound = (range * p) >> 16        (the product is computed without overflow; it needs 48 bits)
```

in place of `bound = (range >> 12) * p`, in both the encoder and the decoder.
Everything else in the range coder is unchanged.

Neighbours `a`, `b`, `c`, `d`, `ww`, `nn`, the third candidate `e`, `pattern`
and `hit` are those of method 3. The border class of pixel `(x, y)` is

```
class = 1 if y == 0
        2 if y > 0 and x == 0
        0 otherwise
```

Method 4 keeps `D1[0..767]`, `D2[0..127]`, `D3[0..127]`, and `T[1..255]`, each
counter starting at `c = 32768`, `n = 0`. Coding a bit with a counter `(c, n)`
is method 3's rule with the 16-bit probability:

```
p    = max(1, c)
rate = (65536 * 2) / (2n + 3)
n    = n + 1 if n < 60 else n
code the bit with probability p/65536 of a 1
if bit == 1 and c < 65535:
    c = c + max(1, ((65535 - c) * rate) >> 16)
else if bit == 0 and c > 0:
    c = c - max(1, (c * rate) >> 16)
```

At each pixel the decisions are those of method 3 ("Counters and decisions"),
except that `D1` uses counter `D1[pattern * 2 + hit + 256 * class]`. `D2`,
`D3` and `T` are indexed as in method 3, the `count == 2` shortcut applies,
and a one-colour image codes one `D1` decision per pixel like any other.

When the literal tree is reached, the decoded index `I` must satisfy
`I < count` and `I not in {a, b, e}`; otherwise the file is rejected. The
index stream must end at exactly `S` bytes after the last pixel.

## Method 5, screen content

Method 5 is valid only in a version 3 file. It codes the `width * height`
pixels in raster order `p = 0 .. width * height - 1` (`p = y * width + x`) as
a sequence of tokens: literals, colour-cache hits, and backward-reference
copies.

### Payload and stream

The payload is one binary range-coded stream of `payload_size` bytes, with
`payload_size >= 5`. Every decision uses the 16-bit probability split of
method 4 ("Index stream" under method 4: `bound = (range * p) >> 16` for
probability `p/65536` of a 1 bit, `1 <= p <= 65535`) with the state, shifting,
flushing, and decoding rules of method 1 ("Range coder"). The decoder must
consume all `payload_size` bytes when `p` reaches `width * height`: a stream
that needs a byte past `payload_size` or leaves bytes unconsumed is rejected.

### Counters, binary trees, and prefix codes

Every adaptive counter `(c, n)` starts at `c = 32768`, `n = 0`, and codes a
bit using the 16-bit counter rule of method 4 (`p = max(1, c)`,
`rate = (65536 * 2) / (2n + 3)`, `n = n + 1` if `n < 60` else `n`, and `c`
moved toward `65535` on `bit = 1` or `0` on `bit = 0` by at least `1` before
reaching that bound).

A `K`-level binary tree `tree(T, K)` codes an unsigned `K`-bit integer `u` in
`0 .. 2^K - 1` with counters `T[1 .. 2^K - 1]`:

```
node = 1
for bit_pos in K - 1 down to 0:
    code bit ((u >> bit_pos) & 1) with counter T[node]
    node = node * 2 + bit
decoded value: u = node - 2^K
```

Copy length offsets and distance codes use the prefix code of RFC 9649 section
3.6.2.2 for positive integers `value >= 1`. Let `d = value - 1`:

```
if d < 4:
    symbol = d,  nbits = 0,  extra = 0
else:
    h      = ilog2(d)                           (h >= 2)
    nbits  = h - 1
    symbol = 2 * h + ((d >> (h - 1)) & 1)
    extra  = d & (2^nbits - 1)

decoder:
if symbol < 4:
    nbits = 0,  value = symbol + 1
else:
    nbits = (symbol - 2) >> 1
    value = ((2 + (symbol & 1)) << nbits) + extra + 1
```

When `nbits >= 1`, the `nbits` bits of `extra` (`0 .. 2^nbits - 1`) are coded
most significant bit first (`bit_pos` from `nbits - 1` down to `0`): the
highest extra bit (`bit_pos = nbits - 1`) is coded with an adaptive counter
`E[symbol]`, and each remaining bit (`bit_pos < nbits - 1`) is coded at fixed
probability `p = 32768` (`32768/65536`) with no counter and no update.

### Colour cache

Method 5 keeps a 256-entry direct-mapped colour cache `C[0..255]`, all entries
starting empty. The slot index of pixel `(R, G, B)` with
`RGB = (R << 16) | (G << 8) | B` is:

```
slot(R, G, B) = ((0x1E35A7BD * (0xFF000000 | RGB)) mod 2^32) >> 24
```

Inserting `(R, G, B)` sets `C[slot(R, G, B)] = (R, G, B)` (occupied). Every
decoded pixel is inserted into `C` as soon as it is produced, in raster order,
including every pixel of a backward-reference copy.

### State, 2D distance map, and tokens

Method 5 keeps these counter tables:

- `is_copy[0..2]` and `is_cache[0..2]`, indexed by `prev_kind` (`0` = literal,
  `1` = colour-cache hit, `2` = copy; initially `0`),
- `lit_g[1..255]`, `lit_rg[0..3][1..255]`, and `lit_bg[0..3][1..255]`,
- `cache_tree[1..255]`,
- `len_tree[1..31]` and `len_extra[0..31]`,
- `dist_tree[0..1][1..63]` (indexed by `prev_short` in `{0, 1}`, initially
  `0`) and `dist_extra[0..63]`.

Distance codes `v` in `1..120` map through the 120-entry 2D neighbourhood
table `M[0..119]` of `(xi, yi)` pairs (RFC 9649 section 3.6.2.2.1, Figure 20):

```
(0,1), (1,0), (1,1), (-1,1), (0,2), (2,0), (1,2), (-1,2), (2,1), (-2,1), (2,2), (-2,2),
(0,3), (3,0), (1,3), (-1,3), (3,1), (-3,1), (2,3), (-2,3), (3,2), (-3,2), (0,4), (4,0),
(1,4), (-1,4), (4,1), (-4,1), (3,3), (-3,3), (2,4), (-2,4), (4,2), (-4,2), (0,5), (3,4),
(-3,4), (4,3), (-4,3), (5,0), (1,5), (-1,5), (5,1), (-5,1), (2,5), (-2,5), (5,2), (-5,2),
(4,4), (-4,4), (3,5), (-3,5), (5,3), (-5,3), (0,6), (6,0), (1,6), (-1,6), (6,1), (-6,1),
(2,6), (-2,6), (6,2), (-6,2), (4,5), (-4,5), (5,4), (-5,4), (3,6), (-3,6), (6,3), (-6,3),
(0,7), (7,0), (1,7), (-1,7), (5,5), (-5,5), (7,1), (-7,1), (4,6), (-4,6), (6,4), (-6,4),
(2,7), (-2,7), (7,2), (-7,2), (3,7), (-3,7), (7,3), (-7,3), (5,6), (-5,6), (6,5), (-6,5),
(8,0), (4,7), (-4,7), (7,4), (-7,4), (8,1), (8,2), (6,6), (-6,6), (8,3), (5,7), (-5,7),
(7,5), (-7,5), (8,4), (6,7), (-6,7), (7,6), (-7,6), (8,5), (7,7), (-7,7), (8,6), (8,7)
```

For `v` in `1..120`, `(xi, yi) = M[v - 1]` and `D = max(1, xi + yi * width)`.
For `v > 120`, `D = v - 120`.

Starting at `p = 0`, `prev_kind = 0`, `prev_short = 0`, while
`p < width * height`:

```
code bit b_copy with is_copy[prev_kind]
if b_copy == 1:
    len_sym = tree(len_tree, 5)                     (must satisfy len_sym <= 23)
    code extra bits of len_sym with E = len_extra -> u = prefix_value(len_sym, extra)
    L = u + 3                                       (must satisfy 4 <= L <= 4096 and p + L <= width * height)
    dist_sym = tree(dist_tree[prev_short], 6)       (must satisfy dist_sym <= 39)
    code extra bits of dist_sym with E = dist_extra -> v = prefix_value(dist_sym, extra)
    D = max(1, M[v - 1].x + M[v - 1].y * width) if v <= 120 else v - 120
                                                    (must satisfy 1 <= D <= p)
    for q = p .. p + L - 1:
        pixel[q] = pixel[q - D]
        insert pixel[q] into C
    p = p + L
    prev_kind = 2
    prev_short = 1 if v <= 120 else 0
else:
    code bit b_cache with is_cache[prev_kind]
    if b_cache == 1:
        idx = tree(cache_tree, 8)                   (C[idx] must be occupied)
        pixel[p] = C[idx]
        insert pixel[p] into C
        p = p + 1
        prev_kind = 1
    else:
        G  = tree(lit_g, 8)
        rg = tree(lit_rg[G >> 6], 8)
        bg = tree(lit_bg[G >> 6], 8)
        R  = (rg + G) mod 256
        B  = (bg + G) mod 256
        pixel[p] = (R, G, B)
        insert pixel[p] into C
        p = p + 1
        prev_kind = 0
```

When `D < L`, each copied pixel `pixel[q]` reads `pixel[q - D]` after earlier
pixels of the same copy have been written.

## Method 6, screen content v2

Method 6 is valid only in a version 3 file. It codes the `width * height`
pixels in raster order `p = 0 .. width * height - 1` (`p = y * width + x`,
`y = p / width`, `x = p mod width`) as backward-reference copies with causal
spatial/recent distance inheritance (`L >= 2`) or decomposed 2D `(dy, dx)`
offsets (`L >= 4`), single-pixel `W` and `N` neighbour runs, 16-entry
move-to-front (`MTF`) colour hits, 2048-entry direct colour-cache hits, and
`W_G`-conditioned subtract-green literals.

### Payload, coder, trees, and prefix codes

The payload framing (`payload_size >= 5`), 16-bit binary range coder
(`bound = (range * p) >> 16`), exact end-of-stream consumption at
`p == width * height`, adaptive counter update rule (`c = 32768`, `n = 0`
initially), `K`-level binary tree `tree(T, K)`, and prefix code
(`symbol`, `nbits`, `extra`, `prefix_value(symbol, extra)`, highest extra bit
coded with `E[symbol]`, lower extra bits at fixed `p = 32768`) are identical to
method 5 ("Payload and stream" and "Counters, binary trees, and prefix codes"
above), with `prefix_value` used for positive integers up to `2^25` (`symbol`
in `0..49`, `nbits` in `0..23`). The maximum copy distance is
`WINDOW = 2^20 - 1 = 1048575`.

### Recency colour state (`mtf` and 2048-entry cache)

Method 6 maintains:

- an ordered list `mtf[0 .. mtf_count - 1]` of `0 <= mtf_count <= 16` distinct
  `(R, G, B)` triples, initially empty (`mtf_count = 0`), and
- a 2048-entry direct-mapped colour cache `C2[0..2047]`, all entries starting
  empty, indexed by the top 11 bits of the 32-bit multiplicative hash of
  `RGB = (R << 16) | (G << 8) | B`:

```
slot11(R, G, B) = ((0x1E35A7BD * (0xFF000000 | RGB)) mod 2^32) >> 21
```

Updating the recency colour state with pixel `P = (R, G, B)` (`update_color(P)`):

1. If `P` is already in `mtf`, remove it from its current index; insert `P` at
   `mtf[0]`, shifting earlier entries up by 1; if `mtf` then has 17 entries,
   discard `mtf[16]` so `mtf_count <= 16`.
2. Set `C2[slot11(R, G, B)] = P` (occupied).

Every decoded pixel updates this state in raster order as soon as it is
produced, including pixel `0` and every pixel of a copy.

### Causal distance ring, repeat queues, and inherited candidates

Let `ring_cap = width + 3`. Method 6 maintains:

- `orig_ring[0 .. ring_cap - 1]`, all entries initially `0`. Whenever pixel `q`
  is produced by a copy of distance `D >= 1`, `orig_ring[q mod ring_cap]` is
  set to `D`; whenever pixel `q` is produced by any non-copy token (including
  pixel `0`), `orig_ring[q mod ring_cap]` is set to `0`. Because `ring_cap = width + 3`,
  at pixel `p >= 1` the entries written at `N = p - width`, `NE = p - width + 1`,
  `NW = p - width - 1`, `NNE = p - width + 2`, `NNW = p - width - 2`, and
  `W = p - 1` have not been overwritten since those causal neighbours were
  decoded.
- `rep[0..3]`, initially `[1, width, width + 1, width - 1]`.
- `rep_dy[0..3]`, initially `[1, 2, 4, 8]`.

`mtf_shift4(Q, v)` updates a 4-entry queue `Q[0..3]` with value `v`: let `pos`
be the smallest `i` in `0..3` with `Q[i] == v`, or `3` if none matches; for
`i` from `pos` down to `1` set `Q[i] = Q[i - 1]`, then set `Q[0] = v`.

At pixel `p >= 1` (`y = p / width`, `x = p mod width`, `maxd = min(p, WINDOW)`),
the inherited candidate list `cands(p)` (`1 <= |cands(p)| <= 4`) starts empty
and visits the following candidate distances `d` in exact order, appending `d`
whenever `1 <= d <= maxd`, `d` is not already in `cands(p)`, and
`|cands(p)| < 4`:

1. When `y >= 1`:
   - `orig_ring[(p - width) mod ring_cap]`,
   - `orig_ring[(p - width + 1) mod ring_cap]` when `x + 1 < width`,
   - `orig_ring[(p - width - 1) mod ring_cap]` when `x >= 1`,
   - `orig_ring[(p - width + 2) mod ring_cap]` when `x + 2 < width`,
   - `orig_ring[(p - width - 2) mod ring_cap]` when `x >= 2`.
2. When `x >= 1`: `orig_ring[(p - 1) mod ring_cap]`.
3. `rep[0]`, `rep[1]`, `rep[2]`, `rep[3]`.
4. `1`, `width`, `width + 1`, `width - 1`.

Because `p >= 1` implies `maxd >= 1`, candidate `1` is always eligible, so
`cands(p)` always has `1` to `4` distinct distances in `1..maxd`.

### Counter tables and token grammar

Token classes are `0` = copy, `1` = `EQ_W`, `2` = `EQ_N`, `3` = `MTF`,
`4` = `CACHE`, and `5` = literal. Method 6 keeps these adaptive counter tables:

- `is_copy[0..5]`, `is_inherit[0..5]`, `inh_b0[0..5]`, `inh_b1`, `inh_b2`,
- `len_tree[0..1][1..31]` and `len_extra[0..1][0..23]`,
- `is_same_row[0..1]` (indexed by `prev_same_row` in `{0, 1}`), `is_rep_dy`,
  `rep_dy_b0`, `rep_dy_b1[0..1]`,
- `dy_tree[1..63]` and `dy_extra[0..39]`,
- `dx0_tree[1..63]` and `dx0_extra[0..39]`,
- `dx_zero[0..1]` (indexed by `in_rdy` in `{0, 1}`), `dx_sign`,
  `dx_tree[1..63]`, and `dx_extra[0..49]`,
- `is_eq_w[0..11]`, `is_eq_n[0..5]`, `is_mtf[0..5]`, `mtf_tree[1..15]`,
- `is_cache[0..5]` and `cache_tree[1..2047]`,
- `lit_g[0..7][1..255]`, `lit_rg[0..3][1..255]`, and `lit_bg[0..3][1..255]`.

Pixel `p = 0` is coded unconditionally as a literal with `wg_ctx = 4` (no
`is_copy`, neighbour, `MTF`, or cache flag is coded at `p = 0`):

```
G  = tree(lit_g[4], 8)
rg = tree(lit_rg[G >> 6], 8)
bg = tree(lit_bg[G >> 6], 8)
pixel[0] = ((rg + G) mod 256, G, (bg + G) mod 256)
orig_ring[0] = 0
update_color(pixel[0])
prev_kind = 5,  prev_same_row = 0,  p = 1
```

While `p < width * height`, let `y = p / width`, `x = p mod width`, and
`C = cands(p)` (`k = |C|` in `1..4`):

```
code bit b_copy with is_copy[prev_kind]
if b_copy == 1:
    code bit use_inh with is_inherit[prev_kind]
    if use_inh == 1:
        if k == 1:
            idx = 0
        else:
            code bit b0 with inh_b0[prev_kind]
            if b0 == 1:
                idx = 0
            else if k == 2:
                idx = 1
            else:
                code bit b1 with inh_b1
                if b1 == 1:
                    idx = 1
                else if k == 3:
                    idx = 2
                else:
                    code bit b2 with inh_b2
                    idx = 2 if b2 == 1 else 3
        D = C[idx]
        len_sym = tree(len_tree[1], 5)                  (must satisfy len_sym <= 23)
        code extra bits of len_sym with E = len_extra[1] -> u = prefix_value(len_sym, extra)
        L = u + 1                                       (must satisfy 2 <= L <= 4096 and p + L <= width * height)
    else:
        len_sym = tree(len_tree[0], 5)                  (must satisfy len_sym <= 23)
        code extra bits of len_sym with E = len_extra[0] -> u = prefix_value(len_sym, extra)
        L = u + 3                                       (must satisfy 4 <= L <= 4096 and p + L <= width * height)
        if y == 0:
            same_row = 1
        else if x == 0:
            same_row = 0
        else:
            code bit same_row with is_same_row[prev_same_row]
        if same_row == 1:
            dx_sym = tree(dx0_tree, 6)                  (must satisfy dx_sym <= 39)
            code extra bits of dx_sym with E = dx0_extra -> dx = prefix_value(dx_sym, extra)
                                                        (must satisfy 1 <= dx <= x and dx <= WINDOW)
            D = dx
            prev_same_row = 1
        else:
            code bit in_rdy with is_rep_dy
            if in_rdy == 1:
                code bit r0 with rep_dy_b0
                if r0 == 1:
                    ridx = 0
                else:
                    code bit r1 with rep_dy_b1[0]
                    if r1 == 1:
                        ridx = 1
                    else:
                        code bit r2 with rep_dy_b1[1]
                        ridx = 2 if r2 == 1 else 3
                dy = rep_dy[ridx]
            else:
                dy_sym = tree(dy_tree, 6)               (must satisfy dy_sym <= 39)
                code extra bits of dy_sym with E = dy_extra -> dy = prefix_value(dy_sym, extra)
                                                        (for either branch, must satisfy 1 <= dy <= y and dy <= WINDOW)
            mtf_shift4(rep_dy, dy)
            code bit dz with dx_zero[in_rdy]
            if dz == 1:
                dx = 0
            else:
                code bit neg with dx_sign
                dx_sym = tree(dx_tree, 6)               (must satisfy dx_sym <= 49)
                code extra bits of dx_sym with E = dx_extra -> adx = prefix_value(dx_sym, extra)
                dx = -adx if neg == 1 else adx
            qx = x - dx                                 (must satisfy 0 <= qx < width)
            D  = dy * width + dx                        (must satisfy 1 <= D <= WINDOW)
            prev_same_row = 0
    for q = p .. p + L - 1:
        pixel[q] = pixel[q - D]
        orig_ring[q mod ring_cap] = D
        update_color(pixel[q])
    mtf_shift4(rep, D)
    p = p + L
    prev_kind = 0
else:
    has_w = (x >= 1),  has_n = (y >= 1)
    done = 0
    if has_w:
        wn_eq = 1 if (has_n and pixel[p - 1] == pixel[p - width]) else 0
        code bit bw with is_eq_w[prev_kind * 2 + wn_eq]
        if bw == 1:
            pixel[p] = pixel[p - 1]
            tk = 1,  done = 1
    if done == 0 and has_n and (not has_w or pixel[p - width] != pixel[p - 1]):
        code bit bn with is_eq_n[prev_kind]
        if bn == 1:
            pixel[p] = pixel[p - width]
            tk = 2,  done = 1
    if done == 0:
        code bit bm with is_mtf[prev_kind]
        if bm == 1:
            midx = tree(mtf_tree, 4)                    (must satisfy midx < mtf_count)
            pixel[p] = mtf[midx]
            tk = 3
        else:
            code bit bc with is_cache[prev_kind]
            if bc == 1:
                cidx = tree(cache_tree, 11)             (C2[cidx] must be occupied)
                pixel[p] = C2[cidx]
                tk = 4
            else:
                wg_ctx = (pixel[p - 1].G >> 5) if has_w else 4
                G  = tree(lit_g[wg_ctx], 8)
                rg = tree(lit_rg[G >> 6], 8)
                bg = tree(lit_bg[G >> 6], 8)
                pixel[p] = ((rg + G) mod 256, G, (bg + G) mod 256)
                tk = 5
    orig_ring[p mod ring_cap] = 0
    update_color(pixel[p])
    p = p + 1
    prev_kind = tk
```

When `D < L`, each copied pixel `pixel[q]` reads `pixel[q - D]` after earlier
pixels of the same copy have been written.

## Rejection rules

Reject, and do not produce a completed output file, when:

- the file is shorter than 32 bytes or longer than 256 MiB;
- the magic is not `RFI` (malformed), or the fourth magic byte and `version`
  do not agree or are not 2 or 3, `header_size` is not 32, `pixel_format` is
  not 1, `method` is not 0, 1, 2, 3, 4, 5, or 6, or `method` is 4, 5, or 6 in
  a version 2 file (unsupported);
- `header_crc32` or `payload_crc32` does not match, `reserved` is not 0, or
  `payload_size` is not `file_size - 32`;
- a dimension is 0 (malformed) or outside the limits (size limit);
- method 0 has `payload_size != width * height * 3`;
- method 1 in version 2 has a payload shorter than 12 bytes, or in version 3
  has an empty payload, `S < 1`, `S > 4`, `S > height`, or a payload shorter
  than `1 + 12 * S` bytes; or in either version has a stream shorter than 5
  bytes, stream sizes that do not add up to `payload_size`, a stream that runs
  out or has bytes left, or a decoded residual of +128;
- method 2 or 3 has a payload shorter than 6 bytes, `count - 1 > 255`, a
  payload shorter than `6 + 3 * count`, a duplicate colour in the palette, a
  stream shorter than 5 bytes, `6 + 3 * count + stream_size != payload_size`, a
  decoded literal index `>= count` or equal to an excluded candidate (`a`, `b`,
  or `d` in method 2; `a`, `b`, or `e` in method 3), or a stream
  that runs out or has bytes left;
- method 4 has a payload shorter than 8 bytes, a `kR`, `kG` or `kB` above 8,
  a palette field that needs a bit past the end of the payload, a `rice`
  quotient above 511, an `R`, `G` or `B` outside `0..255`, an entry not
  strictly greater than the entry before it, a non-zero padding bit, fewer
  than 5 bytes after the palette field, a decoded literal index `>= count` or
  equal to an excluded candidate (`a`, `b`, or `e`), or a stream that runs out
  or has bytes left;
- method 5 has a payload shorter than 5 bytes, a length symbol above 23, a
  decoded copy length above 4096 or extending past `width * height`, a
  distance symbol above 39, a copy distance above `p`, a colour-cache index
  naming an empty slot, or a stream that runs out or has bytes left;
- method 6 has a payload shorter than 5 bytes, a length symbol above 23 on
  `len_tree[0]` or `len_tree[1]`, a decoded copy length above 4096 or
  extending past `width * height`, a same-row distance symbol above 39 or
  `dx > x` or `dx > 2^20 - 1`, a vertical offset symbol above 39 or `dy > y`
  or `dy > 2^20 - 1` (including via `rep_dy`), a horizontal offset symbol
  above 49, a source column `x - dx` outside `0 .. width - 1`, a 2D copy
  distance `dy * width + dx` above `2^20 - 1`, an `mtf_tree` index
  `>= mtf_count`, a `cache_tree` index naming an empty `C2` slot, or a stream
  that runs out or has bytes left.

The reference tool writes outputs to `destination.partial` with `CREATE_NEW`,
flushes, and renames onto the destination only after success. An existing
destination or temporary file is never touched.

## Reference encoder policy (non-normative)

The encoder codes the image with method 1 using `S = 3` strips when
`height >= 768`, `S = 2` when `512 <= height < 768`, and `S = 1` when
`height < 512` (kept when every strip stream `(s, c)` is at most
`width * h_s + 64 / S` bytes) and, when the image has at most
256 distinct colours, also with method 4, choosing each of `kR`, `kG` and `kB`
as the smallest value in `0..8` that gives its channel the fewest palette
field bits. It writes whichever eligible candidate
of method 0 (raw), method 1, and method 4 has the smallest payload, breaking
ties in order method 0, then method 1, then method 4. Next, unless method 4
produced an eligible candidate with `count <= 16` (`count - 1 < 16`, so the
entire colour alphabet already fits within `mtf`'s 16-entry capacity), it runs
the method 6 backward-reference parse across the image: pixel `0` is coded as a
literal (`wg_ctx = 4`), and at each pixel `p >= 1` (`limit = min(4096, width * height - p)`)
it evaluates the inherited candidate list `cands(p)` (`L >= 2`, picking the
first candidate on length ties -> `(best_inh_len, best_inh_idx, best_inh_dist)`)
and the 4-pixel general search (`1`, `width` when `p >= width` and
`width <= 2^20 - 1`, and up to 64 positions from the `2^16`-bucket FNV-1a hash
chain of the 12-byte 4-pixel key within distance `2^20 - 1`, `L >= 4`, picking
the smaller distance on length ties -> `(best_gen_len, best_gen_dist)`),
inserting every pixel `q` with `q + 4 <= width * height` into its bucket chain.
When `best_inh_len >= 2` and (`best_gen_len < 4` or
`best_inh_len + 1 >= best_gen_len`), it codes an inherited copy
`(best_inh_len, best_inh_dist)`; else when `best_gen_len >= 4`, it codes
`(best_gen_len, best_gen_dist)` as an inherited copy if
`best_gen_dist in cands(p)` and as a non-inherited 2D `(dy, dx)` copy
otherwise; else it codes `EQ_W`, else `EQ_N`, else an `mtf` hit, else a `C2`
cache hit, else a literal. When copies of length at least 4 (`L >= 4`,
inherited or non-inherited) cover at least half the pixels
(`covered4 * 2 >= width * height`) and the resulting method 6 payload is
strictly smaller than the chosen method 0/1/4 payload, the encoder writes
method 6 instead. It never writes method 2, method 3, or method 5. The `3 * S`
streams of method 1 depend only on the warm-start handoff rows and same-pixel
channel values, and methods 0, 4, and 6 are single-stream and deterministic, so
any thread count produces the same bytes.

## BMP subset accepted by the reference tool

`rasterfold encode` accepts uncompressed 24-bit `BITMAPINFOHEADER` files:
`bfType == "BM"`, `biSize == 40`, `biPlanes == 1`, `biBitCount == 24`,
`biCompression == 0`, `biWidth` in 1..32768, `biHeight` non-zero and not
`0x80000000` with `|biHeight|` in 1..32768, at most 33554432 pixels,
`bfOffBits >= 54` with `bfOffBits + row_stride * |height|` bytes present,
`bfSize == 0` or `pixel_end <= bfSize <= file_size`, and `biSizeImage == 0` or
`biSizeImage >= row_stride * |height|`. Row stride is `(3 * biWidth + 3) & ~3`.
A positive height is bottom-up, a negative one top-down. Padding bytes, the
bytes between offset 54 and `bfOffBits`, and the reserved and resolution
fields are ignored.

`rasterfold decode` writes a canonical bottom-up 24-bit BMP: a 54-byte header,
`bfOffBits = 54`, `biSizeImage = row_stride * height`, 3780 pixels per metre,
zero padding. Lossless means the width, height and RGB samples match.
