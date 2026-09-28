"""Whole-image LZ77 with long matches and a sparse Huffman table.

This is a size prototype. It is not the shipping codec.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image

LIT_END = 256


def _bases(start: int, limit: int) -> list[tuple[int, int]]:
    bases = []
    value = start
    extra = 0
    while value <= limit:
        bases.append((value, extra))
        value += 1 << extra
        extra += 1
    return bases


LENGTH_BASES = _bases(3, 8_000_000)
DIST_BASES = _bases(1, 8_000_000)
SYM_MATCH0 = 257
N_LITLEN = SYM_MATCH0 + len(LENGTH_BASES)


def _code(bases: list[tuple[int, int]], value: int) -> tuple[int, int, int]:
    for i, (base, bits) in enumerate(bases):
        span = 1 << bits
        if base <= value < base + span:
            return i, value - base, bits
    raise ValueError(value)


def length_symbol(length: int) -> tuple[int, int, int]:
    i, extra, bits = _code(LENGTH_BASES, length)
    return SYM_MATCH0 + i, extra, bits


def dist_symbol(dist: int) -> tuple[int, int, int]:
    return _code(DIST_BASES, dist)


class BitIO:
    def __init__(self, data: bytes = b"") -> None:
        self.buf = bytearray(data)
        self.acc = 0
        self.n = 0
        self.i = 0

    def write(self, value: int, bits: int) -> None:
        if bits <= 0:
            return
        self.acc |= (value & ((1 << bits) - 1)) << self.n
        self.n += bits
        while self.n >= 8:
            self.buf.append(self.acc & 255)
            self.acc >>= 8
            self.n -= 8

    def write_msb(self, value: int, bits: int) -> None:
        for shift in range(bits - 1, -1, -1):
            self.write((value >> shift) & 1, 1)

    def finish(self) -> bytes:
        if self.n:
            self.buf.append(self.acc & 255)
            self.n = 0
            self.acc = 0
        return bytes(self.buf)

    def read(self, bits: int) -> int:
        while self.n < bits:
            if self.i >= len(self.buf):
                raise EOFError
            self.acc |= self.buf[self.i] << self.n
            self.i += 1
            self.n += 8
        value = self.acc & ((1 << bits) - 1)
        self.acc >>= bits
        self.n -= bits
        return value


def huffman_lengths(freq: list[int]) -> list[int]:
    import heapq

    leaves = [(f, i) for i, f in enumerate(freq) if f]
    if not leaves:
        return [0] * len(freq)
    if len(leaves) == 1:
        out = [0] * len(freq)
        out[leaves[0][1]] = 1
        return out
    counter = 0
    pq = []
    for f, sym in leaves:
        heapq.heappush(pq, (f, counter, sym))
        counter += 1
    while len(pq) > 1:
        a = heapq.heappop(pq)
        b = heapq.heappop(pq)
        heapq.heappush(pq, (a[0] + b[0], counter, (a, b)))
        counter += 1
    lengths = [0] * len(freq)

    def walk(node, depth: int) -> None:
        payload = node[2]
        if isinstance(payload, int):
            lengths[payload] = max(depth, 1)
        else:
            walk(payload[0], depth + 1)
            walk(payload[1], depth + 1)

    walk(pq[0], 0)
    return lengths


def canonical(lengths: list[int]) -> list[int]:
    max_len = max(lengths) if any(lengths) else 0
    bl_count = [0] * (max_len + 1)
    for n in lengths:
        if n:
            bl_count[n] += 1
    code = 0
    nxt = [0] * (max_len + 1)
    for bits in range(1, max_len + 1):
        code = (code + bl_count[bits - 1]) << 1
        nxt[bits] = code
    codes = [0] * len(lengths)
    for sym, n in enumerate(lengths):
        if n:
            codes[sym] = nxt[n]
            nxt[n] += 1
    return codes


def decode_table(lengths: list[int]) -> tuple[dict, int]:
    codes = canonical(lengths)
    table = {}
    max_len = 0
    for sym, (code, n) in enumerate(zip(codes, lengths)):
        if n:
            table[(n, code)] = sym
            max_len = max(max_len, n)
    return table, max_len


def read_symbol(br: BitIO, table: dict, max_len: int) -> int:
    code = 0
    for n in range(1, max_len + 1):
        code = (code << 1) | br.read(1)
        sym = table.get((n, code))
        if sym is not None:
            return sym
    raise ValueError("bad huffman code")


def write_lengths(bw: BitIO, lengths: list[int]) -> None:
    pairs = [(sym, n) for sym, n in enumerate(lengths) if n]
    bw.write(len(pairs), 16)
    for sym, n in pairs:
        bw.write(sym, 16)
        bw.write(n, 4)


def read_lengths(br: BitIO, nsym: int) -> list[int]:
    count = br.read(16)
    lengths = [0] * nsym
    for _ in range(count):
        sym = br.read(16)
        n = br.read(4)
        if sym >= nsym or n == 0 or n > 15:
            raise ValueError("bad length")
        lengths[sym] = n
    return lengths


def match_at(data: bytes, i: int, dist: int, limit: int) -> int:
    if dist <= 0 or dist > i:
        return 0
    src = i - dist
    n = 0
    while n + 8 <= limit and data[src + n : src + n + 8] == data[i + n : i + n + 8]:
        n += 8
    while n < limit and data[src + n] == data[i + n]:
        n += 1
    return n


def lz77(data: bytes, stride: int = 0) -> list:
    n = len(data)
    head: dict[int, list[int]] = {}
    tokens = []
    i = 0

    def insert(pos: int) -> None:
        if pos + 2 >= n:
            return
        key = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16)
        chain = head.get(key)
        if chain is None:
            head[key] = [pos]
        else:
            chain.append(pos)
            if len(chain) > 12:
                del chain[0]

    while i < n:
        best_len = 0
        best_dist = 0
        limit = n - i
        if stride and i >= stride:
            got = match_at(data, i, stride, limit)
            if got > best_len:
                best_len = got
                best_dist = stride
        if i >= 1:
            got = match_at(data, i, 1, limit)
            if got > best_len:
                best_len = got
                best_dist = 1
        if i + 2 < n and best_len < 64:
            key = data[i] | (data[i + 1] << 8) | (data[i + 2] << 16)
            chain = head.get(key)
            if chain:
                for pos in reversed(chain):
                    dist = i - pos
                    got = match_at(data, i, dist, limit)
                    if got > best_len:
                        best_len = got
                        best_dist = dist
                        if best_len >= 256:
                            break
        if best_len >= 3:
            tokens.append((best_len, best_dist))
            end = i + best_len
            # Insert the first few positions so later hashes still see this run.
            step = 1 if best_len < 32 else max(1, best_len // 8)
            pos = i
            while pos < end:
                insert(pos)
                pos += step
            i = end
        else:
            tokens.append(data[i])
            insert(i)
            i += 1
    return tokens


def encode_stream(data: bytes, stride: int = 0) -> bytes:
    tokens = lz77(data, stride)
    lit_freq = [0] * N_LITLEN
    dist_freq = [0] * len(DIST_BASES)
    lit_freq[LIT_END] = 1
    for tok in tokens:
        if isinstance(tok, int):
            lit_freq[tok] += 1
        else:
            length, dist = tok
            sym, _, _ = length_symbol(length)
            dsym, _, _ = dist_symbol(dist)
            lit_freq[sym] += 1
            dist_freq[dsym] += 1
    lit_len = huffman_lengths(lit_freq)
    dist_len = huffman_lengths(dist_freq)
    if not any(dist_len):
        dist_len[0] = 1
    lit_codes = canonical(lit_len)
    dist_codes = canonical(dist_len)
    bw = BitIO()
    write_lengths(bw, lit_len)
    write_lengths(bw, dist_len)
    for tok in tokens:
        if isinstance(tok, int):
            bw.write_msb(lit_codes[tok], lit_len[tok])
        else:
            length, dist = tok
            sym, extra, bits = length_symbol(length)
            bw.write_msb(lit_codes[sym], lit_len[sym])
            bw.write(extra, bits)
            dsym, dextra, dbits = dist_symbol(dist)
            bw.write_msb(dist_codes[dsym], dist_len[dsym])
            bw.write(dextra, dbits)
    bw.write_msb(lit_codes[LIT_END], lit_len[LIT_END])
    return bw.finish()


def decode_stream(blob: bytes, out_len: int) -> bytes:
    br = BitIO(blob)
    lit_len = read_lengths(br, N_LITLEN)
    dist_len = read_lengths(br, len(DIST_BASES))
    lit_table, lit_max = decode_table(lit_len)
    dist_table, dist_max = decode_table(dist_len)
    out = bytearray()
    while len(out) < out_len:
        sym = read_symbol(br, lit_table, lit_max)
        if sym < 256:
            out.append(sym)
        elif sym == LIT_END:
            break
        else:
            bits = LENGTH_BASES[sym - SYM_MATCH0][1]
            extra = br.read(bits) if bits else 0
            length = LENGTH_BASES[sym - SYM_MATCH0][0] + extra
            dsym = read_symbol(br, dist_table, dist_max)
            dbits = DIST_BASES[dsym][1]
            dist = DIST_BASES[dsym][0] + (br.read(dbits) if dbits else 0)
            if dist <= 0 or dist > len(out) or len(out) + length > out_len:
                raise ValueError("bad match")
            for _ in range(length):
                out.append(out[-dist])
    if len(out) != out_len:
        raise ValueError(f"length {len(out)} != {out_len}")
    return bytes(out)


def up_filter(data: bytes, stride: int) -> bytes:
    out = bytearray(len(data))
    for i, b in enumerate(data):
        up = data[i - stride] if i >= stride else 0
        out[i] = (b - up) & 255
    return bytes(out)


def inv_up(data: bytes, stride: int) -> bytes:
    out = bytearray(len(data))
    for i, b in enumerate(data):
        up = out[i - stride] if i >= stride else 0
        out[i] = (b + up) & 255
    return bytes(out)


def sub_filter(data: bytes, bpp: int) -> bytes:
    out = bytearray(len(data))
    for i, b in enumerate(data):
        left = data[i - bpp] if i >= bpp else 0
        out[i] = (b - left) & 255
    return bytes(out)


def inv_sub(data: bytes, bpp: int) -> bytes:
    out = bytearray(len(data))
    for i, b in enumerate(data):
        left = out[i - bpp] if i >= bpp else 0
        out[i] = (b + left) & 255
    return bytes(out)


def palette(rgb: bytes) -> tuple[bytes, bytes] | None:
    index: dict[bytes, int] = {}
    order: list[bytes] = []
    idxs = bytearray()
    for i in range(0, len(rgb), 3):
        pix = rgb[i : i + 3]
        j = index.get(pix)
        if j is None:
            if len(order) == 256:
                return None
            j = len(order)
            index[pix] = j
            order.append(pix)
        idxs.append(j)
    return bytes([len(order) - 1]) + b"".join(order), bytes(idxs)


def unpalette(head: bytes, idxs: bytes) -> bytes:
    ncolors = head[0] + 1
    pal = head[1:]
    if len(pal) != ncolors * 3 or len(idxs) * 3 != len(idxs) * 3:
        pass
    out = bytearray(len(idxs) * 3)
    for i, idx in enumerate(idxs):
        if idx >= ncolors:
            raise ValueError("index")
        out[i * 3 : i * 3 + 3] = pal[idx * 3 : idx * 3 + 3]
    return bytes(out)


def best_payload(rgb: bytes, w: int, h: int, verify: bool) -> tuple[int, str, bytes]:
    """Return total file size estimate, mode name, and a blob the decoder can invert."""
    options = []

    def add(name: str, stream: bytes, stride: int, kind: str, extra: bytes = b"") -> None:
        comp = encode_stream(stream, stride)
        blob = bytes([ord(kind[0])]) + extra + comp
        if verify:
            back = decode_stream(comp, len(stream))
            if back != stream:
                raise RuntimeError(f"{name} stream mismatch")
        options.append((32 + len(blob), name, blob))

    add("raw", rgb, w * 3, "r")
    add("sub", sub_filter(rgb, 3), w * 3, "s")
    add("up", up_filter(rgb, w * 3), w * 3, "u")
    pal = palette(rgb)
    if pal is not None:
        head, idxs = pal
        add("pal", idxs, w, "p", head)
        add("pal-up", up_filter(idxs, w), w, "q", head)
        add("pal-sub", sub_filter(idxs, 1), w, "t", head)
    options.sort()
    return options[0]


if __name__ == "__main__":
    root = Path("benchmarks/work")
    for group in ("dev", "holdout"):
        print("==", group, flush=True)
        for bmp in sorted((root / group).glob("*.bmp")):
            if bmp.name.count(".") > 1:
                continue
            image = Image.open(bmp).convert("RGB")
            rgb = image.tobytes()
            w, h = image.size
            webp = bmp.with_suffix(".webp").stat().st_size
            size, name, _blob = best_payload(rgb, w, h, verify=False)
            flag = "WIN" if size < webp else "LOSE"
            print(f"{group}/{bmp.stem:10} {w:4}x{h:<4} webp={webp:7} lz={size:7} {flag} {name}", flush=True)
