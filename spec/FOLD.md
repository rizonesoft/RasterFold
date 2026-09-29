# RasterFold image format, version 2

This document is the contract for `.fold` files and for the BMP subset the
reference tool accepts. A second implementation can decode a file from this
text alone; `tests/rfi_ref.py` is such an implementation, written from it.
The reference encoder's choice between methods is not part of the contract.

Version 2 replaces the experimental version 1 (64×64 blocks with PackBits,
palette, Rice and binary rANS modes). Version 1 files are rejected as an
unsupported version.

## Conventions

- Multi-byte integers are little-endian.
- Integer arithmetic is exact, on signed integers wide enough that nothing
  overflows unless a step says otherwise. `a >> n` is an arithmetic shift: the
  floor of `a / 2^n`, also for negative `a`. `a / b` appears only for
  non-negative operands and is truncating integer division.
- `clamp(v, lo, hi)` is `lo` if `v < lo`, `hi` if `v > hi`, otherwise `v`.
- `sign(v)` is -1, 0 or 1. `ilog2(v)` for `v >= 1` is the index of the highest
  set bit (`ilog2(1) = 0`).
- No field is trusted. Every size and count is checked before use.

## File layout

```
offset  size  field
0       4     magic = 52 46 49 02   ('R','F','I', 0x02)
4       2     version = 2
6       2     header_size = 32
8       4     width
12      4     height
16      1     method: 0 = raw, 1 = CM
17      1     reserved = 0
18      2     pixel_format = 1
20      4     payload_size
24      4     payload_crc32
28      4     header_crc32
32      N     payload, N = payload_size
```

`pixel_format = 1` is 8-bit RGB with no alpha and no colour-space metadata.
Samples are the BMP byte values after the BGR-to-RGB swap.

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
```

### Per-channel state

Each channel keeps, for two rows (`R = y mod 2`), arrays over positions
`0 .. width+1`: the four sub-predictor errors `perr[k]`, the predictor error
`err`, the residual `er` and the final error `fe`. All start at 0. Position `x`
of row `y` is written when sample `(x, y)` is coded. `cur` names row `y mod 2`
and `prv` names row `(y - 1) mod 2`; at `y = 0` the `prv` row is all zeros
except for the accumulation described in "Updates". (This is the layout of the
JPEG XL weighted predictor; `prv` of row 0 is the same array that row 1 will
overwrite as `cur`.)

### Weighted predictor

This is the JPEG XL self-correcting predictor with its default parameters,
computed in units of 1/8.

```
pN  = position x of prv
pNE = position x+1 of prv if x+1 < width else pN
pNW = position x-1 of prv if x > 0 else pN

errw(e, m): s = max(0, ilog2(e + 1) - 5)
            return 4 + ((m * DIVL[e >> s]) >> s)
            where DIVL[i] = 2^24 / (i + 1)

wt_k = errw(perr_k[pN] + perr_k[pNE] + perr_k[pNW], m_k),  m = (13, 12, 12, 12)

teW  = err[cur, x-1] if x > 0 else 0
teN  = err[pN],  teNW = err[pNW],  teNE = err[pNE]
n8, w8, ne8 = 8N, 8W, 8NE
pr_0 = w8 + ne8 - n8
pr_1 = n8 - (((teN + teW + teNE) * 16) >> 5)
pr_2 = w8 - (((teN + teW + teNW) * 10) >> 5)
pr_3 = n8 - (((teNW + teN + teNE) * 7) >> 5)

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
-2047..2047. The channel has twelve weights `u_k`, all 0 at the start.

```
corr  = clamp((sum_k u_k * x_k) >> 16, -256, 256)
pred8 = wpred + corr
pred  = (pred8 + 4) >> 3
predv = clamp(pred + b_c, 0, 255)
```

### Residual

```
r = (v_c - predv) mod 256;  if r >= 128: r = r - 256        (r in -128..127)
decoder: v_c = (predv + r) mod 256
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
scl = (E >> 2) + 2

count(a, t...) = number of thresholds t_i with a >= t_i
q9(v)  = 4 + sign(v) * count(|v|, 1, 2, 4, 8)
q7(v)  = 3 + sign(v) * count(|v|, 1, 3, 7)
qj(v)  = 4 + sign(v) * count(|v|, 1, 3, 7, 21)
qn(v)  = 3 + sign(v) * count(6|v|, scl, 3 scl, 7 scl)
med(a, b, c) = min(a, b) if c >= max(a, b); max(a, b) if c <= min(a, b); else a + b - c
gq = q9(EG) for channels 1, 2, else 4
rq = q9(ER) for channel 2, else 4

k0 = ((((qn(W-pred)*7 + qn(N-pred))*7 + qn(NE-pred))*7 + qn(NW-pred))*3 + band
k1 = qE*81 + gq*9 + rq
k2 = (qj(NE-N)*81 + qj(N-NW)*9 + qj(NW-W))*5 + (gq+1)/2
k3 = ((q7(med(W,N,NW)-pred)*7 + q7(W+N-NW-pred))*7 + q7(((W+NE+1)>>1)-pred))*7 + q7(N+NE-NNE-pred)
k4 = (q9(W-pred)*9 + q9(N-pred))*16 + qE
row_m = k_m mod 2048                                   for model m = 0..4
wsel = qE*8 + j,  j = 3 for channel 0; for channels 1, 2 j = 0 if gq = 4,
       else 1 if |EG| <= 3, else 2
```

### Models

Each channel has five tables of 2048 rows by 32 16-bit cells. Cell 0 of a row
is its visit count `n`, starting at 0. Cells 1..31 are probabilities of a 1 bit
in units of 1/65536, starting at 32768. Before the sample is coded, for each
model m in order 0..4:

```
n = cell 0 of row_m
rate_m = 65536 * 2 / (2n + 3)
cell 0 of row_m = n + 1 if n < 60 else n
```

Each channel has 128 × 32 weight sets of six signed 32-bit weights, all
`19661` except the sixth, which starts at 0. The set for a decision is
`(wsel, node)`.

### One decision

A decision codes one bit at node `d` (1..29) with the five cells
`c_m = cell d of row_m` and the weight set `w = (wsel, d)`:

```
STRETCH, SQUASH: see below
s_m = STRETCH[c_m >> 4]                                  (m = 0..4)
x   = clamp((sum_m w_m * s_m + w_5 * 64) >> 16, -2047, 2047)
p   = SQUASH(x)                                          (1..4095)
code the bit with probability p/4096 of a 1 (range coder below)
e   = 4096*bit - p
w_m = w_m + ((s_m * e + 4096) >> 13)       for m = 0..4, in 32-bit two's complement
w_5 = w_5 + ((64 * e + 4096) >> 13)        likewise
c_m = c_m + (((65535*bit) - c_m) * rate_m) >> 16          for m = 0..4
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

```
node 1   bit r != 0                         (stop here when 0)
node 2   bit r < 0
a = |r|  (1..128), e = ilog2(a)
nodes 3 .. 3+e-1: bit 1 each;  node 3+e: bit 0, only when e < 7
mantissa m = a - 2^e, its e bits from the top:
    bit e-1 at node 10 + e
    bit e-2 at node 14 + 2e + (bit e-1)
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

The G residual and final error at this pixel are what R and B read as `EG`
and `FG`; those of R are `ER` and `FR`. `FG` and `FR` are clamped to
-2047..2047. A decoder that produced `r = 128` passes it on as -128; the file is
rejected anyway.

## Rejection rules

Reject, and do not produce a completed output file, when:

- the file is shorter than 32 bytes or longer than 256 MiB;
- the magic is not `RFI` (malformed), or the fourth magic byte and `version`
  are not 2, `header_size` is not 32, `pixel_format` is not 1, or `method` is
  not 0 or 1 (unsupported);
- `header_crc32` or `payload_crc32` does not match, `reserved` is not 0, or
  `payload_size` is not `file_size - 32`;
- a dimension is 0 (malformed) or outside the limits (size limit);
- method 0 has `payload_size != width * height * 3`;
- method 1 has a payload shorter than 12 bytes, a stream shorter than 5 bytes,
  stream sizes that do not add up, a stream that runs out or has bytes left, or
  a decoded residual of +128.

The reference tool writes outputs to `destination.partial` with `CREATE_NEW`,
flushes, and renames onto the destination only after success. An existing
destination or temporary file is never touched.

## Reference encoder policy (non-normative)

The encoder codes the image with method 1 and keeps it when the payload is
smaller than the raw image; otherwise it writes method 0. The streams of the
three channels are independent of the schedule, so one thread or three produce
the same bytes.

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
