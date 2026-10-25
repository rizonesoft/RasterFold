"""Independent pixel round trips and rejection tests for RasterFold."""

from __future__ import annotations

import random
import struct
import os
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import rfi_ref as rf  # noqa: E402

EXE = Path(os.environ.get("RF_TEST_EXE", ROOT / "bin" / ("windows-x64/bin/rasterfold.exe" if os.name == "nt" else "linux-x64/bin/rasterfold")))
WORK = ROOT / "tests" / "work"
passed = 0
failed = 0

# A 16x8 image and its complete .fold file (method 1). Written by the oracle and
# the codec alike; a change here is a change of the format.
KAT_W, KAT_H = 16, 8
KAT_RGB = bytes((x * 9 + y * 5 + c * 40 + ((x * 7 + y * 3) % 3)) & 255 for y in range(8) for x in range(16) for c in range(3))
KAT_FOLD = bytes.fromhex(
    "52464902020020001000000008000000010001003a000000b8923a9c7b2e13551e00000009000000070000000041ee93002ddf10975bffba"
    "9a240a210014024daf691d221f14715e2e6c0001c05edda7566f6e0041f37cefce5b"
)


class Fail(Exception):
    pass


def check(cond: bool, message: str) -> None:
    if not cond:
        raise Fail(message)


def run(args: list[str]) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run([str(EXE), *args], capture_output=True)


def expect_ok(args: list[str]) -> subprocess.CompletedProcess[bytes]:
    result = run(args)
    check(result.returncode == 0, f"{args} -> {result.returncode} {result.stderr.decode(errors='replace')}")
    return result


def expect_fail(args: list[str], code: int, text: str) -> None:
    result = run(args)
    err = result.stderr.decode(errors="replace")
    check(result.returncode == code, f"{args} exit {result.returncode}, want {code}: {err}")
    check(text in err, f"{args} stderr missing {text!r}: {err}")
    check(result.stdout == b"", f"{args} wrote stdout on failure")


# `-k TEXT` (repeatable) runs only the cases whose name contains one of the
# texts, for iterating on one area; a commit still runs the whole suite.
SELECT = [sys.argv[i + 1] for i, a in enumerate(sys.argv[:-1]) if a == "-k"]
skipped = 0


def case(name: str, fn) -> None:
    global passed, failed, skipped
    if SELECT and not any(s in name for s in SELECT):
        skipped += 1
        return
    try:
        fn()
    except Exception as exc:
        failed += 1
        print(f"FAIL {name}: {exc}")
    else:
        passed += 1
        print(f"OK   {name}")


def oracle_self_check() -> None:
    check(rf.crc32(b"123456789") == 0xCBF43926, "crc vector")
    check(rf.SQUASH[2047] == 2047 and rf.SQUASH[0] == 1 and rf.SQUASH[4094] == 4094, "squash endpoints")
    check(all(rf.SQUASH[rf.STRETCH[p] + 2047] >= p or rf.STRETCH[p] == 2047 for p in range(1, 4096)), "stretch inverts squash")
    check(rf.encode_image(KAT_W, KAT_H, KAT_RGB) == KAT_FOLD, "known-answer file")
    check(rf.decode_image(KAT_FOLD) == (KAT_W, KAT_H, KAT_RGB), "known-answer decode")


def known_answer() -> None:
    work = WORK / "kat"
    work.mkdir(parents=True, exist_ok=True)
    src, out, back = work / "in.bmp", work / "out.fold", work / "out.bmp"
    for f in (out, back):
        f.unlink(missing_ok=True)
    src.write_bytes(rf.write_bmp24(KAT_W, KAT_H, KAT_RGB))
    expect_ok(["encode", str(src), str(out)])
    check(out.read_bytes() == KAT_FOLD, "codec bytes differ from the known-answer file")
    expect_ok(["decode", str(out), str(back)])
    check(rf.read_bmp24(back.read_bytes()) == (KAT_W, KAT_H, KAT_RGB), "known-answer pixels")


def solid(w: int, h: int, color: tuple[int, int, int]) -> bytes:
    return bytes(color) * (w * h)


def gradient(w: int, h: int) -> bytes:
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes(((x * 3) & 255, (y * 5) & 255, (x + y * 2) & 255))
    return bytes(out)


def checker(w: int, h: int, cell: int) -> bytes:
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += b"\x20\x30\x40" if ((x // cell) ^ (y // cell)) & 1 else b"\xE0\xE8\xF0"
    return bytes(out)


def noise(w: int, h: int, seed: int) -> bytes:
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(w * h * 3))


def stripes(w: int, h: int) -> bytes:
    out = bytearray()
    for y in range(h):
        color = bytes(((y * 17) & 255, 40, (200 - y * 3) & 255))
        out += color * w
    return bytes(out)


def extremes(w: int, h: int) -> bytes:
    """Saturated colours and wraparound differences, the corners of every range."""
    vals = (0, 255, 1, 254, 128, 127)
    rng = random.Random(w * 31 + h)
    return bytes(vals[rng.randrange(len(vals))] for _ in range(w * h * 3))


def photo(w: int, h: int) -> bytes:
    with Image.open(ROOT / "testdata" / "photos" / "kodim23.png") as im:
        return im.convert("RGB").crop((260, 180, 260 + w, 180 + h)).tobytes()


def draw_ui(w: int, h: int, seed: int) -> bytes:
    image = Image.new("RGB", (w, h), (245, 245, 245))
    pen = ImageDraw.Draw(image)
    pen.rectangle((0, 0, w - 1, 28), fill=(32, 64, 128))
    pen.rectangle((12, 48, w // 2, h - 20), fill=(255, 255, 255), outline=(180, 180, 180))
    pen.rectangle((w // 2 + 16, 48, w - 12, 140), fill=(220, 236, 220), outline=(40, 120, 40))
    for i in range(6):
        y = 160 + i * 28
        pen.rectangle((w // 2 + 16, y, w - 12, y + 20), fill=(255, 255, 255), outline=(160, 160, 160))
    pen.text((16, 6), f"RasterFold {seed}", fill=(255, 255, 255))
    pen.text((24, 60), "AaBb 0123", fill=(20, 20, 20))
    return image.tobytes()


def roundtrip(name: str, width: int, height: int, rgb: bytes, **bmp_kw) -> None:
    work = WORK / name
    work.mkdir(parents=True, exist_ok=True)
    src_bmp = work / "in.bmp"
    out_fold = work / "out.fold"
    one_fold = work / "one.fold"
    out_bmp = work / "out.bmp"
    one_bmp = work / "one.bmp"
    three_bmp = work / "three.bmp"
    oracle_fold = work / "oracle.fold"
    oracle_bmp = work / "oracle.bmp"
    for stale in (out_fold, one_fold, out_bmp, one_bmp, three_bmp, oracle_fold, oracle_bmp, Path(str(out_fold) + ".partial")):
        stale.unlink(missing_ok=True)
    src_bmp.write_bytes(rf.write_bmp24(width, height, rgb, **bmp_kw))
    check(rf.read_bmp24(src_bmp.read_bytes()) == (width, height, rgb), "independent BMP reader mismatch")
    expect_ok(["encode", str(src_bmp), str(out_fold)])
    expect_ok(["encode", "--threads", "1", str(src_bmp), str(one_fold)])
    encoded = out_fold.read_bytes()
    check(one_fold.read_bytes() == encoded, "one-thread and default encodes differ")
    oracle = rf.encode_image(width, height, rgb)
    check(encoded == oracle, "encoder bytes differ from the oracle")
    check(rf.decode_image(encoded) == (width, height, rgb), "oracle decode of codec file")
    expect_ok(["decode", str(out_fold), str(out_bmp)])
    expect_ok(["decode", "--threads", "1", str(out_fold), str(one_bmp)])
    expect_ok(["decode", "--threads", "3", str(out_fold), str(three_bmp)])
    canonical_bmp = rf.write_bmp24(width, height, rgb)
    check(rf.read_bmp24(out_bmp.read_bytes()) == (width, height, rgb), "decoded BMP pixels")
    check(out_bmp.read_bytes() == canonical_bmp, "decoded BMP bytes differ from canonical BMP")
    check(one_bmp.read_bytes() == out_bmp.read_bytes(), "one-thread and default decodes differ")
    check(three_bmp.read_bytes() == canonical_bmp, "three-thread decoded BMP bytes differ from canonical BMP")
    with Image.open(out_bmp) as pillow:
        check(pillow.size == (width, height), "pillow size")
        check(pillow.convert("RGB").tobytes() == rgb, "pillow pixels")
    oracle_fold.write_bytes(oracle)
    expect_ok(["decode", str(oracle_fold), str(oracle_bmp)])
    check(rf.read_bmp24(oracle_bmp.read_bytes()) == (width, height, rgb), "codec decode of oracle file")
    check(oracle_bmp.read_bytes() == canonical_bmp, "codec decode of oracle file BMP bytes differ from canonical BMP")


def make_bad_bmp(kind: str) -> bytes:
    good = rf.write_bmp24(2, 2, solid(2, 2, (1, 2, 3)))
    blob = bytearray(good)
    if kind == "sig":
        blob[0:2] = b"BA"
    elif kind == "bits8":
        struct.pack_into("<H", blob, 28, 8)
    elif kind == "bits32":
        struct.pack_into("<H", blob, 28, 32)
    elif kind == "rle":
        struct.pack_into("<I", blob, 30, 1)
    elif kind == "v4":
        struct.pack_into("<I", blob, 14, 108)
    elif kind == "core":
        struct.pack_into("<I", blob, 14, 12)
    elif kind == "zero":
        struct.pack_into("<i", blob, 18, 0)
    elif kind == "negw":
        struct.pack_into("<i", blob, 18, -3)
    elif kind == "intmin":
        struct.pack_into("<i", blob, 22, -2147483648)
    elif kind == "trunc":
        return bytes(blob[:60])
    elif kind == "limit":
        struct.pack_into("<i", blob, 18, 40000)
        struct.pack_into("<i", blob, 22, 1)
    else:
        raise AssertionError(kind)
    return bytes(blob)


def recrc(blob: bytes | bytearray) -> bytes:
    """Recompute both checksums so a mutation reaches the checks behind them."""
    b = bytearray(blob)
    struct.pack_into("<I", b, 20, len(b) - 32)
    struct.pack_into("<I", b, 24, rf.crc32(bytes(b[32:])))
    struct.pack_into("<I", b, 28, rf.crc32(bytes(b[:28])))
    return bytes(b)


def with_streams(sizes: tuple[int, int, int], data: bytes) -> bytes:
    return struct.pack("<III", *sizes) + data


def corrupt_cases() -> None:
    work = WORK / "corrupt"
    work.mkdir(parents=True, exist_ok=True)
    good = KAT_FOLD
    n = 0

    def fold(blob: bytes) -> Path:
        nonlocal n
        n += 1
        path = work / f"bad{n}.fold"
        path.write_bytes(blob)
        return path

    def reject(blob: bytes, code: int, text: str) -> None:
        dest = work / f"bad{n + 1}.bmp"
        dest.unlink(missing_ok=True)
        expect_fail(["decode", str(fold(blob)), str(dest)], code, text)
        check(not dest.exists(), f"{text}: an output was left behind")
        try:
            rf.decode_image(blob)
        except rf.FormatError:
            pass
        else:
            check(False, f"{text}: oracle accepted corrupt .fold")

    reject(good[:10], 4, "invalid fold: truncated")
    reject(b"XYZ" + good[3:], 4, "invalid fold: magic")
    v1 = bytearray(good)
    v1[3] = 1
    reject(bytes(v1), 3, "unsupported fold: version")
    ver = bytearray(good)
    struct.pack_into("<H", ver, 4, 3)
    reject(bytes(ver), 3, "unsupported fold: version")
    hs = bytearray(good)
    struct.pack_into("<H", hs, 6, 40)
    reject(bytes(hs), 3, "unsupported fold: header size")
    hcrc = bytearray(good)
    hcrc[8] ^= 1
    reject(bytes(hcrc), 4, "invalid fold: header checksum")
    pcrc = bytearray(good)
    pcrc[-1] ^= 1
    reject(bytes(pcrc), 4, "invalid fold: payload checksum")
    method = bytearray(good)
    method[16] = 2
    reject(recrc(method), 3, "unsupported fold: method")
    res = bytearray(good)
    res[17] = 1
    reject(recrc(res), 4, "invalid fold: reserved byte")
    dims = bytearray(good)
    struct.pack_into("<I", dims, 8, 0)
    reject(recrc(dims), 4, "invalid fold: dimensions")
    big = bytearray(good)
    struct.pack_into("<II", big, 8, 32768, 32768)
    reject(recrc(big), 5, "image exceeds size limit")
    extra = bytearray(good) + b"\x00"
    struct.pack_into("<I", extra, 28, rf.crc32(bytes(extra[:28])))
    reject(bytes(extra), 4, "invalid fold: payload length")
    raw = rf.build_file(2, 1, rf.METHOD_RAW, bytes(5))
    reject(raw, 4, "invalid fold: raw length")

    # CM stream framing and contents, with checksums recomputed.
    header = good[:32]
    sizes = struct.unpack_from("<III", good, 32)
    streams = good[44:]
    reject(recrc(header + with_streams((sizes[0] + 1, sizes[1], sizes[2]), streams)), 4, "invalid fold: cm streams")
    reject(recrc(header + with_streams((4, sizes[1], sizes[2] + sizes[0] - 4), streams)), 4, "invalid fold: cm streams")
    reject(recrc(header + good[32:40]), 4, "invalid fold: cm streams")
    cut = header + with_streams((sizes[0] - 1, sizes[1], sizes[2]), streams[: sizes[0] - 1] + streams[sizes[0]:])
    reject(recrc(cut), 4, "invalid fold: cm stream")
    longer = header + with_streams((sizes[0] + 1, sizes[1], sizes[2]), streams[: sizes[0]] + b"\x55" + streams[sizes[0]:])
    reject(recrc(longer), 4, "invalid fold: cm stream")
    flipped = bytearray(good)
    flipped[44 + sizes[0] // 2] ^= 0x40
    reject(recrc(flipped), 4, "invalid fold: cm stream")

    # A hand-built raw file decodes.
    one = bytes([4, 5, 6])
    raw_out = work / "raw.bmp"
    raw_out.unlink(missing_ok=True)
    expect_ok(["decode", str(fold(rf.build_file(1, 1, rf.METHOD_RAW, one))), str(raw_out)])
    check(rf.read_bmp24(raw_out.read_bytes())[2] == one, "forced raw")

    bmp_cases = [
        ("sig.bmp", "sig", 4, "invalid BMP: signature"),
        ("bits8.bmp", "bits8", 3, "unsupported BMP: bit count"),
        ("bits32.bmp", "bits32", 3, "unsupported BMP: bit count"),
        ("rle.bmp", "rle", 3, "unsupported BMP: compression"),
        ("v4.bmp", "v4", 3, "unsupported BMP: header"),
        ("core.bmp", "core", 3, "unsupported BMP: header"),
        ("zero.bmp", "zero", 4, "invalid BMP: dimensions"),
        ("negw.bmp", "negw", 4, "invalid BMP: dimensions"),
        ("intmin.bmp", "intmin", 4, "invalid BMP: dimensions"),
        ("trunc.bmp", "trunc", 4, "invalid BMP: truncated"),
        ("limit.bmp", "limit", 5, "image exceeds size limit"),
    ]
    for filename, kind, code, text in bmp_cases:
        bad_bmp = make_bad_bmp(kind)
        path = work / filename
        path.write_bytes(bad_bmp)
        dest = work / f"{filename}.fold"
        dest.unlink(missing_ok=True)
        expect_fail(["encode", str(path), str(dest)], code, text)
        check(not dest.exists(), f"{filename} left an output")
        try:
            rf.read_bmp24(bad_bmp)
        except rf.FormatError:
            pass
        else:
            check(False, f"{filename}: oracle accepted corrupt BMP ({text})")

    missing = work / "missing.bmp"
    expect_fail(["encode", str(missing), str(work / "missing.fold")], 2, "cannot read input")
    empty = work / "empty.bmp"
    empty.write_bytes(b"")
    expect_fail(["encode", str(empty), str(work / "empty.fold")], 4, "invalid input: empty file")

    src = work / "keep.bmp"
    src.write_bytes(rf.write_bmp24(1, 1, bytes([1, 2, 3])))
    occupied = work / "occupied.fold"
    occupied.write_bytes(b"keep-me")
    expect_fail(["encode", str(src), str(occupied)], 2, "output file already exists")
    check(occupied.read_bytes() == b"keep-me", "existing output was modified")
    partial = work / "fresh.fold.partial"
    partial.write_bytes(b"partial")
    dest = work / "fresh.fold"
    dest.unlink(missing_ok=True)
    expect_fail(["encode", str(src), str(dest)], 2, "temporary output already exists")
    check(not dest.exists(), "destination created despite partial")
    check(partial.read_bytes() == b"partial", "partial file was modified")
    same = work / "same.bmp"
    same.write_bytes(rf.write_bmp24(1, 1, bytes([8, 8, 8])))
    before = same.read_bytes()
    expect_fail(["encode", str(same), str(same)], 2, "output file already exists")
    check(same.read_bytes() == before, "input overwritten")
    expect_fail(["encode", "--threads", "0", str(src), str(work / "t0.fold")], 1, "usage")


def padding_roundtrip() -> None:
    work = WORK / "padding"
    work.mkdir(parents=True, exist_ok=True)
    w, h = 3, 2
    rgb = gradient(w, h)
    raw_bmp = bytearray(rf.write_bmp24(w, h, rgb))
    stride = rf.row_stride(w)
    for y in range(h):
        row_start = 54 + y * stride
        raw_bmp[row_start + w * 3 : row_start + stride] = b"\xA5\x5A\xFF"
    src_bmp = work / "in.bmp"
    out_fold = work / "out.fold"
    out_bmp = work / "out.bmp"
    for f in (out_fold, out_bmp):
        f.unlink(missing_ok=True)
    src_bmp.write_bytes(bytes(raw_bmp))
    check(rf.read_bmp24(src_bmp.read_bytes()) == (w, h, rgb), "oracle rejected or misread nonzero BMP padding")
    expect_ok(["encode", str(src_bmp), str(out_fold)])
    oracle_fold = rf.encode_image(w, h, rgb)
    check(out_fold.read_bytes() == oracle_fold, "codec .fold bytes on nonzero-padded BMP differ from oracle")
    check(rf.decode_image(out_fold.read_bytes()) == (w, h, rgb), "oracle decode of nonzero-padded BMP .fold mismatch")
    expect_ok(["decode", str(out_fold), str(out_bmp)])
    check(rf.read_bmp24(out_bmp.read_bytes()) == (w, h, rgb), "codec decoded pixels mismatch on nonzero-padded BMP")


def oracle_acceptance() -> None:
    with tempfile.TemporaryDirectory(prefix="rf_accept_") as tmp_dir:
        work = Path(tmp_dir)
        _oracle_acceptance_in(work)


def _oracle_acceptance_in(work: Path) -> None:

    def make_bmp_full(w: int, h_signed: int, bf_size: int | None = None, trailing: bytes = b"") -> bytes:
        h_abs = abs(h_signed)
        stride = rf.row_stride(w)
        total = 54 + stride * h_abs
        buf = bytearray(total + len(trailing))
        actual_bf = total if bf_size is None else bf_size
        struct.pack_into("<2sIHHI", buf, 0, b"BM", actual_bf, 0, 0, 54)
        struct.pack_into("<IiiHHIIiiII", buf, 14, 40, w, h_signed, 1, 24, 0, stride * h_abs, 3780, 3780, 0, 0)
        if trailing:
            buf[total:] = trailing
        return bytes(buf)

    def write_test_file(path: Path, blob: bytes, prefix_len: int) -> None:
        if len(blob) > 65536 and blob[prefix_len:] == bytes(len(blob) - prefix_len):
            with open(path, "wb") as f:
                f.write(blob[:prefix_len])
                f.truncate(len(blob))
        else:
            path.write_bytes(blob)

    def check_bmp_refusal(name: str, blob: bytes, code: int, cli_text: str, oracle_text: str) -> None:
        in_bmp = work / f"{name}.bmp"
        out_fold = work / f"{name}.fold"
        out_fold.unlink(missing_ok=True)
        write_test_file(in_bmp, blob, 54)
        try:
            expect_fail(["encode", str(in_bmp), str(out_fold)], code, cli_text)
            check(not out_fold.exists(), f"{name}: codec left output")
        finally:
            in_bmp.unlink(missing_ok=True)
        try:
            rf.read_bmp24(blob)
        except rf.FormatError as exc:
            check(oracle_text in str(exc), f"{name}: oracle raised {exc!r}, expected {oracle_text!r}")
        else:
            check(False, f"{name}: oracle accepted BMP that codec refused ({cli_text})")

    # 1. Full-payload BMPs exceeding biWidth, |biHeight|, or MAX_PIXELS (not truncated).
    check_bmp_refusal("bmp_w_32769", make_bmp_full(32769, 1), 5, "image exceeds size limit", "bmp size limit")
    check_bmp_refusal("bmp_h_32769", make_bmp_full(1, 32769), 5, "image exceeds size limit", "bmp size limit")
    check_bmp_refusal("bmp_h_neg32769", make_bmp_full(1, -32769), 5, "image exceeds size limit", "bmp size limit")
    check_bmp_refusal("bmp_pixels_over", make_bmp_full(32768, 1025), 5, "image exceeds size limit", "bmp size limit")

    # 2. Invalid bfSize bounds (0 < bfSize < pixel_end, and bfSize > file_size).
    need_2x2 = 54 + rf.row_stride(2) * 2
    check_bmp_refusal("bmp_bfsize_low", make_bmp_full(2, 2, bf_size=need_2x2 - 1), 4, "invalid BMP: file size", "bmp file size")
    check_bmp_refusal("bmp_bfsize_high", make_bmp_full(2, 2, bf_size=need_2x2 + 1), 4, "invalid BMP: file size", "bmp file size")

    # 3. Valid bfSize boundary cases: bfSize == 0 and pixel_end == bfSize < file_size.
    for tag, bf_val, tail in (("bfsize_zero", 0, b""), ("bfsize_pixel_end_with_tail", need_2x2, b"\x00" * 16)):
        valid_blob = make_bmp_full(2, 2, bf_size=bf_val, trailing=tail)
        in_bmp = work / f"{tag}.bmp"
        out_fold = work / f"{tag}.fold"
        out_fold.unlink(missing_ok=True)
        in_bmp.write_bytes(valid_blob)
        w_got, h_got, rgb_got = rf.read_bmp24(valid_blob)
        check((w_got, h_got, rgb_got) == (2, 2, bytes(12)), f"{tag}: oracle read mismatch")
        expect_ok(["encode", str(in_bmp), str(out_fold)])
        check(out_fold.read_bytes() == rf.encode_image(2, 2, bytes(12)), f"{tag}: codec and oracle .fold differ")

    # 4. .fold file longer than 256 MiB (268,435,457 bytes).
    over_fold = bytearray(rf.MAX_PIXELS * 8 + 1)
    over_fold[: len(KAT_FOLD)] = KAT_FOLD
    over_fold_bytes = bytes(over_fold)
    del over_fold
    in_fold = work / "over_256mib.fold"
    out_bmp = work / "over_256mib.bmp"
    out_bmp.unlink(missing_ok=True)
    write_test_file(in_fold, over_fold_bytes, len(KAT_FOLD))
    try:
        expect_fail(["decode", str(in_fold), str(out_bmp)], 5, "input exceeds size limit")
        check(not out_bmp.exists(), "over_256mib: codec left output")
    finally:
        in_fold.unlink(missing_ok=True)
    try:
        rf.decode_image(over_fold_bytes)
    except rf.FormatError as exc:
        check("file size" in str(exc), f"over_256mib: oracle raised {exc!r}, expected 'file size'")
    else:
        check(False, "over_256mib: oracle accepted >256 MiB .fold")


def truncated_stream_abort() -> None:
    work = WORK / "trunc_abort"
    work.mkdir(parents=True, exist_ok=True)

    def check_reject_fast(name: str, blob: bytes, threads: str, want_text: str) -> None:
        in_fold = work / f"{name}_t{threads}.fold"
        out_bmp = work / f"{name}_t{threads}.bmp"
        partial = Path(str(out_bmp) + ".partial")
        out_bmp.unlink(missing_ok=True)
        partial.unlink(missing_ok=True)
        in_fold.write_bytes(blob)
        t0 = time.perf_counter()
        result = subprocess.run(
            [str(EXE), "decode", "--threads", threads, str(in_fold), str(out_bmp)],
            capture_output=True,
            timeout=5.0,
        )
        dt = time.perf_counter() - t0
        err = result.stderr.decode(errors="replace")
        check(result.returncode == 4, f"{name} t={threads} exit {result.returncode}, want 4: {err}")
        check(want_text in err, f"{name} t={threads} stderr missing {want_text!r}: {err}")
        check(result.stdout == b"", f"{name} t={threads} wrote stdout on failure")
        check(dt < 1.0, f"{name} t={threads} took {dt:.3f}s (>= 1.0s)")
        check(not out_bmp.exists(), f"{name} t={threads} left output BMP")
        check(not partial.exists(), f"{name} t={threads} left .partial file")

    # 1. 4096x4096 CM header with a 20-byte payload (framing rejection) and with
    #    a structurally valid 27-byte CM payload (three 5-byte 0x80 streams that
    #    enter run_pipelined / run_channel and exhaust dec.in >= dec.lim on row 0).
    fold_20 = rf.build_file(4096, 4096, rf.METHOD_CM, with_streams((5, 5, 5), b"\x80" * 8))
    fold_27 = rf.build_file(4096, 4096, rf.METHOD_CM, with_streams((5, 5, 5), b"\x80" * 15))
    for t in ("1", "3"):
        check_reject_fast("cm4096_20b", fold_20, t, "invalid fold: cm streams")
        check_reject_fast("cm4096_27b", fold_27, t, "invalid fold: cm stream")

    # 2. Truncate each of the three streams (G, R, B) in turn on an image taller
    #    than XROWS (64x64, h=64 > XROWS=32) both at 5 bytes (row-0 exhaustion)
    #    and at 75% of the valid stream length (exhausting after row 32 so
    #    y >= XROWS backpressure waits execute before abort).
    valid_64 = rf.encode_image(64, 64, photo(64, 64))
    sz0, sz1, sz2 = struct.unpack_from("<III", valid_64, 32)
    s0 = valid_64[44 : 44 + sz0]
    s1 = valid_64[44 + sz0 : 44 + sz0 + sz1]
    s2 = valid_64[44 + sz0 + sz1 : 44 + sz0 + sz1 + sz2]
    orig_streams = [s0, s1, s2]
    short_stream = b"\x80" * 5
    for c in range(3):
        parts = list(orig_streams)
        parts[c] = short_stream
        trunc_payload = with_streams((len(parts[0]), len(parts[1]), len(parts[2])), b"".join(parts))
        trunc_fold = rf.build_file(64, 64, rf.METHOD_CM, trunc_payload)
        mid_parts = list(orig_streams)
        mid_len = max(5, (len(orig_streams[c]) * 3) // 4)
        mid_parts[c] = orig_streams[c][:mid_len]
        mid_payload = with_streams((len(mid_parts[0]), len(mid_parts[1]), len(mid_parts[2])), b"".join(mid_parts))
        mid_fold = rf.build_file(64, 64, rf.METHOD_CM, mid_payload)
        for t in ("1", "3"):
            check_reject_fast(f"trunc_ch{c}", trunc_fold, t, "invalid fold: cm stream")
            check_reject_fast(f"trunc_mid_ch{c}", mid_fold, t, "invalid fold: cm stream")


def main() -> int:
    if not EXE.exists():
        print(f"missing {EXE}; run build.ps1 first")
        return 1
    WORK.mkdir(parents=True, exist_ok=True)
    case("oracle", oracle_self_check)
    case("known-answer", known_answer)
    images = [
        ("px1", 1, 1, bytes([0, 0, 0])),
        ("white", 1, 1, bytes([255, 255, 255])),
        ("rgb", 1, 1, bytes([255, 0, 0])),
        ("odd1", 1, 3, solid(1, 3, (12, 34, 56))),
        ("odd2", 2, 2, bytes([10, 20, 30, 10, 20, 30, 10, 20, 30, 11, 20, 30])),
        ("odd3", 3, 1, bytes([1, 2, 3, 4, 5, 6, 7, 8, 9])),
        ("odd7", 7, 5, gradient(7, 5)),
        ("row64", 64, 1, stripes(64, 1)),
        ("col64", 1, 64, stripes(1, 64)),
        ("b64", 64, 64, solid(64, 64, (30, 144, 255))),
        ("b65", 65, 63, checker(65, 63, 8)),
        ("b127", 127, 3, gradient(127, 3)),
        ("solid", 48, 32, solid(48, 32, (240, 240, 240))),
        ("checker", 80, 60, checker(80, 60, 4)),
        ("gradient", 96, 64, gradient(96, 64)),
        ("stripes", 70, 40, stripes(70, 40)),
        ("noise", 40, 25, noise(40, 25, 1234)),
        ("noise2", 17, 19, noise(17, 19, 99)),
        ("extremes", 33, 21, extremes(33, 21)),
        ("photo", 120, 80, photo(120, 80)),
        ("ui", 320, 200, draw_ui(320, 200, 1)),
        ("text", 280, 120, draw_ui(280, 120, 2)),
    ]
    for name, w, h, rgb in images:
        case(name, lambda n=name, w=w, h=h, rgb=rgb: roundtrip(n, w, h, rgb))
    case("topdown", lambda: roundtrip("topdown", 9, 4, gradient(9, 4), topdown=True))
    case("gap", lambda: roundtrip("gap", 5, 4, checker(5, 4, 2), gap=16))
    case("sizeimage0", lambda: roundtrip("sizeimage0", 6, 2, solid(6, 2, (1, 1, 1)), sizeimage_zero=True))
    case("reserved", lambda: roundtrip("reserved", 4, 4, noise(4, 4, 7), reserved=0x12345678))
    case("bfsize0", lambda: roundtrip("bfsize0", 3, 3, solid(3, 3, (2, 3, 4)), bf_size_zero=True))
    case(
        "orientation-identical",
        lambda: check(
            rf.encode_image(9, 4, gradient(9, 4)) == open(WORK / "topdown" / "out.fold", "rb").read(),
            "top-down input encoded differently",
        ),
    )
    case("grey", lambda: roundtrip("grey", 16, 8, solid(16, 8, (128, 128, 128))))
    case("padding", padding_roundtrip)
    case("acceptance", oracle_acceptance)
    case("corrupt", corrupt_cases)
    case("truncated-abort", truncated_stream_abort)
    # The checksum in a successful file matches zlib, not only our encoder.
    case(
        "zlib-crc",
        lambda: check(
            zlib.crc32(open(WORK / "photo" / "out.fold", "rb").read()[32:]) & 0xFFFFFFFF
            == struct.unpack_from("<I", open(WORK / "photo" / "out.fold", "rb").read(), 24)[0],
            "payload crc",
        ),
    )
    print(f"\n{passed} passed, {failed} failed" + (f", {skipped} skipped by -k" if skipped else ""))
    return 0 if failed == 0 and passed > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
