"""Mutation fuzzing of the decoder: damaged .fold files must be rejected cleanly.

Each case mutates a valid file (byte flips, truncation, inserted bytes, changed
stream sizes, changed dimensions) and recomputes both checksums so the damage
reaches the method decoders. The decoder must exit 0 (the mutation happened to be
valid) or 4 (malformed), agree with the independent oracle on both acceptance and
rejection, leave no output or .partial file on failure, and finish in time.

  python tests/fuzz_decode.py [iterations] [seed]
"""

from __future__ import annotations

import base64
from concurrent.futures import FIRST_COMPLETED, Future, ProcessPoolExecutor, wait
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from PIL import Image

_ORIG_SUBPROCESS_RUN = subprocess.run

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import rfi_ref as rf  # noqa: E402

EXE = Path(os.environ.get("RF_TEST_EXE", ROOT / "bin" / ("windows-x64/bin/rasterfold.exe" if os.name == "nt" else "linux-x64/bin/rasterfold")))
WORK = ROOT / "tests" / "work" / "fuzz"


@contextlib.contextmanager
def _staged_fuzz_env(exe: Path | None = None):
    """Stage scratch files in local tmpfs (/dev/shm on Linux) or native %TEMP%
    (on Windows). Keep the executable at its native path on POSIX so a noexec
    tmpfs mount never prevents execution; on Windows, stage a local copy in
    %TEMP% to avoid \\\\wsl.localhost 9P image-load latency, falling back to the
    source path if %TEMP% disallows execution."""
    target_exe = EXE if exe is None else exe
    WORK.mkdir(parents=True, exist_ok=True)
    shm = Path("/dev/shm")
    base_tmp = str(shm) if os.name != "nt" and shm.is_dir() and os.access(shm, os.W_OK | os.X_OK) else None
    with tempfile.TemporaryDirectory(prefix="rf_fuzz_", dir=base_tmp) as td:
        stage = Path(td)
        work_dir = stage / "work"
        work_dir.mkdir(parents=True, exist_ok=True)
        staged_exe = target_exe
        if os.name == "nt" and target_exe.is_file():
            cand_exe = stage / target_exe.name
            try:
                shutil.copy2(target_exe, cand_exe)
                _ORIG_SUBPROCESS_RUN(
                    [str(cand_exe), "--version"],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10,
                    check=False,
                )
                staged_exe = cand_exe
            except OSError:
                staged_exe = target_exe
        try:
            yield staged_exe, work_dir
        finally:
            for item in work_dir.iterdir():
                if any(tag in item.name for tag in ("hang", "bad", "mismatch")):
                    shutil.copy2(item, WORK / item.name)

PHOTO_CROPS: tuple[tuple[str, int, int, int, int], ...] = (
    ("kodim01.png", 120, 80, 48, 36),
    ("kodim03.png", 160, 0, 36, 512),
    ("kodim05.png", 180, 140, 64, 40),
    ("kodim13.png", 200, 0, 36, 512),
    ("kodim23.png", 200, 160, 36, 34),
    ("deer_crop.png", 220, 0, 36, 768),
    ("fireworks_crop.png", 140, 200, 36, 34),
    ("flower_foveon_crop.png", 240, 0, 36, 768),
    ("nightshot_iso_1600_crop.png", 150, 220, 36, 34),
    ("spider_web_crop.png", 160, 240, 36, 34),
)
FORCE_STRIPS: dict[str, int] = {
    "kodim13.png": 2,
    "nightshot_iso_1600_crop.png": 4,
    "spider_web_crop.png": 4,
}


def recrc(b: bytearray, update_payload_size: bool = True) -> bytes:
    if update_payload_size:
        struct.pack_into("<I", b, 20, len(b) - 32)
    struct.pack_into("<I", b, 24, rf.crc32(bytes(b[32:])))
    struct.pack_into("<I", b, 28, rf.crc32(bytes(b[:28])))
    return bytes(b)


def seed_records() -> list[tuple[str, int, int, int, bytes]]:
    WORK.mkdir(parents=True, exist_ok=True)
    seed_src = (ROOT / "tests" / "rfi_ref.py").read_bytes() + repr((PHOTO_CROPS, FORCE_STRIPS, "sc6_v6")).encode("utf-8")
    cache_key = hashlib.sha256(seed_src).hexdigest()[:16]
    cache_path = WORK / f"seeds_v3_{cache_key}.json"
    if cache_path.is_file():
        try:
            raw = json.loads(cache_path.read_text(encoding="utf-8"))
            records = [(r["name"], int(r["w"]), int(r["h"]), int(r["method"]), base64.b64decode(r["b64"])) for r in raw]
            if len(records) == 21:
                return records
        except (KeyError, ValueError, json.JSONDecodeError):
            pass

    noise_rng = random.Random(101)
    noise_w, noise_h = 64, 48
    noise_rgb = bytes(noise_rng.randrange(256) for _ in range(noise_w * noise_h * 3))
    noise_fold = rf.encode_image(noise_w, noise_h, noise_rgb)
    assert noise_fold[16] == rf.METHOD_RAW

    pal_w, pal_h = 64, 48
    chk_rgb = b"".join(
        b"\x20\x30\x40" if ((x // 3) ^ (y // 3)) & 1 else b"\xE0\xE8\xF0"
        for y in range(pal_h)
        for x in range(pal_w)
    )
    # The encoder writes method 4; method 3 (like method 2) is decode-only and built explicitly.
    pal4_2col = rf.encode_image(pal_w, pal_h, chk_rgb)
    assert pal4_2col[16] == rf.METHOD_PAL3
    assert pal4_2col[32] + 1 == 2
    pal3_2col_payload = rf.pal_encode(pal_w, pal_h, chk_rgb, method=rf.METHOD_PAL2)
    assert pal3_2col_payload is not None
    pal3_2col = rf.build_file(pal_w, pal_h, rf.METHOD_PAL2, pal3_2col_payload)
    assert struct.unpack_from("<H", pal3_2col, 32)[0] + 1 == 2
    pal2_2col_payload = rf.pal_encode(pal_w, pal_h, chk_rgb, method=rf.METHOD_PAL)
    assert pal2_2col_payload is not None
    pal2_2col = rf.build_file(pal_w, pal_h, rf.METHOD_PAL, pal2_2col_payload)
    assert struct.unpack_from("<H", pal2_2col, 32)[0] + 1 == 2

    pal200 = [bytes(((i * 53 + 17) & 0xFF, (i * 97 + 43) & 0xFF, (i * 151 + 89) & 0xFF)) for i in range(200)]
    assert len(set(pal200)) == 200
    rng200 = random.Random(200)
    blocks200 = [b if b < 200 else rng200.randrange(200) for b in range((pal_w * pal_h) // 3)]
    pal200_rgb = b"".join(pal200[blocks200[p // 3]] for p in range(pal_w * pal_h))
    pal4_200col = rf.encode_image(pal_w, pal_h, pal200_rgb)
    assert pal4_200col[16] == rf.METHOD_PAL3
    assert pal4_200col[32] + 1 == 200
    pal3_200col_payload = rf.pal_encode(pal_w, pal_h, pal200_rgb, method=rf.METHOD_PAL2)
    assert pal3_200col_payload is not None
    pal3_200col = rf.build_file(pal_w, pal_h, rf.METHOD_PAL2, pal3_200col_payload)
    assert struct.unpack_from("<H", pal3_200col, 32)[0] + 1 == 200
    pal2_200col_payload = rf.pal_encode(pal_w, pal_h, pal200_rgb, method=rf.METHOD_PAL)
    assert pal2_200col_payload is not None
    pal2_200col = rf.build_file(pal_w, pal_h, rf.METHOD_PAL, pal2_200col_payload)
    assert struct.unpack_from("<H", pal2_200col, 32)[0] + 1 == 200

    with Image.open(ROOT / "research" / "screens-assets" / "dv_s01.png") as sc_im:
        sc_screen_rgb = sc_im.convert("RGB").crop((40, 160, 40 + pal_w, 160 + pal_h)).tobytes()
    # The encoder writes method 6; method 5 is decode-only and built explicitly.
    sc6_screen = rf.encode_image(pal_w, pal_h, sc_screen_rgb)
    assert sc6_screen[16] == rf.METHOD_SC2
    sc5_screen_payload = rf.sc_encode(pal_w, pal_h, sc_screen_rgb)
    assert sc5_screen_payload is not None
    sc5_screen = rf.build_file(pal_w, pal_h, rf.METHOD_SC, sc5_screen_payload)

    sc_tile = [bytes(((i * 73 + 11) & 0xFF, ((i * 139 + 29) + (i >> 8)) & 0xFF, (i * 197 + 53) & 0xFF)) for i in range(320)]
    assert len(set(sc_tile)) == 320
    random.Random(320).shuffle(sc_tile)
    sc_runs_rgb = b"".join(sc_tile[((y % 10) * 32) + (x & 31)] for y in range(pal_h) for x in range(pal_w))
    sc6_runs = rf.encode_image(pal_w, pal_h, sc_runs_rgb)
    assert sc6_runs[16] == rf.METHOD_SC2
    sc5_runs_payload = rf.sc_encode(pal_w, pal_h, sc_runs_rgb)
    assert sc5_runs_payload is not None
    sc5_runs = rf.build_file(pal_w, pal_h, rf.METHOD_SC, sc5_runs_payload)

    photo_folds: list[tuple[str, int, int, int, bytes]] = []
    photos_dir = ROOT / "testdata" / "photos"
    assert sorted(p.name for p in photos_dir.glob("*.png")) == sorted(name for name, _, _, _, _ in PHOTO_CROPS)
    for name, x, y, w, h in PHOTO_CROPS:
        assert w >= 36 and h >= 34 and w * h > 1200
        with Image.open(photos_dir / name) as im:
            crop = im.convert("RGB").crop((x, y, x + w, y + h))
            rgb = crop.tobytes()
            force_s = FORCE_STRIPS.get(name)
            if force_s is not None:
                blob = rf.build_file(w, h, rf.METHOD_CM, rf.cm_encode(w, h, rgb, nstrips=force_s))
            else:
                blob = rf.encode_image(w, h, rgb)
        assert blob[16] == rf.METHOD_CM
        stem = name.removesuffix(".png")
        photo_folds.append((f"{stem}_{w}x{h}", w, h, rf.METHOD_CM, blob))

    # Order the first 7 slots so small runs (e.g. 10 iterations) exercise methods 0, 1, 2, 3, 4, 5, 6.
    records = [
        ("noise_64x48", noise_w, noise_h, rf.METHOD_RAW, noise_fold),
        photo_folds[0],
        ("pal2_2col_64x48", pal_w, pal_h, rf.METHOD_PAL, pal2_2col),
        ("pal3_200col_64x48", pal_w, pal_h, rf.METHOD_PAL2, pal3_200col),
        ("pal4_200col_64x48", pal_w, pal_h, rf.METHOD_PAL3, pal4_200col),
        ("sc5_screen_64x48", pal_w, pal_h, rf.METHOD_SC, sc5_screen),
        ("sc6_screen_64x48", pal_w, pal_h, rf.METHOD_SC2, sc6_screen),
        ("pal2_200col_64x48", pal_w, pal_h, rf.METHOD_PAL, pal2_200col),
        ("pal3_2col_64x48", pal_w, pal_h, rf.METHOD_PAL2, pal3_2col),
        ("pal4_2col_64x48", pal_w, pal_h, rf.METHOD_PAL3, pal4_2col),
        ("sc5_runs_64x48", pal_w, pal_h, rf.METHOD_SC, sc5_runs),
        ("sc6_runs_64x48", pal_w, pal_h, rf.METHOD_SC2, sc6_runs),
        *photo_folds[1:],
    ]
    assert len(records) == 21
    assert {m for _, _, _, m, _ in records[:7]} == {
        rf.METHOD_RAW,
        rf.METHOD_CM,
        rf.METHOD_PAL,
        rf.METHOD_PAL2,
        rf.METHOD_PAL3,
        rf.METHOD_SC,
        rf.METHOD_SC2,
    }
    for _, w, h, m, blob in records:
        assert w >= 36 and h >= 34 and w * h > 1200
        assert struct.unpack_from("<II", blob, 8) == (w, h)
        assert blob[16] == m
    assert {blob[32] for _, _, _, m, blob in records if m == rf.METHOD_CM} == {1, 2, 3, 4}
    cache_path.write_text(
        json.dumps(
            [{"name": n, "w": w, "h": h, "method": m, "b64": base64.b64encode(b).decode("ascii")} for n, w, h, m, b in records],
            indent=2,
        ),
        encoding="utf-8",
    )
    return records


def seeds() -> list[bytes]:
    return [blob for _, _, _, _, blob in seed_records()]


def mutate(blob: bytes, rng: random.Random) -> bytes:
    b = bytearray(blob)
    method = b[16]
    kind = rng.randrange(6)
    if method == rf.METHOD_RAW:
        payload_len = len(b) - 32
        if kind == 0:                               # flip bits in raw RGB payload
            for _ in range(rng.randrange(1, 4)):
                i = rng.randrange(32, len(b))
                b[i] ^= 1 << rng.randrange(8)
        elif kind == 1:                             # truncate raw payload
            cut = rng.randrange(1, min(64, max(2, payload_len)))
            del b[len(b) - cut :]
        elif kind == 2:                             # extend raw payload
            add = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 17)))
            b.extend(add)
        elif kind == 3:                             # header payload_size disagrees with byte length
            delta = rng.choice((-3, -1, 1, 3, 64))
            struct.pack_into("<I", b, 20, max(0, payload_len + delta))
            return recrc(b, update_payload_size=False)
        elif kind == 4:                             # other dimensions for the same raw bytes
            w, h = struct.unpack_from("<II", b, 8)
            dw, dh = rng.randrange(-3, 4), rng.randrange(-3, 4)
            if dw == 0 and dh == 0:
                dw = 1
            struct.pack_into("<II", b, 8, max(1, w + dw), max(1, h + dh))
        else:                                       # random byte replacements in raw payload
            for i in range(32, len(b)):
                if rng.random() < 0.05:
                    b[i] = rng.randrange(256)
        return recrc(b)
    if method in (rf.METHOD_PAL, rf.METHOD_PAL2):
        count = struct.unpack_from("<H", b, 32)[0] + 1
        s_off = 32 + 2 + 3 * count
        s_start = s_off + 4
        s_len = struct.unpack_from("<I", b, s_off)[0]
        if kind == 0:                               # flip bits in palette or stream
            for _ in range(rng.randrange(1, 4)):
                i = rng.randrange(32, len(b))
                b[i] ^= 1 << rng.randrange(8)
        elif kind == 1:                             # truncate index stream
            cut = rng.randrange(1, max(2, s_len))
            del b[len(b) - cut :]
            struct.pack_into("<I", b, s_off, max(0, s_len - cut))
        elif kind == 2:                             # extend index stream
            add = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
            b.extend(add)
            struct.pack_into("<I", b, s_off, s_len + len(add))
        elif kind == 3:                             # mutate count or stream_size field
            if rng.randrange(2) == 0:
                struct.pack_into("<H", b, 32, (count - 1 + rng.choice((-1, 1, 256))) & 0xFFFF)
            else:
                struct.pack_into("<I", b, s_off, max(0, s_len + rng.choice((-2, -1, 1, 2))))
        elif kind == 4:                             # other dimensions for the same stream
            w, h = struct.unpack_from("<II", b, 8)
            struct.pack_into("<II", b, 8, max(1, w + rng.randrange(-3, 4)), max(1, h + rng.randrange(-3, 4)))
        else:                                       # random stream bytes
            for i in range(s_start, len(b)):
                if rng.random() < 0.1:
                    b[i] = rng.randrange(256)
        return recrc(b)
    if method == rf.METHOD_PAL3:
        # Method 4: one count byte, a bit-packed palette field, then the index stream to the end.
        count = b[32] + 1
        _palette, field_len = rf._pal4_read_field(bytes(b[32:]), count)
        f_start = 33
        s_start = f_start + field_len
        if kind == 0:                               # flip bits in the palette field or the stream
            for _ in range(rng.randrange(1, 4)):
                i = rng.randrange(32, len(b))
                b[i] ^= 1 << rng.randrange(8)
        elif kind == 1:                             # truncate the index stream
            cut = rng.randrange(1, max(2, len(b) - s_start))
            del b[len(b) - cut :]
        elif kind == 2:                             # extend the index stream
            b.extend(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9))))
        elif kind == 3:                             # the count byte or one of the three k nibbles
            which = rng.randrange(4)
            if which == 0:
                b[32] = (b[32] + rng.choice((-1, 1, 128))) & 0xFF
            elif which == 1:
                b[f_start] = (b[f_start] & 0x0F) | (rng.choice((0, 8, 9, 15)) << 4)
            elif which == 2:
                b[f_start] = (b[f_start] & 0xF0) | rng.choice((0, 8, 9, 15))
            else:
                b[f_start + 1] = (b[f_start + 1] & 0x0F) | (rng.choice((0, 8, 9, 15)) << 4)
        elif kind == 4:                             # a Rice code, the padding, or other dimensions
            which = rng.randrange(3)
            if which == 0:                          # one bit inside the Rice-coded entries
                bit = rng.randrange(12, field_len * 8)
                b[f_start + (bit >> 3)] ^= 0x80 >> (bit & 7)
            elif which == 1:                        # the field's last bit: padding when the field is not byte-aligned
                b[s_start - 1] ^= 1
            else:
                w, h = struct.unpack_from("<II", b, 8)
                struct.pack_into("<II", b, 8, max(1, w + rng.randrange(-3, 4)), max(1, h + rng.randrange(-3, 4)))
        else:                                       # random stream bytes
            for i in range(s_start, len(b)):
                if rng.random() < 0.1:
                    b[i] = rng.randrange(256)
        return recrc(b)
    if method in (rf.METHOD_SC, rf.METHOD_SC2):
        # Methods 5 and 6: payload is the range-coded token stream alone (no header field).
        payload_len = len(b) - 32
        if kind == 0:                               # flip bits in the token stream
            for _ in range(rng.randrange(1, 4)):
                i = rng.randrange(32, len(b))
                b[i] ^= 1 << rng.randrange(8)
        elif kind == 1:                             # truncate the token stream (including < 5 bytes)
            cut = rng.randrange(1, max(2, payload_len + 1))
            del b[max(32, len(b) - cut) :]
        elif kind == 2:                             # extend the token stream with trailing bytes
            b.extend(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9))))
        elif kind == 3:                             # corrupt initial range-coder bytes (32..36)
            limit = min(len(b), 37)
            if limit > 32:
                b[rng.randrange(32, limit)] = rng.randrange(256)
        elif kind == 4:                             # other dimensions for the same token stream
            w, h = struct.unpack_from("<II", b, 8)
            dw, dh = rng.randrange(-3, 4), rng.randrange(-3, 4)
            if dw == 0 and dh == 0:
                dw = 1
            struct.pack_into("<II", b, 8, max(1, w + dw), max(1, h + dh))
        else:                                       # random byte replacements in the token stream
            for i in range(32, len(b)):
                if rng.random() < 0.1:
                    b[i] = rng.randrange(256)
        return recrc(b)
    version = b[4]
    nstrips = 1 if version == 2 else max(1, min(4, b[32]))
    hdr_off = 32 if version == 2 else 33
    n_streams = 3 * nstrips
    s_start = 32 + (12 if version == 2 else 1 + 12 * nstrips)
    if kind == 0:                                   # flip bits in the streams (or strip count)
        if version == 3 and rng.randrange(8) == 0:
            b[32] = rng.choice((0, 2, 3, 4, 5, 255))
        else:
            for _ in range(rng.randrange(1, 4)):
                i = rng.randrange(s_start, len(b))
                b[i] ^= 1 << rng.randrange(8)
    elif kind == 1:                                 # truncate one stream
        c = rng.randrange(n_streams)
        sizes = list(struct.unpack_from(f"<{n_streams}I", b, hdr_off))
        cut = rng.randrange(1, max(2, sizes[c]))
        start = s_start + sum(sizes[:c]) + sizes[c] - cut
        del b[start : start + cut]
        sizes[c] -= cut
        struct.pack_into(f"<{n_streams}I", b, hdr_off, *sizes)
    elif kind == 2:                                 # extend one stream
        c = rng.randrange(n_streams)
        sizes = list(struct.unpack_from(f"<{n_streams}I", b, hdr_off))
        add = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
        at = s_start + sum(sizes[: c + 1])
        b[at:at] = add
        sizes[c] += len(add)
        struct.pack_into(f"<{n_streams}I", b, hdr_off, *sizes)
    elif kind == 3:                                 # shuffle bytes between streams
        sizes = list(struct.unpack_from(f"<{n_streams}I", b, hdr_off))
        d = rng.randrange(-4, 5)
        c0 = rng.randrange(n_streams - 1)
        sizes[c0] += d
        sizes[c0 + 1] -= d
        struct.pack_into(f"<{n_streams}I", b, hdr_off, *[s & 0xFFFFFFFF for s in sizes])
    elif kind == 4:                                 # other dimensions for the same streams
        w, h = struct.unpack_from("<II", b, 8)
        struct.pack_into("<II", b, 8, max(1, w + rng.randrange(-3, 4)), max(1, h + rng.randrange(-3, 4)))
    else:                                           # random stream bytes
        for i in range(s_start, len(b)):
            if rng.random() < 0.05:
                b[i] = rng.randrange(256)
    return recrc(b)


class _AbortedByPeer(Exception):
    pass


def _run_decoder_cmd(
    cmd: list[str],
    timeout: float,
    stop_file: Path | None,
) -> subprocess.CompletedProcess[bytes]:
    if subprocess.run is not _ORIG_SUBPROCESS_RUN:
        return subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout)
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + timeout
    try:
        while True:
            if stop_file is not None and stop_file.exists():
                proc.kill()
                proc.communicate()
                raise _AbortedByPeer()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                proc.kill()
                proc.communicate()
                raise subprocess.TimeoutExpired(cmd, timeout)
            try:
                out, err = proc.communicate(timeout=min(0.05, remaining))
                return subprocess.CompletedProcess(cmd, proc.returncode, out, err)
            except subprocess.TimeoutExpired:
                continue
    except BaseException:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
        raise


def verify_mutation(
    i: int,
    blob: bytes,
    threads: str,
    exe: Path = EXE,
    work_dir: Path = WORK,
    stop_file: Path | None = None,
) -> tuple[int, str | None]:
    pid = os.getpid()
    src = work_dir / f"f_{pid}_{i}.fold"
    dest = work_dir / f"f_{pid}_{i}.bmp"
    partial = Path(f"{dest}.partial")
    if stop_file is not None and stop_file.exists():
        return -2, None
    src.write_bytes(blob)
    dest.unlink(missing_ok=True)
    partial.unlink(missing_ok=True)
    try:
        try:
            r = _run_decoder_cmd(
                [str(exe), "decode", "--threads", threads, str(src), str(dest)],
                timeout=60.0,
                stop_file=stop_file,
            )
        except _AbortedByPeer:
            return -2, None
        except subprocess.TimeoutExpired:
            (work_dir / f"hang{i}.fold").write_bytes(blob)
            return -1, f"HANG at iteration {i}"

        if r.returncode not in (0, 4) or partial.exists() or (r.returncode == 4 and dest.exists()) or (r.returncode == 0 and not dest.exists()):
            (work_dir / f"bad{i}.fold").write_bytes(blob)
            return -1, f"BAD exit {r.returncode:#x} at iteration {i}: {r.stderr.decode(errors='replace').strip()}"

        oracle_res: tuple[int, int, bytes] | None = None
        try:
            oracle_res = rf.decode_image(blob)
        except rf.FormatError:
            oracle_res = None

        if r.returncode == 0:
            bmp_bytes = dest.read_bytes()
            try:
                w, h, rgb = rf.read_bmp24(bmp_bytes)
            except rf.FormatError as exc:
                (work_dir / f"mismatch{i}.fold").write_bytes(blob)
                (work_dir / f"mismatch{i}.bmp").write_bytes(bmp_bytes)
                return -1, f"MISMATCH invalid BMP output at iteration {i}: {exc}"
            if oracle_res != (w, h, rgb):
                (work_dir / f"mismatch{i}.fold").write_bytes(blob)
                (work_dir / f"mismatch{i}.bmp").write_bytes(bmp_bytes)
                return -1, f"MISMATCH with the oracle at iteration {i}"
        else:
            if oracle_res is not None:
                (work_dir / f"mismatch{i}.fold").write_bytes(blob)
                return -1, f"MISMATCH with the oracle at iteration {i} (native rejected oracle-valid input)"

        return r.returncode, None
    finally:
        src.unlink(missing_ok=True)
        dest.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)


def _worker_verify(args: tuple[int, bytes, str, str, str]) -> tuple[int, int, str | None]:
    i, blob, threads, exe_str, work_str = args
    work_dir = Path(work_str)
    rc, err = verify_mutation(i, blob, threads, Path(exe_str), work_dir, work_dir / ".fuzz_stop")
    return i, rc, err


def _stop_and_drain_pool(
    pool: ProcessPoolExecutor,
    pending: set[Future[tuple[int, int, str | None]]],
    stop_file: Path | None,
) -> None:
    if stop_file is not None:
        try:
            stop_file.write_bytes(b"1")
        except OSError:
            pass
    for fut in pending:
        fut.cancel()
    if pending:
        _, still_running = wait(pending, timeout=3.0)
    else:
        still_running = set()
    procs = list((getattr(pool, "_processes", None) or {}).values())
    pool.shutdown(wait=not still_running, cancel_futures=True)
    if still_running:
        for proc in procs:
            try:
                proc.terminate()
            except OSError:
                pass
        for proc in procs:
            try:
                proc.join(timeout=2.0)
            except OSError:
                pass


def run_tasks_parallel(
    tasks,
    workers: int,
    worker_fn=_worker_verify,
    valid_rcs: tuple[int, ...] = (0, 4),
) -> tuple[dict[int, int], str | None]:
    counts = {rc: 0 for rc in valid_rcs}
    it = iter(tasks)
    first_item = next(it, None)
    if first_item is None:
        return counts, None
    stop_file = Path(first_item[-1]) / ".fuzz_stop"
    stop_file.unlink(missing_ok=True)
    max_inflight = max(1, workers * 2)
    pool = ProcessPoolExecutor(max_workers=workers)
    pending: set[Future] = set()
    try:
        pending.add(pool.submit(worker_fn, first_item))
        for _ in range(max_inflight - 1):
            try:
                pending.add(pool.submit(worker_fn, next(it)))
            except StopIteration:
                break
        while pending:
            done, pending = wait(pending, return_when=FIRST_COMPLETED)
            for fut in done:
                res = fut.result()
                rc, err = res[1], res[2]
                if err is not None:
                    _stop_and_drain_pool(pool, pending, stop_file)
                    return counts, err
                if rc in counts:
                    counts[rc] += 1
                try:
                    pending.add(pool.submit(worker_fn, next(it)))
                except StopIteration:
                    pass
    except BaseException:
        _stop_and_drain_pool(pool, pending, stop_file)
        raise
    finally:
        stop_file.unlink(missing_ok=True)
    pool.shutdown(wait=True)
    return counts, None


def png_seeds() -> list[bytes]:
    w, h = 17, 13
    gradient = bytes(((x * 5 + y * 9 + c * 31) & 255) for y in range(h) for x in range(w) for c in range(3))
    rng = random.Random(77)
    noise = bytes(rng.randrange(256) for _ in range(w * h * 3))
    checker = b"".join(b"\x18\x28\x38" if ((x // 4) ^ (y // 4)) & 1 else b"\xd8\xe0\xe8" for y in range(h) for x in range(w))
    blobs: list[bytes] = []
    for rgb in (gradient, noise, checker):
        buf = io.BytesIO()
        Image.frombytes("RGB", (w, h), rgb).save(buf, format="PNG")
        blobs.append(buf.getvalue())
    rgba = bytearray()
    for i in range(0, len(gradient), 3):
        rgba += gradient[i:i + 3] + b"\xff"
    buf = io.BytesIO()
    Image.frombytes("RGBA", (w, h), bytes(rgba)).save(buf, format="PNG")
    blobs.append(buf.getvalue())
    blobs.append(_adam7_png(w, h, gradient))
    return blobs


def _adam7_png(w: int, h: int, rgb: bytes) -> bytes:
    """A genuine Adam7 PNG, built pass by pass with filter-0 rows."""
    import zlib as _zlib

    x0 = (0, 4, 0, 2, 0, 1, 0)
    y0 = (0, 0, 4, 0, 2, 0, 1)
    dx = (8, 8, 4, 4, 2, 2, 1)
    dy = (8, 8, 8, 4, 4, 2, 2)
    raw = bytearray()
    for p in range(7):
        pw = (w - x0[p] + dx[p] - 1) // dx[p] if w > x0[p] else 0
        ph = (h - y0[p] + dy[p] - 1) // dy[p] if h > y0[p] else 0
        if not pw or not ph:
            continue
        for yy in range(ph):
            raw.append(0)
            for xx in range(pw):
                gx, gy = x0[p] + xx * dx[p], y0[p] + yy * dy[p]
                raw += rgb[(gy * w + gx) * 3:(gy * w + gx) * 3 + 3]
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 1)
    return _png_build([(b"IHDR", ihdr), (b"IDAT", _zlib.compress(bytes(raw))), (b"IEND", b"")])


def _png_chunks(blob: bytes) -> list[tuple[bytes, bytes]] | None:
    if len(blob) < 8 or blob[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    chunks: list[tuple[bytes, bytes]] = []
    pos = 8
    while pos + 12 <= len(blob):
        length = struct.unpack_from(">I", blob, pos)[0]
        if pos + 12 + length > len(blob):
            return None
        stored_crc = struct.unpack_from(">I", blob, pos + 8 + length)[0]
        if zlib_crc(bytes(blob[pos + 4:pos + 8 + length])) != stored_crc:
            return None  # a chunk whose CRC fails is not valid framing
        chunks.append((bytes(blob[pos + 4:pos + 8]), bytes(blob[pos + 8:pos + 8 + length])))
        pos += 12 + length
        if chunks[-1][0] == b"IEND":
            break
    if chunks and chunks[-1][0] == b"IEND" and pos != len(blob):
        return None  # trailing bytes after IEND are not valid framing
    return chunks or None


def _png_build(chunks: list[tuple[bytes, bytes]]) -> bytes:
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    for typ, data in chunks:
        out += struct.pack(">I", len(data)) + typ + data
        out += struct.pack(">I", zlib_crc(typ + data))
    return bytes(out)


def zlib_crc(data: bytes) -> int:
    import zlib as _zlib

    return _zlib.crc32(data) & 0xFFFFFFFF


RF_MAX_DIM = 32768
RF_MAX_PIXELS = 33554432
_PPM_WS = frozenset(b" \t\r\n\x0b\x0c")


def mutate_png(blob: bytes, rng: random.Random) -> bytes:
    chunks = _png_chunks(blob)
    kind = rng.randrange(6)
    if chunks is None or kind == 1:
        cut = rng.randrange(1, len(blob))
        return blob[:cut]
    mutated = [(typ, bytearray(data)) for typ, data in chunks]
    if kind == 0:  # flip bytes inside one chunk's data; CRC recomputed on rebuild
        idx = rng.randrange(len(mutated))
        data = mutated[idx][1]
        if data:
            start = 8 if mutated[idx][0] == b"IHDR" else 0
            for _ in range(rng.randrange(1, 4)):
                data[rng.randrange(start, len(data))] ^= 1 << rng.randrange(8)
    elif kind == 2:  # damage IHDR fields (depth, type, methods, interlace)
        data = mutated[0][1]
        at = rng.randrange(8, len(data))
        data[at] = rng.randrange(256)
    elif kind == 3:  # insert bytes into IDAT data; CRC recomputed on rebuild
        for idx, (typ, data) in enumerate(mutated):
            if typ == b"IDAT":
                at = rng.randrange(len(data) + 1)
                data[at:at] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
                break
    elif kind == 5 and mutated[0][0] == b"IHDR" and len(mutated[0][1]) == 13:
        # Perturb IHDR dimensions (0, over-limit, or +/- delta).
        data = mutated[0][1]
        w, h = struct.unpack_from(">II", data, 0)
        sub = rng.randrange(3)
        if sub == 0:
            w, h = rng.choice([(0, h), (w, 0), (RF_MAX_DIM + 1, h), (w, RF_MAX_DIM + 1), (8192, 4097)])
        elif sub == 1:
            w = max(1, w + rng.choice((-1, 1, 2)))
        else:
            # Inject an ancillary or tRNS chunk before IDAT while keeping IHDR valid.
            anc_type = rng.choice((b"tEXt", b"tRNS", b"zTXt", b"ABCD"))
            anc_payload = b"key\x00val" if anc_type == b"tEXt" else b"\x00\x00\x00\x00\x00\x00"
            mutated.insert(1, (anc_type, bytearray(anc_payload)))
        struct.pack_into(">II", data, 0, w & 0xFFFFFFFF, h & 0xFFFFFFFF)
    else:  # delete bytes from a chunk's data; CRC recomputed on rebuild
        idx = rng.randrange(len(mutated))
        data = mutated[idx][1]
        if len(data) > 1:
            at = rng.randrange(len(data))
            del data[at:at + rng.randrange(1, min(8, len(data) - at) + 1)]
    return _png_build([(typ, bytes(data)) for typ, data in mutated])


_PNG_KNOWN_CHUNKS = {b"IHDR", b"PLTE", b"IDAT", b"IEND", b"tRNS", b"gAMA", b"cHRM", b"sRGB", b"iCCP", b"tEXt", b"zTXt", b"iTXt", b"bKGD", b"hIST", b"pHYs", b"sBIT", b"sPLT", b"tIME"}


def _png_ihdr_over_limit(blob: bytes) -> bool:
    """True iff the first chunk is a CRC-valid 13-byte IHDR with non-zero
    dimensions exceeding RF_MAX_DIM or RF_MAX_PIXELS (native exit code 5)."""
    if len(blob) < 33 or blob[:8] != b"\x89PNG\r\n\x1a\n":
        return False
    length = struct.unpack_from(">I", blob, 8)[0]
    if length != 13 or blob[12:16] != b"IHDR":
        return False
    stored_crc = struct.unpack_from(">I", blob, 29)[0]
    if zlib_crc(blob[12:29]) != stored_crc:
        return False
    w, h = struct.unpack_from(">II", blob, 16)
    return w > 0 and h > 0 and (w > RF_MAX_DIM or h > RF_MAX_DIM or w * h > RF_MAX_PIXELS)


def _png_is_supported_valid(blob: bytes) -> bool:
    """True when the blob is a valid PNG inside the supported subset
    (8-bit RGB or opaque RGBA, sound chunk structure), independently
    of the native reader: such a mutation must be accepted."""
    chunks = _png_chunks(blob)
    if not chunks or chunks[0][0] != b"IHDR" or chunks[-1][0] != b"IEND" or chunks[-1][1] != b"":
        return False
    if sum(1 for typ, _ in chunks if typ == b"IHDR") != 1:
        return False
    seen_idat = False
    idat_ended = False
    seen_plte = False
    for typ, data in chunks:
        if typ == b"PLTE":
            if seen_plte or seen_idat or len(data) == 0 or len(data) % 3 != 0 or len(data) > 768:
                return False
            seen_plte = True
        if typ == b"IDAT":
            if idat_ended:
                return False
            seen_idat = True
        elif seen_idat and typ != b"IEND":
            idat_ended = True
        if typ not in _PNG_KNOWN_CHUNKS and b"A" <= typ[:1] <= b"Z":
            return False
        if typ == b"tRNS":
            return False
    if not seen_idat:
        return False
    ihdr = chunks[0][1]
    if len(ihdr) != 13 or ihdr[8] != 8 or ihdr[9] not in (2, 6) or ihdr[10] != 0 or ihdr[11] != 0 or ihdr[12] > 1:
        return False
    # Pillow tolerates some stream damage (a bad Adler-32 loads with a
    # warning), so verify the zlib stream strictly here as well.
    import zlib as _zlib

    w, h = struct.unpack_from(">II", ihdr, 0)
    if w == 0 or h == 0 or w > RF_MAX_DIM or h > RF_MAX_DIM or w * h > RF_MAX_PIXELS:
        return False
    channels = 3 if ihdr[9] == 2 else 4
    if ihdr[12]:
        x0 = (0, 4, 0, 2, 0, 1, 0)
        y0 = (0, 0, 4, 0, 2, 0, 1)
        dx = (8, 8, 4, 4, 2, 2, 1)
        dy = (8, 8, 8, 4, 4, 2, 2)
        want_raw = 0
        for p in range(7):
            pw = (w - x0[p] + dx[p] - 1) // dx[p] if w > x0[p] else 0
            ph = (h - y0[p] + dy[p] - 1) // dy[p] if h > y0[p] else 0
            if pw and ph:
                want_raw += ph * (pw * channels + 1)
    else:
        want_raw = h * (w * channels + 1)
    try:
        streamer = _zlib.decompressobj()
        raw = streamer.decompress(b"".join(data for typ, data in chunks if typ == b"IDAT"))
        # Bytes after the zlib stream ends are not valid IDAT content,
        # however lenient a decompressor is about ignoring them.
        if not streamer.eof or streamer.unused_data:
            return False
    except Exception:
        return False
    if len(raw) != want_raw:
        return False
    try:
        with Image.open(io.BytesIO(blob)) as im:
            im.load()
            if im.mode not in ("RGB", "RGBA"):
                return False
            if im.mode == "RGBA" and any(a != 255 for a in im.convert("RGBA").tobytes()[3::4]):
                return False
    except Exception:
        return False
    return True


def verify_png_mutation(
    i: int,
    blob: bytes,
    exe: Path = EXE,
    work_dir: Path = WORK,
    stop_file: Path | None = None,
) -> tuple[int, str | None]:
    pid = os.getpid()
    src = work_dir / f"p_{pid}_{i}.png"
    dest = work_dir / f"p_{pid}_{i}.fold"
    partial = Path(f"{dest}.partial")
    if stop_file is not None and stop_file.exists():
        return -2, None
    src.write_bytes(blob)
    dest.unlink(missing_ok=True)
    partial.unlink(missing_ok=True)
    try:
        try:
            r = _run_decoder_cmd([str(exe), "encode", str(src), str(dest)], timeout=60.0, stop_file=stop_file)
        except _AbortedByPeer:
            return -2, None
        except subprocess.TimeoutExpired:
            (work_dir / f"pnghang{i}.png").write_bytes(blob)
            return -1, f"PNG HANG at iteration {i}"
        over_limit = _png_ihdr_over_limit(blob)
        allowed_rcs = (5,) if over_limit else (0, 3, 4)
        if r.returncode not in allowed_rcs or partial.exists() or (r.returncode != 0 and dest.exists()) or (r.returncode == 0 and not dest.exists()):
            (work_dir / f"pngbad{i}.png").write_bytes(blob)
            return -1, f"PNG BAD exit {r.returncode:#x} at iteration {i}: {r.stderr.decode(errors='replace').strip()}"
        valid_sup = _png_is_supported_valid(blob)
        if r.returncode != 0 and valid_sup:
            (work_dir / f"pngmismatch{i}.png").write_bytes(blob)
            return -1, f"PNG MISMATCH valid supported PNG refused with exit {r.returncode} at iteration {i}"
        if r.returncode == 0:
            if not valid_sup:
                (work_dir / f"pngmismatch{i}.png").write_bytes(blob)
                return -1, f"PNG MISMATCH encoder accepted oracle-invalid PNG at iteration {i}"
            try:
                with Image.open(src) as im:
                    rgba = im.convert("RGBA")
                    if any(a != 255 for a in rgba.tobytes()[3::4]):
                        return -1, f"PNG MISMATCH non-opaque alpha accepted at iteration {i}"
                    want = (im.width, im.height, im.convert("RGB").tobytes())
            except Exception as exc:
                return -1, f"PNG MISMATCH Pillow rejected encoder-accepted input at iteration {i}: {exc}"
            try:
                got = rf.decode_image(dest.read_bytes())
            except rf.FormatError as exc:
                return -1, f"PNG MISMATCH oracle rejected encoded output at iteration {i}: {exc}"
            if got != want:
                return -1, f"PNG MISMATCH pixels differ at iteration {i}"
        return r.returncode, None
    finally:
        src.unlink(missing_ok=True)
        dest.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)


def bmp_seeds() -> list[bytes]:
    """Supported valid 24-bit uncompressed BMP seeds (bottom-up, top-down,
    odd widths with row padding, pixel-offset gap, and zero bfSize/biSizeImage)."""
    blobs: list[bytes] = []
    for w, h, topdown, gap, zero_bf, zero_bi in (
        (16, 12, False, 0, False, False),
        (15, 13, False, 0, False, False),   # 1-byte row padding
        (14, 12, True, 0, False, False),    # top-down + 2-byte row padding
        (13, 12, False, 16, False, True),   # 3-byte row padding + header gap + biSizeImage == 0
        (17, 12, True, 8, True, False),     # top-down + bfSize == 0
    ):
        rgb = bytes(((x * 7 + y * 13 + c * 43) & 255) for y in range(h) for x in range(w) for c in range(3))
        stride = (w * 3 + 3) & ~3
        off = 54 + gap
        total = off + stride * h
        hdr = bytearray(off)
        struct.pack_into("<H", hdr, 0, 0x4D42)
        struct.pack_into("<I", hdr, 2, 0 if zero_bf else total)
        struct.pack_into("<I", hdr, 10, off)
        struct.pack_into("<I", hdr, 14, 40)
        struct.pack_into("<i", hdr, 18, w)
        struct.pack_into("<i", hdr, 22, -h if topdown else h)
        struct.pack_into("<H", hdr, 26, 1)
        struct.pack_into("<H", hdr, 28, 24)
        struct.pack_into("<I", hdr, 30, 0)
        struct.pack_into("<I", hdr, 34, 0 if zero_bi else stride * h)
        rows = bytearray(stride * h)
        for y in range(h):
            file_row = y if topdown else (h - 1 - y)
            base = file_row * stride
            src_row = y * w * 3
            for x in range(w):
                r_ch, g_ch, b_ch = rgb[src_row + 3 * x : src_row + 3 * x + 3]
                rows[base + 3 * x : base + 3 * x + 3] = bytes((b_ch, g_ch, r_ch))
        blobs.append(bytes(hdr + rows))
    return blobs


def bmp_ref_load(blob: bytes) -> tuple[int, tuple[int, int, bytes] | None]:
    """Independent Python specification of the BMP subset in spec/FOLD.md:
    returns (0, (w, h, rgb)), (3, None) for unsupported BMP features,
    (4, None) for malformed BMP data, or (5, None) when dimensions exceed limits."""
    size = len(blob)
    if size < 54:
        return 4, None
    if struct.unpack_from("<H", blob, 0)[0] != 0x4D42:
        return 4, None
    if struct.unpack_from("<I", blob, 14)[0] != 40:
        return 3, None
    sw = struct.unpack_from("<i", blob, 18)[0]
    if sw < 1:
        return 4, None
    if sw > RF_MAX_DIM:
        return 5, None
    sh = struct.unpack_from("<i", blob, 22)[0]
    if sh == 0 or sh == -2147483648:
        return 4, None
    topdown = sh < 0
    w, h = sw, (-sh if topdown else sh)
    if h > RF_MAX_DIM:
        return 5, None
    if struct.unpack_from("<H", blob, 26)[0] != 1:
        return 3, None
    if struct.unpack_from("<H", blob, 28)[0] != 24:
        return 3, None
    if struct.unpack_from("<I", blob, 30)[0] != 0:
        return 3, None
    if w * h > RF_MAX_PIXELS:
        return 5, None
    off = struct.unpack_from("<I", blob, 10)[0]
    if off < 54:
        return 4, None
    stride = (w * 3 + 3) & ~3
    pixel_end = off + stride * h
    if pixel_end > size:
        return 4, None
    bf_size = struct.unpack_from("<I", blob, 2)[0]
    if bf_size != 0 and (bf_size > size or bf_size < pixel_end):
        return 4, None
    image_size = struct.unpack_from("<I", blob, 34)[0]
    if image_size != 0 and image_size < stride * h:
        return 4, None
    out = bytearray(w * h * 3)
    for y in range(h):
        file_row = y if topdown else (h - 1 - y)
        row_off = off + stride * file_row
        dst_off = y * w * 3
        for x in range(w):
            b_ch, g_ch, r_ch = blob[row_off + 3 * x : row_off + 3 * x + 3]
            out[dst_off + 3 * x : dst_off + 3 * x + 3] = bytes((r_ch, g_ch, b_ch))
    return 0, (w, h, bytes(out))


def mutate_bmp(blob: bytes, rng: random.Random) -> bytes:
    b = bytearray(blob)
    kind = rng.randrange(7)
    if kind == 0:  # flip bits in pixel or padding bytes (often stays valid)
        start = min(len(b) - 1, max(54, struct.unpack_from("<I", b, 10)[0] if len(b) >= 14 else 54))
        for _ in range(rng.randrange(1, 5)):
            b[rng.randrange(start, len(b))] ^= 1 << rng.randrange(8)
    elif kind == 1:  # truncate anywhere
        return bytes(b[: rng.randrange(0, len(b))])
    elif kind == 2:  # mutate DIB header size, planes, bpp, or compression (probes exit 3)
        field = rng.randrange(4)
        if field == 0 and len(b) >= 18:
            struct.pack_into("<I", b, 14, rng.choice((12, 40, 108, 124, 999)))
        elif field == 1 and len(b) >= 28:
            struct.pack_into("<H", b, 26, rng.choice((0, 2, 255)))
        elif field == 2 and len(b) >= 30:
            struct.pack_into("<H", b, 28, rng.choice((1, 4, 8, 16, 32)))
        elif len(b) >= 34:
            struct.pack_into("<I", b, 30, rng.choice((1, 2, 3, 6)))
    elif kind == 3 and len(b) >= 26:  # mutate width/height (0, negative, INT_MIN, >RF_MAX_DIM, >RF_MAX_PIXELS, +/- delta)
        sw, sh = struct.unpack_from("<ii", b, 18)
        sub = rng.randrange(3)
        if sub == 0:
            sw, sh = rng.choice([
                (0, sh), (-5, sh), (sw, 0), (sw, -2147483648),
                (RF_MAX_DIM + 1, 1), (1, RF_MAX_DIM + 1), (1, -(RF_MAX_DIM + 1)),
                (8192, 4097), (8192, -4097),
            ])
        elif sub == 1:
            sh = -sh
        else:
            sw = max(1, sw + rng.randrange(-3, 4))
        struct.pack_into("<ii", b, 18, sw, sh)
    elif kind == 4 and len(b) >= 38:  # mutate bfSize, bfOffBits, or biSizeImage
        field = rng.randrange(3)
        if field == 0:
            struct.pack_into("<I", b, 2, rng.choice((0, 1, 53, len(b) - 1, len(b), len(b) + 16)))
        elif field == 1:
            struct.pack_into("<I", b, 10, rng.choice((0, 14, 53, 54, len(b) + 4)))
        else:
            struct.pack_into("<I", b, 34, rng.choice((0, 1, 12, len(b))))
    elif kind == 5:  # append trailing bytes (valid when bfSize == 0 or bfSize <= new size, invalid if bfSize > size)
        b.extend(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 17))))
    else:  # corrupt signature or random header byte
        b[rng.randrange(min(len(b), 54))] ^= 1 << rng.randrange(8)
    return bytes(b)


def verify_bmp_mutation(
    i: int,
    blob: bytes,
    exe: Path = EXE,
    work_dir: Path = WORK,
    stop_file: Path | None = None,
) -> tuple[int, str | None]:
    pid = os.getpid()
    src = work_dir / f"b_{pid}_{i}.bmp"
    dest = work_dir / f"b_{pid}_{i}.fold"
    partial = Path(f"{dest}.partial")
    if stop_file is not None and stop_file.exists():
        return -2, None
    src.write_bytes(blob)
    dest.unlink(missing_ok=True)
    partial.unlink(missing_ok=True)
    try:
        try:
            r = _run_decoder_cmd([str(exe), "encode", str(src), str(dest)], timeout=60.0, stop_file=stop_file)
        except _AbortedByPeer:
            return -2, None
        except subprocess.TimeoutExpired:
            (work_dir / f"bmphang{i}.bmp").write_bytes(blob)
            return -1, f"BMP HANG at iteration {i}"
        if r.returncode not in (0, 3, 4, 5) or partial.exists() or (r.returncode != 0 and dest.exists()) or (r.returncode == 0 and not dest.exists()):
            (work_dir / f"bmpbad{i}.bmp").write_bytes(blob)
            return -1, f"BMP BAD exit {r.returncode:#x} at iteration {i}: {r.stderr.decode(errors='replace').strip()}"
        ref_rc, ref_img = bmp_ref_load(blob)
        if r.returncode != ref_rc:
            (work_dir / f"bmpmismatch{i}.bmp").write_bytes(blob)
            return -1, f"BMP MISMATCH exit {r.returncode} != oracle {ref_rc} at iteration {i}"
        if r.returncode == 0:
            try:
                got = rf.decode_image(dest.read_bytes())
            except rf.FormatError as exc:
                (work_dir / f"bmpmismatch{i}.bmp").write_bytes(blob)
                return -1, f"BMP MISMATCH oracle rejected encoded output at iteration {i}: {exc}"
            if got != ref_img:
                (work_dir / f"bmpmismatch{i}.bmp").write_bytes(blob)
                return -1, f"BMP MISMATCH pixels differ at iteration {i}"
        return r.returncode, None
    finally:
        src.unlink(missing_ok=True)
        dest.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)


def ppm_seeds() -> list[bytes]:
    """Supported valid binary P6 maxval-255 PPM seeds, including comments,
    varied ASCII whitespace, and first raster bytes starting with '#' or whitespace."""
    w, h = 17, 13
    gradient = bytes(((x * 5 + y * 9 + c * 31) & 255) for y in range(h) for x in range(w) for c in range(3))
    rng = random.Random(91)
    noise = bytes(rng.randrange(256) for _ in range(w * h * 3))
    # Raster whose very first byte is '#' and second is ' ' so separator rules are exercised.
    tricky = bytearray(gradient)
    tricky[0:3] = b"# \n"
    return [
        f"P6\n{w} {h}\n255\n".encode("ascii") + gradient,
        f"P6\t# header comment\n{w}\r\n# mid comment\n{h}\t255 ".encode("ascii") + noise,
        f"P6\n# c1\n# c2\n{w} {h}\n255\n".encode("ascii") + bytes(tricky),
        b"P6\x0b13\x0c11\r255\t" + bytes(((i * 17) & 255) for i in range(13 * 11 * 3)),
    ]


def _ppm_skip(blob: bytes, pos: int) -> int:
    n = len(blob)
    while True:
        while pos < n and blob[pos] in _PPM_WS:
            pos += 1
        if pos < n and blob[pos] == ord("#"):
            while pos < n and blob[pos] != ord("\n"):
                pos += 1
            continue
        return pos


def _ppm_token(blob: bytes, pos: int) -> tuple[int, int, int]:
    """Returns (rc, value, next_pos) matching ppm_number in src/ppm.c."""
    n = len(blob)
    p = _ppm_skip(blob, pos)
    if p >= n or not (0x30 <= blob[p] <= 0x39):
        return 4, 0, p
    v = 0
    while p < n and 0x30 <= blob[p] <= 0x39:
        v = v * 10 + (blob[p] - 0x30)
        if v > 4294967295:
            return 5, 0, p
        p += 1
    if p < n and blob[p] not in _PPM_WS and blob[p] != ord("#"):
        return 4, 0, p
    return 0, v, p


def ppm_ref_load(blob: bytes) -> tuple[int, tuple[int, int, bytes] | None]:
    """Independent Python specification of binary P6 PPM (maxval 255):
    returns (0, (w, h, rgb)), (3, None) for P3 or maxval != 255,
    (4, None) for malformed framing, or (5, None) for token/dimension limits."""
    size = len(blob)
    if size < 2:
        return 4, None
    if blob[0] != ord("P") or blob[1] not in (ord("6"), ord("3")):
        return 4, None
    if blob[1] == ord("3"):
        return 3, None
    if size < 3 or blob[2] not in _PPM_WS:
        return 4, None
    rc, w, pos = _ppm_token(blob, 2)
    if rc != 0:
        return rc, None
    rc, h, pos = _ppm_token(blob, pos)
    if rc != 0:
        return rc, None
    rc, maxval, pos = _ppm_token(blob, pos)
    if rc != 0:
        return rc, None
    if w == 0 or h == 0:
        return 4, None
    if w > RF_MAX_DIM or h > RF_MAX_DIM or w * h > RF_MAX_PIXELS:
        return 5, None
    if maxval != 255:
        return 3, None
    if pos >= size or blob[pos] not in _PPM_WS:
        return 4, None
    pos += 1
    need = w * h * 3
    if size - pos != need:
        return 4, None
    return 0, (w, h, blob[pos : pos + need])


def mutate_ppm(blob: bytes, rng: random.Random) -> bytes:
    kind = rng.randrange(7)
    if kind == 0:  # flip raster bytes while keeping header intact (stays valid)
        b = bytearray(blob)
        start = min(len(b) - 1, 24)
        for _ in range(rng.randrange(1, 5)):
            b[rng.randrange(start, len(b))] ^= 1 << rng.randrange(8)
        return bytes(b)
    if kind == 1:  # truncate
        return blob[: rng.randrange(0, len(blob))]
    if kind == 2:  # append trailing byte(s) (must be rejected with 4)
        return blob + bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9)))
    if kind == 3:  # probe P3, bad magic, or maxval != 255 (exit 3 or 4)
        sub = rng.randrange(3)
        if sub == 0:
            return b"P3\n2 2\n255\n0 0 0  255 255 255  1 2 3  4 5 6\n"
        if sub == 1:
            mv = rng.choice((0, 1, 15, 254, 256, 65535))
            return f"P6\n4 3\n{mv}\n".encode("ascii") + bytes(36 if mv <= 255 else 72)
        b = bytearray(blob)
        b[1] = rng.choice((ord("1"), ord("2"), ord("4"), ord("5"), ord("7"), 0))
        return bytes(b)
    if kind == 4:  # probe dimension / token overflow limits (exit 4 or 5)
        hdr = rng.choice((
            b"P6\n0 4\n255\n",
            b"P6\n4 0\n255\n",
            b"P6\n32769 1\n255\n",
            b"P6\n1 32769\n255\n",
            b"P6\n8192 4097\n255\n",
            b"P6\n4294967296 1\n255\n",
            b"P6\n1 99999999999999999999\n255\n",
            b"P6\n4 4\n4294967296\n",
        ))
        return hdr + bytes(rng.randrange(256) for _ in range(12))
    if kind == 5:  # probe comments, double whitespace after maxval, or comment after maxval
        w, h = rng.choice(((4, 3), (7, 5), (16, 8)))
        sep = rng.choice((b"\n", b" ", b"\t", b"\n# comment after maxval\n", b"  ", b""))
        c_ins = rng.choice((b"#ok\n", b"", b" #c\r\n"))
        raster = bytes(rng.randrange(256) for _ in range(w * h * 3))
        return b"P6\n" + c_ins + f"{w} {h}\n".encode("ascii") + c_ins + b"255" + sep + raster
    b = bytearray(blob)
    b[rng.randrange(len(b))] = rng.randrange(256)
    return bytes(b)


def verify_ppm_mutation(
    i: int,
    blob: bytes,
    exe: Path = EXE,
    work_dir: Path = WORK,
    stop_file: Path | None = None,
) -> tuple[int, str | None]:
    pid = os.getpid()
    src = work_dir / f"m_{pid}_{i}.ppm"
    dest = work_dir / f"m_{pid}_{i}.fold"
    partial = Path(f"{dest}.partial")
    if stop_file is not None and stop_file.exists():
        return -2, None
    src.write_bytes(blob)
    dest.unlink(missing_ok=True)
    partial.unlink(missing_ok=True)
    try:
        try:
            r = _run_decoder_cmd([str(exe), "encode", str(src), str(dest)], timeout=60.0, stop_file=stop_file)
        except _AbortedByPeer:
            return -2, None
        except subprocess.TimeoutExpired:
            (work_dir / f"ppmhang{i}.ppm").write_bytes(blob)
            return -1, f"PPM HANG at iteration {i}"
        if r.returncode not in (0, 3, 4, 5) or partial.exists() or (r.returncode != 0 and dest.exists()) or (r.returncode == 0 and not dest.exists()):
            (work_dir / f"ppmbad{i}.ppm").write_bytes(blob)
            return -1, f"PPM BAD exit {r.returncode:#x} at iteration {i}: {r.stderr.decode(errors='replace').strip()}"
        ref_rc, ref_img = ppm_ref_load(blob)
        if r.returncode != ref_rc:
            (work_dir / f"ppmmismatch{i}.ppm").write_bytes(blob)
            return -1, f"PPM MISMATCH exit {r.returncode} != oracle {ref_rc} at iteration {i}"
        if r.returncode == 0:
            try:
                got = rf.decode_image(dest.read_bytes())
            except rf.FormatError as exc:
                (work_dir / f"ppmmismatch{i}.ppm").write_bytes(blob)
                return -1, f"PPM MISMATCH oracle rejected encoded output at iteration {i}: {exc}"
            if got != ref_img:
                (work_dir / f"ppmmismatch{i}.ppm").write_bytes(blob)
                return -1, f"PPM MISMATCH pixels differ at iteration {i}"
        return r.returncode, None
    finally:
        src.unlink(missing_ok=True)
        dest.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)


def _worker_verify_input(args: tuple[str, int, bytes, str, str]) -> tuple[int, int, str | None, str]:
    fmt, i, blob, exe_str, work_str = args
    work_dir = Path(work_str)
    stop_file = work_dir / ".fuzz_stop"
    exe_path = Path(exe_str)
    if fmt == "png":
        rc, err = verify_png_mutation(i, blob, exe_path, work_dir, stop_file)
    elif fmt == "bmp":
        rc, err = verify_bmp_mutation(i, blob, exe_path, work_dir, stop_file)
    elif fmt == "ppm":
        rc, err = verify_ppm_mutation(i, blob, exe_path, work_dir, stop_file)
    else:
        raise ValueError(f"unknown input format {fmt!r}")
    return i, rc, err, fmt


def _worker_verify_any(args: tuple[str, int, bytes, str, str, str]) -> tuple[int, int, str | None, str]:
    fmt, i, blob, threads, exe_str, work_str = args
    work_dir = Path(work_str)
    stop_file = work_dir / ".fuzz_stop"
    exe_path = Path(exe_str)
    if fmt == "fold":
        rc, err = verify_mutation(i, blob, threads, exe_path, work_dir, stop_file)
    elif fmt == "png":
        rc, err = verify_png_mutation(i, blob, exe_path, work_dir, stop_file)
    elif fmt == "bmp":
        rc, err = verify_bmp_mutation(i, blob, exe_path, work_dir, stop_file)
    elif fmt == "ppm":
        rc, err = verify_ppm_mutation(i, blob, exe_path, work_dir, stop_file)
    else:
        raise ValueError(f"unknown format {fmt!r}")
    return i, rc, err, fmt


def _check_png_seeds_and_oracle(base: list[bytes]) -> str | None:
    for s, seed in enumerate(base):
        rc, err = verify_png_mutation(-(s + 1), seed)
        if err is not None:
            return err
        if rc != 0:
            return f"PNG seed {s} refused with exit {rc}, want 0"
    seed_chunks = _png_chunks(base[0]) or []
    trailing = base[0] + b"x"
    plte_late = _png_build(seed_chunks[:-1] + [(b"PLTE", b"\x01\x02\x03"), seed_chunks[-1]])
    plte_dup = _png_build(seed_chunks[:1] + [(b"PLTE", b"\x01\x02\x03"), (b"PLTE", b"\x04\x05\x06")] + seed_chunks[1:])
    ihdr_dup = _png_build(seed_chunks[:1] + seed_chunks[:1] + seed_chunks[1:])
    bad_crc = bytearray(base[0])
    bad_crc[29] ^= 0xFF  # inside IHDR data; its CRC no longer matches
    over_ihdr = bytearray(seed_chunks[0][1])
    struct.pack_into(">II", over_ihdr, 0, RF_MAX_DIM + 1, 1)
    over_png = _png_build([(b"IHDR", bytes(over_ihdr)), *seed_chunks[1:]])
    for name, variant in (("trailing", trailing), ("plte_late", plte_late), ("plte_dup", plte_dup),
                          ("ihdr_dup", ihdr_dup), ("bad_crc", bytes(bad_crc)), ("over_png", over_png)):
        if _png_is_supported_valid(variant):
            return f"PNG oracle self-check failed: {name} judged valid"
    if not _png_ihdr_over_limit(over_png):
        return "PNG oracle self-check failed: over_png not classified as over-limit"
    return None


def _check_bmp_seeds_and_oracle(base: list[bytes]) -> str | None:
    for s, seed in enumerate(base):
        rc, err = verify_bmp_mutation(-(s + 1), seed)
        if err is not None:
            return err
        if rc != 0:
            return f"BMP seed {s} refused with exit {rc}, want 0"
    trunc = base[0][:40]
    unsup_bpp = bytearray(base[0])
    struct.pack_into("<H", unsup_bpp, 28, 32)
    over_dim = bytearray(base[0])
    struct.pack_into("<i", over_dim, 18, RF_MAX_DIM + 1)
    int_min_h = bytearray(base[0])
    struct.pack_into("<i", int_min_h, 22, -2147483648)
    for name, blob, want_rc in (
        ("trunc", trunc, 4),
        ("unsup_bpp", bytes(unsup_bpp), 3),
        ("over_dim", bytes(over_dim), 5),
        ("int_min_h", bytes(int_min_h), 4),
    ):
        got_rc, _ = bmp_ref_load(blob)
        if got_rc != want_rc:
            return f"BMP oracle self-check failed on {name}: got {got_rc}, want {want_rc}"
    return None


def _check_ppm_seeds_and_oracle(base: list[bytes]) -> str | None:
    for s, seed in enumerate(base):
        rc, err = verify_ppm_mutation(-(s + 1), seed)
        if err is not None:
            return err
        if rc != 0:
            return f"PPM seed {s} refused with exit {rc}, want 0"
    for name, blob, want_rc in (
        ("trailing", base[0] + b"x", 4),
        ("p3", b"P3\n1 1\n255\n0 0 0\n", 3),
        ("maxval16", b"P6\n1 1\n65535\n\x00\x00\x00\x00\x00\x00", 3),
        ("over_tok", b"P6\n4294967296 1\n255\n\x00\x00\x00", 5),
        ("over_dim", b"P6\n32769 1\n255\n\x00\x00\x00", 5),
        ("comment_after_maxval", b"P6\n1 1\n255#c\n\x00\x00\x00", 4),
    ):
        got_rc, _ = ppm_ref_load(blob)
        if got_rc != want_rc:
            return f"PPM oracle self-check failed on {name}: got {got_rc}, want {want_rc}"
    return None


def _run_input_fuzz(fmt: str, iterations: int, rng: random.Random) -> int:
    with _staged_fuzz_env() as (staged_exe, work_dir):
        if fmt == "png":
            base = png_seeds()
            err = _check_png_seeds_and_oracle(base)
            mutate_fn = mutate_png
            verify_fn = verify_png_mutation
            label = "PNG"
        elif fmt == "bmp":
            base = bmp_seeds()
            err = _check_bmp_seeds_and_oracle(base)
            mutate_fn = mutate_bmp
            verify_fn = verify_bmp_mutation
            label = "BMP"
        else:
            base = ppm_seeds()
            err = _check_ppm_seeds_and_oracle(base)
            mutate_fn = mutate_ppm
            verify_fn = verify_ppm_mutation
            label = "PPM"
        if err is not None:
            print(err)
            return 1
        if iterations <= 20:
            counts = {0: 0, 3: 0, 4: 0, 5: 0}
            for i in range(iterations):
                blob = mutate_fn(base[i % len(base)], rng)
                rc, err = verify_fn(i, blob, staged_exe, work_dir)
                if err is not None:
                    print(err)
                    return 1
                counts[rc] += 1
        else:
            exe_s, work_s = str(staged_exe), str(work_dir)
            tasks = ((fmt, i, mutate_fn(base[i % len(base)], rng), exe_s, work_s) for i in range(iterations))
            workers = min(os.cpu_count() or 4, 12)
            counts, err = run_tasks_parallel(
                tasks, workers, worker_fn=_worker_verify_input, valid_rcs=(0, 3, 4, 5)
            )
            if err is not None:
                print(err)
                return 1
    limit_note = f", {counts[5]} over-limit" if counts[5] else ""
    print(f"{iterations} mutated {label} files: {counts[0]} accepted, {counts[3]} unsupported, {counts[4]} malformed{limit_note}, no crash, hang or mismatch")
    return 0


def run_png_fuzz(iterations: int, rng: random.Random) -> int:
    return _run_input_fuzz("png", iterations, rng)


def run_bmp_fuzz(iterations: int, rng: random.Random) -> int:
    return _run_input_fuzz("bmp", iterations, rng)


def run_ppm_fuzz(iterations: int, rng: random.Random) -> int:
    return _run_input_fuzz("ppm", iterations, rng)


_ALL_FOLD_RING: tuple[int, ...] = (0, 1, 2, 3, 4, 5, 6, 0, 2, 3, 4, 5, 6, 0, 2, 3, 4, 5, 6, 0, 2, 3, 4, 5, 6)
_ALL_FOLD_THREADS: tuple[str, ...] = ("1", "3", "6", "9")


def _iter_all_formats_schedule(
    iterations: int,
    fold_records: list[tuple[str, int, int, int, bytes]],
    n_bmp: int,
    n_png: int,
    n_ppm: int,
):
    """Yield (fmt, iteration, seed_idx, threads) for --all-formats, rotating
    --threads 1/3/6/9 independently per .fold seed so every multi-strip fixture
    exercises serial and multi-threaded decoding."""
    by_method: dict[int, list[int]] = {m: [] for m in range(7)}
    cm_compact: list[int] = []
    cm_tall: list[int] = []
    for idx, (_, w, h, m, _) in enumerate(fold_records):
        by_method[m].append(idx)
        if m == 1:
            (cm_compact if w * h <= 4096 else cm_tall).append(idx)
    fold_idx = 0
    cm_idx = 0
    in_idx = 0
    per_m_idx = {m: 0 for m in range(7)}
    seed_thr_idx = [0] * len(fold_records)
    for i in range(iterations):
        if i % 6 != 5:
            m = _ALL_FOLD_RING[fold_idx % len(_ALL_FOLD_RING)]
            fold_idx += 1
            if m == 1:
                if cm_tall and cm_idx % 16 == 15:
                    seed_idx = cm_tall[(cm_idx // 16) % len(cm_tall)]
                else:
                    seed_idx = cm_compact[cm_idx % len(cm_compact)]
                cm_idx += 1
            else:
                pool_m = by_method[m]
                seed_idx = pool_m[per_m_idx[m] % len(pool_m)]
                per_m_idx[m] += 1
            threads = _ALL_FOLD_THREADS[seed_thr_idx[seed_idx] % len(_ALL_FOLD_THREADS)]
            seed_thr_idx[seed_idx] += 1
            yield ("fold", i, seed_idx, threads)
        else:
            fmt = ("bmp", "png", "ppm")[in_idx % 3]
            k = in_idx // 3
            in_idx += 1
            n_pool = n_bmp if fmt == "bmp" else (n_png if fmt == "png" else n_ppm)
            yield (fmt, i, k % n_pool, "1")


def run_all_formats_fuzz(iterations: int, rng: random.Random) -> int:
    """Unified campaign across .fold methods 0..6 and input formats BMP, PNG, PPM."""
    fold_records = seed_records()
    fold_base = [blob for _, _, _, _, blob in fold_records]
    bmp_base = bmp_seeds()
    png_base = png_seeds()
    ppm_base = ppm_seeds()

    method_counts = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0, 5: 0, 6: 0}
    fmt_counts: dict[str, dict[int, int]] = {
        "fold": {0: 0, 4: 0},
        "bmp": {0: 0, 3: 0, 4: 0, 5: 0},
        "png": {0: 0, 3: 0, 4: 0, 5: 0},
        "ppm": {0: 0, 3: 0, 4: 0, 5: 0},
    }

    with _staged_fuzz_env() as (staged_exe, work_dir):
        for s, seed_blob in enumerate(fold_base):
            rc, err = verify_mutation(-(s + 1), seed_blob, _ALL_FOLD_THREADS[s % len(_ALL_FOLD_THREADS)], staged_exe, work_dir)
            if err is not None or rc != 0:
                print(err or f"FOLD seed {s} refused with exit {rc}, want 0")
                return 1
        for check_err in (
            _check_bmp_seeds_and_oracle(bmp_base),
            _check_png_seeds_and_oracle(png_base),
            _check_ppm_seeds_and_oracle(ppm_base),
        ):
            if check_err is not None:
                print(check_err)
                return 1

        exe_s, work_s = str(staged_exe), str(work_dir)

        def task_iter():
            for fmt, i, seed_idx, threads in _iter_all_formats_schedule(
                iterations, fold_records, len(bmp_base), len(png_base), len(ppm_base)
            ):
                if fmt == "fold":
                    seed_blob = fold_base[seed_idx]
                    method_counts[seed_blob[16]] += 1
                    blob = mutate(seed_blob, rng)
                    yield ("fold", i, blob, threads, exe_s, work_s)
                else:
                    if fmt == "bmp":
                        blob = mutate_bmp(bmp_base[seed_idx], rng)
                    elif fmt == "png":
                        blob = mutate_png(png_base[seed_idx], rng)
                    else:
                        blob = mutate_ppm(ppm_base[seed_idx], rng)
                    yield (fmt, i, blob, threads, exe_s, work_s)

        if iterations <= 20:
            for fmt, i, blob, threads, _, _ in task_iter():
                _, rc, err, _ = _worker_verify_any((fmt, i, blob, threads, exe_s, work_s))
                if err is not None:
                    print(err)
                    return 1
                fmt_counts[fmt][rc] += 1
        else:
            workers = min(os.cpu_count() or 4, 16)
            stop_file = work_dir / ".fuzz_stop"
            stop_file.unlink(missing_ok=True)
            it = task_iter()
            max_inflight = max(1, workers * 2)
            pool = ProcessPoolExecutor(max_workers=workers)
            pending: set[Future] = set()
            try:
                for _ in range(max_inflight):
                    try:
                        pending.add(pool.submit(_worker_verify_any, next(it)))
                    except StopIteration:
                        break
                while pending:
                    done, pending = wait(pending, return_when=FIRST_COMPLETED)
                    for fut in done:
                        _, rc, err, fmt = fut.result()
                        if err is not None:
                            _stop_and_drain_pool(pool, pending, stop_file)
                            print(err)
                            return 1
                        if rc in fmt_counts[fmt]:
                            fmt_counts[fmt][rc] += 1
                        try:
                            pending.add(pool.submit(_worker_verify_any, next(it)))
                        except StopIteration:
                            pass
            except BaseException:
                _stop_and_drain_pool(pool, pending, stop_file)
                raise
            finally:
                stop_file.unlink(missing_ok=True)
            pool.shutdown(wait=True)

    m_summary = ", ".join(f"{m}:{method_counts[m]}" for m in (0, 1, 2, 3, 4, 5, 6))
    fold_tot = sum(fmt_counts["fold"].values())
    bmp_c, png_c, ppm_c = fmt_counts["bmp"], fmt_counts["png"], fmt_counts["ppm"]
    print(
        f"{iterations} mutated files across all methods and input formats "
        f"(fold={fold_tot} [methods {m_summary}; {fmt_counts['fold'][4]} rejected, {fmt_counts['fold'][0]} valid]; "
        f"BMP={sum(bmp_c.values())} [{bmp_c[0]} ok, {bmp_c[3]} unsup, {bmp_c[4]} bad, {bmp_c[5]} limit]; "
        f"PNG={sum(png_c.values())} [{png_c[0]} ok, {png_c[3]} unsup, {png_c[4]} bad, {png_c[5]} limit]; "
        f"PPM={sum(ppm_c.values())} [{ppm_c[0]} ok, {ppm_c[3]} unsup, {ppm_c[4]} bad, {ppm_c[5]} limit]): "
        f"no crash, hang or mismatch"
    )
    return 0


def main() -> int:
    flags = {a for a in sys.argv[1:] if a in ("--png", "--bmp", "--ppm", "--all-formats")}
    if len(flags) > 1:
        print("choose at most one mode flag (--png, --bmp, --ppm, --all-formats)")
        return 2
    argv = [a for a in sys.argv[1:] if a not in flags]
    iterations = int(argv[0]) if len(argv) > 0 else 300
    rng = random.Random(int(argv[1]) if len(argv) > 1 else 1)
    if "--png" in flags:
        return run_png_fuzz(iterations, rng)
    if "--bmp" in flags:
        return run_bmp_fuzz(iterations, rng)
    if "--ppm" in flags:
        return run_ppm_fuzz(iterations, rng)
    if "--all-formats" in flags:
        return run_all_formats_fuzz(iterations, rng)
    base = seeds()
    counts = {0: 0, 4: 0}
    method_counts = {0: 0, 1: 0, 2: 0, 3: 0, 4: 0, 5: 0, 6: 0}
    with _staged_fuzz_env() as (staged_exe, work_dir):
        exe_s, work_s = str(staged_exe), str(work_dir)

        def fold_tasks():
            seed_thr_idx = [0] * len(base)
            for i in range(iterations):
                seed_idx = i % len(base)
                seed_blob = base[seed_idx]
                method_counts[seed_blob[16]] += 1
                blob = mutate(seed_blob, rng)
                threads = _ALL_FOLD_THREADS[seed_thr_idx[seed_idx] % len(_ALL_FOLD_THREADS)]
                seed_thr_idx[seed_idx] += 1
                yield (i, blob, threads, exe_s, work_s)

        if iterations <= 20:
            for i, blob, threads, _, _ in fold_tasks():
                rc, err = verify_mutation(i, blob, threads, staged_exe, work_dir)
                if err is not None:
                    print(err)
                    return 1
                counts[rc] += 1
        else:
            workers = min(os.cpu_count() or 4, 12)
            counts, err = run_tasks_parallel(fold_tasks(), workers)
            if err is not None:
                print(err)
                return 1

    m_summary = ", ".join(f"{m}:{method_counts[m]}" for m in (0, 1, 2, 3, 4, 5, 6))
    print(f"{iterations} mutated files (methods {m_summary}): {counts[4]} rejected, {counts[0]} still valid, no crash, hang or mismatch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
