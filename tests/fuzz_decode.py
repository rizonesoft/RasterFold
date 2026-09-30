"""Mutation fuzzing of the decoder: damaged .fold files must be rejected cleanly.

Each case mutates a valid file (byte flips, truncation, inserted bytes, changed
stream sizes, changed dimensions) and recomputes both checksums so the damage
reaches the CM decoder. The decoder must exit 0 (the mutation happened to be
valid) or 4 (malformed), leave no output on failure, and finish in time.

  python tests/fuzz_decode.py [iterations] [seed]
"""

from __future__ import annotations

import random
import struct
import os
import subprocess
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import rfi_ref as rf  # noqa: E402

EXE = Path(os.environ.get("RF_TEST_EXE", ROOT / "bin" / ("windows-x64/bin/rasterfold.exe" if os.name == "nt" else "linux-x64/bin/rasterfold")))
WORK = ROOT / "tests" / "work" / "fuzz"


def recrc(b: bytearray) -> bytes:
    struct.pack_into("<I", b, 20, len(b) - 32)
    struct.pack_into("<I", b, 24, rf.crc32(bytes(b[32:])))
    struct.pack_into("<I", b, 28, rf.crc32(bytes(b[:28])))
    return bytes(b)


def seeds() -> list[bytes]:
    out = []
    with Image.open(ROOT / "testdata" / "photos" / "kodim05.png") as im:
        for (x, y, w, h) in ((100, 100, 40, 30), (0, 0, 17, 9), (300, 200, 64, 3)):
            crop = im.convert("RGB").crop((x, y, x + w, y + h))
            out.append(rf.encode_image(w, h, crop.tobytes()))
    return [b for b in out if b[16] == rf.METHOD_CM]


def mutate(blob: bytes, rng: random.Random) -> bytes:
    b = bytearray(blob)
    kind = rng.randrange(6)
    if kind == 0:                                   # flip bits in the streams
        for _ in range(rng.randrange(1, 4)):
            i = rng.randrange(44, len(b))
            b[i] ^= 1 << rng.randrange(8)
    elif kind == 1:                                 # truncate one stream
        c = rng.randrange(3)
        sizes = list(struct.unpack_from("<III", b, 32))
        cut = rng.randrange(1, max(2, sizes[c]))
        start = 44 + sum(sizes[:c]) + sizes[c] - cut
        del b[start:start + cut]
        sizes[c] -= cut
        struct.pack_into("<III", b, 32, *sizes)
    elif kind == 2:                                 # extend one stream
        c = rng.randrange(3)
        sizes = list(struct.unpack_from("<III", b, 32))
        add = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
        at = 44 + sum(sizes[: c + 1])
        b[at:at] = add
        sizes[c] += len(add)
        struct.pack_into("<III", b, 32, *sizes)
    elif kind == 3:                                 # shuffle bytes between streams
        sizes = list(struct.unpack_from("<III", b, 32))
        d = rng.randrange(-4, 5)
        sizes[0] += d
        sizes[1] -= d
        struct.pack_into("<III", b, 32, *[s & 0xFFFFFFFF for s in sizes])
    elif kind == 4:                                 # other dimensions for the same streams
        w, h = struct.unpack_from("<II", b, 8)
        struct.pack_into("<II", b, 8, max(1, w + rng.randrange(-3, 4)), max(1, h + rng.randrange(-3, 4)))
    else:                                           # random stream bytes
        for i in range(44, len(b)):
            if rng.random() < 0.05:
                b[i] = rng.randrange(256)
    return recrc(b)


def main() -> int:
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    WORK.mkdir(parents=True, exist_ok=True)
    base = seeds()
    counts = {0: 0, 4: 0}
    for i in range(iterations):
        blob = mutate(base[i % len(base)], rng)
        src, dest = WORK / "f.fold", WORK / "f.bmp"
        src.write_bytes(blob)
        dest.unlink(missing_ok=True)
        threads = "1" if i % 2 else "3"
        try:
            r = subprocess.run([str(EXE), "decode", "--threads", threads, str(src), str(dest)], capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            print(f"HANG at iteration {i}")
            (WORK / f"hang{i}.fold").write_bytes(blob)
            return 1
        if r.returncode not in (0, 4) or (r.returncode == 4 and dest.exists()):
            print(f"BAD exit {r.returncode:#x} at iteration {i}: {r.stderr.decode(errors='replace').strip()}")
            (WORK / f"bad{i}.fold").write_bytes(blob)
            return 1
        if r.returncode == 0:
            w, h, rgb = rf.read_bmp24(dest.read_bytes())
            if rf.decode_image(blob) != (w, h, rgb):
                print(f"MISMATCH with the oracle at iteration {i}")
                (WORK / f"mismatch{i}.fold").write_bytes(blob)
                return 1
        counts[r.returncode] += 1
    print(f"{iterations} mutated files: {counts[4]} rejected, {counts[0]} still valid, no crash, hang or mismatch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
