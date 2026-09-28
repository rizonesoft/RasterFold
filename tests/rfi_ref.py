"""Independent RasterFold version-1 codec and 24-bit BMP helpers.

This module is a test oracle. It is not the codec implementation.
"""

from __future__ import annotations

import array
import struct
import zlib

MAGIC = b"RFI\x01"
BLOCK = 64
MAX_DIM = 32768
MAX_PIXELS = 32 * 1024 * 1024
HEADER = 32


class FormatError(Exception):
    pass


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def packbits_encode(src: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        run = 1
        while run < 128 and i + run < n and src[i + run] == src[i]:
            run += 1
        if run >= 2:
            out.append((257 - run) & 0xFF)
            out.append(src[i])
            i += run
            continue
        start = i
        lit = 0
        while i < n and lit < 128:
            if i + 2 < n and src[i] == src[i + 1] == src[i + 2]:
                break
            i += 1
            lit += 1
        if lit == 0:
            raise RuntimeError("packbits literal gather produced an empty run")
        out.append(lit - 1)
        out.extend(src[start : start + lit])
    return bytes(out)


def packbits_decode(src: bytes, expected: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(src):
        control = src[i]
        i += 1
        if control == 0x80:
            raise FormatError("packbits nop")
        if control <= 127:
            count = control + 1
            if i + count > len(src) or len(out) + count > expected:
                raise FormatError("packbits literal overrun")
            out.extend(src[i : i + count])
            i += count
        else:
            count = 257 - control
            if count < 2 or count > 128 or i >= len(src) or len(out) + count > expected:
                raise FormatError("packbits repeat overrun")
            out.extend(bytes([src[i]]) * count)
            i += 1
    if i != len(src) or len(out) != expected:
        raise FormatError("packbits length")
    return bytes(out)


def predict(rgb: bytes, bw: int, bh: int) -> bytes:
    npix = bw * bh
    planes = [bytearray(npix), bytearray(npix), bytearray(npix)]
    i = 0
    for _y in range(bh):
        left = [0, 0, 0]
        for _x in range(bw):
            r, g, b = rgb[i * 3 : i * 3 + 3]
            rp = (r - g) & 255
            gp = g & 255
            bp = (b - g) & 255
            planes[0][i] = (rp - left[0]) & 255
            planes[1][i] = (gp - left[1]) & 255
            planes[2][i] = (bp - left[2]) & 255
            left = [rp, gp, bp]
            i += 1
    return bytes(planes[0] + planes[1] + planes[2])


def unpredict(residual: bytes, bw: int, bh: int) -> bytes:
    npix = bw * bh
    if len(residual) != npix * 3:
        raise FormatError("residual length")
    pr = residual[0:npix]
    pg = residual[npix : npix * 2]
    pb = residual[npix * 2 :]
    out = bytearray()
    i = 0
    for _y in range(bh):
        left = [0, 0, 0]
        for _x in range(bw):
            rp = (pr[i] + left[0]) & 255
            gp = (pg[i] + left[1]) & 255
            bp = (pb[i] + left[2]) & 255
            out.append((rp + gp) & 255)
            out.append(gp)
            out.append((bp + gp) & 255)
            left = [rp, gp, bp]
            i += 1
    return bytes(out)


RICE_RESET = 32


def _qb(mag: int) -> int:
    if mag <= 1:
        return mag if mag > 0 else 0
    if mag <= 2:
        return 2
    if mag <= 4:
        return 3
    if mag <= 8:
        return 4
    if mag <= 16:
        return 5
    if mag <= 32:
        return 6
    return 7


def _med(left: int, up: int, up_left: int) -> int:
    if up_left >= left and up_left >= up:
        pred = left if left < up else up
    elif up_left <= left and up_left <= up:
        pred = left if left > up else up
    else:
        pred = left + up - up_left
    if pred < 0:
        return 0
    if pred > 255:
        return 255
    return pred


def _delta(value: int, pred: int) -> int:
    err = (value - pred) & 255
    if err >= 128:
        return err - 256
    return err


def _rice_k(acc: int, count: int) -> int:
    k = 0
    while k < 8 and (count << k) < acc:
        k += 1
    return k


def _to_m(err: int) -> int:
    if err >= 0:
        return err << 1
    return ((-err) << 1) - 1


def _from_m(mapped: int) -> int:
    if mapped & 1:
        return -((mapped + 1) >> 1)
    return mapped >> 1


class RiceState:
    def __init__(self) -> None:
        self.acc = [[4] * 64 for _ in range(3)]
        self.count = [[1] * 64 for _ in range(3)]


class _RiceWriter:
    def __init__(self, limit: int) -> None:
        self.acc = 0
        self.n = 0
        self.out = bytearray()
        self.limit = limit
        self.over = False

    def bit(self, value: int) -> None:
        if self.over:
            return
        if value:
            self.acc |= 1 << self.n
        self.n += 1
        if self.n == 8:
            self.out.append(self.acc)
            self.acc = 0
            self.n = 0
            if len(self.out) >= self.limit:
                self.over = True

    def bits(self, value: int, width: int) -> None:
        for shift in range(width):
            self.bit((value >> shift) & 1)

    def finish(self) -> bytes | None:
        if self.over:
            return None
        if self.n:
            self.out.append(self.acc)
        if len(self.out) >= self.limit:
            return None
        return bytes(self.out)


class _RiceReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.i = 0
        self.acc = 0
        self.n = 0

    def bit(self) -> int:
        if self.n == 0:
            if self.i >= len(self.data):
                raise FormatError("rice")
            self.acc = self.data[self.i]
            self.i += 1
            self.n = 8
        value = self.acc & 1
        self.acc >>= 1
        self.n -= 1
        return value

    def bits(self, width: int) -> int:
        value = 0
        for shift in range(width):
            if self.bit():
                value |= 1 << shift
        return value

    def finish(self) -> None:
        if self.acc != 0 or self.i != len(self.data):
            raise FormatError("rice")


def _channel_rgb(rgb: bytes | bytearray, width: int, height: int, x: int, y: int, channel: int) -> int:
    if x < 0 or y < 0 or x >= width or y >= height:
        return 0
    base = (y * width + x) * 3
    red = rgb[base]
    green = rgb[base + 1]
    blue = rgb[base + 2]
    if channel == 0:
        return green
    if channel == 1:
        return (red - green) & 255
    return (blue - green) & 255


def _residual_at(sample, x: int, y: int) -> int:
    if x < 0 or y < 0:
        return 0
    value = sample(x, y)
    left = sample(x - 1, y) if x > 0 else 0
    up = sample(x, y - 1) if y > 0 else 0
    up_left = sample(x - 1, y - 1) if x > 0 and y > 0 else 0
    return _delta(value, _med(left, up, up_left))


def _bump(acc: list[int], count: list[int], ctx: int, err: int) -> None:
    mag = -err if err < 0 else err
    acc[ctx] += mag
    count[ctx] += 1
    if count[ctx] >= RICE_RESET:
        acc[ctx] >>= 1
        count[ctx] >>= 1
        if count[ctx] < 1:
            count[ctx] = 1


class _RiceStop(Exception):
    pass


def _emit(writer: _RiceWriter, acc: list[int], count: list[int], ctx: int, err: int) -> bool:
    k = _rice_k(acc[ctx], count[ctx])
    mapped = _to_m(err)
    quot = mapped >> k
    for _ in range(quot):
        writer.bit(1)
    writer.bit(0)
    if k:
        writer.bits(mapped & ((1 << k) - 1), k)
    _bump(acc, count, ctx, err)
    return not writer.over


def _read_err(reader: _RiceReader, acc: list[int], count: list[int], ctx: int) -> int:
    k = _rice_k(acc[ctx], count[ctx])
    quot = 0
    while reader.bit():
        quot += 1
        if quot > 255:
            raise FormatError("rice")
    rem = reader.bits(k) if k else 0
    mapped = (quot << k) | rem
    if mapped > 255:
        raise FormatError("rice")
    err = _from_m(mapped)
    _bump(acc, count, ctx, err)
    return err


def _for_pixels(x0: int, y0: int, bw: int, bh: int, sample, step) -> None:
    prev = [0] * bw
    left_e = 0
    for ly in range(bh):
        y = y0 + ly
        for lx in range(bw):
            x = x0 + lx
            if x <= 0:
                left_e = 0
            elif lx == 0:
                left_e = _residual_at(sample, x - 1, y)
            if y <= 0:
                up_e = 0
            elif ly == 0:
                up_e = _residual_at(sample, x, y - 1)
            else:
                up_e = prev[lx]
            ctx = _qb(abs(left_e)) * 8 + _qb(abs(up_e))
            left = sample(x - 1, y) if x > 0 else 0
            up = sample(x, y - 1) if y > 0 else 0
            up_left = sample(x - 1, y - 1) if x > 0 and y > 0 else 0
            err = step(ctx, _med(left, up, up_left), x, y, lx, ly)
            left_e = err
            prev[lx] = err


def rice_encode_block(
    rgb: bytes,
    width: int,
    height: int,
    x0: int,
    y0: int,
    bw: int,
    bh: int,
    state: RiceState,
    limit: int,
) -> bytes | None:
    acc = [row[:] for row in state.acc]
    count = [row[:] for row in state.count]
    writer = _RiceWriter(limit)
    for channel in range(3):

        def sample(x: int, y: int, channel: int = channel) -> int:
            return _channel_rgb(rgb, width, height, x, y, channel)

        def step(ctx: int, pred: int, x: int, y: int, lx: int, ly: int, channel: int = channel, sample=sample) -> int:
            err = _delta(sample(x, y), pred)
            if not _emit(writer, acc[channel], count[channel], ctx, err):
                raise _RiceStop
            return err

        try:
            _for_pixels(x0, y0, bw, bh, sample, step)
        except _RiceStop:
            return None
    blob = writer.finish()
    if blob is None:
        return None
    state.acc = acc
    state.count = count
    return blob


def rice_decode_block(
    body: bytes,
    image: bytearray,
    width: int,
    height: int,
    x0: int,
    y0: int,
    bw: int,
    bh: int,
    state: RiceState,
) -> bytes:
    reader = _RiceReader(body)
    planes = [bytearray(bw * bh) for _ in range(3)]
    for channel in range(3):

        def sample(x: int, y: int, channel: int = channel) -> int:
            if x0 <= x < x0 + bw and y0 <= y < y0 + bh:
                return planes[channel][(y - y0) * bw + (x - x0)]
            return _channel_rgb(image, width, height, x, y, channel)

        def step(ctx: int, pred: int, x: int, y: int, lx: int, ly: int, channel: int = channel) -> int:
            err = _read_err(reader, state.acc[channel], state.count[channel], ctx)
            planes[channel][ly * bw + lx] = (pred + err) & 255
            return err

        _for_pixels(x0, y0, bw, bh, sample, step)
    reader.finish()
    out = bytearray(bw * bh * 3)
    for i in range(bw * bh):
        green = planes[0][i]
        red_p = planes[1][i]
        blue_p = planes[2][i]
        out[i * 3] = (red_p + green) & 255
        out[i * 3 + 1] = green
        out[i * 3 + 2] = (blue_p + green) & 255
    return bytes(out)


RANS_L = 1 << 23
RANS_SCALE = 12
RANS_ONE = 1 << RANS_SCALE
RANS_INIT = 2816
RANS_RATE = 4
RANS_NCTX = 13851


def qjls(delta: int) -> int:
    if delta <= -21:
        return -4
    if delta <= -7:
        return -3
    if delta <= -3:
        return -2
    if delta < 0:
        return -1
    if delta == 0:
        return 0
    if delta < 3:
        return 1
    if delta < 7:
        return 2
    if delta < 21:
        return 3
    return 4


class RansModel:
    def __init__(self) -> None:
        self.p = array.array("H", [RANS_INIT]) * (RANS_NCTX * 256)

    def clone(self) -> "RansModel":
        other = RansModel.__new__(RansModel)
        other.p = array.array("H", self.p)
        return other


def _adapt_p(prob: int, bit: int) -> int:
    if bit == 0:
        prob += (RANS_ONE - prob) >> RANS_RATE
        if prob > RANS_ONE - 1:
            return RANS_ONE - 1
        return prob
    prob -= prob >> RANS_RATE
    if prob < 1:
        return 1
    return prob


def _rans_put(buf: bytearray, ptr: int, state: int, cum: int, freq: int) -> tuple[int, int] | None:
    x_max = ((RANS_L >> RANS_SCALE) << 8) * freq
    while state >= x_max:
        if ptr == 0:
            return None
        ptr -= 1
        buf[ptr] = state & 255
        state >>= 8
    state = ((state // freq) << RANS_SCALE) + (state % freq) + cum
    return ptr, state


def _plane_sample(rgb: bytes | bytearray, width: int, height: int, x: int, y: int, channel: int) -> int:
    return _channel_rgb(rgb, width, height, x, y, channel)


def _jctx(
    rgb: bytes | bytearray,
    width: int,
    height: int,
    x: int,
    y: int,
    channel: int,
    x0: int,
    y0: int,
    bw: int,
    sample,
) -> int:
    west = sample(x - 1, y, channel) if x > 0 else 0
    north = sample(x, y - 1, channel) if y > 0 else 0
    northwest = sample(x - 1, y - 1, channel) if x > 0 and y > 0 else 0
    ne_known = y > 0 and x + 1 < width and ((y - 1) < y0 or (x + 1) < x0 + bw)
    northeast = sample(x + 1, y - 1, channel) if ne_known else 0
    q1 = qjls(northeast - north)
    q2 = qjls(north - northwest)
    q3 = qjls(northwest - west)
    return (q1 + 4) * 81 + (q2 + 4) * 9 + (q3 + 4)


def _rans_index(
    rgb: bytes | bytearray,
    width: int,
    height: int,
    x: int,
    y: int,
    channel: int,
    x0: int,
    y0: int,
    bw: int,
    sample,
) -> int:
    joint = _jctx(rgb, width, height, x, y, channel, x0, y0, bw, sample)
    if channel == 0:
        return joint
    green_err = _residual_at(lambda px, py, channel=0: sample(px, py, channel), x, y)
    bucket = qjls(green_err) + 4
    return 729 + (channel - 1) * 6561 + joint * 9 + bucket


def rans_encode_block(
    rgb: bytes,
    width: int,
    height: int,
    x0: int,
    y0: int,
    bw: int,
    bh: int,
    model: RansModel,
    limit: int,
) -> bytes | None:
    if limit < 4:
        return None
    saved = model.clone()

    def sample(x: int, y: int, channel: int) -> int:
        return _plane_sample(rgb, width, height, x, y, channel)

    snap: list[int] = []
    probs = model.p
    for channel in range(3):
        for ly in range(bh):
            y = y0 + ly
            for lx in range(bw):
                x = x0 + lx
                err = _residual_at(lambda px, py, channel=channel: sample(px, py, channel), x, y)
                mapped = _to_m(err)
                ctx = _rans_index(rgb, width, height, x, y, channel, x0, y0, bw, sample)
                node = 1
                for shift in range(7, -1, -1):
                    bit = (mapped >> shift) & 1
                    slot = ctx * 256 + node
                    prob = probs[slot]
                    snap.append((bit << 15) | prob)
                    probs[slot] = _adapt_p(prob, bit)
                    node = node * 2 + bit
    buf = bytearray(limit)
    ptr = limit
    state = RANS_L
    for packed in reversed(snap):
        prob = packed & 0x0FFF
        bit = packed >> 15
        if bit:
            cum, freq = prob, RANS_ONE - prob
        else:
            cum, freq = 0, prob
        put = _rans_put(buf, ptr, state, cum, freq)
        if put is None:
            model.p = saved.p
            return None
        ptr, state = put
    if ptr < 4:
        model.p = saved.p
        return None
    ptr -= 4
    buf[ptr] = state & 255
    buf[ptr + 1] = (state >> 8) & 255
    buf[ptr + 2] = (state >> 16) & 255
    buf[ptr + 3] = (state >> 24) & 255
    body = bytes(buf[ptr:])
    if len(body) >= limit:
        model.p = saved.p
        return None
    return body


def rans_decode_block(
    body: bytes,
    image: bytearray,
    width: int,
    height: int,
    x0: int,
    y0: int,
    bw: int,
    bh: int,
    model: RansModel,
) -> bytes:
    if len(body) < 4:
        raise FormatError("rans")
    state = body[0] | (body[1] << 8) | (body[2] << 16) | (body[3] << 24)
    pos = 4
    planes = [bytearray(bw * bh) for _ in range(3)]
    probs = model.p

    def sample(x: int, y: int, channel: int) -> int:
        if x0 <= x < x0 + bw and y0 <= y < y0 + bh:
            return planes[channel][(y - y0) * bw + (x - x0)]
        return _plane_sample(image, width, height, x, y, channel)

    for channel in range(3):
        for ly in range(bh):
            y = y0 + ly
            for lx in range(bw):
                x = x0 + lx
                ctx = _rans_index(image, width, height, x, y, channel, x0, y0, bw, sample)
                node = 1
                mapped = 0
                for _shift in range(7, -1, -1):
                    slot = ctx * 256 + node
                    prob = probs[slot]
                    bit = 1 if (state & (RANS_ONE - 1)) >= prob else 0
                    if bit:
                        cum, freq = prob, RANS_ONE - prob
                    else:
                        cum, freq = 0, prob
                    state = freq * (state >> RANS_SCALE) + (state & (RANS_ONE - 1)) - cum
                    while state < RANS_L:
                        if pos >= len(body):
                            raise FormatError("rans")
                        state = (state << 8) | body[pos]
                        pos += 1
                    probs[slot] = _adapt_p(prob, bit)
                    mapped = (mapped << 1) | bit
                    node = node * 2 + bit
                err = _from_m(mapped)
                left = sample(x - 1, y, channel) if x > 0 else 0
                up = sample(x, y - 1, channel) if y > 0 else 0
                up_left = sample(x - 1, y - 1, channel) if x > 0 and y > 0 else 0
                pred = _med(left, up, up_left)
                planes[channel][ly * bw + lx] = (pred + err) & 255
        if channel == 0:
            for ly in range(bh):
                y = y0 + ly
                row = y * width
                for lx in range(bw):
                    image[((row + x0 + lx) * 3) + 1] = planes[0][ly * bw + lx]
    if state != RANS_L or pos != len(body):
        raise FormatError("rans")
    out = bytearray(bw * bh * 3)
    for i in range(bw * bh):
        green = planes[0][i]
        out[i * 3] = (planes[1][i] + green) & 255
        out[i * 3 + 1] = green
        out[i * 3 + 2] = (planes[2][i] + green) & 255
    return bytes(out)


def try_palette(rgb: bytes) -> bytes | None:
    order: list[bytes] = []
    index: dict[bytes, int] = {}
    idxs = bytearray()
    for i in range(0, len(rgb), 3):
        pix = rgb[i : i + 3]
        found = index.get(pix)
        if found is None:
            if len(order) == 256:
                return None
            index[pix] = len(order)
            order.append(pix)
            found = index[pix]
        idxs.append(found)
    body = bytes([len(order) - 1]) + b"".join(order) + packbits_encode(bytes(idxs))
    return body


def encode_block(
    rgb: bytes,
    bw: int,
    bh: int,
    full: bytes | None = None,
    width: int = 0,
    height: int = 0,
    x0: int = 0,
    y0: int = 0,
    rice_state: RiceState | None = None,
    rans_model: RansModel | None = None,
) -> bytes:
    raw = rgb
    best_mode = 0
    best = raw
    packed = packbits_encode(raw)
    if len(packed) < len(best):
        best_mode, best = 1, packed
    residual = packbits_encode(predict(raw, bw, bh))
    if len(residual) < len(best):
        best_mode, best = 2, residual
    palette = try_palette(raw)
    if palette is not None and len(palette) < len(best):
        best_mode, best = 3, palette
    rice_won = False
    rice_acc = None
    rice_count = None
    if rice_state is not None and full is not None:
        rice_acc = [row[:] for row in rice_state.acc]
        rice_count = [row[:] for row in rice_state.count]
        rice = rice_encode_block(full, width, height, x0, y0, bw, bh, rice_state, len(best))
        if rice is not None:
            best_mode, best = 4, rice
            rice_won = True
    if rans_model is not None and full is not None:
        # Keep the rANS model continuous. A body shorter than raw is written
        # even when Rice is smaller, because dropping the block would reset
        # the context the later blocks rely on.
        raw_len = bw * bh * 3
        frozen = rans_model.clone()
        rans = rans_encode_block(full, width, height, x0, y0, bw, bh, rans_model, raw_len)
        if rans is not None:
            if rice_won and rice_state is not None and rice_acc is not None and rice_count is not None:
                rice_state.acc = rice_acc
                rice_state.count = rice_count
            best_mode, best = 5, rans
        else:
            rans_model.p = frozen.p
    return bytes([best_mode, 0]) + struct.pack("<I", len(best)) + best


def iter_blocks(width: int, height: int):
    blocks_x = (width + BLOCK - 1) // BLOCK
    blocks_y = (height + BLOCK - 1) // BLOCK
    for by in range(blocks_y):
        y0 = by * BLOCK
        bh = min(BLOCK, height - y0)
        for bx in range(blocks_x):
            x0 = bx * BLOCK
            bw = min(BLOCK, width - x0)
            yield x0, y0, bw, bh


def crop_block(rgb: bytes, width: int, x0: int, y0: int, bw: int, bh: int) -> bytes:
    out = bytearray()
    stride = width * 3
    for y in range(y0, y0 + bh):
        start = y * stride + x0 * 3
        out += rgb[start : start + bw * 3]
    return bytes(out)


def paste_block(image: bytearray, width: int, x0: int, y0: int, bw: int, bh: int, block: bytes) -> None:
    stride = width * 3
    pos = 0
    row = bw * 3
    for y in range(y0, y0 + bh):
        start = y * stride + x0 * 3
        image[start : start + row] = block[pos : pos + row]
        pos += row


def build_file(width: int, height: int, payload: bytes) -> bytes:
    header = bytearray(HEADER)
    header[0:4] = MAGIC
    struct.pack_into("<H", header, 4, 1)
    struct.pack_into("<H", header, 6, HEADER)
    struct.pack_into("<I", header, 8, width)
    struct.pack_into("<I", header, 12, height)
    struct.pack_into("<H", header, 16, BLOCK)
    struct.pack_into("<H", header, 18, 1)
    struct.pack_into("<I", header, 20, len(payload))
    struct.pack_into("<I", header, 24, crc32(payload))
    struct.pack_into("<I", header, 28, crc32(header[:28]))
    return bytes(header) + payload


def encode_image(width: int, height: int, rgb: bytes) -> bytes:
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    payload = bytearray()
    rice_state = RiceState()
    rans_model = RansModel()
    for x0, y0, bw, bh in iter_blocks(width, height):
        payload += encode_block(
            crop_block(rgb, width, x0, y0, bw, bh),
            bw,
            bh,
            full=rgb,
            width=width,
            height=height,
            x0=x0,
            y0=y0,
            rice_state=rice_state,
            rans_model=rans_model,
        )
    return build_file(width, height, bytes(payload))


def decode_block(
    data: bytes,
    bw: int,
    bh: int,
    image: bytearray | None = None,
    width: int = 0,
    height: int = 0,
    x0: int = 0,
    y0: int = 0,
    rice_state: RiceState | None = None,
    rans_model: RansModel | None = None,
) -> bytes:
    if len(data) < 6:
        raise FormatError("truncated block")
    mode = data[0]
    flags = data[1]
    size = struct.unpack_from("<I", data, 2)[0]
    if flags != 0:
        raise FormatError("block flags")
    if size != len(data) - 6:
        raise FormatError("block length")
    body = data[6:]
    raw_len = bw * bh * 3
    if mode == 0:
        if len(body) != raw_len:
            raise FormatError("raw length")
        return body
    if mode == 1:
        return packbits_decode(body, raw_len)
    if mode == 2:
        return unpredict(packbits_decode(body, raw_len), bw, bh)
    if mode == 3:
        if not body:
            raise FormatError("palette")
        ncolors = body[0] + 1
        pal_bytes = ncolors * 3
        if len(body) < 1 + pal_bytes:
            raise FormatError("palette")
        indexes = packbits_decode(body[1 + pal_bytes :], bw * bh)
        out = bytearray()
        palette = body[1 : 1 + pal_bytes]
        for idx in indexes:
            if idx >= ncolors:
                raise FormatError("palette index")
            out += palette[idx * 3 : idx * 3 + 3]
        return bytes(out)
    if mode == 4:
        if image is None or rice_state is None:
            raise FormatError("rice")
        return rice_decode_block(body, image, width, height, x0, y0, bw, bh, rice_state)
    if mode == 5:
        if image is None or rans_model is None:
            raise FormatError("rans")
        return rans_decode_block(body, image, width, height, x0, y0, bw, bh, rans_model)
    raise FormatError("block mode")


def decode_image(blob: bytes) -> tuple[int, int, bytes]:
    if len(blob) < HEADER or blob[0:4] != MAGIC:
        raise FormatError("magic")
    version, header_size = struct.unpack_from("<HH", blob, 4)
    if version != 1:
        raise FormatError("version")
    if header_size != HEADER:
        raise FormatError("header size")
    width, height = struct.unpack_from("<II", blob, 8)
    block_size, pixel_format = struct.unpack_from("<HH", blob, 16)
    payload_size, payload_crc, header_crc = struct.unpack_from("<III", blob, 20)
    if block_size != BLOCK or pixel_format != 1:
        raise FormatError("format")
    if crc32(blob[:28]) != header_crc:
        raise FormatError("header checksum")
    if not (1 <= width <= MAX_DIM and 1 <= height <= MAX_DIM):
        raise FormatError("dimensions")
    if width * height > MAX_PIXELS:
        raise FormatError("limit")
    if payload_size != len(blob) - HEADER:
        raise FormatError("payload length")
    payload = blob[HEADER:]
    if crc32(payload) != payload_crc:
        raise FormatError("payload checksum")
    image = bytearray(width * height * 3)
    offset = 0
    rice_state = RiceState()
    rans_model = RansModel()
    for x0, y0, bw, bh in iter_blocks(width, height):
        if offset + 6 > len(payload):
            raise FormatError("truncated block")
        size = struct.unpack_from("<I", payload, offset + 2)[0]
        take = 6 + size
        if offset + take > len(payload):
            raise FormatError("block length")
        block = decode_block(
            payload[offset : offset + take],
            bw,
            bh,
            image=image,
            width=width,
            height=height,
            x0=x0,
            y0=y0,
            rice_state=rice_state,
            rans_model=rans_model,
        )
        paste_block(image, width, x0, y0, bw, bh, block)
        offset += take
    if offset != len(payload):
        raise FormatError("trailing data")
    return width, height, bytes(image)


def row_stride(width: int) -> int:
    return (width * 3 + 3) & ~3


def write_bmp24(
    width: int,
    height: int,
    rgb: bytes,
    *,
    topdown: bool = False,
    gap: int = 0,
    sizeimage_zero: bool = False,
    reserved: int = 0,
    bf_size_zero: bool = False,
) -> bytes:
    stride = row_stride(width)
    pixels = stride * height
    off = 54 + gap
    file_size = off + pixels
    out = bytearray(file_size)
    struct.pack_into("<2sIHHI", out, 0, b"BM", 0 if bf_size_zero else file_size, reserved & 0xFFFF, (reserved >> 16) & 0xFFFF, off)
    image_size = 0 if sizeimage_zero else pixels
    stored_h = -height if topdown else height
    struct.pack_into("<IiiHHIIiiII", out, 14, 40, width, stored_h, 1, 24, 0, image_size, 3780, 3780, 0, 0)
    if gap:
        out[54 : 54 + gap] = b"\xA5" * gap
    for y in range(height):
        src_y = y if topdown else height - 1 - y
        src = rgb[src_y * width * 3 : (src_y + 1) * width * 3]
        dst = off + y * stride
        for x in range(width):
            r, g, b = src[x * 3 : x * 3 + 3]
            out[dst + x * 3 : dst + x * 3 + 3] = bytes((b, g, r))
    return bytes(out)


def read_bmp24(blob: bytes) -> tuple[int, int, bytes]:
    if len(blob) < 54 or blob[0:2] != b"BM":
        raise FormatError("bmp signature")
    _bf_size, _r1, _r2, off = struct.unpack_from("<IHHI", blob, 2)
    header = struct.unpack_from("<IiiHHIIiiII", blob, 14)
    bi_size, width, height, planes, bits, comp, sizeimage, _x, _y, _used, _imp = header
    if bi_size != 40 or planes != 1 or bits != 24 or comp != 0:
        raise FormatError("bmp subset")
    if width <= 0 or height == 0 or height == -2147483648:
        raise FormatError("bmp dimensions")
    topdown = height < 0
    height = abs(height)
    stride = row_stride(width)
    need = off + stride * height
    if off < 54 or need > len(blob):
        raise FormatError("bmp truncated")
    if sizeimage not in (0, ) and sizeimage < stride * height:
        raise FormatError("bmp image size")
    rgb = bytearray(width * height * 3)
    for file_y in range(height):
        img_y = file_y if topdown else height - 1 - file_y
        src = off + file_y * stride
        for x in range(width):
            b, g, r = blob[src + x * 3 : src + x * 3 + 3]
            base = (img_y * width + x) * 3
            rgb[base : base + 3] = bytes((r, g, b))
        pad = blob[src + width * 3 : src + stride]
        if any(pad):
            raise FormatError("bmp padding")
    return width, height, bytes(rgb)
