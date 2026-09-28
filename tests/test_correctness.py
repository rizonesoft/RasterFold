"""Independent pixel round trips and rejection tests for RasterFold."""

from __future__ import annotations

import random
import struct
import subprocess
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import rfi_ref as rf  # noqa: E402

EXE = ROOT / "Bin" / "rasterfold.exe"
WORK = ROOT / "tests" / "work"
passed = 0
failed = 0


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


def case(name: str, fn) -> None:
    global passed, failed
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
    check(rf.packbits_encode(bytes([1, 1, 1, 2, 3])) == bytes([0xFE, 1, 1, 2, 3]), "packbits run")
    check(rf.packbits_encode(bytes([1, 2, 3, 3, 3])) == bytes([1, 1, 2, 0xFE, 3]), "packbits literal")
    check(rf.packbits_encode(bytes(128)) == bytes([0x81, 0]), "packbits 128")
    check(rf.packbits_encode(bytes([0xAB])) == bytes([0x00, 0xAB]), "packbits one")
    try:
        rf.packbits_decode(bytes([0x80]), 1)
    except rf.FormatError:
        pass
    else:
        raise Fail("nop was accepted")
    rgb = bytes([10, 20, 30, 10, 20, 30, 10, 20, 30, 11, 20, 30])
    residual = rf.predict(rgb, 2, 2)
    check(
        residual == bytes([0xF6, 0x00, 0xF6, 0x01, 0x14, 0x00, 0x14, 0x00, 0x0A, 0x00, 0x0A, 0x00]),
        f"predict bytes {residual.hex()}",
    )
    check(rf.unpredict(residual, 2, 2) == rgb, "unpredict")
    row = bytes([10, 20, 30, 10, 20, 30])
    check(rf.predict(row, 2, 1) == bytes([0xF6, 0x00, 0x14, 0x00, 0x0A, 0x00]), "spec worked example")


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


def draw_ui(w: int, h: int, seed: int) -> bytes:
    image = Image.new("RGB", (w, h), (245, 245, 245))
    pen = ImageDraw.Draw(image)
    pen.rectangle((0, 0, w - 1, 28), fill=(32, 64, 128))
    pen.rectangle((12, 48, w // 2, h - 20), fill=(255, 255, 255), outline=(180, 180, 180))
    pen.rectangle((w // 2 + 16, 48, w - 12, 140), fill=(220, 236, 220), outline=(40, 120, 40))
    rng = random.Random(seed)
    for i in range(6):
        y = 160 + i * 28
        pen.rectangle((w // 2 + 16, y, w - 12, y + 20), fill=(255, 255, 255), outline=(160, 160, 160))
    pen.text((16, 6), f"RasterFold {seed}", fill=(255, 255, 255))
    pen.text((24, 60), "AaBb 0123", fill=(20, 20, 20))
    _ = rng.randrange(3)
    return image.tobytes()


def roundtrip(name: str, width: int, height: int, rgb: bytes, **bmp_kw) -> None:
    work = WORK / name
    work.mkdir(parents=True, exist_ok=True)
    src_bmp = work / "in.bmp"
    out_rfi = work / "out.fold"
    out_bmp = work / "out.bmp"
    for stale in (out_rfi, out_bmp, work / "oracle.fold", work / "oracle.bmp", Path(str(out_rfi) + ".partial")):
        stale.unlink(missing_ok=True)
    src_bmp.write_bytes(rf.write_bmp24(width, height, rgb, **bmp_kw))
    py_w, py_h, py_rgb = rf.read_bmp24(src_bmp.read_bytes())
    check((py_w, py_h, py_rgb) == (width, height, rgb), "independent BMP reader mismatch")
    expect_ok(["encode", str(src_bmp), str(out_rfi)])
    encoded = out_rfi.read_bytes()
    check(encoded == rf.encode_image(width, height, rgb), "encoder bytes differ from the oracle")
    got_w, got_h, got_rgb = rf.decode_image(encoded)
    check((got_w, got_h, got_rgb) == (width, height, rgb), "oracle decode of assembly file")
    expect_ok(["decode", str(out_rfi), str(out_bmp)])
    dec_w, dec_h, dec_rgb = rf.read_bmp24(out_bmp.read_bytes())
    check((dec_w, dec_h, dec_rgb) == (width, height, rgb), "decoded BMP pixels")
    with Image.open(out_bmp) as pillow:
        check(pillow.size == (width, height), "pillow size")
        check(pillow.convert("RGB").tobytes() == rgb, "pillow pixels")
    # Decode the oracle bytes through the assembly decoder as well.
    oracle_path = work / "oracle.fold"
    oracle_bmp = work / "oracle.bmp"
    oracle_path.write_bytes(rf.encode_image(width, height, rgb))
    expect_ok(["decode", str(oracle_path), str(oracle_bmp)])
    ow, oh, orgb = rf.read_bmp24(oracle_bmp.read_bytes())
    check((ow, oh, orgb) == (width, height, rgb), "assembly decode of oracle file")


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


def corrupt_cases() -> None:
    work = WORK / "corrupt"
    work.mkdir(parents=True, exist_ok=True)
    base_rgb = solid(2, 2, (9, 8, 7))
    good = rf.encode_image(2, 2, base_rgb)
    (work / "good.bmp").write_bytes(rf.write_bmp24(2, 2, base_rgb))

    def rfi(name: str, blob: bytes) -> Path:
        path = work / name
        path.write_bytes(blob)
        return path

    expect_fail(["decode", str(rfi("short.fold", good[:10])), str(work / "a.bmp")], 4, "invalid RFI: truncated")
    bad_magic = bytearray(good)
    bad_magic[0] = 0
    expect_fail(["decode", str(rfi("magic.fold", bad_magic)), str(work / "b.bmp")], 4, "invalid RFI: magic")
    bad_ver = bytearray(good)
    bad_ver[4] = 2
    expect_fail(["decode", str(rfi("ver.fold", bad_ver)), str(work / "c.bmp")], 3, "unsupported RFI: version")
    bad_hcrc = bytearray(good)
    bad_hcrc[8] ^= 0x01
    expect_fail(["decode", str(rfi("hcrc.fold", bad_hcrc)), str(work / "d.bmp")], 4, "invalid RFI: header checksum")
    bad_pcrc = bytearray(good)
    bad_pcrc[-1] ^= 0x01
    expect_fail(["decode", str(rfi("pcrc.fold", bad_pcrc)), str(work / "e.bmp")], 4, "invalid RFI: payload checksum")

    raw = bytes([0, 0]) + struct.pack("<I", 3) + bytes([1, 2, 3])
    mode9 = bytearray(raw)
    mode9[0] = 9
    expect_fail(
        ["decode", str(rfi("mode.fold", rf.build_file(1, 1, mode9))), str(work / "f.bmp")],
        4,
        "invalid RFI: block mode",
    )
    flags = bytearray(raw)
    flags[1] = 1
    expect_fail(
        ["decode", str(rfi("flags.fold", rf.build_file(1, 1, flags))), str(work / "g.bmp")],
        4,
        "invalid RFI: block flags",
    )
    nop = bytes([1, 0]) + struct.pack("<I", 1) + bytes([0x80])
    expect_fail(
        ["decode", str(rfi("nop.fold", rf.build_file(1, 1, nop))), str(work / "h.bmp")],
        4,
        "invalid RFI: packbits",
    )
    # One colour, but the index byte is 5.
    pal = bytes([3, 0]) + struct.pack("<I", 1 + 3 + 2) + bytes([0, 9, 8, 7, 0x00, 5])
    expect_fail(
        ["decode", str(rfi("pidx.fold", rf.build_file(1, 1, pal))), str(work / "i.bmp")],
        4,
        "invalid RFI: palette index",
    )
    trailed = rf.build_file(1, 1, raw + b"\x00")
    expect_fail(["decode", str(rfi("trail.fold", trailed)), str(work / "j.bmp")], 4, "invalid RFI: trailing data")
    tiny = rf.build_file(1, 1, b"\x00\x00")
    expect_fail(["decode", str(rfi("tiny.fold", tiny)), str(work / "k.bmp")], 4, "invalid RFI: truncated block")

    # Forced raw and forced predict must decode to the same pixels.
    one = bytes([4, 5, 6])
    raw_file = rf.build_file(1, 1, bytes([0, 0]) + struct.pack("<I", 3) + one)
    raw_out = work / "raw.bmp"
    raw_out.unlink(missing_ok=True)
    expect_ok(["decode", str(rfi("raw.fold", raw_file)), str(raw_out)])
    check(rf.read_bmp24(raw_out.read_bytes())[2] == one, "forced raw")
    row = bytes([10, 20, 30, 10, 20, 30])
    residual = rf.predict(row, 2, 1)
    pred_body = rf.packbits_encode(residual)
    pred_file = rf.build_file(2, 1, bytes([2, 0]) + struct.pack("<I", len(pred_body)) + pred_body)
    pred_out = work / "pred.bmp"
    pred_out.unlink(missing_ok=True)
    expect_ok(["decode", str(rfi("pred.fold", pred_file)), str(pred_out)])
    check(rf.read_bmp24(pred_out.read_bytes())[2] == row, "forced predict")

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
        path = work / filename
        path.write_bytes(make_bad_bmp(kind))
        dest = work / f"{filename}.fold"
        expect_fail(["encode", str(path), str(dest)], code, text)
        check(not dest.exists(), f"{filename} left an output")

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
    expect_fail(["encode", str(src), str(dest)], 2, "temporary output already exists")
    check(not dest.exists(), "destination created despite partial")
    check(partial.read_bytes() == b"partial", "partial file was modified")
    same = work / "same.bmp"
    same.write_bytes(rf.write_bmp24(1, 1, bytes([8, 8, 8])))
    before = same.read_bytes()
    expect_fail(["encode", str(same), str(same)], 2, "output file already exists")
    check(same.read_bytes() == before, "input overwritten")


def main() -> int:
    if not EXE.exists():
        print(f"missing {EXE}; run build.ps1 first")
        return 1
    WORK.mkdir(parents=True, exist_ok=True)
    case("oracle", oracle_self_check)
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
            rf.encode_image(9, 4, gradient(9, 4))
            == open(WORK / "topdown" / "out.fold", "rb").read(),
            "top-down input encoded differently",
        ),
    )
    grey = solid(16, 8, (128, 128, 128))
    case("grey", lambda: roundtrip("grey", 16, 8, grey))
    case("corrupt", corrupt_cases)
    # The checksum in a successful file matches zlib, not only our encoder.
    case(
        "zlib-crc",
        lambda: check(
            zlib.crc32(open(WORK / "px1" / "out.fold", "rb").read()[32:]) & 0xFFFFFFFF
            == struct.unpack_from("<I", open(WORK / "px1" / "out.fold", "rb").read(), 24)[0],
            "payload crc",
        ),
    )
    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
