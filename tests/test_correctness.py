"""Independent pixel round trips and rejection tests for RasterFold."""

from __future__ import annotations

import hashlib
import json
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

# The same 16x8 image encoded as format version 3 (the version 2 file above stays as a decode-only vector).
KAT_FOLD3 = bytes.fromhex(
    "5246490303002000100000000800000001000100340000009d827f7a11c99a2b01160000000900000008000000004071dc90"
    "e22c1ade83da77723fbbf53a1fc445a60c000001b7edd8bfbf83004070d8ea017875"
)

# A 16x8 4-colour image and its complete .fold files: KAT_PAL (method 2, §2) and KAT_PAL2 (method 3, §4).
KAT_PAL_W, KAT_PAL_H = 16, 8
_PAL4 = (b"\x10\x20\x30", b"\xE0\x40\x20", b"\x20\xC0\x50", b"\xF0\xF4\xF8")
KAT_PAL_RGB = b"".join(_PAL4[((x // 3) + (y // 2) + ((x + y) & 1)) & 3] for y in range(KAT_PAL_H) for x in range(KAT_PAL_W))
KAT_PAL = bytes.fromhex(
    "524649020200200010000000080000000200010037000000c37a39f65a940d240300102030e0402020c050f0f4f825000000007fbfd92ae2"
    "7bb32a51a99f783de3fe8c97d3903c25a6c11cbfdbfe894c315c5a39db7400"
)
KAT_PAL2 = bytes.fromhex(
    "5246490202002000100000000800000003000100310000002401b19f3de4ffad0300102030e0402020c050f0f4f81f000000007faf571d6a"
    "5deb2169648eec1824c0967cb266634581100506fe76b6c630"
)

# The same palette files with version 3 headers (payloads identical to the version 2 files above).
KAT_PAL3 = bytes.fromhex(
    "524649030300200010000000080000000200010037000000c37a39f67361a3000300102030e0402020c050f0f4f82500"
    "0000007fbfd92ae27bb32a51a99f783de3fe8c97d3903c25a6c11cbfdbfe894c315c5a39db7400"
)
KAT_PAL2_3 = bytes.fromhex(
    "5246490303002000100000000800000003000100310000002401b19f141151890300102030e0402020c050f0f4f81f00"
    "0000007faf571d6a5deb2169648eec1824c0967cb266634581100506fe76b6c630"
)

# The same image as the encoder writes it today: version 3, method 4 (compact palette).
KAT_PAL4 = bytes.fromhex(
    "52464903030020001000000008000000040001002e0000007e45d96afaf682eb035764081021a073ff819fbf40204000"
    "7f6fecc33292ded6fa71f8da8a03279ce7f774541e44a9ac2dcfed94097c"
)

# A 32x16 64-colour repeating-tile image and its complete .fold files:
# KAT_SC5 (version 3, method 5, decode-only) and KAT_SC6 (version 3, method 6, written by the encoder).
KAT_SC_W, KAT_SC_H = 32, 16
_SC_TILE = tuple(bytes(((i * 73 + 11) & 255, (i * 139 + 29) & 255, (i * 197 + 53) & 255)) for i in range(64))
KAT_SC_RGB = b"".join(_SC_TILE[((y & 3) * 16) + (x & 15)] for y in range(KAT_SC_H) for x in range(KAT_SC_W))
KAT_SC5 = bytes.fromhex(
    "5246490303002000200000001000000005000100ee000000a5fc30f706fb575f00f88479d79c1dbc9d69efeb614ef62b58ec"
    "a1a0fc230d13a6851d0955a852db2b3ff75b37f83e18f1ed26133a096b7aaa3c6857a19c5bdb37600e9dc8b87b2b58127e61"
    "fd485cdafe5160ae07943583779d89159dd539251c880e38e7e22f94733cd4f6fec3a487ae49a53624a58b2988efcc1f1ca4"
    "50fd8829260835522b8e9079a8bc1457044e2d706108c570054f17b62cbd1b5d033f5ae6ce0cacfa14ad1193c1e1614ce09"
    "6e9c5424a5de5cf3b2f001b821941b17dc01bd482898d1161841a24a459b56d8b2c5bb1f3af368eaa62f2830e7fa6ad76d2"
    "17b07752a20f17b276eada8fb9fd26a56a8eb7bff8"
)
KAT_SC6 = bytes.fromhex(
    "5246490303002000200000001000000006000100dd0000006b178446ba33612c00e211e7f4753ade756adf0580907c632815"
    "75f7809358d646560e794e5ee6decb8180d4936096c27264e566f6c75b87f84796f1e85ac2477774e25a1444c1d4f0d85f3b"
    "ec30798306bd8ebe5a5d829b750521f76291d6e848fdbebf54e5967817ef54f966be7fa738a4e898e8b51fb0310def6b993c"
    "9f7296ff58ecd2f7230e9264c2db009d7c23d0fe97286b208c0d509d876a4963cc45f53146f5ad9f18aad8c0e163414ba9d3"
    "844e3e92edc89d40c88728cfc63d550b297ba39d9c6741a8c11dc821f00508ff66ec22ee92019bb71fcfc9ff10c68386f88d"
    "6a7400"
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
    check(rf.encode_image(KAT_W, KAT_H, KAT_RGB) == KAT_FOLD3, "known-answer file")
    check(rf.decode_image(KAT_FOLD) == (KAT_W, KAT_H, KAT_RGB), "known-answer decode")
    pal_m2 = rf.pal_encode(KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB, method=rf.METHOD_PAL)
    assert pal_m2 is not None
    check(rf.build_file(KAT_PAL_W, KAT_PAL_H, rf.METHOD_PAL, pal_m2) == KAT_PAL3, "known-answer method 2 palette file")
    check(rf.decode_image(KAT_PAL3) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "known-answer method 2 palette v3 decode")
    check(rf.decode_image(KAT_PAL) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "known-answer method 2 palette decode")
    pal_m3 = rf.pal_encode(KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB, method=rf.METHOD_PAL2)
    assert pal_m3 is not None
    check(rf.build_file(KAT_PAL_W, KAT_PAL_H, rf.METHOD_PAL2, pal_m3) == KAT_PAL2_3, "known-answer method 3 palette file")
    check(rf.encode_image(KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB) == KAT_PAL4, "known-answer method 4 palette file")
    check(rf.decode_image(KAT_PAL4) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "known-answer method 4 palette decode")
    check(rf.decode_image(KAT_PAL2_3) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "known-answer method 3 palette v3 decode")
    check(rf.decode_image(KAT_PAL2) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "known-answer method 3 palette decode")
    sc_m5 = rf.sc_encode(KAT_SC_W, KAT_SC_H, KAT_SC_RGB)
    assert sc_m5 is not None
    check(rf.build_file(KAT_SC_W, KAT_SC_H, rf.METHOD_SC, sc_m5) == KAT_SC5, "known-answer method 5 sc file")
    check(rf.decode_image(KAT_SC5) == (KAT_SC_W, KAT_SC_H, KAT_SC_RGB), "known-answer method 5 sc decode")
    check(rf.encode_image(KAT_SC_W, KAT_SC_H, KAT_SC_RGB) == KAT_SC6, "known-answer method 6 sc file")
    check(rf.decode_image(KAT_SC6) == (KAT_SC_W, KAT_SC_H, KAT_SC_RGB), "known-answer method 6 sc decode")


def known_answer() -> None:
    work = WORK / "kat"
    work.mkdir(parents=True, exist_ok=True)
    src, out, back = work / "in.bmp", work / "out.fold", work / "out.bmp"
    for f in (out, back):
        f.unlink(missing_ok=True)
    src.write_bytes(rf.write_bmp24(KAT_W, KAT_H, KAT_RGB))
    expect_ok(["encode", str(src), str(out)])
    check(out.read_bytes() == KAT_FOLD3, "codec bytes differ from the known-answer file")
    expect_ok(["decode", str(out), str(back)])
    check(rf.read_bmp24(back.read_bytes()) == (KAT_W, KAT_H, KAT_RGB), "known-answer pixels")

    psrc, pout, pback = work / "pal_in.bmp", work / "pal_out.fold", work / "pal_out.bmp"
    pm2_fold, pm2_back = work / "pal_m2.fold", work / "pal_m2.bmp"
    for f in (pout, pback, pm2_fold, pm2_back):
        f.unlink(missing_ok=True)
    psrc.write_bytes(rf.write_bmp24(KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB))
    expect_ok(["encode", str(psrc), str(pout)])
    check(pout.read_bytes() == KAT_PAL4, "codec bytes differ from the method 4 palette known-answer file")
    expect_ok(["decode", str(pout), str(pback)])
    check(rf.read_bmp24(pback.read_bytes()) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "method 4 palette known-answer pixels")
    insp = expect_ok(["inspect", str(pout)]).stdout.decode("utf-8", errors="replace")
    check("method: palette" in insp and "palette_colours: 4" in insp, f"inspect output missing palette fields: {insp}")
    insp_json = json.loads(expect_ok(["inspect", "--json", str(pout)]).stdout.decode("utf-8", errors="replace"))
    check(
        insp_json.get("method") == "palette" and insp_json.get("palette_colours") == 4 and insp_json.get("version") == 3,
        f"inspect --json output missing method 4 palette fields: {insp_json}",
    )

    # Every older palette file still decodes to the same pixels, through the strided BMP writer and
    # through the packed RGB decoder (the PPM output path).
    for tag, blob in (("kat_pal", KAT_PAL), ("kat_pal3", KAT_PAL3), ("kat_pal2", KAT_PAL2), ("kat_pal2_3", KAT_PAL2_3), ("kat_pal4", KAT_PAL4)):
        legacy_fold, legacy_bmp, legacy_ppm = work / f"{tag}.fold", work / f"{tag}.bmp", work / f"{tag}.ppm"
        for f in (legacy_bmp, legacy_ppm):
            f.unlink(missing_ok=True)
        legacy_fold.write_bytes(blob)
        expect_ok(["decode", str(legacy_fold), str(legacy_bmp)])
        check(rf.read_bmp24(legacy_bmp.read_bytes()) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), f"{tag}: BMP decode pixels")
        expect_ok(["decode", str(legacy_fold), str(legacy_ppm)])
        with Image.open(legacy_ppm) as im:
            check(im.size == (KAT_PAL_W, KAT_PAL_H) and im.convert("RGB").tobytes() == KAT_PAL_RGB, f"{tag}: packed RGB decode pixels")
        check(rf.decode_image(blob) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), f"{tag}: oracle decode pixels")

    pm2_fold.write_bytes(KAT_PAL)
    expect_ok(["decode", str(pm2_fold), str(pm2_back)])
    check(rf.read_bmp24(pm2_back.read_bytes()) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "method 2 palette known-answer pixels")
    insp_m2 = expect_ok(["inspect", str(pm2_fold)]).stdout.decode("utf-8", errors="replace")
    check("method: palette" in insp_m2 and "palette_colours: 4" in insp_m2, f"method 2 inspect output missing palette fields: {insp_m2}")

    sc_src, sc_out, sc_bmp, sc_ppm = work / "sc_in.bmp", work / "sc_out.fold", work / "sc_out.bmp", work / "sc_out.ppm"
    sc5_fold, sc5_bmp, sc5_ppm = work / "sc_m5.fold", work / "sc_m5.bmp", work / "sc_m5.ppm"
    for f in (sc_out, sc_bmp, sc_ppm, sc5_fold, sc5_bmp, sc5_ppm):
        f.unlink(missing_ok=True)
    sc_src.write_bytes(rf.write_bmp24(KAT_SC_W, KAT_SC_H, KAT_SC_RGB))
    expect_ok(["encode", str(sc_src), str(sc_out)])
    check(sc_out.read_bytes() == KAT_SC6, "codec bytes differ from the method 6 sc known-answer file")
    expect_ok(["decode", str(sc_out), str(sc_bmp)])
    check(rf.read_bmp24(sc_bmp.read_bytes()) == (KAT_SC_W, KAT_SC_H, KAT_SC_RGB), "method 6 sc known-answer BMP pixels")
    expect_ok(["decode", str(sc_out), str(sc_ppm)])
    with Image.open(sc_ppm) as im:
        check(im.size == (KAT_SC_W, KAT_SC_H) and im.convert("RGB").tobytes() == KAT_SC_RGB, "method 6 sc known-answer packed RGB pixels")
    insp_sc = expect_ok(["inspect", str(sc_out)]).stdout.decode("utf-8", errors="replace")
    check("method: sc" in insp_sc and "palette_colours" not in insp_sc and "stream_" not in insp_sc, f"method 6 inspect output: {insp_sc}")
    insp_sc_json = json.loads(expect_ok(["inspect", "--json", str(sc_out)]).stdout.decode("utf-8", errors="replace"))
    check(
        insp_sc_json.get("method") == "sc"
        and insp_sc_json.get("version") == 3
        and "palette_colours" not in insp_sc_json
        and "streams" not in insp_sc_json,
        f"inspect --json output invalid for method 6: {insp_sc_json}",
    )

    # Legacy method 5 file still decodes through both strided BMP and packed RGB (PPM) and inspects as sc.
    sc5_fold.write_bytes(KAT_SC5)
    expect_ok(["decode", str(sc5_fold), str(sc5_bmp)])
    check(rf.read_bmp24(sc5_bmp.read_bytes()) == (KAT_SC_W, KAT_SC_H, KAT_SC_RGB), "method 5 sc legacy BMP pixels")
    expect_ok(["decode", str(sc5_fold), str(sc5_ppm)])
    with Image.open(sc5_ppm) as im:
        check(im.size == (KAT_SC_W, KAT_SC_H) and im.convert("RGB").tobytes() == KAT_SC_RGB, "method 5 sc legacy packed RGB pixels")
    insp_sc5 = expect_ok(["inspect", str(sc5_fold)]).stdout.decode("utf-8", errors="replace")
    check("method: sc" in insp_sc5 and "palette_colours" not in insp_sc5 and "stream_" not in insp_sc5, f"method 5 inspect output: {insp_sc5}")
    insp_sc5_json = json.loads(expect_ok(["inspect", "--json", str(sc5_fold)]).stdout.decode("utf-8", errors="replace"))
    check(
        insp_sc5_json.get("method") == "sc"
        and insp_sc5_json.get("version") == 3
        and "palette_colours" not in insp_sc5_json
        and "streams" not in insp_sc5_json,
        f"inspect --json output invalid for method 5: {insp_sc5_json}",
    )


def cli_reference() -> None:
    """The generated command-line reference (docs/cli.md) is what this binary prints: the generator reads the help
    constants of src/main.c, and this case holds the built target's `help` and `help <command>` output to that
    extraction, byte for byte once CRLF is read as LF (the Windows target prints CRLF)."""
    import importlib.util

    spec = importlib.util.spec_from_file_location("build_docs", ROOT / "scripts" / "build-docs.py")
    assert spec is not None and spec.loader is not None
    build_docs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build_docs)
    cli = build_docs.parse_cli_help(ROOT, "src/main.c")

    def printed(args: list[str]) -> str:
        return expect_ok(args).stdout.decode("utf-8").replace("\r\n", "\n")

    check(printed(["help"]) == cli["summary"], "`help` output differs from the generator's summary")
    check(printed(["--help"]) == cli["summary"], "`--help` output differs from the generator's summary")
    commands = [command for command, _ in cli["commands"]]
    check(commands == ["encode", "decode", "inspect", "verify", "help", "version"], f"unexpected command list: {commands}")
    for command, body in cli["commands"]:
        check(printed(["help", command]) == body, f"`help {command}` output differs from the generator's extraction")
        check(f"  rasterfold {command}" in cli["summary"], f"summary has no usage line for {command}")
    page = (ROOT / "docs" / "cli.md").read_text(encoding="utf-8")
    entry = next(pg for pg in json.loads((ROOT / "docs" / "site-content.json").read_text(encoding="utf-8"))["pages"] if pg["id"] == "cli")
    check(build_docs.render_cli_page(ROOT, entry) == page, "docs/cli.md is not what the generator renders; run python3 scripts/build-docs.py --write")
    for command, body in cli["commands"]:
        check(body in page, f"docs/cli.md does not carry `help {command}` verbatim")


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


def with_streams3(sizes: tuple[int, ...], data: bytes, nstrips: int = 1) -> bytes:
    return bytes([nstrips]) + struct.pack(f"<{len(sizes)}I", *sizes) + data


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
    v4 = bytearray(good)
    v4[3] = 4
    struct.pack_into("<H", v4, 4, 4)
    reject(recrc(v4), 3, "unsupported fold: version")
    m23 = bytearray(good)
    struct.pack_into("<H", m23, 4, 3)
    reject(recrc(m23), 3, "unsupported fold: version")
    m32 = bytearray(good)
    m32[3] = 3
    reject(recrc(m32), 3, "unsupported fold: version")
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
    method[16] = 4
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

    # Version 3 CM strip-header rejection cases (empty payload, S=0, S=5, S > h, payload < 1 + 12*S).
    reject(rf.build_file(4, 2, rf.METHOD_CM, b""), 4, "invalid fold: cm streams")
    reject(rf.build_file(4, 2, rf.METHOD_CM, with_streams3((5, 5, 5), b"\x00" * 15, nstrips=0)), 4, "invalid fold: cm strips")
    reject(rf.build_file(4, 8, rf.METHOD_CM, with_streams3((5,) * 15, b"\x00" * 75, nstrips=5)), 4, "invalid fold: cm strips")
    reject(rf.build_file(4, 2, rf.METHOD_CM, with_streams3((5,) * 9, b"\x00" * 45, nstrips=3)), 4, "invalid fold: cm strips")
    reject(rf.build_file(4, 4, rf.METHOD_CM, b"\x02" + b"\x00" * 20), 4, "invalid fold: cm streams")

    # Methods 2 and 3 (palette) framing and stream rejection cases, with checksums recomputed.
    for pal_method, kat_blob in ((rf.METHOD_PAL, KAT_PAL), (rf.METHOD_PAL2, KAT_PAL2)):
        reject(rf.build_file(2, 2, pal_method, b"\x00\x00\x01\x02\x03"), 4, "invalid fold: palette")
        pal_257 = (
            struct.pack("<H", 256)
            + b"".join(bytes((i & 255, (i * 3) & 255, (i * 7) & 255)) for i in range(257))
            + struct.pack("<I", 5)
            + b"\x00" * 5
        )
        reject(rf.build_file(16, 17, pal_method, pal_257), 4, "invalid fold: palette")
        pal_short_colors = bytearray(kat_blob)
        struct.pack_into("<H", pal_short_colors, 32, 4)
        reject(recrc(pal_short_colors), 4, "invalid fold: palette")
        pal_stream_lt5 = struct.pack("<H", 0) + b"\x10\x20\x30" + struct.pack("<I", 4) + b"\x00" * 4
        reject(rf.build_file(2, 2, pal_method, pal_stream_lt5), 4, "invalid fold: palette")
        pal_bad_len = bytearray(kat_blob)
        pal_s_off = 32 + 2 + 4 * 3
        pal_s_len = struct.unpack_from("<I", pal_bad_len, pal_s_off)[0]
        struct.pack_into("<I", pal_bad_len, pal_s_off, pal_s_len + 1)
        reject(recrc(pal_bad_len), 4, "invalid fold: palette")
        pal_dup = bytearray(kat_blob)
        pal_dup[34 + 3 : 34 + 6] = pal_dup[34 : 34 + 3]
        reject(recrc(pal_dup), 4, "invalid fold: duplicate palette colour")
        pal_trunc = bytearray(kat_blob[:-1])
        struct.pack_into("<I", pal_trunc, pal_s_off, pal_s_len - 1)
        reject(recrc(pal_trunc), 4, "invalid fold: palette stream")
        pal_trail = bytearray(kat_blob) + b"\x00"
        struct.pack_into("<I", pal_trail, pal_s_off, pal_s_len + 1)
        reject(recrc(pal_trail), 4, "invalid fold: palette stream")

    def _build_bad_pal_literal_m2(mode: str) -> bytes:
        d1 = [rf._PalCounter(False) for _ in range(64)]
        d2 = [rf._PalCounter(False) for _ in range(32)]
        d3 = [rf._PalCounter(False) for _ in range(32)]
        tree = [rf._PalCounter(False) for _ in range(256)]
        enc = rf.RangeEncoder()

        def code_lit(idx: int) -> None:
            node = 1
            for bit_pos in range(7, -1, -1):
                bit = (idx >> bit_pos) & 1
                tree[node].code(enc, None, bit)
                node = node * 2 + bit

        if mode in ("ge_count", "eq_a"):
            # 2x1, 2 colours; at (0, 0), a=b=c=d=0, pat=31, hit=0 -> D1=0, literal = 2 (>= count) or 0 (== a).
            d1[31 * 2 + 0].code(enc, None, 0)
            code_lit(2 if mode == "ge_count" else 0)
            stream = enc.finish()
            pay = struct.pack("<H", 1) + b"\x10\x20\x30\x40\x50\x60" + struct.pack("<I", len(stream)) + stream
            return rf.build_file(2, 1, rf.METHOD_PAL, pay)
        if mode == "eq_b":
            d1[31 * 2 + 0].code(enc, None, 1)
            d1[31 * 2 + 1].code(enc, None, 0)
            code_lit(1)
            d1[11 * 2 + 0].code(enc, None, 1)
            d1[18 * 2 + 1].code(enc, None, 0)
            d2[18].code(enc, None, 0)
            code_lit(1)
            stream = enc.finish()
            pay = struct.pack("<H", 2) + b"\x10\x20\x30\x40\x50\x60\x70\x80\x90" + struct.pack("<I", len(stream)) + stream
            return rf.build_file(2, 2, rf.METHOD_PAL, pay)
        assert mode == "eq_d"
        d1[31 * 2 + 0].code(enc, None, 1)
        d1[31 * 2 + 1].code(enc, None, 0)
        code_lit(1)
        d1[11 * 2 + 0].code(enc, None, 0)
        d3[11].code(enc, None, 0)
        code_lit(1)
        stream = enc.finish()
        pay = struct.pack("<H", 2) + b"\x10\x20\x30\x40\x50\x60\x70\x80\x90" + struct.pack("<I", len(stream)) + stream
        return rf.build_file(2, 2, rf.METHOD_PAL, pay)

    for bad_mode in ("ge_count", "eq_a", "eq_b", "eq_d"):
        reject(_build_bad_pal_literal_m2(bad_mode), 4, "invalid fold: palette stream")

    def _build_bad_pal_literal_m3(mode: str) -> bytes:
        d1 = [rf._PalCounter(True) for _ in range(256)]
        d2 = [rf._PalCounter(True) for _ in range(128)]
        d3 = [rf._PalCounter(True) for _ in range(128)]
        tree = [rf._PalCounter(True) for _ in range(256)]
        enc = rf.RangeEncoder()
        pal3 = b"\x10\x20\x30\x40\x50\x60\x70\x80\x90"

        def code_lit(idx: int) -> None:
            node = 1
            for bit_pos in range(7, -1, -1):
                bit = (idx >> bit_pos) & 1
                tree[node].code(enc, None, bit)
                node = node * 2 + bit

        def emit_rows(rows: list[list[int]], bad_xy: tuple[int, int], bad_lit: int) -> bytes:
            h, w = len(rows), len(rows[0])
            hit = 0
            for y in range(h):
                for x in range(w):
                    a, b, e, pat = rf._pal_neighbours_m3(rows, w, x, y)
                    if (x, y) == bad_xy:
                        d1[pat * 2 + hit].code(enc, None, 0)
                        if b != a:
                            d2[pat].code(enc, None, 0)
                        if e != a and e != b:
                            d3[pat].code(enc, None, 0)
                        code_lit(bad_lit)
                        stream = enc.finish()
                        pay = struct.pack("<H", 2) + pal3 + struct.pack("<I", len(stream)) + stream
                        return rf.build_file(w, h, rf.METHOD_PAL2, pay)
                    idx = rows[y][x]
                    if d1[pat * 2 + hit].code(enc, None, 1 if idx == a else 0):
                        hit = 1
                        continue
                    hit = 0
                    if b != a and d2[pat].code(enc, None, 1 if idx == b else 0):
                        continue
                    if e != a and e != b and d3[pat].code(enc, None, 1 if idx == e else 0):
                        continue
                    code_lit(idx)
            raise AssertionError(mode)

        if mode == "count1_d1_zero":
            # 1x1, 1 colour; at (0, 0), a=b=c=d=ww=nn=0, pat=127, hit=0 -> D1=0, literal=0 (== a).
            d1[127 * 2 + 0].code(enc, None, 0)
            code_lit(0)
            stream = enc.finish()
            pay = struct.pack("<H", 0) + b"\x10\x20\x30" + struct.pack("<I", len(stream)) + stream
            return rf.build_file(1, 1, rf.METHOD_PAL2, pay)
        if mode == "count2_trail":
            # 2x1, 2 colours: valid 2-colour deduction stream with 1 trailing byte.
            pay2 = rf.pal_encode(2, 1, b"\x10\x20\x30\x40\x50\x60", method=rf.METHOD_PAL2)
            assert pay2 is not None
            b2 = bytearray(pay2) + b"\x00"
            s_off = 2 + 2 * 3
            s_len = struct.unpack_from("<I", b2, s_off)[0]
            struct.pack_into("<I", b2, s_off, s_len + 1)
            return rf.build_file(2, 1, rf.METHOD_PAL2, bytes(b2))
        if mode in ("ge_count", "eq_a"):
            return emit_rows([[0, 1, 2]], (0, 0), 3 if mode == "ge_count" else 0)
        if mode == "eq_b":
            # At (1, 1) of [[0, 1], [0, 2]]: a=0, b=1, c=0, d=1, ww=0 -> e=a=0, b!=a -> literal=1 (== b, excluded!).
            return emit_rows([[0, 1], [0, 2]], (1, 1), 1)
        if mode == "eq_e_d":
            # At (0, 1) of [[0, 1], [0, 2]]: a=0, b=0, c=0, d=1 -> e=d=1 -> literal=1 (== e=d, excluded!).
            return emit_rows([[0, 1], [0, 2]], (0, 1), 1)
        if mode == "eq_e_c":
            # At (1, 1) of [[1, 0], [0, 2]]: a=0, b=0, c=1, d=0 -> d in {a, b}, e=c=1 -> literal=1 (== e=c, excluded!).
            return emit_rows([[1, 0], [0, 2]], (1, 1), 1)
        assert mode == "eq_e_ww"
        # At (2, 1) of [[0, 0, 0], [1, 0, 2]]: a=0, b=0, c=0, d=0, ww=1 -> d, c in {a, b}, e=ww=1 -> literal=1 (== e=ww, excluded!).
        return emit_rows([[0, 0, 0], [1, 0, 2]], (2, 1), 1)

    for bad_mode3 in ("count1_d1_zero", "count2_trail", "ge_count", "eq_a", "eq_b", "eq_e_d", "eq_e_c", "eq_e_ww"):
        reject(_build_bad_pal_literal_m3(bad_mode3), 4, "invalid fold: palette stream")

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
    #    a structurally valid 28-byte CM payload (S=1 plus three 5-byte 0x80 streams that
    #    enter run_pipelined / run_channel and exhaust dec.in >= dec.lim on row 0).
    fold_20 = rf.build_file(4096, 4096, rf.METHOD_CM, with_streams3((5, 5, 5), b"\x80" * 7))
    fold_27 = rf.build_file(4096, 4096, rf.METHOD_CM, with_streams3((5, 5, 5), b"\x80" * 15))
    for t in ("1", "3", "6", "9"):
        check_reject_fast("cm4096_20b", fold_20, t, "invalid fold: cm streams")
        check_reject_fast("cm4096_27b", fold_27, t, "invalid fold: cm stream")

    # 2. Truncate each of the three streams (G, R, B) in turn on an image taller
    #    than XROWS (64x64, h=64 > XROWS=32) both at 5 bytes (row-0 exhaustion)
    #    and at 75% of the valid stream length (exhausting after row 32 so
    #    y >= XROWS backpressure waits execute before abort).
    valid_64 = rf.encode_image(64, 64, photo(64, 64))
    sz0, sz1, sz2 = struct.unpack_from("<III", valid_64, 33)
    s0 = valid_64[45 : 45 + sz0]
    s1 = valid_64[45 + sz0 : 45 + sz0 + sz1]
    s2 = valid_64[45 + sz0 + sz1 : 45 + sz0 + sz1 + sz2]
    orig_streams = [s0, s1, s2]
    short_stream = b"\x80" * 5
    for c in range(3):
        parts = list(orig_streams)
        parts[c] = short_stream
        trunc_payload = with_streams3((len(parts[0]), len(parts[1]), len(parts[2])), b"".join(parts))
        trunc_fold = rf.build_file(64, 64, rf.METHOD_CM, trunc_payload)
        mid_parts = list(orig_streams)
        mid_len = max(5, (len(orig_streams[c]) * 3) // 4)
        mid_parts[c] = orig_streams[c][:mid_len]
        mid_payload = with_streams3((len(mid_parts[0]), len(mid_parts[1]), len(mid_parts[2])), b"".join(mid_parts))
        mid_fold = rf.build_file(64, 64, rf.METHOD_CM, mid_payload)
        for t in ("1", "3", "6", "9"):
            check_reject_fast(f"trunc_ch{c}", trunc_fold, t, "invalid fold: cm stream")
            check_reject_fast(f"trunc_mid_ch{c}", mid_fold, t, "invalid fold: cm stream")

    # 3. Single-strip (S=1 at 8x512) and multi-strip (valid S=2 decoder-interoperability
    #    fixture at 8x512, S=3 at 6x768, and valid S=4 decoder-interoperability
    #    fixtures at 6x400 and short-strip 16x16 where warm_rows == h_s) pre-warm
    #    (5-byte 0x80 stream, aborting before warm_rows) and post-warm (75% stream
    #    length, aborting after warm_ready handoff) stream exhaustion across every
    #    strip and channel.
    for mw, mh, want_s, force_s in ((8, 512, 1, None), (8, 512, 2, 2), (6, 768, 3, None), (6, 400, 4, 4), (16, 16, 4, 4)):
        mrgb = bytearray(mw * mh * 3)
        mrng = random.Random(mh * 10 + want_s)
        for y in range(mh):
            for x in range(mw):
                idx = (y * mw + x) * 3
                mrgb[idx] = (x * 29 + y * 13 + mrng.randrange(-15, 16)) & 255
                mrgb[idx + 1] = (x * 17 + y * 7 + mrng.randrange(-11, 12)) & 255
                mrgb[idx + 2] = (x * 31 + y * 19 + mrng.randrange(-19, 20)) & 255
        if force_s is None:
            v_multi = rf.encode_image(mw, mh, bytes(mrgb))
        else:
            v_multi = rf.build_file(mw, mh, rf.METHOD_CM, rf.cm_encode(mw, mh, bytes(mrgb), nstrips=force_s))
        check(v_multi[16] == rf.METHOD_CM and v_multi[32] == want_s, f"expected S={want_s} at {mw}x{mh}")
        hdr_len = 1 + 12 * want_s
        msizes = list(struct.unpack_from(f"<{3 * want_s}I", v_multi, 33))
        mpos = 32 + hdr_len
        mstreams = []
        for msz in msizes:
            mstreams.append(v_multi[mpos : mpos + msz])
            mpos += msz
        for sc in range(3 * want_s):
            pre_parts = list(mstreams)
            pre_parts[sc] = short_stream
            pre_fold = rf.build_file(mw, mh, rf.METHOD_CM, with_streams3(tuple(len(p) for p in pre_parts), b"".join(pre_parts), nstrips=want_s))
            post_parts = list(mstreams)
            post_len = max(5, (len(mstreams[sc]) * 3) // 4)
            post_parts[sc] = mstreams[sc][:post_len]
            post_fold = rf.build_file(mw, mh, rf.METHOD_CM, with_streams3(tuple(len(p) for p in post_parts), b"".join(post_parts), nstrips=want_s))
            for t in ("1", "3", "6", "9"):
                check_reject_fast(f"s{want_s}_{mh}_pre_sc{sc}", pre_fold, t, "invalid fold: cm stream")
                check_reject_fast(f"s{want_s}_{mh}_post_sc{sc}", post_fold, t, "invalid fold: cm stream")


def _row_end_tail_crossings(w: int, h: int, payload: bytes) -> set[int]:
    """Return channels (0..2) whose stream has rem > 48 and crosses sz - 40 on
    the final sample (x == w - 1) of a nonfinal row (y < h - 1)."""
    sizes = struct.unpack_from("<III", payload, 1)
    chans = [rf.Channel(w, rf.NMODEL, c) for c in range(3)]
    pos = 13
    for ch, size in zip(chans, sizes):
        ch.dec = rf.RangeDecoder(payload[pos : pos + size])
        pos += size
    out = bytearray(w * h * 3)
    crossed: set[int] = set()
    for y in range(h):
        for ch in chans:
            ch.begin_row()
        for x in range(w):
            i = (y * w + x) * 3
            before = [ch.dec.pos for ch in chans]
            gs: dict = {}
            g = rf._code_sample(chans[0], 0, x, y, out[i + 1], gs)
            out[i + 1] = g
            cross = {"g": g, "eg": gs["r"], "feg": gs["fe"]}
            rs = dict(cross)
            out[i] = rf._code_sample(chans[1], 1, x, y, out[i], rs)
            cross["er"], cross["fer"] = rs["r"], rs["fe"]
            out[i + 2] = rf._code_sample(chans[2], 2, x, y, out[i + 2], dict(cross))
            after = [ch.dec.pos for ch in chans]
            if y < h - 1 and x == w - 1:
                for c, sz in enumerate(sizes):
                    if sz - 5 > 48 and before[c] < sz - 40 <= after[c]:
                        crossed.add(c)
        if y >= 2:
            for ch in chans:
                ch.drop_row(y - 2)
    return crossed


def tail_threshold_roundtrip() -> None:
    """Multi-row CM streams in both rdec_init regimes (rem <= 48 and rem > 48
    crossing in + n - 40 on nonfinal row endings) round-trip at 1 and 3 threads."""
    work = WORK / "tail_threshold"
    work.mkdir(parents=True, exist_ok=True)
    fixtures = [
        ("tail_le48_solid", 6, 6, solid(6, 6, (42, 100, 200))),
        ("tail_le48_grad", 8, 6, gradient(8, 6)),
        ("tail_mixed_check16", 16, 12, checker(16, 12, 3)),
        ("tail_rowend_ch0", 4, 25, photo(4, 25)),
        ("tail_rowend_ch1", 4, 43, photo(4, 43)),
        ("tail_rowend_ch2", 4, 37, photo(4, 37)),
        ("tail_gt48_photo16", 16, 12, photo(16, 12)),
        ("tail_gt48_photo20", 20, 16, photo(20, 16)),
    ]
    saw_le48 = False
    row_end_channels: set[int] = set()
    for name, w, h, rgb in fixtures:
        oracle_fold = rf.encode_image(w, h, rgb)
        cm_fold = oracle_fold if oracle_fold[16] == rf.METHOD_CM else rf.build_file(w, h, rf.METHOD_CM, rf.cm_encode(w, h, rgb))
        sz0, sz1, sz2 = struct.unpack_from("<III", cm_fold, 33)
        rems = (sz0 - 5, sz1 - 5, sz2 - 5)
        if all(0 <= r <= 48 for r in rems):
            saw_le48 = True
        row_end_channels |= _row_end_tail_crossings(w, h, cm_fold[32:])
        in_bmp = work / f"{name}_in.bmp"
        in_bmp.write_bytes(rf.write_bmp24(w, h, rgb))
        cm_in = work / f"{name}_cm.fold"
        cm_in.write_bytes(cm_fold)
        for t in ("1", "3"):
            out_fold = work / f"{name}_t{t}.fold"
            out_bmp = work / f"{name}_t{t}.bmp"
            cm_bmp = work / f"{name}_cm_t{t}.bmp"
            out_fold.unlink(missing_ok=True)
            out_bmp.unlink(missing_ok=True)
            cm_bmp.unlink(missing_ok=True)
            r_enc = run(["encode", "--threads", t, str(in_bmp), str(out_fold)])
            check(r_enc.returncode == 0, f"{name} encode t={t} failed: {r_enc.stderr.decode(errors='replace')}")
            check(out_fold.read_bytes() == oracle_fold, f"{name} encode t={t} != oracle")
            r_dec = run(["decode", "--threads", t, str(out_fold), str(out_bmp)])
            check(r_dec.returncode == 0, f"{name} decode t={t} failed: {r_dec.stderr.decode(errors='replace')}")
            check(rf.read_bmp24(out_bmp.read_bytes()) == (w, h, rgb), f"{name} decode t={t} pixel mismatch")
            r_cm_dec = run(["decode", "--threads", t, str(cm_in), str(cm_bmp)])
            check(r_cm_dec.returncode == 0, f"{name} cm decode t={t} failed: {r_cm_dec.stderr.decode(errors='replace')}")
            check(rf.read_bmp24(cm_bmp.read_bytes()) == (w, h, rgb), f"{name} cm decode t={t} pixel mismatch")
    check(saw_le48, "tail_threshold fixtures did not cover rem <= 48")
    check(row_end_channels == {0, 1, 2}, f"tail_threshold fixtures did not cover nonfinal row-end tail crossings on all 3 channels: {row_end_channels}")


def warm_strips() -> None:
    """Multi-strip (S=2 at h=512, S=3 at h=768) and single-strip (S=1) byte identity
    across --threads 1, 3, 6, 9, oracle round-trip, and CLI inspect stream_g/r/b sums."""
    work = WORK / "warm_strips"
    work.mkdir(parents=True, exist_ok=True)

    # Verify CLI inspect stream_g/r/b on version 2 KAT_FOLD and version 3 KAT_FOLD3 (S=1).
    for tag, blob, v_hdr in (("v2_kat", KAT_FOLD, 2), ("v3_kat", KAT_FOLD3, 3)):
        f_path = work / f"{tag}.fold"
        f_path.write_bytes(blob)
        insp = expect_ok(["inspect", str(f_path)]).stdout.decode("utf-8", errors="replace")
        if v_hdr == 2:
            exp_g, exp_r, exp_b = struct.unpack_from("<III", blob, 32)
        else:
            exp_g, exp_r, exp_b = struct.unpack_from("<III", blob, 33)
        check(
            f"stream_g: {exp_g}" in insp and f"stream_r: {exp_r}" in insp and f"stream_b: {exp_b}" in insp,
            f"{tag} inspect mismatch: {insp}",
        )

    for w, h, want_s in ((12, 512, 1), (10, 768, 3)):
        rng = random.Random(w * 1000 + h)
        rgb_buf = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                idx = (y * w + x) * 3
                rgb_buf[idx] = (x * 29 + y * 13 + rng.randrange(-15, 16)) & 255
                rgb_buf[idx + 1] = (x * 17 + y * 7 + rng.randrange(-11, 12)) & 255
                rgb_buf[idx + 2] = (x * 31 + y * 19 + rng.randrange(-19, 20)) & 255
        rgb = bytes(rgb_buf)
        oracle_fold = rf.encode_image(w, h, rgb)
        check(oracle_fold[16] == rf.METHOD_CM and oracle_fold[32] == want_s, f"expected method 1 S={want_s} at {w}x{h}")
        check(rf.decode_image(oracle_fold) == (w, h, rgb), f"oracle decode failed at {w}x{h}")
        in_bmp = work / f"in_{h}.bmp"
        in_bmp.write_bytes(rf.write_bmp24(w, h, rgb))
        canonical_bmp = rf.write_bmp24(w, h, rgb)
        for t in ("1", "3", "6", "9"):
            out_fold = work / f"out_{h}_t{t}.fold"
            out_bmp = work / f"out_{h}_t{t}.bmp"
            out_fold.unlink(missing_ok=True)
            out_bmp.unlink(missing_ok=True)
            expect_ok(["encode", "--threads", t, str(in_bmp), str(out_fold)])
            check(out_fold.read_bytes() == oracle_fold, f"{w}x{h} encode t={t} != oracle")
            expect_ok(["decode", "--threads", t, str(out_fold), str(out_bmp)])
            check(out_bmp.read_bytes() == canonical_bmp, f"{w}x{h} decode t={t} != canonical BMP")
        sizes = struct.unpack_from(f"<{3 * want_s}I", oracle_fold, 33)
        exp_sums = [sum(sizes[3 * s + c] for s in range(want_s)) for c in range(3)]
        insp = expect_ok(["inspect", str(work / f"out_{h}_t1.fold")]).stdout.decode("utf-8", errors="replace")
        check(
            f"stream_g: {exp_sums[0]}" in insp and f"stream_r: {exp_sums[1]}" in insp and f"stream_b: {exp_sums[2]}" in insp,
            f"S={want_s} inspect stream sums mismatch: {insp} vs {exp_sums}",
        )

    # Verify decoder interoperability on valid S=2 and S=4 version 3 files constructed by the
    # independent oracle, including S=2 (h=512, warm_rows=68), short strips where warm_rows
    # reaches the strip's final row (h=12, S=4, h_s=3), and tall strips (h=400, S=4, warm_rows=96 < h_s),
    # across --threads 1, 3, 6, 9 and CLI inspect stream_g/r/b sums.
    for w, h, want_s in ((12, 512, 2), (12, 12, 4), (8, 400, 4)):
        rng = random.Random(w * 2000 + h + want_s)
        rgb_buf = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                idx = (y * w + x) * 3
                rgb_buf[idx] = (x * 31 + y * 17 + rng.randrange(-15, 16)) & 255
                rgb_buf[idx + 1] = (x * 19 + y * 11 + rng.randrange(-11, 12)) & 255
                rgb_buf[idx + 2] = (x * 23 + y * 29 + rng.randrange(-19, 20)) & 255
        rgb = bytes(rgb_buf)
        s_fold = rf.build_file(w, h, rf.METHOD_CM, rf.cm_encode(w, h, rgb, nstrips=want_s))
        check(s_fold[16] == rf.METHOD_CM and s_fold[32] == want_s, f"expected method 1 S={want_s} at {w}x{h}")
        check(rf.decode_image(s_fold) == (w, h, rgb), f"oracle S={want_s} decode failed at {w}x{h}")
        s_path = work / f"s{want_s}_{h}.fold"
        s_path.write_bytes(s_fold)
        canonical_bmp = rf.write_bmp24(w, h, rgb)
        for t in ("1", "3", "6", "9"):
            out_bmp = work / f"s{want_s}_{h}_t{t}.bmp"
            out_bmp.unlink(missing_ok=True)
            expect_ok(["decode", "--threads", t, str(s_path), str(out_bmp)])
            check(out_bmp.read_bytes() == canonical_bmp, f"S={want_s} {w}x{h} decode t={t} != canonical BMP")
        sizes = struct.unpack_from(f"<{3 * want_s}I", s_fold, 33)
        exp_sums = [sum(sizes[3 * s + c] for s in range(want_s)) for c in range(3)]
        insp = expect_ok(["inspect", str(s_path)]).stdout.decode("utf-8", errors="replace")
        check(
            f"stream_g: {exp_sums[0]}" in insp and f"stream_r: {exp_sums[1]}" in insp and f"stream_b: {exp_sums[2]}" in insp,
            f"S={want_s} {w}x{h} inspect stream sums mismatch: {insp} vs {exp_sums}",
        )


def palette_method() -> None:
    """Methods 2 and 3 selection (raw vs CM vs palette), 256/257 colour boundary,
    inspect reporting, legacy method 2 counter arithmetic, boundary fallbacks (w=1,2, 3-row nn wrap),
    and positive D3 candidate fallback (e=d, e=c, e=ww, and literal fallback)."""
    work = WORK / "palette_method"
    work.mkdir(parents=True, exist_ok=True)

    # 1. Method selection: 1x1 1-colour uses raw (0), 68-colour KAT_RGB uses CM (1),
    #    4-colour KAT_PAL_RGB uses extended palette (3), and legacy KAT_PAL is method 2.
    check(rf.encode_image(1, 1, b"\x00\x00\x00")[16] == rf.METHOD_RAW, "1x1 did not select METHOD_RAW")
    check(KAT_FOLD[16] == rf.METHOD_CM, "KAT_FOLD did not select METHOD_CM")
    check(KAT_PAL[16] == rf.METHOD_PAL, "KAT_PAL is not METHOD_PAL")
    check(KAT_PAL2[16] == rf.METHOD_PAL2, "KAT_PAL2 did not select METHOD_PAL2")

    # 2. Exactly 256 colours in 3-pixel horizontal runs (96x24 = 2304 pixels, 768 blocks):
    #    3-pixel runs defeat CM and stay below method 5's 4-pixel copy threshold, so method 4 wins
    #    and inspect reports palette_colours: 256.
    w256, h256 = 96, 24
    pal256 = [bytes(((i * 17) & 255, (i * 43 + 9) & 255, (i * 97 + 3) & 255)) for i in range(256)]
    rng256 = random.Random(256)
    order256 = list(range(256))
    rng256.shuffle(order256)
    blocks256 = [order256[b] if b < 256 else rng256.randrange(256) for b in range((w256 // 3) * h256)]
    rgb256 = b"".join(pal256[blocks256[y * (w256 // 3) + (x // 3)]] for y in range(h256) for x in range(w256))
    roundtrip("pal_256", w256, h256, rgb256)
    fold256 = WORK / "pal_256" / "out.fold"
    check(fold256.read_bytes()[16] == rf.METHOD_PAL3, "256-colour run image did not select METHOD_PAL3")
    insp256 = expect_ok(["inspect", str(fold256)]).stdout.decode("utf-8", errors="replace")
    check("method: palette" in insp256 and "palette_colours: 256" in insp256, f"256-colour inspect output: {insp256}")

    # 3. 257th colour fallback: replace the last pixel with a 257th unique colour -> falls back to METHOD_CM (1).
    used256 = set(pal256)
    extra_col = next(bytes((r, g, 255)) for r in range(256) for g in range(256) if bytes((r, g, 255)) not in used256)
    rgb257 = rgb256[:-3] + extra_col
    roundtrip("pal_257_fallback", w256, h256, rgb257)
    fold257 = WORK / "pal_257_fallback" / "out.fold"
    check(fold257.read_bytes()[16] == rf.METHOD_CM, "257-colour image did not fall back to METHOD_CM")
    insp257 = expect_ok(["inspect", str(fold257)]).stdout.decode("utf-8", errors="replace")
    check("method: cm" in insp257 and "palette_colours" not in insp257, f"257-colour inspect output: {insp257}")

    # 4. Long counter runs (both >= 400 zero bits exercising max(1, c >> 4) and >= 500 one bits where
    #    method 2 stalls at c=65474 while method 3 reaches c=65535): verify method 3 roundtrip AND
    #    legacy method 2 decode through CLI (BMP) and oracle (RGB).
    w_clamp, h_clamp = 512, 1
    c0, c1, c2 = b"\x12\x34\x56", b"\xFE\xDC\xBA", b"\x44\x88\xCC"
    c4_clamp = (c0, c1, c2, b"\x99\x11\x77")
    rng_clamp = random.Random(1098)
    seq_clamp = [0, 1]
    for _ in range(2, 450):
        choices = [i for i in range(4) if i != seq_clamp[-1] and i != seq_clamp[-2]]
        seq_clamp.append(choices[rng_clamp.randrange(len(choices))])
    seq_clamp.extend([1] * (w_clamp - 450))
    rgb_clamp = b"".join(c4_clamp[i] for i in seq_clamp)
    roundtrip("pal_clamp", w_clamp, h_clamp, rgb_clamp)
    fold_clamp = WORK / "pal_clamp" / "out.fold"
    check(fold_clamp.read_bytes()[16] == rf.METHOD_PAL3, "pal_clamp did not select METHOD_PAL3")

    w_ones, h_ones = 600, 1
    rgb_ones = b"".join(c0 if x < 520 else (c0 if (x & 1) else (c1 if (x & 2) else c2)) for x in range(w_ones))
    roundtrip("pal_ones_run", w_ones, h_ones, rgb_ones)
    for tag, lw, lh, lrgb in (("m2_zero_run", w_clamp, h_clamp, rgb_clamp), ("m2_one_run", w_ones, h_ones, rgb_ones)):
        pay_m2 = rf.pal_encode(lw, lh, lrgb, method=rf.METHOD_PAL)
        pay_m3 = rf.pal_encode(lw, lh, lrgb, method=rf.METHOD_PAL2)
        assert pay_m2 is not None and pay_m3 is not None
        check(pay_m2 != pay_m3, f"{tag}: method 2 and method 3 payloads did not diverge on long run")
        blob_m2 = rf.build_file(lw, lh, rf.METHOD_PAL, pay_m2)
        check(rf.decode_image(blob_m2) == (lw, lh, lrgb), f"{tag}: oracle failed to decode legacy method 2 file")
        m2_fold = work / f"{tag}.fold"
        m2_bmp = work / f"{tag}.bmp"
        m2_bmp.unlink(missing_ok=True)
        m2_fold.write_bytes(blob_m2)
        expect_ok(["decode", str(m2_fold), str(m2_bmp)])
        check(rf.read_bmp24(m2_bmp.read_bytes()) == (lw, lh, lrgb), f"{tag}: CLI failed to decode legacy method 2 file")
        # Method 3 is decode-only since method 4: its long-run counter arithmetic still decodes.
        blob_m3 = rf.build_file(lw, lh, rf.METHOD_PAL2, pay_m3)
        check(rf.decode_image(blob_m3) == (lw, lh, lrgb), f"{tag}: oracle failed to decode legacy method 3 file")
        m3_fold = work / f"{tag}_m3.fold"
        m3_bmp = work / f"{tag}_m3.bmp"
        m3_bmp.unlink(missing_ok=True)
        m3_fold.write_bytes(blob_m3)
        expect_ok(["decode", str(m3_fold), str(m3_bmp)])
        check(rf.read_bmp24(m3_bmp.read_bytes()) == (lw, lh, lrgb), f"{tag}: CLI failed to decode legacy method 3 file")

    # 5. Per-channel CM stream overflow (channel 1 is 2129 > w*h+64 = 2112 bytes while total CM payload < raw):
    #    both C (1 and 3 threads) and oracle discard the overflowed CM candidate and select METHOD_PAL3 byte-identically.
    rng_ovf = random.Random(1)
    rgb_ch_ovf = b"".join(bytes((rng_ovf.randrange(256), 0, 0)) for _ in range(64 * 32))
    roundtrip("pal_cm_ch_overflow", 64, 32, rgb_ch_ovf)
    fold_ovf = WORK / "pal_cm_ch_overflow" / "out.fold"
    check(fold_ovf.read_bytes()[16] == rf.METHOD_PAL3, "pal_cm_ch_overflow did not select METHOD_PAL3")

    # 6. Boundary fallbacks (ww=a for x<=1; nn=a at y=0, nn=b at y=1, nn=I(x,y-2) at y>=2) and 3-row nn wrap (h=6 >= 4)
    #    for widths 1 and 2 with 2 and 4 colours.
    cols4 = (_PAL4[0], _PAL4[1], _PAL4[2], _PAL4[3])
    for bw in (1, 2):
        rgb_b2 = b"".join(cols4[(x + y + (y // 2)) & 1] for y in range(16) for x in range(bw))
        roundtrip(f"pal_bound_w{bw}_c2", bw, 16, rgb_b2)
        rgb_b4 = b"".join(cols4[(x * 3 + y + (y // 3)) & 3] for y in range(16) for x in range(bw))
        roundtrip(f"pal_bound_w{bw}_c4", bw, 16, rgb_b4)

    # 7. Positive third-candidate (D3 = 1) coverage on 4-colour images (count > 2):
    #    - e = d (d != a, b and I == d)
    #    - e = c (d in {a, b}, c != a, b, and I == c)
    #    - e = ww (d, c in {a, b}, ww != a, b, and I == ww)
    #    - literal fallback (d, c, ww all in {a, b} -> e = a, D3 skipped, coded via 8-bit tree)
    grid_rows = [
        [0, 1, 2, 3],
        [0, 2, 1, 3],   # (1,1): a=0,b=1,c=0,d=2,I=2 -> d!=a,b so e=d=2, I==e (D3=1 via e=d)
        [2, 0, 0, 3],   # sets up row 2 for (1,3): I(0,2)=2, I(1,2)=0, I(2,2)=0
        [0, 2, 0, 1],   # (1,3): a=I(0,3)=0,b=I(1,2)=0,c=I(0,2)=2,d=I(2,2)=0 -> d in {a,b}, c!=a,b so e=c=2, I==e (D3=1 via e=c)
        [1, 0, 0, 1],
        [2, 0, 2, 3],
        [0, 0, 0, 0],
        [2, 0, 2, 3],   # (2,7): a=0,b=0,c=0,d=0,ww=2 -> d,c in {a,b}, ww!=a,b so e=ww=2, I==e (D3=1 via e=ww); (3,7): a=2,b=0,c=0,d=0,ww=0 -> e=a=2, I=3 (literal fallback)
    ]
    rng_d3 = random.Random(1163)
    rgb_d3_all = b"".join(
        cols4[idx]
        for row in grid_rows
        for idx in (row + [rng_d3.randrange(4) for _ in range(12)])
    )
    roundtrip("pal_d3_candidates", 16, 8, rgb_d3_all)
    check((WORK / "pal_d3_candidates" / "out.fold").read_bytes()[16] == rf.METHOD_PAL3, "pal_d3_candidates did not select METHOD_PAL3")

    # 8. Historical decoder compatibility: both the pre-§2 decoder (4635a04f1042f3983d8f56222e1521745a191a6d)
    #    and the shipped §2 decoder (1cd84f3edd687e5fa622733ce9e68884d0cadab0) refuse KAT_PAL2 (method == 3)
    #    at the header with exit 3 (unsupported fold: method) and leave no output file, while shipped §2
    #    decodes KAT_PAL (method == 2).
    clang = ROOT / ("tools/llvm-mingw/bin/clang.exe" if os.name == "nt" else "tools/llvm-mingw-linux/bin/clang")
    check(clang.is_file(), f"pinned clang compiler missing for historical decoder compatibility test: {clang}")
    with tempfile.TemporaryDirectory(prefix="rf_hist_pal_") as htmp:
        ht = Path(htmp)
        kat_pal_path = ht / "kat_pal.fold"
        kat_pal2_path = ht / "kat_pal2.fold"
        kat_pal_path.write_bytes(KAT_PAL)
        kat_pal2_path.write_bytes(KAT_PAL2)
        for tag, ref in (
            ("pre_s2", "4635a04f1042f3983d8f56222e1521745a191a6d"),
            ("shipped_s2", "1cd84f3edd687e5fa622733ce9e68884d0cadab0"),
        ):
            r_cat = subprocess.run(["git", "cat-file", "-e", f"{ref}^{{commit}}"], cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True)
            check(r_cat.returncode == 0, f"historical commit {ref} ({tag}) missing from checkout; fetch full git history")
            d = ht / tag
            (d / "src").mkdir(parents=True)
            (d / "include").mkdir(parents=True)
            (d / "include" / "rasterfold.h").write_bytes(subprocess.check_output(["git", "show", f"{ref}:include/rasterfold.h"], cwd=ROOT, stdin=subprocess.DEVNULL))
            c_files = []
            for s in subprocess.check_output(["git", "ls-tree", "--name-only", f"{ref}:src"], cwd=ROOT, stdin=subprocess.DEVNULL, text=True).splitlines():
                if os.name == "nt" and s == "sys_posix.c":
                    continue
                if os.name != "nt" and s in ("sys.c", "mem.c"):
                    continue
                p = d / "src" / s
                p.write_bytes(subprocess.check_output(["git", "show", f"{ref}:src/{s}"], cwd=ROOT, stdin=subprocess.DEVNULL))
                if s.endswith(".c"):
                    c_files.append(str(p))
            hist_exe = d / ("rasterfold.exe" if os.name == "nt" else "rasterfold")
            if os.name == "nt":
                cmd = [
                    str(clang), "-std=c11", "-O1", "-march=x86-64-v2",
                    "-ffreestanding", "-fno-builtin", "-nostdlib",
                    "-I", str(d / "include"), *c_files,
                    "-Wl,-e,rf_start", "-Wl,--subsystem,console",
                    "-lkernel32", "-lshell32", "-o", str(hist_exe),
                ]
            else:
                cmd = [str(clang), "-std=c11", "-O1", "-march=x86-64-v2", "-I", str(d / "include"), *c_files, "-lpthread", "-o", str(hist_exe)]
            subprocess.check_call(cmd, stdin=subprocess.DEVNULL)
            hist_bmp = d / "out.bmp"
            r_refuse = subprocess.run([str(hist_exe), "decode", str(kat_pal2_path), str(hist_bmp)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
            check(r_refuse.returncode == 3, f"{tag} ({ref}) on KAT_PAL2 exit {r_refuse.returncode}, want 3: {r_refuse.stderr}")
            check("unsupported fold: method" in r_refuse.stderr, f"{tag} ({ref}) stderr missing 'unsupported fold: method': {r_refuse.stderr}")
            check(not hist_bmp.exists(), f"{tag} ({ref}) left output BMP on refused KAT_PAL2")
            if tag == "shipped_s2":
                r_ok = subprocess.run([str(hist_exe), "decode", str(kat_pal_path), str(hist_bmp)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
                check(r_ok.returncode == 0, f"{tag} ({ref}) failed on KAT_PAL: {r_ok.stderr}")
                check(rf.read_bmp24(hist_bmp.read_bytes()) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), f"{tag} ({ref}) decoded pixels mismatch on KAT_PAL")


def _pal4_bits(bits: str) -> bytes:
    """A bit string (spaces ignored), zero-padded to whole bytes."""
    bits = bits.replace(" ", "")
    bits += "0" * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, "big") if bits else b""


def _pal4_coverage(width: int, height: int, rgb: bytes) -> set[tuple[str, int]]:
    """Which method 4 decisions an image reaches in which border class, from the spec's decision order."""
    palette = sorted({rgb[i : i + 3] for i in range(0, len(rgb), 3)})
    index_of = {c: i for i, c in enumerate(palette)}
    rows = [[index_of[rgb[(y * width + x) * 3 : (y * width + x) * 3 + 3]] for x in range(width)] for y in range(height)]
    seen: set[tuple[str, int]] = set()
    for y in range(height):
        for x in range(width):
            a, b, e, _pattern = rf._pal_neighbours_m3(rows, width, x, y)
            cls = 1 if y == 0 else (2 if x == 0 else 0)
            idx = rows[y][x]
            seen.add(("d1", cls))
            if idx == a or len(palette) == 2:
                continue
            if b != a:
                seen.add(("d2", cls))
                if idx == b:
                    continue
            if e != a and e != b:
                seen.add(("d3", cls))
                if idx == e:
                    continue
            seen.add(("lit", cls))
    return seen


def palette_method4() -> None:
    """Method 4 (compact palette): selection, C and oracle byte identity on the border classes and
    count extremes, decoder fixtures with forced Rice parameters, both decode paths, and every refusal."""
    work = WORK / "palette_method4"
    work.mkdir(parents=True, exist_ok=True)
    n = 0

    def put(blob: bytes) -> Path:
        nonlocal n
        n += 1
        path = work / f"f{n}.fold"
        path.write_bytes(blob)
        return path

    def decodes(blob: bytes, width: int, height: int, rgb: bytes, what: str) -> None:
        path = put(blob)
        bmp, ppm = work / f"f{n}.bmp", work / f"f{n}.ppm"
        for f in (bmp, ppm):
            f.unlink(missing_ok=True)
        check(rf.decode_image(blob) == (width, height, rgb), f"{what}: oracle decode")
        expect_ok(["decode", str(path), str(bmp)])
        check(rf.read_bmp24(bmp.read_bytes()) == (width, height, rgb), f"{what}: strided BMP decode")
        expect_ok(["decode", "--threads", "3", str(path), str(ppm)])
        with Image.open(ppm) as im:
            check(im.size == (width, height) and im.convert("RGB").tobytes() == rgb, f"{what}: packed RGB decode")

    def reject(blob: bytes, code: int, text: str, what: str) -> None:
        path = put(blob)
        for ext in ("bmp", "ppm"):
            dest = work / f"f{n}.{ext}"
            dest.unlink(missing_ok=True)
            expect_fail(["decode", str(path), str(dest)], code, text)
            check(not dest.exists(), f"{what}: an output was left behind")
        try:
            rf.decode_image(blob)
        except rf.FormatError:
            pass
        else:
            check(False, f"{what}: oracle accepted the file")

    def selected(name: str, width: int, height: int, rgb: bytes) -> bytes:
        roundtrip(name, width, height, rgb)
        blob = (WORK / name / "out.fold").read_bytes()
        check(blob[16] == rf.METHOD_PAL3, f"{name} did not select METHOD_PAL3 (method {blob[16]})")
        decodes(blob, width, height, rgb, name)
        return blob

    cols = [bytes(((i * 53 + 7) & 255, (i * 101 + 19) & 255, (i * 29 + 3) & 255)) for i in range(256)]
    check(len(set(cols)) == 256, "fixture colours are not distinct")

    # 1. One colour, two colours, one pixel wide, one pixel high.
    one = selected("pal4_one_colour", 64, 48, solid(64, 48, (240, 240, 240)))
    check(one[32] == 0, "one-colour file does not store count - 1 = 0")
    selected("pal4_two_colours", 64, 48, checker(64, 48, 4))
    rng1w = random.Random(41)
    blocks1w = [rng1w.randrange(16) for _ in range(67)]
    selected("pal4_one_wide", 1, 200, b"".join(cols[blocks1w[y // 3]] for y in range(200)))
    rng1h = random.Random(42)
    blocks1h = [rng1h.randrange(16) for _ in range(100)]
    selected("pal4_one_high", 300, 1, b"".join(cols[blocks1h[x // 3]] for x in range(300)))

    # 2. 256 colours with palette index 255 in use (3-pixel runs stay below method 5's 4-pixel copy threshold).
    rng = random.Random(4)
    order = list(range(256))
    rng.shuffle(order)
    rng4_blocks = random.Random(44)
    blocks4 = [order[b] if b < 256 else rng4_blocks.randrange(256) for b in range((96 // 3) * 24)]
    rgb256 = b"".join(cols[blocks4[y * 32 + x // 3]] for y in range(24) for x in range(96))
    blob256 = selected("pal4_256", 96, 24, rgb256)
    check(blob256[32] == 255, "256-colour file does not store count - 1 = 255")
    check(sorted(set(cols))[255] in {rgb256[i : i + 3] for i in range(0, len(rgb256), 3)}, "palette index 255 unused")

    # 3. Every reachable decision in every border class: D1 everywhere, D3 and the literal tree on
    #    row 0 and in column 0, D2 (unreachable on the borders, where a == b) in the interior.
    rgb_cov = b"".join(cols[rng.randrange(9)] for _ in range(40 * 30))
    want = {("d1", 0), ("d1", 1), ("d1", 2), ("d2", 0), ("d3", 0), ("d3", 1), ("d3", 2), ("lit", 0), ("lit", 1), ("lit", 2)}
    got = _pal4_coverage(40, 30, rgb_cov)
    check(want <= got, f"coverage image misses {sorted(want - got)}")
    check(("d2", 1) not in got and ("d2", 2) not in got, "D2 reached on a border")
    selected("pal4_coverage", 40, 30, rgb_cov)

    # 4. Decoder fixtures with explicitly supplied legal Rice parameters, including each k at 0 and at 8
    #    (the encoder never chooses kR = 8). Any legal parameters decode to the same pixels.
    w4, h4 = 24, 10
    rgb4 = b"".join(cols[(x // 3 + y // 2 + ((x + y) & 1)) % 5] for y in range(h4) for x in range(w4))
    ref_payload = rf.pal4_encode(w4, h4, rgb4)
    assert ref_payload is not None
    for ks in ((0, 0, 0), (8, 8, 8), (0, 8, 3), (8, 0, 5), (3, 4, 0), (2, 6, 8)):
        forced = rf.pal4_encode(w4, h4, rgb4, ks=ks)
        assert forced is not None
        check(forced[1] >> 4 == ks[0] and forced[1] & 15 == ks[1] and forced[2] >> 4 == ks[2], f"forced k {ks} not in the field")
        decodes(rf.build_file(w4, h4, rf.METHOD_PAL3, forced), w4, h4, rgb4, f"forced k {ks}")
    check(len(ref_payload) <= min(len(rf.pal4_encode(w4, h4, rgb4, ks=ks) or b"") for ks in ((0, 0, 0), (8, 8, 8))), "reference k is not the cheapest")

    # 5. Refusals, each on a file with valid container checksums.
    bad_field = "invalid fold: palette"
    bad_stream = "invalid fold: palette stream"
    stream5 = b"\x00" * 5
    pal3 = [(16, 32, 48), (64, 80, 96), (112, 128, 144)]
    field3 = rf.pal4_field(pal3)

    def m4(payload: bytes, width: int = 3, height: int = 1) -> bytes:
        return rf.build_file(width, height, rf.METHOD_PAL3, payload)

    reject(m4(b"\x00" + _pal4_bits("0000 0000 0000 0 0 0") + b"\x00" * 4), 4, bad_field, "payload shorter than 8 bytes")
    reject(m4(b"\x02" + _pal4_bits("1001 0000 0000") + b"\x00" * 8), 4, bad_field, "kR above 8")
    reject(m4(b"\x02" + _pal4_bits("0000 1001 0000") + b"\x00" * 8), 4, bad_field, "kG above 8")
    reject(m4(b"\x02" + _pal4_bits("0000 0000 1111") + b"\x00" * 8), 4, bad_field, "kB above 8")
    reject(m4(b"\xff" + field3 + stream5), 4, bad_field, "palette field runs past the payload")
    reject(m4(b"\x02" + _pal4_bits("0000 0000 0000" + "1" * 600) + stream5), 4, bad_field, "Rice quotient above 511")
    reject(m4(b"\x00" + _pal4_bits("0000 0000 0000" + "1" * 256 + "0" + "0" + "0") + stream5, 1, 1), 4, bad_field, "R above 255")
    # zigzag(-1) = 1 under k = 0 is "10": G = R - 1 = -1 at R = 0.
    reject(m4(b"\x00" + _pal4_bits("0000 0000 0000 0 10 0") + stream5, 1, 1), 4, bad_field, "G below 0")
    # zigzag(+1) = 2 under k = 0 is "110": B = G + 1 = 256 at R = G = 255.
    reject(m4(b"\x00" + _pal4_bits("1000 0000 0000 0 11111111 0 110") + stream5, 1, 1), 4, bad_field, "B above 255")
    reject(m4(b"\x01" + rf.pal4_field([(16, 32, 48), (16, 32, 48)]) + stream5, 2, 1), 4, bad_field, "duplicate palette entry")
    reject(m4(b"\x01" + rf.pal4_field([(16, 32, 48), (16, 32, 40)]) + stream5, 2, 1), 4, bad_field, "palette entry below the one before it")
    # One colour (0, 0, 0) with every k = 0 is 15 bits, so the 16th bit is padding.
    decodes(m4(b"\x00" + _pal4_bits("0000 0000 0000 0 0 0 0") + rf.pal4_encode(1, 1, b"\x00\x00\x00")[3:], 1, 1), 1, 1, b"\x00\x00\x00", "zero padding bit")
    reject(m4(b"\x00" + _pal4_bits("0000 0000 0000 0 0 0 1") + rf.pal4_encode(1, 1, b"\x00\x00\x00")[3:], 1, 1), 4, bad_field, "non-zero padding bit")
    reject(m4(b"\x02" + field3 + b"\x00" * 4), 4, bad_field, "fewer than 5 stream bytes")

    def bad_literal(literal: int) -> bytes:
        # 3x1, 3 colours; at (0, 0) every neighbour is index 0, so pattern = 127, hit = 0, class = 1,
        # D2 and D3 are skipped, and the literal tree follows a 0 on D1.
        d1 = [rf._Pal4Counter() for _ in range(768)]
        tree = [rf._Pal4Counter() for _ in range(256)]
        enc = rf.RangeEncoder()
        d1[127 * 2 + 0 + 256].code(enc, None, 0)
        node = 1
        for bit_pos in range(7, -1, -1):
            bit = (literal >> bit_pos) & 1
            tree[node].code(enc, None, bit)
            node = node * 2 + bit
        return m4(b"\x02" + field3 + enc.finish())

    reject(bad_literal(3), 4, bad_stream, "literal index >= count")
    reject(bad_literal(0), 4, bad_stream, "literal index equal to an excluded candidate")
    good3 = rf.pal4_encode(3, 1, bytes(pal3[0]) + bytes(pal3[1]) + bytes(pal3[2]))
    assert good3 is not None
    decodes(m4(good3), 3, 1, bytes(pal3[0]) + bytes(pal3[1]) + bytes(pal3[2]), "3x1 reference file")
    reject(m4(good3 + b"\x00"), 4, bad_stream, "one byte left after the index stream")
    big = rf.pal4_encode(w4, h4, rgb4)
    assert big is not None and len(big) > 1 + 12 + 8
    reject(m4(big[:-3], w4, h4), 4, bad_stream, "truncated index stream")

    # Method 4 is not a version 2 method: the same payload under a version 2 header is unsupported.
    v2 = bytearray(m4(good3))
    v2[3] = 2
    struct.pack_into("<H", v2, 4, 2)
    reject(recrc(v2), 3, "unsupported fold: method", "method 4 in a version 2 file")
    reject(rf.build_file(3, 1, 7, good3), 3, "unsupported fold: method", "method 7")


def screen_content_method5() -> None:
    """Method 5 (screen content): selection, prototype/oracle/C byte identity on one-colour, screenshot
    crop, overlapping cross-row copies (L = 4096), narrow-width duplicate 2D-map distances, window boundary,
    FNV-1a bucket collisions at <= 64 and > 64 chain depths, inspect output, historical refusal, and every
    grammar and framing refusal across strided BMP, packed RGB, and the oracle."""
    if str(ROOT / "research") not in sys.path:
        sys.path.insert(0, str(ROOT / "research"))
    import screencode

    work = WORK / "screen_content_method5"
    work.mkdir(parents=True, exist_ok=True)
    n = 0

    def put(blob: bytes) -> Path:
        nonlocal n
        n += 1
        path = work / f"f{n}.fold"
        path.write_bytes(blob)
        return path

    def decodes(blob: bytes, width: int, height: int, rgb: bytes, what: str) -> None:
        path = put(blob)
        bmp, ppm = work / f"f{n}.bmp", work / f"f{n}.ppm"
        for f in (bmp, ppm):
            f.unlink(missing_ok=True)
        check(rf.decode_image(blob) == (width, height, rgb), f"{what}: oracle decode")
        expect_ok(["decode", str(path), str(bmp)])
        check(rf.read_bmp24(bmp.read_bytes()) == (width, height, rgb), f"{what}: strided BMP decode")
        expect_ok(["decode", "--threads", "3", str(path), str(ppm)])
        with Image.open(ppm) as im:
            check(im.size == (width, height) and im.convert("RGB").tobytes() == rgb, f"{what}: packed RGB decode")

    def reject(blob: bytes, code: int, text: str, what: str) -> None:
        path = put(blob)
        for ext in ("bmp", "ppm"):
            dest = work / f"f{n}.{ext}"
            dest.unlink(missing_ok=True)
            expect_fail(["decode", str(path), str(dest)], code, text)
            check(not dest.exists(), f"{what} ({ext}): an output was left behind")
        try:
            rf.decode_image(blob)
        except rf.FormatError:
            pass
        else:
            check(False, f"{what}: oracle accepted the file")

    def selected_sc(name: str, width: int, height: int, rgb: bytes) -> bytes:
        roundtrip(name, width, height, rgb)
        pay_m5 = rf.sc_encode(width, height, rgb)
        check(pay_m5 is not None, f"{name}: rf.sc_encode returned None")
        assert pay_m5 is not None
        check(pay_m5 == screencode.encode(width, height, rgb), f"{name}: oracle method 5 payload differs from screencode.py")
        blob = rf.build_file(width, height, rf.METHOD_SC, pay_m5)
        decodes(blob, width, height, rgb, f"{name} (legacy method 5)")
        return blob

    # 1. One-colour image (natural roundtrip + forced method 5 byte identity with prototype)
    #    and a >256-colour solid-body image that selects method 5 naturally.
    rgb_one = solid(64, 48, (32, 96, 192))
    roundtrip("sc5_one_colour", 64, 48, rgb_one)
    pay_one = rf.sc_encode(64, 48, rgb_one, force=True)
    assert pay_one is not None
    check(pay_one == screencode.encode(64, 48, rgb_one), "one-colour forced method 5 differs from prototype")
    decodes(rf.build_file(64, 48, rf.METHOD_SC, pay_one), 64, 48, rgb_one, "forced method 5 one-colour")

    rng_sb = random.Random(505)
    sb_cols: list[bytes] = []
    sb_seen: set[bytes] = set()
    while len(sb_cols) < 260:
        col_sb = bytes((rng_sb.randrange(256), rng_sb.randrange(256), rng_sb.randrange(256)))
        if col_sb not in sb_seen:
            sb_seen.add(col_sb)
            sb_cols.append(col_sb)
    hdr260 = b"".join(sb_cols)
    rgb_solid_body = hdr260 + bytes([40, 120, 200]) * (64 * 48 - 260)
    selected_sc("sc5_solid_body", 64, 48, rgb_solid_body)

    # 2. Screenshot crop from development set dv_s01.png (>256 colours, glyph edges, cache hits, 2D and raw copies).
    with Image.open(ROOT / "research" / "screens-assets" / "dv_s01.png") as im:
        crop_im = im.convert("RGB").crop((80, 40, 336, 200))
        crop_w, crop_h = crop_im.size
        crop_rgb = crop_im.tobytes()
    check(len({crop_rgb[i : i + 3] for i in range(0, len(crop_rgb), 3)}) > 256, "dv_s01 crop has <= 256 colours")
    crop_blob = selected_sc("sc5_screen_crop", crop_w, crop_h, crop_rgb)

    # 3. Cross-row overlapping copy reaching L = 4096, plus narrow width w = 6 (< 9) duplicate 2D-map distances.
    motif5 = b"\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff"
    need_px = 64 * 80 - 260
    rgb_ov = hdr260 + (motif5 * ((need_px // 5) + 1))[: need_px * 3]
    selected_sc("sc5_overlap", 64, 80, rgb_ov)

    nar_tile = [bytes(((i * 61 + 9) & 255, (i * 113 + 21) & 255, (i * 179 + 43) & 255)) for i in range(18)]
    rgb_nar = b"".join(nar_tile[(y % 3) * 6 + x] for y in range(60) for x in range(6))
    selected_sc("sc5_narrow", 6, 60, rgb_nar)

    # Exact distance-window boundary D = 2^20 - 120 = 1048456 (prefix symbol 39, matched) vs D = 1048457 (outside window)
    # on 1024x1026 (>256 colours in tail after target_p + 17 so palette is skipped without overwriting key_win; verified
    # at the token level and compared byte-for-byte across C, rfi_ref.sc_encode, and screencode.encode).
    win_w, win_h = 1024, 1026
    max_d = (1 << 20) - 120
    key_win = bytes(((i * 53 + 17) & 255 for i in range(48)))
    win_payloads: dict[str, bytes] = {}
    for tag_w, target_p in (("win_in", max_d), ("win_out", max_d + 1)):
        check(target_p + 17 <= win_w * win_h - 260, f"{tag_w}: tail overwrites target_p")
        buf_w = bytearray(win_w * win_h * 3)
        buf_w[0:48] = key_win
        buf_w[48:51] = b"\x01\x02\x03"
        buf_w[target_p * 3 : target_p * 3 + 48] = key_win
        buf_w[target_p * 3 + 48 : target_p * 3 + 51] = b"\x04\x05\x06"
        buf_w[-260 * 3 :] = hdr260
        rgb_w = bytes(buf_w)
        check(rgb_w[0:48] == key_win and rgb_w[target_p * 3 : target_p * 3 + 48] == key_win, f"{tag_w}: key lost")
        win_toks = list(screencode.tokens(win_w, win_h, rgb_w))
        if tag_w == "win_in":
            check((screencode.COPY, 16, max_d) in win_toks, "win_in did not emit (COPY, 16, 2^20 - 120)")
        else:
            check(
                all(t[0] != screencode.COPY or t[2] < max_d for t in win_toks)
                and (screencode.COPY, 16, max_d) not in win_toks,
                "win_out emitted a copy across the window boundary",
            )
        exp_sc = rf.sc_encode(win_w, win_h, rgb_w, force=True)
        assert exp_sc is not None
        check(exp_sc == screencode.encode(win_w, win_h, rgb_w), f"{tag_w}: oracle != prototype at window boundary")
        w_blob = rf.build_file(win_w, win_h, rf.METHOD_SC, exp_sc)
        decodes(w_blob, win_w, win_h, rgb_w, tag_w)
        win_payloads[tag_w] = exp_sc
    check(
        win_payloads["win_in"] != win_payloads["win_out"]
        and len(win_payloads["win_in"]) < len(win_payloads["win_out"]),
        "window boundary payloads did not reflect inside vs outside match decision",
    )

    # 4. FNV-1a 16-bit bucket collision fixture (66 distinct 4-pixel keys all hashing to top-16-bit bucket 10870):
    #    k0 is matched within 64 bucket probes first, then pushed past depth 64 by two more colliding keys so
    #    its next appearance misses the chain cap, with prototype, oracle, and C agreeing byte-for-byte.
    coll_ids = (
        30690, 116199, 151429, 294000, 312115, 320667, 335786, 450346, 458701, 486739, 505041,
        539753, 614524, 681221, 683534, 697086, 770369, 799259, 825594, 881833, 890101, 914855,
        942492, 999009, 1058686, 1082868, 1111621, 1128294, 1248069, 1288994, 1291159, 1294749,
        1308670, 1352422, 1443426, 1498285, 1517471, 1538638, 1542801, 1572573, 1627532, 1664189,
        1695529, 1754739, 1786036, 1808502, 1846772, 1850455, 1857946, 1899386, 1908082, 1992533,
        2021952, 2131509, 2328364, 2338916, 2346937, 2358858, 2359529, 2365992, 2421597, 2471403,
        2544142, 2573268, 2601473, 2611358,
    )

    def coll_key12(i: int) -> bytes:
        return bytes((
            i & 255, (i >> 8) & 255, (i >> 16) & 255, (i * 3 + 1) & 255,
            (i * 7 + 5) & 255, 89, (i * 11 + 3) & 255, (i * 13 + 9) & 255,
            163, (i * 17 + 7) & 255, (i * 19 + 11) & 255, 231,
        ))

    keys12 = [coll_key12(i) for i in coll_ids]
    check(len(set(keys12)) == 66, "bucket-collision keys are not distinct")
    check({rf._sc_fnv1a_4px(k, 0) for k in keys12} == {10870}, "bucket-collision keys do not share bucket 10870")
    # Build prefix sequentially until bucket 10870 holds >= 63 entries after k0, test k0 (hit within 64),
    # then add more colliding keys so bucket 10870 has > 64 entries after k0's second copy, and test k0 again (miss).
    seq_keys = [keys12[0]] + keys12[1:58] + [keys12[0]] + keys12[1:66] + [keys12[0]] + [keys12[65]] * 180
    # Pad to 64x20 = 1280 pixels (320 4-pixel blocks).
    while len(seq_keys) < 320:
        seq_keys.append(keys12[65])
    rgb_coll = b"".join(seq_keys[:320])
    selected_sc("sc5_bucket_collision", 64, 20, rgb_coll)

    # 5. inspect text and --json on legacy method 5 crop: method is sc, no palette_colours and no stream sizes.
    crop_path = work / "sc5_screen_crop_m5.fold"
    crop_path.write_bytes(crop_blob)
    insp_txt = expect_ok(["inspect", str(crop_path)]).stdout.decode("utf-8", errors="replace")
    check("method: sc" in insp_txt and "palette_colours" not in insp_txt and "stream_" not in insp_txt, f"inspect text: {insp_txt}")
    insp_js = json.loads(expect_ok(["inspect", "--json", str(crop_path)]).stdout.decode("utf-8", errors="replace"))
    check(
        insp_js.get("method") == "sc" and "palette_colours" not in insp_js and "streams" not in insp_js,
        f"inspect --json: {insp_js}",
    )

    # 6. Refusals on valid-checksum .fold files across strided BMP, packed RGB, and the oracle.
    bad_sc = "invalid fold: screen content stream"

    def m5(payload: bytes, width: int = 8, height: int = 1) -> bytes:
        return rf.build_file(width, height, rf.METHOD_SC, payload)

    class _ScBitCraft:
        def __init__(self) -> None:
            self.enc = rf.RangeEncoder()
            self.c_copy = [rf._Pal4Counter() for _ in range(3)]
            self.c_cache = [rf._Pal4Counter() for _ in range(3)]
            self.t_g = [rf._Pal4Counter() for _ in range(256)]
            self.t_rg = [[rf._Pal4Counter() for _ in range(256)] for _ in range(4)]
            self.t_bg = [[rf._Pal4Counter() for _ in range(256)] for _ in range(4)]
            self.t_idx = [rf._Pal4Counter() for _ in range(256)]
            self.t_len = [rf._Pal4Counter() for _ in range(32)]
            self.c_len_ex = [rf._Pal4Counter() for _ in range(24)]
            self.t_dist = [[rf._Pal4Counter() for _ in range(64)] for _ in range(2)]
            self.c_dist_ex = [rf._Pal4Counter() for _ in range(40)]
            self.prev_kind = 0
            self.prev_short = 0

        def _tree(self, tree: list, val: int, bits: int) -> None:
            node = 1
            for bp in range(bits - 1, -1, -1):
                bit = (val >> bp) & 1
                tree[node].code(self.enc, None, bit)
                node = (node << 1) | bit

        def literal(self, r: int, g: int, b: int) -> None:
            self.c_copy[self.prev_kind].code(self.enc, None, 0)
            self.c_cache[self.prev_kind].code(self.enc, None, 0)
            self._tree(self.t_g, g, 8)
            gctx = g >> 6
            self._tree(self.t_rg[gctx], (r - g) & 255, 8)
            self._tree(self.t_bg[gctx], (b - g) & 255, 8)
            self.prev_kind = 0

        def cache(self, idx: int) -> None:
            self.c_copy[self.prev_kind].code(self.enc, None, 0)
            self.c_cache[self.prev_kind].code(self.enc, None, 1)
            self._tree(self.t_idx, idx, 8)
            self.prev_kind = 1

        def copy_raw(self, len_sym: int, len_extra: int, len_nbits: int, dist_sym: int | None = None, dist_extra: int = 0, dist_nbits: int = 0) -> None:
            self.c_copy[self.prev_kind].code(self.enc, None, 1)
            self._tree(self.t_len, len_sym, 5)
            if len_nbits > 0 and len_sym <= 23:
                self.c_len_ex[len_sym].code(self.enc, None, (len_extra >> (len_nbits - 1)) & 1)
                for bp in range(len_nbits - 2, -1, -1):
                    self.enc.bit16((len_extra >> bp) & 1, 32768)
            if dist_sym is not None:
                self._tree(self.t_dist[self.prev_short], dist_sym, 6)
                if dist_nbits > 0 and dist_sym <= 39:
                    self.c_dist_ex[dist_sym].code(self.enc, None, (dist_extra >> (dist_nbits - 1)) & 1)
                    for bp in range(dist_nbits - 2, -1, -1):
                        self.enc.bit16((dist_extra >> bp) & 1, 32768)
            self.prev_kind = 2

        def finish(self) -> bytes:
            return self.enc.finish()

    # Framing: payload shorter than 5 bytes (0 and 4 bytes).
    reject(m5(b""), 4, bad_sc, "empty method 5 payload")
    reject(m5(b"\x00" * 4), 4, bad_sc, "4-byte method 5 payload")

    # len_sym > 23 (24 and 31).
    for bad_lsym in (24, 31):
        sc_b = _ScBitCraft()
        sc_b.literal(10, 20, 30)
        sc_b.copy_raw(bad_lsym, 0, 0)
        reject(m5(sc_b.finish(), 8, 1), 4, bad_sc, f"len_sym {bad_lsym} > 23")

    # L == 4096 (len_sym = 23, extra = 1020 -> L - 3 = 4093) decodes cleanly on 4097x1, whereas L == 4097
    # (extra = 1021 -> L - 3 = 4094) is refused even when p + L <= total (5000x1, p = 1, p + L = 4098 <= 5000).
    sc_ok4096 = _ScBitCraft()
    sc_ok4096.literal(10, 20, 30)
    sc_ok4096.copy_raw(23, 1020, 10, dist_sym=1, dist_extra=0, dist_nbits=0)  # v = 2 -> (1, 0) gives D = 1
    decodes(m5(sc_ok4096.finish(), 4097, 1), 4097, 1, bytes([10, 20, 30]) * 4097, "exact L = 4096 boundary")

    sc_bad4097 = _ScBitCraft()
    sc_bad4097.literal(10, 20, 30)
    sc_bad4097.copy_raw(23, 1021, 10, dist_sym=1, dist_extra=0, dist_nbits=0)
    reject(m5(sc_bad4097.finish(), 5000, 1), 4, bad_sc, "L = 4097 > 4096 with pixels remaining")

    # Copy length past image end (p = 1, L = 4 -> p + L = 5 > 4 on 4x1).
    sc_past = _ScBitCraft()
    sc_past.literal(10, 20, 30)
    sc_past.copy_raw(0, 0, 0, dist_sym=1, dist_extra=0, dist_nbits=0)
    reject(m5(sc_past.finish(), 4, 1), 4, bad_sc, "copy runs past last pixel")

    # dist_sym > 39 (40 and 63).
    for bad_dsym in (40, 63):
        sc_d = _ScBitCraft()
        sc_d.literal(10, 20, 30)
        sc_d.copy_raw(0, 0, 0, dist_sym=bad_dsym)
        reject(m5(sc_d.finish(), 8, 1), 4, bad_sc, f"dist_sym {bad_dsym} > 39")

    # Distance before first pixel via 2D map (v = 1 -> entry (0, 1) gives D = 8 > p = 1 on 8x2).
    sc_dmap = _ScBitCraft()
    sc_dmap.literal(10, 20, 30)
    sc_dmap.copy_raw(0, 0, 0, dist_sym=0, dist_extra=0, dist_nbits=0)
    reject(m5(sc_dmap.finish(), 8, 2), 4, bad_sc, "2D-mapped distance before first pixel")

    # Distance before first pixel via raw distance (v = 122 -> D = 2 > p = 1 on 8x1).
    dsym122, dnb122, dex122 = rf._sc_prefix_encode(122)
    sc_draw = _ScBitCraft()
    sc_draw.literal(10, 20, 30)
    sc_draw.copy_raw(0, 0, 0, dist_sym=dsym122, dist_extra=dex122, dist_nbits=dnb122)
    reject(m5(sc_draw.finish(), 8, 1), 4, bad_sc, "raw distance before first pixel")

    # Cache hit on an entry never set: at p = 0 (all 256 empty) and at p = 1 (slot (slot0 + 1) & 255 empty).
    sc_c0 = _ScBitCraft()
    sc_c0.cache(0)
    reject(m5(sc_c0.finish(), 4, 1), 4, bad_sc, "cache hit at p = 0 on empty cache")

    slot0 = rf._sc_slot(10, 20, 30)
    sc_c1 = _ScBitCraft()
    sc_c1.literal(10, 20, 30)
    sc_c1.cache((slot0 + 1) & 255)
    reject(m5(sc_c1.finish(), 4, 1), 4, bad_sc, "cache hit at p = 1 on unset slot")

    # Truncated stream and trailing byte on KAT_SC5.
    reject(rf.build_file(KAT_SC_W, KAT_SC_H, rf.METHOD_SC, KAT_SC5[32:-3]), 4, bad_sc, "truncated method 5 stream")
    reject(rf.build_file(KAT_SC_W, KAT_SC_H, rf.METHOD_SC, KAT_SC5[32:] + b"\x00"), 4, bad_sc, "trailing byte after method 5 stream")

    # Method 5 in a version 2 header is refused as unsupported method (exit 3), and method 7 in version 3 is refused.
    v2_sc = bytearray(KAT_SC5)
    v2_sc[3] = 2
    struct.pack_into("<H", v2_sc, 4, 2)
    reject(recrc(v2_sc), 3, "unsupported fold: method", "method 5 in a version 2 file")
    reject(rf.build_file(KAT_SC_W, KAT_SC_H, 7, KAT_SC5[32:]), 3, "unsupported fold: method", "method 7 in a version 3 file")

    # 7. Historical decoder compatibility: the pre-port decoder (c974364) refuses KAT_SC5 at the header
    #    with exit 3 (unsupported fold: method) and leaves no output file, while still decoding KAT_PAL4.
    clang = ROOT / ("tools/llvm-mingw/bin/clang.exe" if os.name == "nt" else "tools/llvm-mingw-linux/bin/clang")
    check(clang.is_file(), f"pinned clang compiler missing: {clang}")
    pre_ref = "c974364"
    r_cat = subprocess.run(["git", "cat-file", "-e", f"{pre_ref}^{{commit}}"], cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True)
    check(r_cat.returncode == 0, f"historical commit {pre_ref} missing from checkout")
    with tempfile.TemporaryDirectory(prefix="rf_hist_sc5_") as htmp:
        ht = Path(htmp)
        kat_sc5_path = ht / "kat_sc5.fold"
        kat_pal4_path = ht / "kat_pal4.fold"
        kat_sc5_path.write_bytes(KAT_SC5)
        kat_pal4_path.write_bytes(KAT_PAL4)
        (ht / "src").mkdir(parents=True)
        (ht / "include").mkdir(parents=True)
        (ht / "include" / "rasterfold.h").write_bytes(subprocess.check_output(["git", "show", f"{pre_ref}:include/rasterfold.h"], cwd=ROOT, stdin=subprocess.DEVNULL))
        c_files = []
        for s in subprocess.check_output(["git", "ls-tree", "--name-only", f"{pre_ref}:src"], cwd=ROOT, stdin=subprocess.DEVNULL, text=True).splitlines():
            if os.name == "nt" and s == "sys_posix.c":
                continue
            if os.name != "nt" and s in ("sys.c", "mem.c"):
                continue
            p = ht / "src" / s
            p.write_bytes(subprocess.check_output(["git", "show", f"{pre_ref}:src/{s}"], cwd=ROOT, stdin=subprocess.DEVNULL))
            if s.endswith(".c"):
                c_files.append(str(p))
        hist_exe = ht / ("rasterfold.exe" if os.name == "nt" else "rasterfold")
        if os.name == "nt":
            cmd = [
                str(clang), "-std=c11", "-O1", "-march=x86-64-v2",
                "-ffreestanding", "-fno-builtin", "-nostdlib",
                "-I", str(ht / "include"), *c_files,
                "-Wl,-e,rf_start", "-Wl,--subsystem,console",
                "-lkernel32", "-lshell32", "-o", str(hist_exe),
            ]
        else:
            cmd = [str(clang), "-std=c11", "-O1", "-march=x86-64-v2", "-I", str(ht / "include"), *c_files, "-lpthread", "-o", str(hist_exe)]
        subprocess.check_call(cmd, stdin=subprocess.DEVNULL)
        hist_bmp = ht / "out.bmp"
        r_ref = subprocess.run([str(hist_exe), "decode", str(kat_sc5_path), str(hist_bmp)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        check(r_ref.returncode == 3, f"pre-port ({pre_ref}) on KAT_SC5 exit {r_ref.returncode}, want 3: {r_ref.stderr}")
        check("unsupported fold: method" in r_ref.stderr, f"pre-port stderr missing 'unsupported fold: method': {r_ref.stderr}")
        check(not hist_bmp.exists(), "pre-port decoder left output BMP on refused KAT_SC5")
        r_pal4 = subprocess.run([str(hist_exe), "decode", str(kat_pal4_path), str(hist_bmp)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        check(r_pal4.returncode == 0 and rf.read_bmp24(hist_bmp.read_bytes()) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "pre-port failed on KAT_PAL4")


def screen_content_method6() -> None:
    """Method 6 (screen content v2): selection, prototype/oracle/C byte identity on 1x1, one-colour,
    1xH and Wx1, dv_s01 crop, overlapping cross-row copies (L = 4096), inherited L = 2 vertical stems,
    rep_dy and dx == 0 copies, exact D = 2^20 - 1 window boundary, wide dx and dy symbols, FNV-1a bucket
    collisions, -t 1/-t 3 byte identity, inspect output, pre-method-6 38c0e37 refusal, and all 15 payload/
    stream refusals plus version 2 header refusal across strided BMP, packed RGB, and the oracle."""
    if str(ROOT / "research") not in sys.path:
        sys.path.insert(0, str(ROOT / "research"))
    import screencode2

    work = WORK / "screen_content_method6"
    work.mkdir(parents=True, exist_ok=True)
    n = 0

    def put(blob: bytes) -> Path:
        nonlocal n
        n += 1
        path = work / f"f{n}.fold"
        path.write_bytes(blob)
        return path

    def decodes(blob: bytes, width: int, height: int, rgb: bytes, what: str) -> None:
        path = put(blob)
        bmp, ppm = work / f"f{n}.bmp", work / f"f{n}.ppm"
        for f in (bmp, ppm):
            f.unlink(missing_ok=True)
        check(rf.decode_image(blob) == (width, height, rgb), f"{what}: oracle decode")
        expect_ok(["decode", str(path), str(bmp)])
        check(rf.read_bmp24(bmp.read_bytes()) == (width, height, rgb), f"{what}: strided BMP decode")
        expect_ok(["decode", "--threads", "3", str(path), str(ppm)])
        with Image.open(ppm) as im:
            check(im.size == (width, height) and im.convert("RGB").tobytes() == rgb, f"{what}: packed RGB decode")

    def reject(blob: bytes, code: int, text: str, what: str) -> None:
        path = put(blob)
        for ext in ("bmp", "ppm"):
            dest = work / f"f{n}.{ext}"
            dest.unlink(missing_ok=True)
            expect_fail(["decode", str(path), str(dest)], code, text)
            check(not dest.exists(), f"{what} ({ext}): an output was left behind")
        try:
            rf.decode_image(blob)
        except rf.FormatError:
            pass
        else:
            check(False, f"{what}: oracle accepted the file")

    def selected_sc6(name: str, width: int, height: int, rgb: bytes) -> bytes:
        roundtrip(name, width, height, rgb)
        blob = (WORK / name / "out.fold").read_bytes()
        check(blob[16] == rf.METHOD_SC2, f"{name} did not select METHOD_SC2 (method {blob[16]})")
        check(blob[32:] == screencode2.encode(width, height, rgb), f"{name}: C/oracle payload differs from screencode2.py")
        t3_fold = work / f"{name}_t3.fold"
        t3_fold.unlink(missing_ok=True)
        expect_ok(["encode", "--threads", "3", str(WORK / name / "in.bmp"), str(t3_fold)])
        check(t3_fold.read_bytes() == blob, f"{name}: 3-thread encode differs from 1-thread")
        decodes(blob, width, height, rgb, name)
        return blob

    # 1. 1x1 and one-colour forced method 6 byte identity with prototype + >16-colour solid-body, 1xH, and Wx1.
    rgb_1x1 = bytes([77, 144, 211])
    roundtrip("sc6_1x1", 1, 1, rgb_1x1)
    pay_1x1 = rf.sc2_encode(1, 1, rgb_1x1, force=True)
    assert pay_1x1 is not None
    check(pay_1x1 == screencode2.encode(1, 1, rgb_1x1), "1x1 forced method 6 differs from prototype")
    decodes(rf.build_file(1, 1, rf.METHOD_SC2, pay_1x1), 1, 1, rgb_1x1, "forced method 6 1x1")

    rgb_one = solid(64, 48, (32, 96, 192))
    roundtrip("sc6_one_colour", 64, 48, rgb_one)
    pay_one = rf.sc2_encode(64, 48, rgb_one, force=True)
    assert pay_one is not None
    check(pay_one == screencode2.encode(64, 48, rgb_one), "one-colour forced method 6 differs from prototype")
    decodes(rf.build_file(64, 48, rf.METHOD_SC2, pay_one), 64, 48, rgb_one, "forced method 6 one-colour")

    rng_sb = random.Random(606)
    sb_cols: list[bytes] = []
    sb_seen: set[bytes] = set()
    while len(sb_cols) < 260:
        col_sb = bytes((rng_sb.randrange(256), rng_sb.randrange(256), rng_sb.randrange(256)))
        if col_sb not in sb_seen:
            sb_seen.add(col_sb)
            sb_cols.append(col_sb)
    hdr260 = b"".join(sb_cols)
    rgb_solid_body = hdr260 + bytes([40, 120, 200]) * (64 * 48 - 260)
    selected_sc6("sc6_solid_body", 64, 48, rgb_solid_body)

    tile20 = b"".join(sb_cols[:20])
    rgb_1xh = tile20 * 14
    selected_sc6("sc6_one_wide", 1, 280, rgb_1xh)
    selected_sc6("sc6_one_high", 280, 1, rgb_1xh)

    # 2. Screenshot crop from development set dv_s01.png.
    with Image.open(ROOT / "research" / "screens-assets" / "dv_s01.png") as im:
        crop_im = im.convert("RGB").crop((80, 40, 336, 200))
        crop_w, crop_h = crop_im.size
        crop_rgb = crop_im.tobytes()
    check(len({crop_rgb[i : i + 3] for i in range(0, len(crop_rgb), 3)}) > 256, "dv_s01 crop has <= 256 colours")
    selected_sc6("sc6_screen_crop", crop_w, crop_h, crop_rgb)

    # 3. Overlapping cross-row copy reaching L = 4096, inherited L = 2 vertical glyph stems, and rep_dy + dx == 0.
    motif5 = b"\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff"
    need_px = 64 * 80 - 260
    rgb_ov = hdr260 + (motif5 * ((need_px // 5) + 1))[: need_px * 3]
    selected_sc6("sc6_overlap", 64, 80, rgb_ov)

    # Inherited L = 2 vertical stems: 2-pixel glyph stems translated by D = 3 * 64 + 2 across rows, with unique
    # per-row background pairs in between so general L >= 4 search cannot cover the 2-pixel stems.
    w_l2, h_l2 = 64, 32
    grid_l2 = [sb_cols[(y * 17 + x * 5) % 260] for y in range(h_l2) for x in range(w_l2)]
    stem_pair = [b"\xfe\x11\x22", b"\x11\xfe\x33"]
    hdr_block = sb_cols[:8]
    for y in range(0, h_l2 - 3, 3):
        x0 = (y * 2) % 40
        for k in range(8):
            grid_l2[y * w_l2 + x0 + k] = hdr_block[k]
        for ry in (y + 1, y + 2):
            for k in range(2):
                grid_l2[ry * w_l2 + x0 + k] = stem_pair[k]
    for q in range(w_l2 * 18, w_l2 * h_l2):
        grid_l2[q] = sb_cols[259]
    selected_sc6("sc6_inh_l2", w_l2, h_l2, b"".join(grid_l2))

    # rep_dy and dx == 0: repeated 6-pixel blocks aligned at the exact same column x (dx == 0) separated by dy = 5 rows.
    w_rdy, h_rdy = 64, 36
    grid_rdy = [sb_cols[(y * 19 + x * 7) % 260] for y in range(h_rdy) for x in range(w_rdy)]
    pat6 = sb_cols[200:206]
    for col_x in (4, 18, 34, 48):
        for row_y in (1, 6, 11, 16):
            for k in range(6):
                grid_rdy[row_y * w_rdy + col_x + k] = pat6[k]
    for q in range(w_rdy * 18, w_rdy * h_rdy):
        grid_rdy[q] = sb_cols[258]
    selected_sc6("sc6_rep_dy_dx0", w_rdy, h_rdy, b"".join(grid_rdy))

    # 4. Wide dx (|dx| > 1024 on 1600x4) and wide dy (dy > 1024 on 4x1600) exercising large 2D prefix symbols.
    w_wdx, h_wdx = 1600, 4
    grid_wdx = [sb_cols[0]] * (w_wdx * h_wdx)
    for i in range(260):
        grid_wdx[i] = sb_cols[i]
    sig8 = [bytes(((220 + k) & 255, (40 + k * 13) & 255, (90 + k * 29) & 255)) for k in range(8)]
    for k in range(8):
        grid_wdx[1500 + k] = sig8[k]        # row 0, x = 1500..1507
        grid_wdx[w_wdx + 100 + k] = sig8[k] # row 1, x = 100..107 -> dy = 1, dx = -1400
        grid_wdx[2 * w_wdx + 1550 + k] = sig8[k]  # row 2, x = 1550..1557 -> dx = +1450 vs row 1
    selected_sc6("sc6_wide_dx", w_wdx, h_wdx, b"".join(grid_wdx))

    w_wdy, h_wdy = 4, 1600
    grid_wdy = [sb_cols[0]] * (w_wdy * h_wdy)
    for i in range(260):
        grid_wdy[i] = sb_cols[i]
    for k in range(8):
        grid_wdy[4 * 300 + k] = sig8[k]
        grid_wdy[4 * 1500 + k] = sig8[k]  # dy = 1200 > 1024
    selected_sc6("sc6_wide_dy", w_wdy, h_wdy, b"".join(grid_wdy))

    # 5. Exact distance-window boundary D = 2^20 - 1 = 1048575 (matched) vs D = 1048576 (outside window)
    #    on 1024x1026. The background uses non-adjacent-equal colours at target_p so D = 1 cannot mask key_win.
    win_w, win_h = 1024, 1026
    max_d6 = (1 << 20) - 1
    key_win6 = bytes(((i * 59 + 23) & 255 for i in range(48)))
    win6_payloads: dict[str, bytes] = {}
    for tag_w, target_p in (("win6_in", max_d6), ("win6_out", max_d6 + 1)):
        buf_w = bytearray(bytes([7, 14, 21]) * (win_w * win_h))
        buf_w[0:48] = key_win6
        buf_w[48:51] = b"\x01\x02\x03"
        buf_w[target_p * 3 : target_p * 3 + 48] = key_win6
        buf_w[target_p * 3 + 48 : target_p * 3 + 51] = b"\x04\x05\x06"
        buf_w[-260 * 3 :] = hdr260
        rgb_w = bytes(buf_w)
        w_bmp = work / f"{tag_w}.bmp"
        w_fold = work / f"{tag_w}.fold"
        w_fold.unlink(missing_ok=True)
        w_bmp.write_bytes(rf.write_bmp24(win_w, win_h, rgb_w))
        expect_ok(["encode", str(w_bmp), str(w_fold)])
        w_blob = w_fold.read_bytes()
        check(w_blob[16] == rf.METHOD_SC2, f"{tag_w} did not select METHOD_SC2")
        exp_sc6 = rf.sc2_encode(win_w, win_h, rgb_w, force=True)
        assert exp_sc6 is not None
        check(exp_sc6 == screencode2.encode(win_w, win_h, rgb_w), f"{tag_w}: oracle != prototype at window boundary")
        check(w_blob[32:] == exp_sc6, f"{tag_w}: C != oracle at window boundary")
        decodes(w_blob, win_w, win_h, rgb_w, tag_w)
        win6_payloads[tag_w] = exp_sc6
    check(
        win6_payloads["win6_in"] != win6_payloads["win6_out"]
        and len(win6_payloads["win6_in"]) < len(win6_payloads["win6_out"]),
        "method 6 window boundary payloads did not reflect inside vs outside match decision",
    )

    # 6. FNV-1a 16-bit bucket collision fixture (66 distinct 4-pixel keys in bucket 10870).
    coll_ids = (
        30690, 116199, 151429, 294000, 312115, 320667, 335786, 450346, 458701, 486739, 505041,
        539753, 614524, 681221, 683534, 697086, 770369, 799259, 825594, 881833, 890101, 914855,
        942492, 999009, 1058686, 1082868, 1111621, 1128294, 1248069, 1288994, 1291159, 1294749,
        1308670, 1352422, 1443426, 1498285, 1517471, 1538638, 1542801, 1572573, 1627532, 1664189,
        1695529, 1754739, 1786036, 1808502, 1846772, 1850455, 1857946, 1899386, 1908082, 1992533,
        2021952, 2131509, 2328364, 2338916, 2346937, 2358858, 2359529, 2365992, 2421597, 2471403,
        2544142, 2573268, 2601473, 2611358,
    )

    def coll6_key12(i: int) -> bytes:
        return bytes((
            i & 255, (i >> 8) & 255, (i >> 16) & 255, (i * 3 + 1) & 255,
            (i * 7 + 5) & 255, 89, (i * 11 + 3) & 255, (i * 13 + 9) & 255,
            163, (i * 17 + 7) & 255, (i * 19 + 11) & 255, 231,
        ))

    keys12 = [coll6_key12(i) for i in coll_ids]
    seq_keys = [keys12[0]] + keys12[1:58] + [keys12[0]] + keys12[1:66] + [keys12[0]] + [keys12[65]] * 254
    rgb_coll = b"".join(seq_keys[:320])
    selected_sc6("sc6_bucket_collision", 64, 20, rgb_coll)

    # 7. inspect text and --json on sc6_screen_crop: method is sc, no palette_colours and no stream sizes.
    crop_path = WORK / "sc6_screen_crop" / "out.fold"
    insp_txt = expect_ok(["inspect", str(crop_path)]).stdout.decode("utf-8", errors="replace")
    check("method: sc" in insp_txt and "palette_colours" not in insp_txt and "stream_" not in insp_txt, f"inspect text: {insp_txt}")
    insp_js = json.loads(expect_ok(["inspect", "--json", str(crop_path)]).stdout.decode("utf-8", errors="replace"))
    check(
        insp_js.get("method") == "sc" and "palette_colours" not in insp_js and "streams" not in insp_js,
        f"inspect --json: {insp_js}",
    )

    # 8. All 15 payload/stream refusals plus version 2 and unknown-method header refusals across BMP, PPM, and oracle.
    bad_sc = "invalid fold: screen content stream"

    def m6(payload: bytes, width: int = 8, height: int = 1) -> bytes:
        return rf.build_file(width, height, rf.METHOD_SC2, payload)

    class _Sc2BitCraft:
        def __init__(self, width: int) -> None:
            self.w = width
            self.enc = rf.RangeEncoder()
            self.m = rf._SC2Model()
            self.prev_kind = 5
            self.prev_same_row = 0

        def _put(self, ctr: rf._Pal4Counter, bit: int) -> None:
            ctr.code(self.enc, None, bit)

        def _tree(self, tree: list, val: int, bits: int) -> None:
            rf._sc_tree_put(self.enc, tree, bits, val)

        def _extra(self, ctr: rf._Pal4Counter, nbits: int, extra: int) -> None:
            rf._sc_extra_put(self.enc, ctr, nbits, extra)

        def p0_lit(self, r: int, g: int, b: int) -> None:
            self._tree(self.m.lit_g[4], g, 8)
            gctx = g >> 6
            self._tree(self.m.lit_rg[gctx], (r - g) & 255, 8)
            self._tree(self.m.lit_bg[gctx], (b - g) & 255, 8)

        def finish(self) -> bytes:
            return self.enc.finish()

    # (1) Payload shorter than 5 bytes (0 and 4 bytes).
    reject(m6(b""), 4, bad_sc, "empty method 6 payload")
    reject(m6(b"\x00" * 4), 4, bad_sc, "4-byte method 6 payload")

    # (2) len_tree[1] symbol > 23 and len_tree[0] symbol > 23.
    for bad_lsym in (24, 31):
        bc_i = _Sc2BitCraft(8)
        bc_i.p0_lit(10, 20, 30)
        bc_i._put(bc_i.m.is_copy[bc_i.prev_kind], 1)
        bc_i._put(bc_i.m.is_inherit[bc_i.prev_kind], 1)  # p = 1 on 8x1 has k = 1 candidate
        bc_i._tree(bc_i.m.len_tree[1], bad_lsym, 5)
        reject(m6(bc_i.finish(), 8, 1), 4, bad_sc, f"inherited len_sym {bad_lsym} > 23")

        bc_n = _Sc2BitCraft(8)
        bc_n.p0_lit(10, 20, 30)
        bc_n._put(bc_n.m.is_copy[bc_n.prev_kind], 1)
        bc_n._put(bc_n.m.is_inherit[bc_n.prev_kind], 0)
        bc_n._tree(bc_n.m.len_tree[0], bad_lsym, 5)
        reject(m6(bc_n.finish(), 8, 1), 4, bad_sc, f"new-copy len_sym {bad_lsym} > 23")

    # (3) Valid L == 4096 boundary decodes cleanly, while L == 4097 (> 4096) is refused on both inherited and new copies.
    ls4096_i, lnb4096_i, lex4096_i = rf._sc_prefix_encode(4096 - 1)
    bc_ok4096 = _Sc2BitCraft(4097)
    bc_ok4096.p0_lit(10, 20, 30)
    bc_ok4096._put(bc_ok4096.m.is_copy[bc_ok4096.prev_kind], 1)
    bc_ok4096._put(bc_ok4096.m.is_inherit[bc_ok4096.prev_kind], 1)
    bc_ok4096._tree(bc_ok4096.m.len_tree[1], ls4096_i, 5)
    bc_ok4096._extra(bc_ok4096.m.len_extra[1][ls4096_i], lnb4096_i, lex4096_i)
    decodes(m6(bc_ok4096.finish(), 4097, 1), 4097, 1, bytes([10, 20, 30]) * 4097, "method 6 exact L = 4096")

    ls4097_i, lnb4097_i, lex4097_i = rf._sc_prefix_encode(4097 - 1)
    bc_bad4097_i = _Sc2BitCraft(5000)
    bc_bad4097_i.p0_lit(10, 20, 30)
    bc_bad4097_i._put(bc_bad4097_i.m.is_copy[bc_bad4097_i.prev_kind], 1)
    bc_bad4097_i._put(bc_bad4097_i.m.is_inherit[bc_bad4097_i.prev_kind], 1)
    bc_bad4097_i._tree(bc_bad4097_i.m.len_tree[1], ls4097_i, 5)
    bc_bad4097_i._extra(bc_bad4097_i.m.len_extra[1][ls4097_i], lnb4097_i, lex4097_i)
    reject(m6(bc_bad4097_i.finish(), 5000, 1), 4, bad_sc, "inherited L = 4097 > 4096")

    ls4097_n, lnb4097_n, lex4097_n = rf._sc_prefix_encode(4097 - 3)
    bc_bad4097_n = _Sc2BitCraft(5000)
    bc_bad4097_n.p0_lit(10, 20, 30)
    bc_bad4097_n._put(bc_bad4097_n.m.is_copy[bc_bad4097_n.prev_kind], 1)
    bc_bad4097_n._put(bc_bad4097_n.m.is_inherit[bc_bad4097_n.prev_kind], 0)
    bc_bad4097_n._tree(bc_bad4097_n.m.len_tree[0], ls4097_n, 5)
    bc_bad4097_n._extra(bc_bad4097_n.m.len_extra[0][ls4097_n], lnb4097_n, lex4097_n)
    reject(m6(bc_bad4097_n.finish(), 5000, 1), 4, bad_sc, "new-copy L = 4097 > 4096")

    # (4) Copy past last pixel (p = 1, L = 4 -> p + L = 5 > 4 on 4x1) on inherited and non-inherited copies.
    bc_past_i = _Sc2BitCraft(4)
    bc_past_i.p0_lit(10, 20, 30)
    bc_past_i._put(bc_past_i.m.is_copy[bc_past_i.prev_kind], 1)
    bc_past_i._put(bc_past_i.m.is_inherit[bc_past_i.prev_kind], 1)
    bc_past_i._tree(bc_past_i.m.len_tree[1], 2, 5)  # val = 3 -> L = 4
    reject(m6(bc_past_i.finish(), 4, 1), 4, bad_sc, "inherited copy past last pixel")

    bc_past_n = _Sc2BitCraft(4)
    bc_past_n.p0_lit(10, 20, 30)
    bc_past_n._put(bc_past_n.m.is_copy[bc_past_n.prev_kind], 1)
    bc_past_n._put(bc_past_n.m.is_inherit[bc_past_n.prev_kind], 0)
    bc_past_n._tree(bc_past_n.m.len_tree[0], 0, 5)  # val = 1 -> L = 4
    reject(m6(bc_past_n.finish(), 4, 1), 4, bad_sc, "new copy past last pixel")

    # (5) dx0_tree symbol > 39 and (6) same_row == 1 with dx > px_col.
    for bad_dx0 in (40, 63):
        bc_dx0 = _Sc2BitCraft(8)
        bc_dx0.p0_lit(10, 20, 30)
        bc_dx0._put(bc_dx0.m.is_copy[bc_dx0.prev_kind], 1)
        bc_dx0._put(bc_dx0.m.is_inherit[bc_dx0.prev_kind], 0)
        bc_dx0._tree(bc_dx0.m.len_tree[0], 0, 5)  # py == 0 -> implicit same_row = 1
        bc_dx0._tree(bc_dx0.m.dx0_tree, bad_dx0, 6)
        reject(m6(bc_dx0.finish(), 8, 1), 4, bad_sc, f"dx0_sym {bad_dx0} > 39")

    bc_dx0_big = _Sc2BitCraft(8)
    bc_dx0_big.p0_lit(10, 20, 30)
    bc_dx0_big._put(bc_dx0_big.m.is_copy[bc_dx0_big.prev_kind], 1)
    bc_dx0_big._put(bc_dx0_big.m.is_inherit[bc_dx0_big.prev_kind], 0)
    bc_dx0_big._tree(bc_dx0_big.m.len_tree[0], 0, 5)
    bc_dx0_big._tree(bc_dx0_big.m.dx0_tree, 1, 6)  # dx = 2 > px_col = 1
    reject(m6(bc_dx0_big.finish(), 8, 1), 4, bad_sc, "same_row dx > px_col")

    # Helper to advance p = 1..4 on 4x2 via one inherited L = 4 copy at D = 1, landing at p = 5 (py = 1, px_col = 1).
    def _at_row1_col1() -> _Sc2BitCraft:
        bc = _Sc2BitCraft(4)
        bc.p0_lit(10, 20, 30)
        bc._put(bc.m.is_copy[bc.prev_kind], 1)
        bc._put(bc.m.is_inherit[bc.prev_kind], 1)
        bc._tree(bc.m.len_tree[1], 2, 5)  # val = 3 -> L = 4, covers p = 1..4
        bc.prev_kind = 0
        return bc

    # (7) dy_tree symbol > 39 and (8) dy > py via rep_dy and via dy_tree (4x3 = 12 px so L = 4 fits at p = 5).
    for bad_dy in (40, 63):
        bc_dy = _at_row1_col1()
        bc_dy._put(bc_dy.m.is_copy[bc_dy.prev_kind], 1)
        bc_dy._put(bc_dy.m.is_inherit[bc_dy.prev_kind], 0)
        bc_dy._tree(bc_dy.m.len_tree[0], 0, 5)
        bc_dy._put(bc_dy.m.is_same_row[bc_dy.prev_same_row], 0)
        bc_dy._put(bc_dy.m.is_rep_dy, 0)
        bc_dy._tree(bc_dy.m.dy_tree, bad_dy, 6)
        reject(m6(bc_dy.finish(), 4, 3), 4, bad_sc, f"dy_sym {bad_dy} > 39")

    bc_rdy_over = _at_row1_col1()
    bc_rdy_over._put(bc_rdy_over.m.is_copy[bc_rdy_over.prev_kind], 1)
    bc_rdy_over._put(bc_rdy_over.m.is_inherit[bc_rdy_over.prev_kind], 0)
    bc_rdy_over._tree(bc_rdy_over.m.len_tree[0], 0, 5)
    bc_rdy_over._put(bc_rdy_over.m.is_same_row[bc_rdy_over.prev_same_row], 0)
    bc_rdy_over._put(bc_rdy_over.m.is_rep_dy, 1)
    bc_rdy_over._put(bc_rdy_over.m.rep_dy_b0, 0)
    bc_rdy_over._put(bc_rdy_over.m.rep_dy_b1[0], 1)  # ridx = 1 -> rep_dy[1] = 2 > py = 1
    reject(m6(bc_rdy_over.finish(), 4, 3), 4, bad_sc, "dy > py via rep_dy")

    bc_dy_over = _at_row1_col1()
    bc_dy_over._put(bc_dy_over.m.is_copy[bc_dy_over.prev_kind], 1)
    bc_dy_over._put(bc_dy_over.m.is_inherit[bc_dy_over.prev_kind], 0)
    bc_dy_over._tree(bc_dy_over.m.len_tree[0], 0, 5)
    bc_dy_over._put(bc_dy_over.m.is_same_row[bc_dy_over.prev_same_row], 0)
    bc_dy_over._put(bc_dy_over.m.is_rep_dy, 0)
    bc_dy_over._tree(bc_dy_over.m.dy_tree, 1, 6)  # dy = 2 > py = 1
    reject(m6(bc_dy_over.finish(), 4, 3), 4, bad_sc, "dy > py via dy_tree")

    # (9) dx_tree symbol > 49 and (10) qx = px_col - dx < 0 and qx >= width.
    for bad_dx in (50, 63):
        bc_dx = _at_row1_col1()
        bc_dx._put(bc_dx.m.is_copy[bc_dx.prev_kind], 1)
        bc_dx._put(bc_dx.m.is_inherit[bc_dx.prev_kind], 0)
        bc_dx._tree(bc_dx.m.len_tree[0], 0, 5)
        bc_dx._put(bc_dx.m.is_same_row[bc_dx.prev_same_row], 0)
        bc_dx._put(bc_dx.m.is_rep_dy, 1)
        bc_dx._put(bc_dx.m.rep_dy_b0, 1)  # dy = 1 <= py = 1
        bc_dx._put(bc_dx.m.dx_zero[1], 0)
        bc_dx._put(bc_dx.m.dx_sign, 0)
        bc_dx._tree(bc_dx.m.dx_tree, bad_dx, 6)
        reject(m6(bc_dx.finish(), 4, 3), 4, bad_sc, f"dx_sym {bad_dx} > 49")

    bc_qx_neg = _at_row1_col1()
    bc_qx_neg._put(bc_qx_neg.m.is_copy[bc_qx_neg.prev_kind], 1)
    bc_qx_neg._put(bc_qx_neg.m.is_inherit[bc_qx_neg.prev_kind], 0)
    bc_qx_neg._tree(bc_qx_neg.m.len_tree[0], 0, 5)
    bc_qx_neg._put(bc_qx_neg.m.is_same_row[bc_qx_neg.prev_same_row], 0)
    bc_qx_neg._put(bc_qx_neg.m.is_rep_dy, 1)
    bc_qx_neg._put(bc_qx_neg.m.rep_dy_b0, 1)  # dy = 1
    bc_qx_neg._put(bc_qx_neg.m.dx_zero[1], 0)
    bc_qx_neg._put(bc_qx_neg.m.dx_sign, 0)  # dx = +2 -> qx = 1 - 2 = -1 < 0
    bc_qx_neg._tree(bc_qx_neg.m.dx_tree, 1, 6)
    reject(m6(bc_qx_neg.finish(), 4, 3), 4, bad_sc, "qx = px_col - dx < 0")

    bc_qx_hi = _at_row1_col1()
    bc_qx_hi._put(bc_qx_hi.m.is_copy[bc_qx_hi.prev_kind], 1)
    bc_qx_hi._put(bc_qx_hi.m.is_inherit[bc_qx_hi.prev_kind], 0)
    bc_qx_hi._tree(bc_qx_hi.m.len_tree[0], 0, 5)
    bc_qx_hi._put(bc_qx_hi.m.is_same_row[bc_qx_hi.prev_same_row], 0)
    bc_qx_hi._put(bc_qx_hi.m.is_rep_dy, 1)
    bc_qx_hi._put(bc_qx_hi.m.rep_dy_b0, 1)  # dy = 1
    bc_qx_hi._put(bc_qx_hi.m.dx_zero[1], 0)
    bc_qx_hi._put(bc_qx_hi.m.dx_sign, 1)  # dx = -3 -> qx = 1 - (-3) = 4 >= width = 4
    bc_qx_hi._tree(bc_qx_hi.m.dx_tree, 2, 6)
    reject(m6(bc_qx_hi.finish(), 4, 3), 4, bad_sc, "qx = px_col - dx >= width")

    # (11) D = dy * width + dx > WINDOW (1024x1026: reach p = 1025 * 1024 = 1049600 at px_col = 0, then dy = 1025, dx = 0).
    bc_dwin = _Sc2BitCraft(1024)
    bc_dwin.p0_lit(10, 20, 30)
    cur_p = 1
    target_row_p = 1025 * 1024
    while cur_p < target_row_p:
        step = min(4096, target_row_p - cur_p)
        ls_s, lnb_s, lex_s = rf._sc_prefix_encode(step - 1)
        bc_dwin._put(bc_dwin.m.is_copy[bc_dwin.prev_kind], 1)
        bc_dwin._put(bc_dwin.m.is_inherit[bc_dwin.prev_kind], 1)
        # At cur_p == 1 on 1024x1026, cands = [1] (k = 1); once cur_p >= 1024, cands has >1 entry and idx 0 is D = 1.
        if cur_p >= 1024:
            bc_dwin._put(bc_dwin.m.inh_b0[bc_dwin.prev_kind], 1)
        bc_dwin._tree(bc_dwin.m.len_tree[1], ls_s, 5)
        bc_dwin._extra(bc_dwin.m.len_extra[1][ls_s], lnb_s, lex_s)
        bc_dwin.prev_kind = 0
        cur_p += step
    dys_w, dynb_w, dyex_w = rf._sc_prefix_encode(1025)
    bc_dwin._put(bc_dwin.m.is_copy[bc_dwin.prev_kind], 1)
    bc_dwin._put(bc_dwin.m.is_inherit[bc_dwin.prev_kind], 0)
    bc_dwin._tree(bc_dwin.m.len_tree[0], 0, 5)  # px_col == 0 -> implicit same_row = 0
    bc_dwin._put(bc_dwin.m.is_rep_dy, 0)
    bc_dwin._tree(bc_dwin.m.dy_tree, dys_w, 6)
    bc_dwin._extra(bc_dwin.m.dy_extra[dys_w], dynb_w, dyex_w)
    bc_dwin._put(bc_dwin.m.dx_zero[0], 1)  # dx = 0 -> D = 1025 * 1024 = 1049600 > 1048575
    reject(m6(bc_dwin.finish(), 1024, 1026), 4, bad_sc, "2D copy distance dy * width + dx > WINDOW")

    # (12) mtf_tree index >= mtf_count (midx = 1 at p = 1 when mtf_count == 1).
    bc_mtf = _Sc2BitCraft(4)
    bc_mtf.p0_lit(10, 20, 30)
    bc_mtf._put(bc_mtf.m.is_copy[bc_mtf.prev_kind], 0)
    bc_mtf._put(bc_mtf.m.is_eq_w[bc_mtf.prev_kind * 2 + 0], 0)
    bc_mtf._put(bc_mtf.m.is_mtf[bc_mtf.prev_kind], 1)
    bc_mtf._tree(bc_mtf.m.mtf_tree, 1, 4)
    reject(m6(bc_mtf.finish(), 4, 1), 4, bad_sc, "mtf_tree index >= mtf_count")

    # (13) cache_tree index naming an unset 2048-entry slot at p = 1.
    slot11_0 = ((0x1E35A7BD * (0xFF000000 | (10 << 16) | (20 << 8) | 30)) & 0xFFFFFFFF) >> 21
    bc_c2 = _Sc2BitCraft(4)
    bc_c2.p0_lit(10, 20, 30)
    bc_c2._put(bc_c2.m.is_copy[bc_c2.prev_kind], 0)
    bc_c2._put(bc_c2.m.is_eq_w[bc_c2.prev_kind * 2 + 0], 0)
    bc_c2._put(bc_c2.m.is_mtf[bc_c2.prev_kind], 0)
    bc_c2._put(bc_c2.m.is_cache[bc_c2.prev_kind], 1)
    bc_c2._tree(bc_c2.m.cache_tree, (slot11_0 + 1) & 2047, 11)
    reject(m6(bc_c2.finish(), 4, 1), 4, bad_sc, "cache_tree unset 11-bit slot")

    # (14) Truncated stream and (15) trailing byte after stream on KAT_SC6.
    reject(rf.build_file(KAT_SC_W, KAT_SC_H, rf.METHOD_SC2, KAT_SC6[32:-3]), 4, bad_sc, "truncated method 6 stream")
    reject(rf.build_file(KAT_SC_W, KAT_SC_H, rf.METHOD_SC2, KAT_SC6[32:] + b"\x00"), 4, bad_sc, "trailing byte after method 6 stream")

    # Version 2 header refusal (exit 3) and unknown method 7 header refusal (exit 3).
    v2_sc6 = bytearray(KAT_SC6)
    v2_sc6[3] = 2
    struct.pack_into("<H", v2_sc6, 4, 2)
    reject(recrc(v2_sc6), 3, "unsupported fold: method", "method 6 in a version 2 file")
    reject(rf.build_file(KAT_SC_W, KAT_SC_H, 7, KAT_SC6[32:]), 3, "unsupported fold: method", "method 7 in a version 3 file")

    # 9. Historical decoder compatibility: pre-method-6 decoder (38c0e37) refuses KAT_SC6 with exit 3
    #    (unsupported fold: method) and leaves no output file, while still decoding KAT_SC5 and KAT_PAL4.
    clang = ROOT / ("tools/llvm-mingw/bin/clang.exe" if os.name == "nt" else "tools/llvm-mingw-linux/bin/clang")
    check(clang.is_file(), f"pinned clang compiler missing: {clang}")
    pre_ref6 = "38c0e37"
    r_cat6 = subprocess.run(["git", "cat-file", "-e", f"{pre_ref6}^{{commit}}"], cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True)
    check(r_cat6.returncode == 0, f"historical commit {pre_ref6} missing from checkout")
    with tempfile.TemporaryDirectory(prefix="rf_hist_sc6_") as htmp6:
        ht6 = Path(htmp6)
        kat_sc6_path = ht6 / "kat_sc6.fold"
        kat_sc5_path = ht6 / "kat_sc5.fold"
        kat_pal4_path = ht6 / "kat_pal4.fold"
        kat_sc6_path.write_bytes(KAT_SC6)
        kat_sc5_path.write_bytes(KAT_SC5)
        kat_pal4_path.write_bytes(KAT_PAL4)
        (ht6 / "src").mkdir(parents=True)
        (ht6 / "include").mkdir(parents=True)
        (ht6 / "include" / "rasterfold.h").write_bytes(subprocess.check_output(["git", "show", f"{pre_ref6}:include/rasterfold.h"], cwd=ROOT, stdin=subprocess.DEVNULL))
        c_files6 = []
        for s in subprocess.check_output(["git", "ls-tree", "--name-only", f"{pre_ref6}:src"], cwd=ROOT, stdin=subprocess.DEVNULL, text=True).splitlines():
            if os.name == "nt" and s == "sys_posix.c":
                continue
            if os.name != "nt" and s in ("sys.c", "mem.c"):
                continue
            p = ht6 / "src" / s
            p.write_bytes(subprocess.check_output(["git", "show", f"{pre_ref6}:src/{s}"], cwd=ROOT, stdin=subprocess.DEVNULL))
            if s.endswith(".c"):
                c_files6.append(str(p))
        hist_exe6 = ht6 / ("rasterfold.exe" if os.name == "nt" else "rasterfold")
        if os.name == "nt":
            cmd6 = [
                str(clang), "-std=c11", "-O1", "-march=x86-64-v2",
                "-ffreestanding", "-fno-builtin", "-nostdlib",
                "-I", str(ht6 / "include"), *c_files6,
                "-Wl,-e,rf_start", "-Wl,--subsystem,console",
                "-lkernel32", "-lshell32", "-o", str(hist_exe6),
            ]
        else:
            cmd6 = [str(clang), "-std=c11", "-O1", "-march=x86-64-v2", "-I", str(ht6 / "include"), *c_files6, "-lpthread", "-o", str(hist_exe6)]
        subprocess.check_call(cmd6, stdin=subprocess.DEVNULL)
        hist_bmp6 = ht6 / "out.bmp"
        r_ref6 = subprocess.run([str(hist_exe6), "decode", str(kat_sc6_path), str(hist_bmp6)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        check(r_ref6.returncode == 3, f"pre-method-6 ({pre_ref6}) on KAT_SC6 exit {r_ref6.returncode}, want 3: {r_ref6.stderr}")
        check("unsupported fold: method" in r_ref6.stderr, f"pre-method-6 stderr missing 'unsupported fold: method': {r_ref6.stderr}")
        check(not hist_bmp6.exists(), "pre-method-6 decoder left output BMP on refused KAT_SC6")
        r_sc5 = subprocess.run([str(hist_exe6), "decode", str(kat_sc5_path), str(hist_bmp6)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        check(r_sc5.returncode == 0 and rf.read_bmp24(hist_bmp6.read_bytes()) == (KAT_SC_W, KAT_SC_H, KAT_SC_RGB), "38c0e37 failed on KAT_SC5")
        hist_bmp6.unlink(missing_ok=True)
        r_pal4_6 = subprocess.run([str(hist_exe6), "decode", str(kat_pal4_path), str(hist_bmp6)], stdin=subprocess.DEVNULL, capture_output=True, text=True)
        check(r_pal4_6.returncode == 0 and rf.read_bmp24(hist_bmp6.read_bytes()) == (KAT_PAL_W, KAT_PAL_H, KAT_PAL_RGB), "38c0e37 failed on KAT_PAL4")


def fuzz_harness() -> None:
    import unittest.mock as mock
    import fuzz_decode

    records = fuzz_decode.seed_records()
    check(len(records) == 21, f"expected 21 fuzz seeds, got {len(records)}")
    check({m for _, _, _, m, _ in records[:7]} == {0, 1, 2, 3, 4, 5, 6}, "first 7 fuzz seeds do not cover methods 0..6")
    for name, w, h, m, blob in records:
        check(w >= 36 and h >= 34 and w * h > 1200, f"seed {name} dimensions {w}x{h} smaller than 36x34 (>1200 px)")
        check(blob[16] == m, f"seed {name} header method {blob[16]} != {m}")
        check(struct.unpack_from("<II", blob, 8) == (w, h), f"seed {name} header dimensions mismatch")
        if m in (2, 3, 4):
            stored = blob[32] if m == 4 else struct.unpack_from("<H", blob, 32)[0]     # method 4 stores count - 1 in one byte
            if "2col" in name:
                check(stored + 1 == 2, f"seed {name} palette count != 2")
            if "200col" in name:
                check(stored + 1 == 200, f"seed {name} palette count != 200")
    m4_seed = next(blob for _, _, _, m, blob in records if m == 4 and blob[32] == 199)
    m4_rng = random.Random(11)
    m4_muts = [fuzz_decode.mutate(m4_seed, m4_rng) for _ in range(300)]
    check(any(len(mb) > 34 and mb[32] != m4_seed[32] for mb in m4_muts), "method 4 mutations never change the count byte")
    check(any(len(mb) > 34 and mb[33:35] != m4_seed[33:35] for mb in m4_muts), "method 4 mutations never change a Rice parameter")
    check(any(len(mb) != len(m4_seed) for mb in m4_muts), "method 4 mutations never change the stream length")
    m5_seed = next(blob for _, _, _, m, blob in records if m == 5)
    m5_rng = random.Random(15)
    m5_muts = [fuzz_decode.mutate(m5_seed, m5_rng) for _ in range(300)]
    check(any(mb[16] == 5 and len(mb) <= 36 for mb in m5_muts), "method 5 mutations never probe <5-byte short payloads")
    check(any(mb[16] == 5 and len(mb) > len(m5_seed) for mb in m5_muts), "method 5 mutations never probe trailing bytes")
    check(any(mb[16] == 5 and len(mb) == len(m5_seed) and mb[32:] != m5_seed[32:] for mb in m5_muts), "method 5 mutations never mutate stream bytes")
    m6_seed = next(blob for _, _, _, m, blob in records if m == 6)
    m6_rng = random.Random(16)
    m6_muts = [fuzz_decode.mutate(m6_seed, m6_rng) for _ in range(300)]
    check(any(mb[16] == 6 and len(mb) <= 36 for mb in m6_muts), "method 6 mutations never probe <5-byte short payloads")
    check(any(mb[16] == 6 and len(mb) > len(m6_seed) for mb in m6_muts), "method 6 mutations never probe trailing bytes")
    check(any(mb[16] == 6 and len(mb) == len(m6_seed) and mb[32:] != m6_seed[32:] for mb in m6_muts), "method 6 mutations never mutate stream bytes")
    cm_strips = {blob[32] for _, _, _, m, blob in records if m == 1}
    check(cm_strips == {1, 2, 3, 4}, f"expected CM fuzz seeds to cover S in {{1, 2, 3, 4}}, got {cm_strips}")
    mut_rng = random.Random(7)
    for want_s in (2, 3, 4):
        s_seed = next(blob for _, _, _, m, blob in records if m == 1 and blob[32] == want_s)
        saw_decoded = False
        for _ in range(16):
            mut_blob = fuzz_decode.mutate(s_seed, mut_rng)
            if len(mut_blob) >= 32 + 1 + 12 * want_s and mut_blob[32] == want_s:
                sizes = struct.unpack_from(f"<{3 * want_s}I", mut_blob, 33)
                if all(sz >= 5 for sz in sizes) and 32 + 1 + 12 * want_s + sum(sizes) == len(mut_blob):
                    saw_decoded = True
                    break
        check(saw_decoded, f"mutations of S={want_s} seed did not preserve multi-strip framing")

    with tempfile.TemporaryDirectory(prefix="rf_fuzz_test_") as tmp:
        td = Path(tmp)
        rc, err = fuzz_decode.verify_mutation(0, KAT_PAL2, "1", EXE, td)
        check(rc == 0 and err is None, f"expected valid KAT_PAL2 to pass verify_mutation, got {(rc, err)}")

        bad_blob = bytearray(KAT_PAL2)
        bad_blob[-1] ^= 0xFF
        rc_bad, err_bad = fuzz_decode.verify_mutation(1, bytes(bad_blob), "1", EXE, td)
        check(rc_bad == 4 and err_bad is None, f"expected corrupt KAT_PAL2 to be rejected cleanly, got {(rc_bad, err_bad)}")

        fake_reject = subprocess.CompletedProcess(args=[], returncode=4, stdout=b"", stderr=b"invalid fold\n")
        with mock.patch.object(fuzz_decode.subprocess, "run", return_value=fake_reject):
            rc_mis, err_mis = fuzz_decode.verify_mutation(999, KAT_PAL2, "1", EXE, td)
        check(rc_mis == -1 and err_mis is not None and "native rejected oracle-valid input" in err_mis, f"expected native exit 4 on oracle-valid input to fail harness, got {(rc_mis, err_mis)}")
        check((td / "mismatch999.fold").read_bytes() == KAT_PAL2, "mismatch999.fold was not retained on false rejection")

        def fake_malformed_bmp(cmd: list[str], **_kwargs: object) -> subprocess.CompletedProcess[bytes]:
            Path(cmd[-1]).write_bytes(b"NOT_A_BMP")
            return subprocess.CompletedProcess(args=cmd, returncode=0, stdout=b"", stderr=b"")

        with mock.patch.object(fuzz_decode.subprocess, "run", side_effect=fake_malformed_bmp):
            rc_bmp, err_bmp = fuzz_decode.verify_mutation(998, KAT_PAL2, "1", EXE, td)
        check(rc_bmp == -1 and err_bmp is not None and "invalid BMP output at iteration 998" in err_bmp, f"expected malformed BMP on exit 0 to fail cleanly, got {(rc_bmp, err_bmp)}")
        check((td / "mismatch998.fold").read_bytes() == KAT_PAL2, "mismatch998.fold was not retained on malformed BMP")
        check((td / "mismatch998.bmp").read_bytes() == b"NOT_A_BMP", "mismatch998.bmp was not retained on malformed BMP")

        unsup_blob = bytearray(KAT_PAL2)
        unsup_blob[16] = 99
        unsup_bytes = fuzz_decode.recrc(unsup_blob)
        sleep_blob = bytearray(KAT_PAL2)
        sleep_blob[16] = 98
        sleep_bytes = fuzz_decode.recrc(sleep_blob)

        clang = ROOT / ("tools/llvm-mingw/bin/clang.exe" if os.name == "nt" else "tools/llvm-mingw-linux/bin/clang")
        check(clang.is_file(), f"pinned clang compiler missing: {clang}")
        helper_c = td / "hang_helper.c"
        helper_exe = td / ("hang_helper.exe" if os.name == "nt" else "hang_helper")
        pid_file = td / "sleeper.childpid"
        pid_file_c = str(pid_file).replace("\\", "\\\\")
        helper_c.write_text(
            "#include <stdio.h>\n"
            "#ifdef _WIN32\n"
            "#include <windows.h>\n"
            "static int my_pid(void) { return (int)GetCurrentProcessId(); }\n"
            "static void my_sleep_ms(int ms) { Sleep((DWORD)ms); }\n"
            "#else\n"
            "#include <unistd.h>\n"
            "static int my_pid(void) { return (int)getpid(); }\n"
            "static void my_sleep_ms(int ms) { usleep((unsigned)ms * 1000u); }\n"
            "#endif\n"
            f"static const char *PID_PATH = \"{pid_file_c}\";\n"
            "int main(int argc, char **argv) {\n"
            "    if (argc < 6) return 2;\n"
            "    FILE *f = fopen(argv[4], \"rb\");\n"
            "    if (!f) return 1;\n"
            "    unsigned char hdr[32];\n"
            "    size_t n = fread(hdr, 1, 32, f);\n"
            "    fclose(f);\n"
            "    if (n < 32) return 4;\n"
            "    if (hdr[16] == 98) {\n"
            "        char tmp_path[512];\n"
            "        snprintf(tmp_path, sizeof(tmp_path), \"%s.tmp\", PID_PATH);\n"
            "        FILE *pf = fopen(tmp_path, \"wb\");\n"
            "        if (pf) { fprintf(pf, \"%d\\n\", my_pid()); fclose(pf); rename(tmp_path, PID_PATH); }\n"
            "        my_sleep_ms(30000);\n"
            "        return 0;\n"
            "    }\n"
            "    for (int w = 0; w < 200; ++w) {\n"
            "        FILE *pf = fopen(PID_PATH, \"rb\");\n"
            "        if (pf) {\n"
            "            char buf[16];\n"
            "            size_t rn = fread(buf, 1, sizeof(buf), pf);\n"
            "            fclose(pf);\n"
            "            if (rn > 0) break;\n"
            "        }\n"
            "        my_sleep_ms(50);\n"
            "    }\n"
            "    return 3;\n"
            "}\n",
            encoding="utf-8",
        )
        subprocess.check_call([str(clang), "-O1", str(helper_c), "-o", str(helper_exe)], stdin=subprocess.DEVNULL)

        par_tasks = [
            (0, sleep_bytes, "1", str(helper_exe), str(td)),
            *[(i, unsup_bytes, "1", str(helper_exe), str(td)) for i in range(1, 20)],
        ]
        t_par = time.perf_counter()
        _, par_err = fuzz_decode.run_tasks_parallel(par_tasks, workers=2)
        par_elapsed = time.perf_counter() - t_par
        check(par_err is not None and "BAD exit 0x3 at iteration" in par_err, f"expected run_tasks_parallel to fail promptly, got {par_err}")
        check(par_elapsed < 10.0, f"run_tasks_parallel took {par_elapsed:.2f}s (expected < 10.0s after killing sibling child)")
        bad_artifacts = list(td.glob("bad*.fold"))
        check(1 <= len(bad_artifacts) <= 6, f"expected bounded parallel execution on failure (1..6 tasks), got {len(bad_artifacts)}")
        check(list(td.glob("f_*.fold")) == [] and list(td.glob("f_*.bmp")) == [], "worker temporary f_* files were not cleaned up after sibling failure")
        check(pid_file.is_file(), "expected sleeper.childpid from sleeping sibling")
        child_pid = int(pid_file.read_text(encoding="utf-8").strip())
        if os.name == "nt":
            import ctypes
            kernel32 = ctypes.windll.kernel32
            h_proc = kernel32.OpenProcess(0x00100000, False, child_pid)  # SYNCHRONIZE
            if h_proc:
                wait_rc = kernel32.WaitForSingleObject(h_proc, 0)
                kernel32.CloseHandle(h_proc)
                check(wait_rc == 0, f"sleeping child process {child_pid} still alive on Windows (WaitForSingleObject={wait_rc})")
        else:
            alive = True
            try:
                os.kill(child_pid, 0)
            except ProcessLookupError:
                alive = False
            check(not alive, f"sleeping child process {child_pid} survived run_tasks_parallel shutdown")

        # Bidirectional input-oracle checks across BMP, PNG and PPM: every seed
        # succeeds, every oracle self-check holds, and false refusals or swapped
        # refusal classes (3 vs 4 vs 5) are caught immediately.
        bmp_s = fuzz_decode.bmp_seeds()
        png_s = fuzz_decode.png_seeds()
        ppm_s = fuzz_decode.ppm_seeds()
        check(fuzz_decode._check_bmp_seeds_and_oracle(bmp_s) is None, "BMP seed/oracle self-check failed")
        check(fuzz_decode._check_png_seeds_and_oracle(png_s) is None, "PNG seed/oracle self-check failed")
        check(fuzz_decode._check_ppm_seeds_and_oracle(ppm_s) is None, "PPM seed/oracle self-check failed")
        fake_in_reject = subprocess.CompletedProcess(args=[], returncode=4, stdout=b"", stderr=b"invalid input\n")
        with mock.patch.object(fuzz_decode.subprocess, "run", return_value=fake_in_reject):
            rc_b, err_b = fuzz_decode.verify_bmp_mutation(901, bmp_s[0], EXE, td)
            rc_p, err_p = fuzz_decode.verify_png_mutation(902, png_s[0], EXE, td)
            rc_m, err_m = fuzz_decode.verify_ppm_mutation(903, ppm_s[0], EXE, td)
        check(rc_b == -1 and err_b is not None and "BMP MISMATCH" in err_b, f"BMP false refusal not caught: {(rc_b, err_b)}")
        check(rc_p == -1 and err_p is not None and "PNG MISMATCH" in err_p, f"PNG false refusal not caught: {(rc_p, err_p)}")
        check(rc_m == -1 and err_m is not None and "PPM MISMATCH" in err_m, f"PPM false refusal not caught: {(rc_m, err_m)}")
        unsup_bmp = bytearray(bmp_s[0])
        struct.pack_into("<H", unsup_bmp, 28, 32)
        unsup_ppm = b"P3\n1 1\n255\n0 0 0\n"
        with mock.patch.object(fuzz_decode.subprocess, "run", return_value=fake_in_reject):
            rc_b3, err_b3 = fuzz_decode.verify_bmp_mutation(904, bytes(unsup_bmp), EXE, td)
            rc_m3, err_m3 = fuzz_decode.verify_ppm_mutation(905, unsup_ppm, EXE, td)
        check(rc_b3 == -1 and err_b3 is not None and "exit 4 != oracle 3" in err_b3, f"BMP 3-vs-4 swap not caught: {(rc_b3, err_b3)}")
        check(rc_m3 == -1 and err_m3 is not None and "exit 4 != oracle 3" in err_m3, f"PPM 3-vs-4 swap not caught: {(rc_m3, err_m3)}")

        # Staging environment: POSIX never executes from tmpfs (/dev/shm may be
        # noexec), and Windows falls back to EXE if %TEMP% blocks execution.
        with fuzz_decode._staged_fuzz_env(EXE) as (st_exe, st_work):
            check(st_work.is_dir(), "expected staged scratch directory")
            if os.name != "nt":
                check(st_exe == EXE, f"POSIX must not execute from tmpfs stage (got {st_exe} != {EXE})")
            (st_work / "mismatch_probe.fold").write_bytes(b"PROBE")
        probe_saved = fuzz_decode.WORK / "mismatch_probe.fold"
        check(probe_saved.read_bytes() == b"PROBE", "reproducer artifact was not preserved from staged work_dir")
        probe_saved.unlink(missing_ok=True)
        with mock.patch.object(fuzz_decode, "_ORIG_SUBPROCESS_RUN", side_effect=PermissionError("noexec")):
            with fuzz_decode._staged_fuzz_env(EXE) as (fb_exe, _):
                check(fb_exe == EXE, f"noexec stage must fall back to EXE, got {fb_exe}")

        # Per-seed thread rotation: across one 30,000-iteration --all-formats
        # schedule, every .fold seed (including tall S=2 and S=3 CM fixtures)
        # exercises all four thread counts {"1", "3", "6", "9"}.
        per_seed_threads: dict[int, set[str]] = {idx: set() for idx in range(len(records))}
        for fmt, _, s_idx, th in fuzz_decode._iter_all_formats_schedule(
            30000, records, len(bmp_s), len(png_s), len(ppm_s)
        ):
            if fmt == "fold":
                per_seed_threads[s_idx].add(th)
        for idx, (sname, _, _, sm, sblob) in enumerate(records):
            s_note = f" (CM S={sblob[32]})" if sm == 1 else ""
            check(
                per_seed_threads[idx] == {"1", "3", "6", "9"},
                f"seed {idx} {sname}{s_note} missed thread counts: {per_seed_threads[idx]}",
            )


def compat_corpus() -> None:
    """Twenty-year compatibility corpus guard (testdata/compat/): every archived
    v2 and v1.0.0 vector matches its recorded SHA-256 and known-answer constant,
    and decodes to its exact pixel CRC-32 through both the independent oracle and
    the native decoder across thread counts."""
    compat_dir = ROOT / "testdata" / "compat"
    manifest_path = compat_dir / "manifest.json"
    check(manifest_path.is_file(), f"missing compatibility manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    check(manifest.get("schema") == 1 and manifest.get("frozen_at_tag") == "rasterfold-v1.0.0", f"unexpected manifest header: {manifest}")
    vectors = manifest.get("vectors") or []
    check(len(vectors) >= 12, f"expected >= 12 compatibility vectors, got {len(vectors)}")

    kat_map = {
        "KAT_FOLD": KAT_FOLD,
        "KAT_PAL": KAT_PAL,
        "KAT_PAL2": KAT_PAL2,
        "KAT_FOLD3": KAT_FOLD3,
        "KAT_PAL3": KAT_PAL3,
        "KAT_PAL2_3": KAT_PAL2_3,
        "KAT_PAL4": KAT_PAL4,
        "KAT_SC5": KAT_SC5,
        "KAT_SC6": KAT_SC6,
    }
    listed_paths = set()
    seen_pairs: set[tuple[int, int]] = set()
    work = WORK / "compat"
    work.mkdir(parents=True, exist_ok=True)

    for idx, vec in enumerate(vectors):
        rel = vec["path"]
        listed_paths.add(rel)
        fpath = compat_dir / rel
        check(fpath.is_file(), f"missing compatibility vector: {fpath}")
        blob = fpath.read_bytes()
        check(len(blob) == vec["size"], f"{rel}: size {len(blob)} != {vec['size']}")
        check(hashlib.sha256(blob).hexdigest() == vec["sha256"], f"{rel}: sha256 mismatch")
        check(blob[4] == vec["format_version"] and blob[16] == vec["method"], f"{rel}: header version/method mismatch")
        if vec["strips"] is not None:
            got_strips = 1 if blob[4] == 2 else blob[32]
            check(got_strips == vec["strips"], f"{rel}: strip count {got_strips} != {vec['strips']}")
        if vec["kat_constant"] is not None:
            check(blob == kat_map[vec["kat_constant"]], f"{rel}: bytes differ from {vec['kat_constant']}")
        seen_pairs.add((vec["format_version"], vec["method"]))

        ow, oh, orgb = rf.decode_image(blob)
        check((ow, oh) == (vec["width"], vec["height"]), f"{rel}: oracle dims {(ow, oh)} != {(vec['width'], vec['height'])}")
        got_crc = f"0x{zlib.crc32(orgb) & 0xFFFFFFFF:08x}"
        check(got_crc == vec["pixel_crc32"], f"{rel}: oracle pixel_crc32 {got_crc} != {vec['pixel_crc32']}")

        v_res = expect_ok(["verify", str(fpath)])
        check(f"pixel_crc32: {vec['pixel_crc32']}".encode("ascii") in v_res.stdout, f"{rel}: verify pixel_crc32 mismatch: {v_res.stdout!r}")
        for th in ("1", "3", "6", "9"):
            out_bmp = work / f"out_{idx}_t{th}.bmp"
            out_bmp.unlink(missing_ok=True)
            expect_ok(["decode", "--threads", th, str(fpath), str(out_bmp)])
            check(rf.read_bmp24(out_bmp.read_bytes()) == (ow, oh, orgb), f"{rel}: native decode pixels differ at --threads {th}")
            out_bmp.unlink(missing_ok=True)

        # While the live encoder targets format version 3, every vector marked
        # encoder_writes_in_v1_0_0 must still be produced byte-identically.
        if rf.VERSION == 3 and vec["encoder_writes_in_v1_0_0"]:
            check(rf.encode_image(ow, oh, orgb) == blob, f"{rel}: v3 oracle encode drifted from frozen v1.0.0 vector")
            src_ppm = work / f"src_{idx}.ppm"
            re_fold = work / f"re_{idx}.fold"
            src_ppm.write_bytes(f"P6\n{ow} {oh}\n255\n".encode("ascii") + orgb)
            re_fold.unlink(missing_ok=True)
            expect_ok(["encode", "--threads", "3", str(src_ppm), str(re_fold)])
            check(re_fold.read_bytes() == blob, f"{rel}: native v3 encode drifted from frozen v1.0.0 vector")
            src_ppm.unlink(missing_ok=True)
            re_fold.unlink(missing_ok=True)

    disk_paths = {p.relative_to(compat_dir).as_posix() for p in compat_dir.rglob("*.fold")}
    check(disk_paths == listed_paths, f"unlisted or missing .fold files in testdata/compat: disk={disk_paths ^ listed_paths}")
    want_pairs = {(2, 0), (2, 1), (2, 2), (2, 3), (3, 0), (3, 1), (3, 2), (3, 3), (3, 4), (3, 5), (3, 6)}
    check(want_pairs.issubset(seen_pairs), f"compatibility corpus missing version/method pairs: {want_pairs - seen_pairs}")


def gated_fixtures() -> dict[str, tuple[int, int, bytes]]:
    """D01 T04 §10's boundary fixtures for the version 3 gated gradient: narrow and short images whose planes
    wrap modulo 256, with deliberate misses and a noise band so method 1 is chosen."""
    rnd = random.Random(20261030)
    out = {}

    def make(name, w, h, plane_rows, plane_cols, f, misses):
        px = [[None] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                if y < plane_rows and x < plane_cols:
                    px[y][x] = f(x, y)
                else:
                    px[y][x] = tuple(rnd.randrange(256) for _ in range(3))
        for (x, y, c, d) in misses:
            p = list(px[y][x])
            p[c] = (p[c] + d) & 255
            px[y][x] = tuple(p)
        check(len({p for row in px for p in row}) > 256, f"{name}: fewer than 257 colours")
        out[name] = (w, h, bytes(b for row in px for p in row for b in p))

    def pl(x, y):
        return ((3 * x + 7 * y + 9) & 255, (5 * x + 3 * y + 200) & 255, (250 - 7 * x - 5 * y) & 255)

    make("w1", 1, 700, 500, 1, pl, [(0, 200, 0, 100)])
    make("h1", 700, 1, 1, 400, pl, [(200, 0, 1, 77)])
    make("w2", 2, 600, 400, 2, pl, [(1, 150, 2, 31), (0, 300, 0, 9)])
    make("h2", 600, 2, 2, 400, pl, [(150, 1, 0, 31), (300, 0, 2, 9)])
    make("w3", 3, 500, 300, 3, pl, [(2, 100, 1, 50), (0, 200, 0, 1)])
    make("sq", 40, 40, 40, 25, pl, [(10, 10, 0, 3), (0, 20, 1, 4), (20, 0, 2, 5)])

    # gl: R is a non-wrapping plane x + y with +100 glitches on odd rows at x % 4 == 0, so gated misses reach
    # the cap and drive Eg to its halving; G and B wrap.
    def gl(x, y):
        return ((x + y) & 255, (5 * x + 3 * y + 200) & 255, (250 - 7 * x - 5 * y) & 255)

    make("gl", 120, 120, 120, 100, gl, [(x, y, 0, 100) for y in range(1, 120, 2) for x in range(4, 100, 4)])
    # ge: in the top half, isolated +100 glitches on all three channels, spaced so their neighbours stay exact and
    # the gate stays open: R and B keep their plane values (minus G), so the default prediction hits where the raw
    # gradient misses and Eg alone reaches its halving. The clean bottom half then shows the halving's effect on
    # when the gradient is chosen again.
    make("ge", 60, 120, 120, 50, gl, [(x, y, c, 100) for y in range(1, 60, 3) for x in range(4, 50, 4) for c in range(3)])
    return out


GATED_BOUNDARIES = (
    "empty-start", "tie-keeps-pt", "margin-keeps-pt", "gp-chosen", "fold", "cap",
    "halve-by-eg", "halve-by-et", "closed", "open-col0", "open-row0",
)


def gated_gradient() -> None:
    rf.GG_STATS.clear()
    for name, (w, h, rgb) in gated_fixtures().items():
        roundtrip(f"gated-{name}", w, h, rgb)
        check((WORK / f"gated-{name}" / "out.fold").read_bytes()[16] == 1, f"gated-{name}: method is not 1")
    missing = [k for k in GATED_BOUNDARIES if not rf.GG_STATS.get(k)]
    check(not missing, f"gated-gradient boundaries never reached: {missing} (counts {rf.GG_STATS})")


def cli_commands() -> None:
    work = WORK / "cli"
    work.mkdir(parents=True, exist_ok=True)
    v3 = work / "kat3.fold"
    v3.write_bytes(KAT_FOLD3)
    v2 = work / "kat2.fold"
    v2.write_bytes(KAT_FOLD)
    want_hash = f"pixel_crc32: 0x{zlib.crc32(KAT_RGB) & 0xFFFFFFFF:08x}".encode()
    for path in (v3, v2):
        result = expect_ok(["verify", str(path)])
        check(want_hash in result.stdout, f"verify hash for {path}: {result.stdout!r}")
        check(b"status: ok" in result.stdout, f"verify status for {path}")
    # verify refuses a bad payload checksum and stream damage hidden behind
    # recomputed container checksums, prints no success report, and leaves
    # the input byte-identical.
    payload = bytearray(KAT_FOLD3[32:])
    payload[-1] ^= 0xFF
    bad_crc = work / "badcrc.fold"
    bad_crc.write_bytes(KAT_FOLD3[:32] + bytes(payload))
    variants = [
        bad_crc,
        work / "truncstream.fold",
        work / "trailstream.fold",
    ]
    # Truncated and trailing coded streams: adjust the last stream's size
    # in the payload's stream table so the container checks all pass and
    # the refusal comes from stream decoding itself.
    nstreams = KAT_FOLD3[32]
    size_at = 33 + 4 * (3 * nstreams - 1)
    last = struct.unpack_from("<I", KAT_FOLD3, size_at)[0]
    trunc = bytearray(KAT_FOLD3[:-1])
    struct.pack_into("<I", trunc, size_at, last - 1)
    trail = bytearray(KAT_FOLD3 + b"\x00")
    struct.pack_into("<I", trail, size_at, last + 1)
    variants[1].write_bytes(recrc(bytes(trunc)))
    variants[2].write_bytes(recrc(bytes(trail)))
    variant_diag = [(variants[0], b"invalid fold: payload checksum"),
                    (variants[1], b"invalid fold: cm stream"),
                    (variants[2], b"invalid fold: cm stream")]
    for path, diag in variant_diag:
        before = path.read_bytes()
        result = run(["verify", str(path)])
        check(result.returncode == 4, f"verify on {path.name} exited {result.returncode}, want 4: {result.stdout!r} {result.stderr!r}")
        check(result.stderr.strip() == b"rasterfold: " + diag, f"verify diagnostic for {path.name}: {result.stderr!r}")
        check(b"status: ok" not in result.stdout, f"verify reported success for {path.name}")
        check(path.read_bytes() == before, f"verify modified {path.name}")
    # inspect --json: one JSON object on stdout; --json --time is refused.
    result = expect_ok(["inspect", "--json", str(v3)])
    report = json.loads(result.stdout.decode())
    check(report["width"] == KAT_W and report["height"] == KAT_H, f"json dims: {report}")
    check(report["method"] == "cm" and report["version"] == 3, f"json method/version: {report}")
    check(report["status"] == "ok", f"json status: {report}")
    expect_fail(["inspect", "--json", "--time", str(v3)], 1, "usage")
    # help and version.
    for args, text in [
        (["help"], b"verify"),
        (["--help"], b"verify"),
        (["help", "encode"], b"usage: rasterfold encode [--time] [--threads N] [--overwrite] input.bmp|input.png|input.ppm output.fold"),
        (["help", "encode"], b"An existing output file is refused unless --overwrite is given."),
        (["help", "decode"], b"usage: rasterfold decode [--time] [--threads N] [--overwrite] input.fold output.bmp|output.png|output.ppm"),
        (["help", "decode"], b"An existing output file is refused unless --overwrite is given."),
        (["help"], b"  rasterfold encode [--time] [--threads N] [--overwrite] input.bmp|input.png|input.ppm output.fold"),
        (["help", "verify"], b"CRC32 of the decoded pixels"),
        (["version"], b"api version: 1.0"),
        (["--version"], b"format version written: 3"),
    ]:
        result = expect_ok(list(args))
        check(text in result.stdout, f"{args} missing {text!r}: {result.stdout!r}")
    expect_fail(["help", "bogus"], 1, "usage")
    # Extension dispatch: known formats in the wrong place report
    # unsupported (3); unknown extensions are usage errors (1).
    bmp = work / "pic.bmp"
    bmp.write_bytes(rf.write_bmp24(KAT_W, KAT_H, KAT_RGB))
    expect_fail(["encode", str(v3), str(work / "x.fold")], 3, "unsupported input format")
    expect_fail(["encode", str(bmp), str(work / "x.bmp")], 3, "unsupported output format")
    expect_fail(["encode", str(bmp), str(work / "x.gif")], 1, "usage")
    png_out = work / "x.png"
    png_out.unlink(missing_ok=True)
    expect_ok(["decode", str(v3), str(png_out)])
    with Image.open(png_out) as im:
        check(im.convert("RGB").tobytes() == KAT_RGB, "decode to PNG pixels differ from the BMP path")
    expect_fail(["inspect", str(bmp)], 3, "unsupported input format")
    expect_fail(["inspect", str(work / "x.gif")], 1, "usage")
    png_named = work / "in.png"
    png_named.write_bytes(bmp.read_bytes())
    expect_fail(["encode", str(png_named), str(work / "x2.fold")], 4, "invalid PNG")
    # Dotfiles classify by basename, identically under either spelling:
    # a leading dot is not an extension, so both reach the .fold reader
    # and fail as malformed .fold (4), not as an unsupported format (3).
    dotfile = work / ".bmp"
    dotfile.write_bytes(bmp.read_bytes())
    dotted = "./" + dotfile.relative_to(ROOT).as_posix()
    for spelling in (str(dotfile), dotted):
        result = run(["inspect", spelling])
        check(result.returncode == 4, f"inspect {spelling} exited {result.returncode}, want 4: {result.stderr!r}")
    if os.name != "nt":
        # On POSIX a backslash is a filename character, not a separator:
        # the unknown extension is a usage error (1).
        expect_fail(["inspect", "missing.fold\\x"], 1, "usage")


def png_io() -> None:
    work = WORK / "png"
    work.mkdir(parents=True, exist_ok=True)

    def chunk(typ: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + typ + data + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    def crafted(w: int, h: int, bitdepth: int, ctype: int, raw: bytes, extra: bytes = b"", interlace: int = 0) -> bytes:
        ihdr = struct.pack(">IIBBBBB", w, h, bitdepth, ctype, 0, 0, interlace)
        return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + extra + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")

    def fresh(path: Path) -> Path:
        path.unlink(missing_ok=True)
        Path(f"{path}.partial").unlink(missing_ok=True)
        return path

    def pillow_pixels(path: Path) -> bytes:
        with Image.open(path) as im:
            return im.convert("RGB").tobytes()

    w, h = 37, 23
    rgb = bytes(((x * 7 + y * 13 + c * 41) & 255) for y in range(h) for x in range(w) for c in range(3))
    src = work / "in.png"
    Image.frombytes("RGB", (w, h), rgb).save(src)
    fold = fresh(work / "in.fold")
    expect_ok(["encode", str(src), str(fold)])
    check(rf.decode_image(fold.read_bytes()) == (w, h, rgb), "PNG encode pixels differ from the oracle decode")
    # Not in.bmp: tests/work/*/in.bmp is the API corpus-fixture shape
    # (tests/test_api.py EXPECTED_FIXTURES), and this directory is the
    # PNG workspace, not a BMP corpus fixture.
    bmp = work / "from_bmp.bmp"
    bmp.write_bytes(rf.write_bmp24(w, h, rgb))
    fold_from_bmp = fresh(work / "in_bmp.fold")
    expect_ok(["encode", str(bmp), str(fold_from_bmp)])
    check(fold.read_bytes() == fold_from_bmp.read_bytes(), "PNG and BMP encode produced different .fold bytes")
    out_png = fresh(work / "out.png")
    expect_ok(["decode", str(fold), str(out_png)])
    check(pillow_pixels(out_png) == rgb, "written PNG re-reads with different pixels in Pillow")
    out_bmp = fresh(work / "out.bmp")
    expect_ok(["decode", str(fold), str(out_bmp)])
    check(rf.read_bmp24(out_bmp.read_bytes()) == (w, h, rgb), "PNG-sourced fold decodes differently to BMP")

    # Interlaced (Adam7) input succeeds with exact pixels. The fixture
    # is built pass by pass here, so its IHDR interlace byte is 1 by
    # construction (a Pillow save was verified to write interlace 0).
    def adam7_png(w: int, h: int, rgb: bytes, channels: int = 3) -> bytes:
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
                    px = rgb[((gy * w + gx) * channels):((gy * w + gx) * channels) + channels]
                    raw += px
        ihdr = struct.pack(">IIBBBBB", w, h, 8, 2 if channels == 3 else 6, 0, 0, 1)
        return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")

    for (iw, ih) in ((31, 17), (1, 1), (5, 3), (32, 32)):
        irgb = bytes(((x * 11 + y * 5 + c * 67) & 255) for y in range(ih) for x in range(iw) for c in range(3))
        isrc = work / f"interlaced_{iw}x{ih}.png"
        isrc.write_bytes(adam7_png(iw, ih, irgb))
        with Image.open(isrc) as im:
            check(im.convert("RGB").tobytes() == irgb, f"Pillow disagrees on the {iw}x{ih} Adam7 fixture")
        ifold = fresh(work / f"interlaced_{iw}x{ih}.fold")
        expect_ok(["encode", str(isrc), str(ifold)])
        check(rf.decode_image(ifold.read_bytes()) == (iw, ih, irgb), f"interlaced {iw}x{ih} PNG pixels differ")
    argb = bytes(((x * 3 + y * 17 + c * 29) & 255) for y in range(5) for x in range(7) for c in range(3))
    argba = bytearray()
    for i in range(0, len(argb), 3):
        argba += argb[i:i + 3] + b"\xff"
    asrc7 = work / "interlaced_rgba.png"
    asrc7.write_bytes(adam7_png(7, 5, bytes(argba), channels=4))
    afold7 = fresh(work / "interlaced_rgba.fold")
    expect_ok(["encode", str(asrc7), str(afold7)])
    check(rf.decode_image(afold7.read_bytes()) == (7, 5, argb), "interlaced RGBA PNG pixels differ")

    # Fully opaque RGBA is accepted; any other alpha is refused with 3.
    rgba = bytearray()
    for i in range(0, len(rgb), 3):
        rgba += rgb[i:i + 3] + b"\xff"
    asrc = work / "opaque.png"
    Image.frombytes("RGBA", (w, h), bytes(rgba)).save(asrc)
    afold = fresh(work / "opaque.fold")
    expect_ok(["encode", str(asrc), str(afold)])
    check(rf.decode_image(afold.read_bytes()) == (w, h, rgb), "opaque RGBA pixels differ")
    rgba[3] = 128
    nsrc = work / "alpha.png"
    Image.frombytes("RGBA", (w, h), bytes(rgba)).save(nsrc)
    nfold = fresh(work / "alpha.fold")
    expect_fail(["encode", str(nsrc), str(nfold)], 3, "alpha")
    check(not nfold.exists(), "non-opaque alpha produced an output file")

    def refusal(name: str, data: bytes, code: int, text: str) -> None:
        bad = work / name
        bad.write_bytes(data)
        dest = fresh(work / (name + ".fold"))
        expect_fail(["encode", str(bad), str(dest)], code, text)
        check(not dest.exists(), f"{name} produced an output file on refusal")

    raw2 = b"\x00" + bytes([255, 0, 0, 0, 255, 0]) + b"\x00" + bytes([0, 0, 255, 255, 255, 0])
    refusal("trns.png", crafted(2, 2, 8, 2, raw2, extra=chunk(b"tRNS", b"\x00\x00\x00\x00\x00\x00")), 3, "transparency")
    raw16 = b"\x00" + struct.pack(">HHH", 1000, 2000, 3000) * 2 + b"\x00" + struct.pack(">HHH", 4000, 5000, 6000) * 2
    refusal("depth16.png", crafted(2, 2, 16, 2, raw16), 3, "bit depth")
    refusal("gray.png", crafted(2, 2, 8, 0, b"\x00\x01\x02\x00\x03\x04"), 3, "colour type")
    good = crafted(2, 2, 8, 2, raw2)
    refusal("truncated.png", good[: len(good) // 2], 4, "invalid PNG")
    badcrc = bytearray(good)
    at = good.index(b"IDAT")
    badcrc[at + 6] ^= 0xFF
    refusal("badcrc.png", bytes(badcrc), 4, "chunk CRC")
    stream = bytearray(zlib.compress(raw2))
    stream[-1] ^= 0xFF
    ihdr = struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)
    badadler = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", bytes(stream)) + chunk(b"IEND", b"")
    refusal("badadler.png", badadler, 4, "Adler")
    badfilter_raw = b"\x05" + raw2[1:7] + b"\x00" + raw2[8:]
    refusal("badfilter.png", crafted(2, 2, 8, 2, badfilter_raw), 4, "filter")
    ihdr2 = struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0)
    refusal("critical.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr2) + chunk(b"ABCD", b"xx") + chunk(b"IDAT", zlib.compress(raw2)) + chunk(b"IEND", b""), 4, "unknown critical chunk")
    stream2 = zlib.compress(raw2)
    half = len(stream2) // 2
    split = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr2) + chunk(b"IDAT", stream2[:half])
             + chunk(b"tEXt", b"k\x00v") + chunk(b"IDAT", stream2[half:]) + chunk(b"IEND", b""))
    refusal("splitidat.png", split, 4, "IDAT sequence")
    refusal("ienddata.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr2) + chunk(b"IDAT", stream2) + chunk(b"IEND", b"x"), 4, "IEND")
    refusal("trailing.png", crafted(2, 2, 8, 2, raw2) + b"x", 4, "trailing data")
    refusal("depth3.png", crafted(2, 2, 3, 2, raw2), 4, "bit depth")
    # A zlib header declaring a window larger than 32K (CINFO 8) is
    # malformed even when FCHECK is recomputed to match.
    cmf = 0x88
    flg = 0
    while (cmf * 256 + flg) % 31 != 0:
        flg += 1
    badcinfo_stream = bytes([cmf, flg]) + stream2[2:]
    refusal("badcinfo.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr2) + chunk(b"IDAT", badcinfo_stream) + chunk(b"IEND", b""), 4, "invalid zlib")
    # A dynamic block whose literal code is oversubscribed (three
    # length-1 codes) must be refused while its tables are read.
    bits = bytearray()
    bitpos = [0]

    def put(value: int, n: int) -> None:
        for i in range(n):
            at = bitpos[0] // 8
            while at >= len(bits):
                bits.append(0)
            if (value >> i) & 1:
                bits[at] |= 1 << (bitpos[0] % 8)
            bitpos[0] += 1

    def put_code(code: int, n: int) -> None:
        rev = 0
        for i in range(n):
            rev = (rev << 1) | ((code >> i) & 1)
        put(rev, n)

    put(1, 1)          # BFINAL
    put(2, 2)          # BTYPE dynamic
    put(0, 5)          # HLIT 257
    put(0, 5)          # HDIST 1
    put(14, 4)         # HCLEN 18
    order = (16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15)
    cl_lengths = {1: 1, 18: 1}
    for sym in order[:18]:
        put(cl_lengths.get(sym, 0), 3)
    # Canonical CL codes at length 1: symbol 1 -> 0, symbol 18 -> 1.
    for _ in range(3):
        put_code(0, 1)             # literal lengths 1, 1, 1
    put_code(1, 1); put(127, 7)    # 138 zeros (11 + 127)
    put_code(1, 1); put(106, 7)    # 117 zeros (11 + 106): 255 in all
    over = bytes(bits)
    ihdr1 = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    refusal("overhuff.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr1) + chunk(b"IDAT", b"\x78\x9c" + over + b"\x00\x00\x00\x01") + chunk(b"IEND", b""), 4, "invalid deflate")

    # The same bit writer builds an incomplete literal code (two
    # length-2 codes, Kraft sum one half), which must also be refused.
    bits = bits[:0]
    bitpos[0] = 0
    put(1, 1); put(2, 2); put(0, 5); put(0, 5); put(14, 4)
    cl_lengths2 = {2: 1, 18: 1}
    for sym in order[:18]:
        put(cl_lengths2.get(sym, 0), 3)
    # Canonical CL codes at length 1: symbol 2 -> 0, symbol 18 -> 1.
    put_code(0, 1); put_code(0, 1)   # literal lengths 2, 2
    put_code(1, 1); put(127, 7)      # 138 zeros
    put_code(1, 1); put(107, 7)      # 118 zeros: 258 entries in all
    refusal("incomplete.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr1) + chunk(b"IDAT", b"\x78\x9c" + bytes(bits) + b"\x00\x00\x00\x01") + chunk(b"IEND", b""), 4, "invalid deflate")

    # A fixed-block stream whose back-reference reaches before the
    # first output byte must be refused.
    bits = bits[:0]
    bitpos[0] = 0
    put(1, 1); put(1, 2)             # BFINAL, BTYPE fixed
    put_code(0x30 + 65, 8)           # literal 'A' (fixed code)
    put_code(1, 7)                   # length symbol 257 (fixed code)
    put_code(1, 5)                   # distance symbol 1: distance 2 > 1 byte out
    refusal("badref.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr1) + chunk(b"IDAT", b"\x78\x9c" + bytes(bits) + b"\x00\x00\x00\x01") + chunk(b"IEND", b""), 4, "invalid deflate")

    # Every filter, exercised on crafted rows of a known image.
    fw, fh = 24, 9
    frgb = bytes(((x * 9 + y * 31 + c * 53) & 255) for y in range(fh) for x in range(fw) for c in range(3))

    def filtered_row(row: bytes, prev: bytes | None, ftype: int) -> bytes:
        out = bytearray()
        for x, v in enumerate(row):
            a = row[x - 3] if x >= 3 else 0
            b = prev[x] if prev else 0
            c = prev[x - 3] if (prev and x >= 3) else 0
            if ftype == 0: fv = v
            elif ftype == 1: fv = (v - a) & 255
            elif ftype == 2: fv = (v - b) & 255
            elif ftype == 3: fv = (v - (a + b) // 2) & 255
            else:
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                fv = (v - pr) & 255
            out.append(fv)
        return bytes(out)

    for ftype in range(5):
        rawf = bytearray()
        prev = None
        for yy in range(fh):
            row = frgb[yy * fw * 3:(yy + 1) * fw * 3]
            rawf.append(ftype)
            rawf += filtered_row(row, prev, ftype)
            prev = row
        fsrc = work / f"filter{ftype}.png"
        fsrc.write_bytes(crafted(fw, fh, 8, 2, bytes(rawf)))
        fdest = fresh(work / f"filter{ftype}.fold")
        expect_ok(["encode", str(fsrc), str(fdest)])
        check(rf.decode_image(fdest.read_bytes()) == (fw, fh, frgb), f"filter {ftype} pixels differ")

    # Stored, fixed-Huffman, and dynamic-Huffman inflate blocks.
    def block_type(stream: bytes) -> int:
        first = stream[2]
        return (first >> 1) & 3

    raw_fixed = b"".join(b"\x00" + bytes([yy * 20] * 72) for yy in range(fh))
    fixed_pixels = b"".join(bytes([yy * 20] * 72) for yy in range(fh))
    comp_stored = zlib.compressobj(0)
    stored_stream = comp_stored.compress(raw_fixed) + comp_stored.flush()
    comp_fixed = zlib.compressobj(6, zlib.DEFLATED, 15, 8, zlib.Z_FIXED)
    fixed_stream = comp_fixed.compress(raw_fixed) + comp_fixed.flush()
    big_raw = bytearray()
    rng_bytes = random.Random(42)
    big_rgb = bytes(((x * 9 + y * 31 + c * 53) & 255) for y in range(64) for x in range(64) for c in range(3))
    for yy in range(64):
        big_raw.append(0)
        big_raw += big_rgb[yy * 64 * 3:(yy + 1) * 64 * 3]
    comp_dyn = zlib.compressobj(9)
    dyn_stream = comp_dyn.compress(bytes(big_raw)) + comp_dyn.flush()
    for name, stream, exp_type, dims, pixels in (
        ("stored", stored_stream, 0, (24, fh), fixed_pixels),
        ("fixed", fixed_stream, 1, (24, fh), fixed_pixels),
        ("dynamic", dyn_stream, 2, (64, 64), big_rgb),
    ):
        check(block_type(stream) == exp_type, f"{name} fixture has block type {block_type(stream)}, want {exp_type}")
        bsrc = work / f"block_{name}.png"
        ihdr_b = struct.pack(">IIBBBBB", dims[0], dims[1], 8, 2, 0, 0, 0)
        bsrc.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr_b) + chunk(b"IDAT", stream) + chunk(b"IEND", b""))
        bdest = fresh(work / f"block_{name}.fold")
        expect_ok(["encode", str(bsrc), str(bdest)])
        check(rf.decode_image(bdest.read_bytes()) == (dims[0], dims[1], pixels), f"{name} block pixels differ")

    # Inflated data longer than the dimension-derived size is refused.
    refusal("toobig.png", crafted(2, 2, 8, 2, raw2 + raw2), 4, "invalid deflate")
    # Illegal depth/colour combinations are malformed, not unsupported.
    refusal("rgbdepth1.png", crafted(2, 2, 1, 2, raw2), 4, "depth and colour")
    refusal("index16.png", crafted(2, 2, 16, 3, raw2), 4, "depth and colour")
    # A suggested palette on an RGB image is accepted only when whole.
    ok_plte = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr2) + chunk(b"PLTE", b"\x01\x02\x03")
               + chunk(b"IDAT", zlib.compress(raw2)) + chunk(b"IEND", b""))
    psrc = work / "plte_ok.png"
    psrc.write_bytes(ok_plte)
    pdest = fresh(work / "plte_ok.fold")
    expect_ok(["encode", str(psrc), str(pdest)])
    refusal("plte_short.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr2) + chunk(b"PLTE", b"\x01") + chunk(b"IDAT", zlib.compress(raw2)) + chunk(b"IEND", b""), 4, "PLTE")
    refusal("plte_late.png", b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr2) + chunk(b"IDAT", zlib.compress(raw2)) + chunk(b"PLTE", b"\x01\x02\x03") + chunk(b"IEND", b""), 4, "PLTE")

    # Export across the writer's block bound: a 600x600 noise image's
    # filtered rows exceed 1 MiB, and the written PNG (dynamic blocks)
    # re-reads exactly; a tiny export uses a fixed block.
    nrgb = bytes(random.Random(7).randrange(256) for _ in range(600 * 600 * 3))
    nbmp = work / "big.bmp"
    nbmp.write_bytes(rf.write_bmp24(600, 600, nrgb))
    nfold = fresh(work / "big.fold")
    expect_ok(["encode", str(nbmp), str(nfold)])
    npng = fresh(work / "big.png")
    expect_ok(["decode", str(nfold), str(npng)])
    with Image.open(npng) as im:
        check(im.convert("RGB").tobytes() == nrgb, "multi-block PNG export pixels differ")
    ndata = npng.read_bytes()
    idat_at = ndata.index(b"IDAT")
    idat_len = struct.unpack(">I", ndata[idat_at - 4:idat_at])[0]
    idat = ndata[idat_at + 4:idat_at + 4 + idat_len]
    check(len(zlib.decompress(idat)) > 1 << 20, "multi-block fixture did not cross the block bound")
    check(block_type(idat) == 2, "multi-block export did not use dynamic blocks")
    tiny_png = fresh(work / "tiny.png")
    expect_ok(["decode", str(fold_from_bmp), str(tiny_png)])
    tdata = tiny_png.read_bytes()
    tidat_at = tdata.index(b"IDAT")
    tidat_len = struct.unpack(">I", tdata[tidat_at - 4:tidat_at])[0]
    check(block_type(tdata[tidat_at + 4:tidat_at + 4 + tidat_len]) in (0, 1), "tiny export used a dynamic block")
    

def ppm_io() -> None:
    work = WORK / "ppm"
    work.mkdir(parents=True, exist_ok=True)

    def fresh(path: Path) -> Path:
        path.unlink(missing_ok=True)
        Path(f"{path}.partial").unlink(missing_ok=True)
        return path

    w, h = 19, 11
    # First raster byte is whitespace (10): the parser must not eat it.
    rgb = bytes([10]) + bytes(((x * 11 + y * 17 + c * 29) & 255) for y in range(h) for x in range(w) for c in range(3))[1:]
    src = work / "in.ppm"
    Image.frombytes("RGB", (w, h), rgb).save(src)
    check(src.read_bytes().startswith(b"P6"), "Pillow did not write a P6 PPM")
    fold = fresh(work / "in.fold")
    expect_ok(["encode", str(src), str(fold)])
    check(rf.decode_image(fold.read_bytes()) == (w, h, rgb), "PPM encode pixels differ from the oracle decode")
    bmp = work / "from_bmp.bmp"
    bmp.write_bytes(rf.write_bmp24(w, h, rgb))
    fold_from_bmp = fresh(work / "in_bmp.fold")
    expect_ok(["encode", str(bmp), str(fold_from_bmp)])
    check(fold.read_bytes() == fold_from_bmp.read_bytes(), "PPM and BMP encode produced different .fold bytes")
    out_ppm = fresh(work / "out.ppm")
    expect_ok(["decode", str(fold), str(out_ppm)])
    with Image.open(out_ppm) as im:
        check(im.convert("RGB").tobytes() == rgb, "written PPM re-reads with different pixels in Pillow")
    # Existing destination is refused, bytes unchanged, no .partial left.
    keep = work / "keep.ppm"
    keep.write_bytes(b"KEEP")
    expect_fail(["decode", str(fold), str(keep)], 2, "exists")
    check(keep.read_bytes() == b"KEEP", "refused decode changed the destination")
    check(not Path(f"{keep}.partial").exists(), "refused decode left a .partial file")
    # Unsupported variants and malformed inputs keep their exit codes.
    p3 = work / "in_p3.ppm"
    p3.write_bytes(b"P3\n2 1\n255\n255 0 0 0 255 0\n")
    expect_fail(["encode", str(p3), str(fresh(work / "p3.fold"))], 3, "unsupported PPM: P3")
    bad_max = work / "in_max.ppm"
    bad_max.write_bytes(b"P6\n2 1\n65535\n" + bytes(12))
    expect_fail(["encode", str(bad_max), str(fresh(work / "max.fold"))], 3, "unsupported PPM: maxval")
    huge = work / "in_huge.ppm"
    huge.write_bytes(b"P6\n4294967296 1\n255\n")
    expect_fail(["encode", str(huge), str(fresh(work / "huge.fold"))], 5, "image exceeds size limit")
    trunc = work / "in_trunc.ppm"
    trunc.write_bytes(b"P6\n4 4\n255\n" + bytes(10))
    expect_fail(["encode", str(trunc), str(fresh(work / "trunc.fold"))], 4, "invalid PPM: truncated")



def overwrite_io() -> None:
    work = WORK / "overwrite"
    work.mkdir(parents=True, exist_ok=True)

    def fresh(path: Path) -> Path:
        path.unlink(missing_ok=True)
        Path(f"{path}.partial").unlink(missing_ok=True)
        return path

    w, h = 13, 7
    rgb = bytes(((x * 3 + y * 19 + c * 37) & 255) for y in range(h) for x in range(w) for c in range(3))
    # Not `in.bmp`: `tests/work/*/in.bmp` beside `out.fold` is the round-trip fixture pair tests/test_api.py
    # replays, and this case deliberately replaces `out.fold` with a different image.
    (work / "in.bmp").unlink(missing_ok=True)
    src = work / "first.bmp"
    src.write_bytes(rf.write_bmp24(w, h, rgb))
    fold = fresh(work / "out.fold")
    expect_ok(["encode", str(src), str(fold)])
    before = fold.read_bytes()
    # Default: an existing destination is refused and unchanged.
    expect_fail(["encode", str(src), str(fold)], 2, "exists")
    check(fold.read_bytes() == before, "refused encode changed the destination")
    # With --overwrite the destination is replaced with complete new output.
    rgb2 = bytes((255 - b) & 255 for b in rgb)
    src2 = work / "in2.bmp"
    src2.write_bytes(rf.write_bmp24(w, h, rgb2))
    expect_ok(["encode", "--overwrite", str(src2), str(fold)])
    check(fold.read_bytes() != before, "--overwrite did not replace the destination")
    check(rf.decode_image(fold.read_bytes()) == (w, h, rgb2), "overwritten .fold decodes to wrong pixels")
    check(not Path(f"{fold}.partial").exists(), "successful overwrite left a .partial file")
    # A failed overwrite leaves the destination bytes intact.
    bad = work / "bad.bmp"
    bad.write_bytes(b"not a bmp at all")
    expect_fail(["encode", "--overwrite", str(bad), str(fold)], 4, "invalid BMP")
    check(rf.decode_image(fold.read_bytes()) == (w, h, rgb2), "failed overwrite changed the destination")
    # --overwrite is refused on non-writing commands.
    expect_fail(["inspect", "--overwrite", str(fold)], 1, "usage")
    expect_fail(["--overwrite", "--help"], 1, "usage")
    expect_fail(["help", "--overwrite"], 1, "usage")
    expect_fail(["version", "--overwrite"], 1, "usage")
    # Decode replacement succeeds and reads back complete.
    out_bmp = fresh(work / "out.bmp")
    expect_ok(["decode", str(fold), str(out_bmp)])
    out_bmp.write_bytes(b"OLD")
    expect_ok(["decode", "--overwrite", str(fold), str(out_bmp)])
    check(rf.read_bmp24(out_bmp.read_bytes()) == (w, h, rgb2), "overwritten decode reads back wrong pixels")
    # A pre-existing .partial blocks the writer and is left untouched.
    partial = Path(f"{fold}.partial")
    partial.write_bytes(b"PARTIALKEEP")
    expect_fail(["encode", "--overwrite", str(src), str(fold)], 2, "temporary output already exists")
    check(partial.read_bytes() == b"PARTIALKEEP", "writer touched a pre-existing .partial")
    check(rf.decode_image(fold.read_bytes()) == (w, h, rgb2), "blocked overwrite changed the destination")
    partial.unlink()
    # Publication cannot start in a read-only directory: destination survives.
    ro_dir = work / "rodir"
    ro_dir.mkdir(exist_ok=True)
    ro_dest = ro_dir / "out.fold"
    ro_dest.write_bytes(before)
    if os.name != "nt":  # Windows chmod does not deny directory writes
        ro_dir.chmod(0o500)
        try:
            expect_fail(["encode", "--overwrite", str(src), str(ro_dest)], 2, "cannot write output")
            check(ro_dest.read_bytes() == before, "unwritable-directory overwrite changed the destination")
        finally:
            ro_dir.chmod(0o700)
    # A write failure after the .partial is created (LD_PRELOAD shim failing
    # writes to *.partial) preserves the destination and cleans the temporary.
    cc = str(ROOT / "tools" / "llvm-mingw-linux" / "bin" / "clang")
    shim = work / "wfail.so"
    shim_src = work / "wfail.c"
    shim_src.write_text(
        "#define _GNU_SOURCE\n#include <unistd.h>\n#include <string.h>\n#include <stdlib.h>\n#include <errno.h>\n#include <dlfcn.h>\n#include <stdio.h>\n#include <sys/stat.h>\n"
        "static ssize_t (*real_write)(int, const void*, size_t);\n"
        "static int (*real_fsync)(int);\n"
        "static int is_partial(int fd) { char fdpath[64]; snprintf(fdpath, sizeof fdpath, \"/proc/self/fd/%d\", fd); char target[512]; ssize_t m = readlink(fdpath, target, sizeof target - 1); if (m <= 0) return 0; target[m] = 0; return strstr(target, \".partial\") != 0; }\n"
        "int fsync(int fd) { if (!real_fsync) real_fsync = dlsym(RTLD_NEXT, \"fsync\"); if (getenv(\"RF_FAIL_FSYNC\") && is_partial(fd)) { errno = EIO; return -1; } if (getenv(\"RF_FAIL_DIRFSYNC\")) { struct stat st; if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { errno = EIO; return -1; } } return real_fsync(fd); }\n"
        "#include <stdarg.h>\n#include <sys/syscall.h>\n"
        "static long (*real_syscall)(long, ...);\n"
        "long syscall(long n, ...) { va_list ap; va_start(ap, n); long a1 = va_arg(ap, long), a2 = va_arg(ap, long), a3 = va_arg(ap, long), a4 = va_arg(ap, long), a5 = va_arg(ap, long), a6 = va_arg(ap, long); va_end(ap); if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, \"syscall\"); if (getenv(\"RF_FAIL_RENAME\") && (n == SYS_renameat2 || n == SYS_renameat)) { errno = EIO; return -1; } return real_syscall(n, a1, a2, a3, a4, a5, a6); }\n"
        "ssize_t write(int fd, const void *buf, size_t n) {\n"
        "    if (!real_write) real_write = dlsym(RTLD_NEXT, \"write\");\n"
        "    char fdpath[64]; snprintf(fdpath, sizeof fdpath, \"/proc/self/fd/%d\", fd);\n"
        "    char target[512]; ssize_t m = readlink(fdpath, target, sizeof target - 1);\n"
        "    if (m > 0) { target[m] = 0; if (getenv(\"RF_FAIL_WRITE\") && strstr(target, \".partial\")) { errno = EIO; return -1; } }\n"
        "    return real_write(fd, buf, n); }\n"
    )
    if os.name != "nt":
        built = subprocess.run([cc, "-shared", "-fPIC", "-o", str(shim), str(shim_src), "-ldl"], capture_output=True)
        check(built.returncode == 0, f"write-failure shim did not compile: {built.stderr!r}")
        dest_before = fold.read_bytes()
        for mode, env_extra in (("write", {"RF_FAIL_WRITE": "1"}), ("fsync", {"RF_FAIL_FSYNC": "1"}), ("rename", {"RF_FAIL_RENAME": "1"}), ("dirfsync", {"RF_FAIL_DIRFSYNC": "1"})):
            env = dict(os.environ, LD_PRELOAD=str(shim), **env_extra)
            result = subprocess.run([str(EXE), "encode", "--overwrite", str(src), str(fold)], capture_output=True, env=env)
            if mode == "dirfsync":
                # Directory flush fails only after publication: the completed
                # replacement stands, with complete new bytes and no temporary.
                check(result.returncode == 0, f"dirfsync overwrite exited {result.returncode}, want 0: {result.stderr!r}")
                check(rf.decode_image(fold.read_bytes()) == (w, h, rgb), "dirfsync overwrite did not publish complete new bytes")
                check(not Path(f"{fold}.partial").exists(), "dirfsync overwrite left its .partial behind")
                continue
            check(result.returncode == 2, f"{mode}-failed overwrite exited {result.returncode}, want 2: {result.stderr!r}")
            check(fold.read_bytes() == dest_before, f"{mode}-failed overwrite changed the destination")
            check(not Path(f"{fold}.partial").exists(), f"{mode}-failed overwrite left its .partial behind")
    # Writing the temporary in an unwritable location fails before publication.
    dest_snapshot = fold.read_bytes()
    expect_fail(["encode", "--overwrite", str(src), "/dev/rf-nonexistent-dir/out.fold"], 2, "cannot write output")
    check(fold.read_bytes() == dest_snapshot, "unwritable-location overwrite changed the destination")
    # A directory destination is refused and remains a directory.
    dir_dest = work / "destdir"
    dir_dest.mkdir(exist_ok=True)
    expect_fail(["encode", "--overwrite", str(src), str(dir_dest)], 2, "cannot write output")
    check(dir_dest.is_dir(), "directory destination was replaced")


def main() -> int:
    if not EXE.exists():
        print(f"missing {EXE}; run build.ps1 first")
        return 1
    WORK.mkdir(parents=True, exist_ok=True)
    case("oracle", oracle_self_check)
    case("cli-commands", cli_commands)
    case("cli-reference", cli_reference)
    case("png-io", png_io)
    case("ppm-io", ppm_io)
    case("overwrite-io", overwrite_io)
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
    case("gated-gradient", gated_gradient)
    case("padding", padding_roundtrip)
    case("acceptance", oracle_acceptance)
    case("corrupt", corrupt_cases)
    case("truncated-abort", truncated_stream_abort)
    case("tail-threshold", tail_threshold_roundtrip)
    case("warm-strips", warm_strips)
    case("palette-method", palette_method)
    case("palette-method4", palette_method4)
    case("screen-content-method5", screen_content_method5)
    case("screen-content-method6", screen_content_method6)
    case("compat-corpus", compat_corpus)

    case("fuzz-harness", fuzz_harness)
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
