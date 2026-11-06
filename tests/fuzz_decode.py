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
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import time

from PIL import Image

_ORIG_SUBPROCESS_RUN = subprocess.run

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import rfi_ref as rf  # noqa: E402

EXE = Path(os.environ.get("RF_TEST_EXE", ROOT / "bin" / ("windows-x64/bin/rasterfold.exe" if os.name == "nt" else "linux-x64/bin/rasterfold")))
WORK = ROOT / "tests" / "work" / "fuzz"

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
    seed_src = (ROOT / "tests" / "rfi_ref.py").read_bytes() + repr((PHOTO_CROPS, FORCE_STRIPS)).encode("utf-8")
    cache_key = hashlib.sha256(seed_src).hexdigest()[:16]
    cache_path = WORK / f"seeds_v3_{cache_key}.json"
    if cache_path.is_file():
        try:
            raw = json.loads(cache_path.read_text(encoding="utf-8"))
            records = [(r["name"], int(r["w"]), int(r["h"]), int(r["method"]), base64.b64decode(r["b64"])) for r in raw]
            if len(records) == 15:
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
    pal3_2col = rf.encode_image(pal_w, pal_h, chk_rgb)
    assert pal3_2col[16] == rf.METHOD_PAL2
    assert struct.unpack_from("<H", pal3_2col, 32)[0] + 1 == 2
    pal2_2col_payload = rf.pal_encode(pal_w, pal_h, chk_rgb, method=rf.METHOD_PAL)
    assert pal2_2col_payload is not None
    pal2_2col = rf.build_file(pal_w, pal_h, rf.METHOD_PAL, pal2_2col_payload)
    assert struct.unpack_from("<H", pal2_2col, 32)[0] + 1 == 2

    pal200 = [bytes(((i * 53 + 17) & 0xFF, (i * 97 + 43) & 0xFF, (i * 151 + 89) & 0xFF)) for i in range(200)]
    assert len(set(pal200)) == 200
    pal200_rgb = b"".join(
        pal200[(y * pal_w + x) if (y * pal_w + x) < 200 else ((x * 7 + y * 13 + ((x ^ y) & 3)) % 200)]
        for y in range(pal_h)
        for x in range(pal_w)
    )
    pal3_200col = rf.encode_image(pal_w, pal_h, pal200_rgb)
    assert pal3_200col[16] == rf.METHOD_PAL2
    assert struct.unpack_from("<H", pal3_200col, 32)[0] + 1 == 200
    pal2_200col_payload = rf.pal_encode(pal_w, pal_h, pal200_rgb, method=rf.METHOD_PAL)
    assert pal2_200col_payload is not None
    pal2_200col = rf.build_file(pal_w, pal_h, rf.METHOD_PAL, pal2_200col_payload)
    assert struct.unpack_from("<H", pal2_200col, 32)[0] + 1 == 200

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

    # Order the first 6 slots so small runs (e.g. 10 iterations) exercise methods 0, 1, 2, 3.
    records = [
        ("noise_64x48", noise_w, noise_h, rf.METHOD_RAW, noise_fold),
        photo_folds[0],
        ("pal2_2col_64x48", pal_w, pal_h, rf.METHOD_PAL, pal2_2col),
        ("pal3_200col_64x48", pal_w, pal_h, rf.METHOD_PAL2, pal3_200col),
        ("pal2_200col_64x48", pal_w, pal_h, rf.METHOD_PAL, pal2_200col),
        ("pal3_2col_64x48", pal_w, pal_h, rf.METHOD_PAL2, pal3_2col),
        *photo_folds[1:],
    ]
    assert len(records) == 15
    assert {m for _, _, _, m, _ in records[:4]} == {rf.METHOD_RAW, rf.METHOD_CM, rf.METHOD_PAL, rf.METHOD_PAL2}
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
    tasks: list[tuple[int, bytes, str, str, str]],
    workers: int,
    worker_fn=_worker_verify,
) -> tuple[dict[int, int], str | None]:
    counts = {0: 0, 4: 0}
    stop_file = Path(tasks[0][4]) / ".fuzz_stop" if tasks else None
    if stop_file is not None:
        stop_file.unlink(missing_ok=True)
    it = iter(tasks)
    max_inflight = max(1, workers * 2)
    pool = ProcessPoolExecutor(max_workers=workers)
    pending: set[Future[tuple[int, int, str | None]]] = set()
    try:
        for _ in range(max_inflight):
            try:
                pending.add(pool.submit(worker_fn, next(it)))
            except StopIteration:
                break
        while pending:
            done, pending = wait(pending, return_when=FIRST_COMPLETED)
            for fut in done:
                _, rc, err = fut.result()
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
        if stop_file is not None:
            stop_file.unlink(missing_ok=True)
    pool.shutdown(wait=True)
    return counts, None


def main() -> int:
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    WORK.mkdir(parents=True, exist_ok=True)
    base = seeds()
    counts = {0: 0, 4: 0}
    method_counts = {0: 0, 1: 0, 2: 0, 3: 0}
    tasks: list[tuple[int, bytes, str, str, str]] = []
    for i in range(iterations):
        seed_blob = base[i % len(base)]
        method_counts[seed_blob[16]] += 1
        blob = mutate(seed_blob, rng)
        threads = ("1", "3", "6", "9")[i % 4]
        tasks.append((i, blob, threads, str(EXE), str(WORK)))

    if iterations <= 20:
        for i, blob, threads, _, _ in tasks:
            rc, err = verify_mutation(i, blob, threads, EXE, WORK)
            if err is not None:
                print(err)
                return 1
            counts[rc] += 1
    else:
        workers = min(os.cpu_count() or 4, 12)
        counts, err = run_tasks_parallel(tasks, workers)
        if err is not None:
            print(err)
            return 1

    m_summary = ", ".join(f"{m}:{method_counts[m]}" for m in (0, 1, 2, 3))
    print(f"{iterations} mutated files (methods {m_summary}): {counts[4]} rejected, {counts[0]} still valid, no crash, hang or mismatch")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
