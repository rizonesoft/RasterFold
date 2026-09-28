# RasterFold image format, experimental version 1

Status: experimental. Version 1 is allowed to change incompatibly until the
magic byte is revised. This document is the contract for `.rfi` files and for
the BMP subset the reference tool accepts.

A second implementation can decode a file from this text alone. The reference
encoder's mode choice is deliberately not part of the contract.

## Conventions

- Multi-byte integers are unsigned and little-endian, unless a field is
  explicitly described as signed.
- `u8`, `u16`, and `u32` name those widths.
- Byte offsets are from the start of the file or the start of the region being
  defined.
- "Mod 256" means the low 8 bits of an 8-bit two's-complement sum or
  difference. It is the result of an x86 `ADD` or `SUB` on a byte register.
- No field is trusted. Dimensions, sizes, offsets, mode bytes, palette
  indexes, and compressed bytes are bounds-checked before use.
- The reference limits below are part of version 1. A file outside them is
  rejected even if the rest of the framing is well formed.

## File layout

```
offset  size  field
0       4     magic = 52 46 49 01   ('R','F','I', 0x01)
4       2     version = 1
6       2     header_size = 32
8       4     width
12      4     height
16      2     block_size = 64
18      2     pixel_format = 1
20      4     payload_size
24      4     payload_crc32
28      4     header_crc32
32      N     payload, N = payload_size
```

The header is exactly 32 bytes. `header_size` exists so a later version can
grow the header; a version-1 decoder rejects every other value.

`pixel_format = 1` means 8-bit RGB, channels stored R then G then B, with no
alpha and no colour-space metadata. Channel values are the BMP byte values
after the BGR-to-RGB swap. The codec does not interpret gamma or an ICC
profile.

There is no padding between the header and the payload, and no trailing bytes
after the payload. `payload_size` must equal `file_size - 32`.

### Magic and version

The fourth magic byte and the `version` field both identify experimental
version 1. A decoder rejects the file when they disagree, when the magic is
not `52 46 49 01`, or when `version` is not 1.

### Dimensions

```
1 <= width  <= 32768
1 <= height <= 32768
width * height <= 33554432
```

The pixel product is computed in 64-bit arithmetic. A product outside the
limit, or a dimension of 0, is rejected. The reference tool also rejects an
input file larger than 256 MiB (268435456 bytes).

## Checksums

Both checksums are CRC-32 as used by zlib, PNG, and Ethernet (ISO-HDLC):

- reflected polynomial `0xEDB88320`
- initial register `0xFFFFFFFF`
- final XOR `0xFFFFFFFF`
- no reflection beyond the usual reflected-polynomial formulation

For each input byte `b`:

```
crc = table[(crc xor b) and 0xFF] xor (crc >> 8)
```

`>>` is a logical shift. The table entry `i` is `i`, reflected eight times by
the same polynomial. The well-known check value is mandatory:

```
CRC-32("123456789") = 0xCBF43926
```

Coverage:

- `payload_crc32` is the CRC-32 of the payload only, the `payload_size` bytes
  that begin at file offset 32. It does not cover the header.
- `header_crc32` is the CRC-32 of file bytes `[0, 28)`. That range includes
  `payload_crc32` and excludes `header_crc32` itself.

A mismatch in either checksum rejects the file. The decoder does not repair
or ignore a bad checksum.

## Blocks

Pixels are partitioned into independent square blocks of 64 by 64, in row-major
order, left to right, then top to bottom. Edge blocks are smaller. Block
`bx, by` covers:

```
x0 = bx * 64
y0 = by * 64
bw = min(64, width  - x0)
bh = min(64, height - y0)
blocks_x = ceil(width  / 64)
blocks_y = ceil(height / 64)
```

`blocks_x * blocks_y` blocks appear in the payload in that order. The file
does not store `bx` or `by`; the position is implied by the block's ordinal.
Prediction, runs, and palettes do not cross a block boundary.

Each block is:

```
offset  size  field
0       1     mode
1       1     flags
2       4     data_size
6       M     data, M = data_size
```

`flags` must be 0 in version 1. `data_size` is the number of bytes after the
6-byte block header. The sum of `6 + data_size` over every block must equal
`payload_size` exactly. A short final block, an unknown mode, a non-zero
flags byte, or bytes left over after the last block rejects the file.

`data_size` is untrusted. `6 + data_size` is computed in 64-bit arithmetic and
rejected when it would pass the end of the payload. A decoder must not
allocate a buffer whose size is an unchecked `data_size`. The reference
decoder uses fixed scratch sized for one 64×64 RGB block.

Inside a block, pixels are visited row by row, top to bottom, left to right.
A linear RGB stream is `bh` rows of `bw` pixels, each pixel the three bytes
`R, G, B`. Its length is `bw * bh * 3`. A linear index stream has length
`bw * bh`.

## Modes

### Mode 0, raw RGB

`data_size` must equal `bw * bh * 3`. `data` is the linear RGB stream. No
padding bytes are present.

### Mode 1, PackBits of RGB

`data` is a PackBits stream that decodes to exactly the linear RGB stream of
mode 0. The stream must consume every input byte.

### Mode 2, subtract-green, left residual, PackBits

Each pixel is transformed, then predicted from its left neighbour inside the
block, then the residual stream is PackBits-coded.

Subtract-green, per pixel:

```
G' = G
R' = (R - G) mod 256
B' = (B - G) mod 256
```

Left predictor, per channel, for local column `lx` in `0 .. bw-1`. Rows do
not share predictor state. Blocks do not share predictor state. There is no
above predictor in version 1.

```
if lx == 0:
    pred = 0
else:
    pred = the transformed channel of the pixel at lx - 1 in the same row

residual = (transformed - pred) mod 256
```

Residuals are stored as three planes, not interleaved. The plane order is
R', then G', then B'. Each plane is `bh` rows of `bw` residuals, top to
bottom, left to right. The coded byte stream is the concatenation of the
three planes, length `bw * bh * 3`. One PackBits stream covers that entire
concatenation, so a run may cross a plane boundary. PackBits must decode to
exactly `bw * bh * 3` bytes.

Planar order is what makes a constant per-channel residual compressible: a
horizontal gradient becomes a run of equal bytes in one plane. An interleaved
`R', G', B'` stream would break those runs.

Reconstruction:

```
if lx == 0:
    transformed = residual
else:
    transformed = (residual + left_transformed) mod 256

G = G'
R = (R' + G) mod 256
B = (B' + G) mod 256
```

`left_transformed` is the reconstructed R', G', B' of the previous pixel in
the row, not that pixel's residual.

Worked example, one row, two identical pixels `(R,G,B) = (10, 20, 30)`:

```
transformed pixel 0 = (246, 20, 10)
residual    pixel 0 = (246, 20, 10)     because pred = 0
residual    pixel 1 = (0, 0, 0)
```

Planar byte stream before PackBits: `F6 00 14 00 0A 00`.

A second row starts again with `pred = 0`. It must not continue from the last
pixel of the previous row.

### Mode 3, palette plus PackBits indexes

```
offset  size                 field
0       1                    colour_count_minus_one
1       colour_count * 3    palette entries, R,G,B in index order
rest    remaining bytes     PackBits of one index byte per pixel
```

`colour_count = colour_count_minus_one + 1`, so the count is in `1 .. 256`
and an empty palette cannot be encoded. The palette occupies
`colour_count * 3` bytes. The remaining bytes, which may be empty only if
`bw * bh = 0` (impossible for a legal block), are a PackBits stream that
decodes to exactly `bw * bh` index bytes.

An index `i` must satisfy `i < colour_count`. The pixel is
`palette[i * 3 .. i * 3 + 3]`. Palette entries need not be unique for a
decoder to accept the block. The reference encoder emits each distinct colour
once, in first-seen scan order, and uses a palette only when the block has
at most 256 distinct colours.

### Mode 4, subtract-green, MED, Rice

The block data is one LSB-first bit stream and nothing else. A partial final
byte stores the unused high bits as zero. The decoder rejects a stream that
ends early, has a non-zero unused bit, or has unread bytes.

Samples are visited in three planes, G then R' then B', each plane row by
row. The colour transform is the same subtract-green as mode 2:

```
G' = G
R' = (R - G) mod 256
B' = (B - G) mod 256
```

Each sample is predicted with the MED predictor from already reconstructed
samples of that same plane. Neighbours outside the image are 0. Neighbours
outside this block are the reconstructed samples of earlier blocks. `L` is
the left sample, `U` the sample above, and `UL` the sample above-left.

```
if UL >= L and UL >= U: pred = min(L, U)
else if UL <= L and UL <= U: pred = max(L, U)
else: pred = L + U - UL
pred = min(255, max(0, pred))
```

The residual is the wrapped difference, then signed:

```
err = (sample - pred) mod 256
if err >= 128: err = err - 256
```

Reconstruction is `(pred + err) mod 256`, then the inverse of subtract-green.

The signed residual is mapped to an integer `m` in `0 .. 255`:

```
if err >= 0: m = err * 2
else:        m = (-err) * 2 - 1
```

Context index, from the signed residuals `Eleft` and `Eup` of the left and
above samples (0 when that neighbour is outside the image):

```
Q(0) = 0
Q(1) = 1
Q(2) = 2
Q(3) or Q(4) = 3
Q(5) through Q(8) = 4
Q(9) through Q(16) = 5
Q(17) through Q(32) = 6
Q(33) and above = 7
context = Q(abs(Eleft)) * 8 + Q(abs(Eup))
```

`Eleft` and `Eup` are the MED residuals of those neighbours, not a second
coding. Each channel has its own 64 contexts. At the start of the file every
accumulator `A` is 4 and every count `N` is 1. A mode other than 4 does not
change the model. Mode 5 does not change it either. Each mode-4 block, in
payload order, updates the model after every residual:

```
k = 0
while k < 8 and (N << k) < A: k = k + 1
q = m >> k
remainder = m & ((1 << k) - 1)
```

The stream then receives `q` one-bits, one zero-bit, and `remainder` in `k`
bits, least significant bit first. After the symbol:

```
A = A + abs(err)
N = N + 1
if N >= 32:
    A = A >> 1
    N = N >> 1
    if N < 1: N = 1
```

A quotient above 255, or a mapped value above 255, rejects the block.
Inverse subtract-green is

```
G = G'
R = (R' + G) mod 256
B = (B' + G) mod 256
```

### Mode 5, subtract-green, MED, binary rANS

The colour transform, visit order, and MED predictor are the same as mode 4.
The coded symbol is the same mapped residual `m` in `0 .. 255`. What changes
is the context and the entropy coder.

Neighbours outside the image are 0. The above-right neighbour is also 0 when
it belongs to a block that has not been decoded yet: on every row of a block
except the first, the sample just past the block's right edge. `W`, `N`,
`NW`, and `NE` are reconstructed samples of the plane being coded.

```
Q(d) = -4 when d <= -21
       -3 when -20 <= d <= -7
       -2 when -6 <= d <= -3
       -1 when d is -2 or -1
        0 when d = 0
        1 when d is 1 or 2
        2 when 3 <= d <= 6
        3 when 7 <= d <= 20
        4 when d >= 21

j = (Q(NE - N) + 4) * 81 + (Q(N - NW) + 4) * 9 + (Q(NW - W) + 4)
```

For the green plane the context index is `j` (`0 .. 728`). For R' and B' it
also mixes the green MED residual `eG` at the same pixel, which is known
because green is coded first:

```
g = Q(eG) + 4
index = 729 + (channel - 1) * 6561 + j * 9 + g
```

`channel` is 1 for R' and 2 for B'. There are 13851 contexts in total. Each
context owns 256 binary-tree nodes. Node 1 is the root. A symbol's eight bits
are coded MSB first, and the node for the next bit is `node * 2 + bit`.

Each node stores a probability `p0` in `1 .. 4095`, the chance of bit 0 out
of 4096. Every node starts at 2816. After the bit is coded:

```
if bit = 0: p0 = min(4095, p0 + ((4096 - p0) >> 4))
else:       p0 = max(1, p0 - (p0 >> 4))
```

A mode other than 5 does not change these probabilities. The probabilities
are updated for a mode-5 block only when that block is the one written.

The block data is one rANS byte stream, state `L = 2^23`, scale 12. The
encoder writes bytes backward and finishes by storing the 32-bit state
little-endian at the front of the buffer. The decoder loads that state and
then reads forward. For a bit with probability `p0`:

```
if bit = 0: cum = 0,          freq = p0
else:       cum = p0,         freq = 4096 - p0
```

Encoding renorms while `state >= freq * 2^19`, emitting the low byte each
time, then sets:

```
state = (state / freq) * 4096 + (state mod freq) + cum
```

Decoding sets:

```
state = freq * (state >> 12) + (state mod 4096) - cum
```

and refills a byte from the stream while `state < L`. Division and remainder
are unsigned. After the last symbol the state must be `L` again and the
decoder must have consumed every byte. Anything else rejects the block. A
stream shorter than 4 bytes is rejected.

Inverse subtract-green is the same as mode 4.

## PackBits

This is Apple PackBits with one extra rejection: the no-op marker is illegal.

```
in_pos  = 0
out_pos = 0
while in_pos < encoded_length:
    control = encoded[in_pos]          # unsigned
    in_pos += 1
    if control == 0x80:
        reject
    if control <= 127:
        count = control + 1            # 1..128 literal bytes
        copy encoded[in_pos .. in_pos+count] to the output
        in_pos += count
    else:
        count = 257 - control          # control 0x81..0xFF => 128..2 copies
        value = encoded[in_pos]
        in_pos += 1
        append `value` to the output `count` times
    if an input or output bound would be exceeded:
        reject
if out_pos != expected_length or in_pos != encoded_length:
    reject
```

`expected_length` is `bw * bh * 3` for modes 1 and 2, and `bw * bh` for mode
3. A stream that decodes short, long, or with bytes left over is malformed.
The reference encoder never emits `0x80`.

Repeat-run marker examples:

| run length | control byte |
|------------|--------------|
| 2          | 0xFF         |
| 3          | 0xFE         |
| 128        | 0x81         |

Literal marker: a run of `n` literal bytes (`1 <= n <= 128`) is stored as the
byte `n - 1` followed by those bytes.

Reference encoder, non-normative but implemented by the tool and the test
oracle: at each input position, measure the following run of identical bytes,
capped at 128. If the run length is at least 2, emit one repeat packet and
advance by that length. Otherwise gather a literal packet of at most 128
bytes, stopping before a run of three or more identical bytes so that the
outer rule can emit it as a repeat. A run of exactly two identical bytes is
absorbed into a surrounding literal when it is encountered in the middle of
that gather; it is emitted as a repeat only when the outer rule starts on it.

## Reference encoder policy (non-normative)

For each block the reference encoder tries the applicable modes and writes the
one with the smallest `data_size`. Ties keep the lowest mode number, so raw
wins a tie. Palette is skipped when the block has more than 256 colours. A
PackBits result and a mode-4 Rice result are used only when strictly smaller
than the current best size. A mode-5 rANS body is written whenever it is
strictly smaller than the raw block, even if another mode is smaller, so the
probability model is not interrupted. The Rice model is updated only for a
mode-4 block that is actually written, and the rANS model only for a mode-5
block that is actually written. Trying mode 5 after a successful mode-4 trial
rolls the Rice model back when mode 5 wins. This policy is not a decoder
requirement. Any valid mode may be selected per block.

## Rejection rules

Reject, and do not produce a completed output file, when any of the following
is true:

- The file is shorter than 32 bytes, or longer than 256 MiB.
- Magic, version, `header_size`, `block_size`, or `pixel_format` is not the
  version-1 value. A wrong version, header size, block size, or pixel format
  is an unsupported-file error. A wrong magic, a checksum mismatch, or broken
  framing is a malformed-file error.
- `width` or `height` is outside the limits above, or `width * height`
  overflows the pixel limit.
- `payload_size` does not equal `file_size - 32`, or either checksum mismatches.
- A block header is truncated, `flags` is not 0, `mode` is not 0..5, or
  `data_size` runs past the payload.
- A mode-4 Rice stream ends early, leaves a non-zero unused bit, leaves
  unread bytes, or produces a quotient or mapped residual above 255.
- A mode-5 rANS stream is shorter than 4 bytes, ends early, finishes with a
  state other than `2^23`, or leaves unread bytes.
- A decoded block does not produce exactly the expected number of bytes, a
  PackBits marker is `0x80`, a palette index is out of range, or the payload
  does not end on a block boundary after `blocks_x * blocks_y` blocks.
- An arithmetic step on a size, offset, or count overflows.

The reference tool writes an output by creating a sibling temporary file
(`destination + ".partial"`) with `CREATE_NEW`, flushing it, and renaming it
onto the destination only after the bytes are complete. An existing
destination is left untouched. An existing temporary name is not overwritten.
A failed encode or decode deletes the temporary file and does not create the
destination.

## BMP subset accepted by the reference tool

The codec stores pixels, not BMP headers. `rasterfold encode` accepts only
this subset. Anything else is rejected with an explicit error.

Required:

- `bfType == "BM"`.
- `biSize == 40` (`BITMAPINFOHEADER` only; OS/2, BITMAPV4, and BITMAPV5 are
  rejected).
- `biPlanes == 1`, `biBitCount == 24`, `biCompression == 0` (`BI_RGB`).
- `biWidth` in `1 .. 32768`. `biHeight` non-zero, not `0x80000000`, and
  `abs(biHeight)` in `1 .. 32768`.
- The pixel product is within the limit above.
- `bfOffBits >= 54`, and the file contains `bfOffBits + row_stride * abs(height)`
  bytes.
- `bfSize == 0`, or `pixel_end <= bfSize <= file_size`.
- `biSizeImage == 0`, or `biSizeImage >= row_stride * abs(height)`.

Row stride is `((biWidth * 3 + 3) & ~3)`. Padding bytes are ignored. Pixel
bytes in the file are `B, G, R`. A positive height is bottom-up. A negative
height is top-down. Bytes between offset 54 and `bfOffBits`, and
`bfReserved1` / `bfReserved2`, are ignored. `biClrUsed` and the resolution
fields are ignored.

`rasterfold decode` writes a canonical bottom-up 24-bit BMP:

- 54-byte `BITMAPFILEHEADER` + `BITMAPINFOHEADER`
- positive height, `biCompression = 0`, `biBitCount = 24`, `biPlanes = 1`
- `bfOffBits = 54`
- `biSizeImage = row_stride * height`, `bfSize = 54 + biSizeImage`
- `biXPelsPerMeter = biYPelsPerMeter = 3780`
- padding bytes set to 0
- rows stored bottom-up, channels `B, G, R`

Lossless means the decoded width, height, and RGB samples match the source.
Headers, metadata, and padding bytes are not preserved byte for byte.
