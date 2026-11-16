#!/usr/bin/env python3
"""Differential reference, oracle, metadata, budget, and mutation tests for D10 T02 s1.

Drives host C11 test_basic (if RFC_HOST_BASIC_EXE is set) and isolated native
Windows x64 bin/converter/windows-x64/rfc_test_basic.exe (against NTFS and
restricted System32 PATH), cross-checking:
  - RasterFold (.fold) against independent Python oracle tests/rfi_ref.py
  - BMP/PNG/PNM/PAM against pure-Python decoders in tests/converter/reference.py
    and Pillow (flagging Pillow/zlib shared C lineage explicitly in test notes)
  - Bounded .fold limits_override preflight with rf_internal_alloc_peak == 0
  - Dual PNG low-budget RFC_ERR_OUT_OF_MEMORY routes (shared rf_png_load + libpng)
  - All 4 eXIf RGB8 PNG orientation outcomes (normal, preserve refusal, ack, strip)
  - Deterministic seeded mutation and truncation fuzzing without crash or hang
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import random
import shlex
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

from PIL import Image, ImageCms

ROOT = Path(__file__).resolve().parents[2]


def _load_mod(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_ref = _load_mod("_rfc_ref", ROOT / "tests" / "converter" / "reference.py")
_rfi_ref = _load_mod("_rf_oracle", ROOT / "tests" / "rfi_ref.py")

SAMPLE_GRAY8 = _ref.SAMPLE_GRAY8
SAMPLE_GRAY16 = _ref.SAMPLE_GRAY16
SAMPLE_RGB8 = _ref.SAMPLE_RGB8
SAMPLE_RGBA8 = _ref.SAMPLE_RGBA8
SAMPLE_RGB16 = _ref.SAMPLE_RGB16
SAMPLE_RGBA16 = _ref.SAMPLE_RGBA16

ALPHA_NONE = _ref.ALPHA_NONE
ALPHA_STRAIGHT = _ref.ALPHA_STRAIGHT

META_ITEM_ORIENTATION = 2


def ps_quote(val: str) -> str:
    return "'" + val.replace("'", "''") + "'"


def parse_kv(stdout: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in stdout.strip().splitlines():
        for tok in line.strip().split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                out[k] = v
    return out


def pack_rfbe(
    width: int,
    height: int,
    sample: int,
    alpha: int,
    bps: int,
    pixels: bytes,
    orientation: int = 1,
    icc: bytes = b"",
) -> bytes:
    hdr = struct.pack(
        "<4s9I",
        b"RFBE",
        width,
        height,
        sample,
        alpha,
        bps,
        orientation,
        len(icc),
        len(pixels),
        0,
    )
    return hdr + icc + pixels


def unpack_rfbd(blob: bytes) -> dict[str, object]:
    if len(blob) < 40 or blob[:4] != b"RFBD":
        raise AssertionError(f"invalid RFBD dump header len={len(blob)}")
    magic, w, h, sf, am, bps, row_b, ori, icc_len, pay_len = struct.unpack_from(
        "<4s9I", blob, 0
    )
    assert magic == b"RFBD"
    icc_start = 40
    pay_start = icc_start + icc_len
    assert pay_start + pay_len == len(blob)
    assert pay_len == row_b * h
    return {
        "width": w,
        "height": h,
        "sample": sf,
        "alpha": am,
        "bps": bps,
        "row_bytes": row_b,
        "orientation": ori,
        "icc": blob[icc_start:pay_start],
        "pixels": blob[pay_start:],
    }


def make_srgb_icc() -> bytes:
    prof = ImageCms.createProfile("sRGB")
    return ImageCms.ImageCmsProfile(prof).tobytes()


def png_insert_chunk_before_idat(png_bytes: bytes, tag: bytes, cdata: bytes) -> bytes:
    pos = 8
    while pos + 12 <= len(png_bytes):
        clen = struct.unpack_from(">I", png_bytes, pos)[0]
        ctype = png_bytes[pos + 4 : pos + 8]
        if ctype == b"IDAT":
            crc = zlib.crc32(tag + cdata) & 0xFFFFFFFF
            chunk = struct.pack(">I", len(cdata)) + tag + cdata + struct.pack(">I", crc)
            return png_bytes[:pos] + chunk + png_bytes[pos:]
        pos += 12 + clen
    raise AssertionError("PNG missing IDAT chunk")


def make_exif_orientation(orientation: int) -> bytes:
    return struct.pack(
        "<2sHIHHHIHHI",
        b"II",
        42,
        8,
        1,
        0x0112,
        3,
        1,
        orientation,
        0,
        0,
    )


def make_pillow_bmp_rgb8(w: int, h: int, seed: int) -> tuple[bytes, bytes]:
    rgb = bytes(
        ((x * 19 + y * 37 + c * 61 + seed) & 0xFF)
        for y in range(h)
        for x in range(w)
        for c in range(3)
    )
    im = Image.frombytes("RGB", (w, h), rgb)
    import io

    buf = io.BytesIO()
    im.save(buf, format="BMP")
    return buf.getvalue(), rgb


def make_pillow_bmp_gray8(w: int, h: int, seed: int) -> tuple[bytes, bytes]:
    gray = bytes(((x * 23 + y * 41 + seed) & 0xFF) for y in range(h) for x in range(w))
    im = Image.frombytes("L", (w, h), gray)
    import io

    buf = io.BytesIO()
    im.save(buf, format="BMP")
    return buf.getvalue(), gray


def make_pillow_pbm(w: int, h: int, ascii_p1: bool = False) -> tuple[bytes, bytes]:
    # Bits in gray8 left-aligned 1-bit: 0x00=black, 0x80=white
    pixels = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            white = ((x + y * 2) % 3) != 0
            pixels[y * w + x] = 0x80 if white else 0x00
    if ascii_p1:
        rows = []
        for y in range(h):
            # PBM bit 0 = white, bit 1 = black
            rows.append(
                " ".join("0" if pixels[y * w + x] else "1" for x in range(w))
            )
        blob = f"P1\n# Pillow/Netpbm polarity check\n{w} {h}\n" + "\n".join(rows) + "\n"
        return blob.encode("ascii"), bytes(pixels)
    im = Image.frombytes("L", (w, h), bytes(255 if p else 0 for p in pixels)).convert("1")
    import io

    buf = io.BytesIO()
    im.save(buf, format="PPM")
    return buf.getvalue(), bytes(pixels)


class BasicAdapterVerificationMixin:
    """Shared differential and adversarial test matrix executed on host or Windows."""

    def run_cli(self, args: list[str], timeout: float = 30.0) -> tuple[int, str, str]:
        raise NotImplementedError

    def work_dir(self) -> Path:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        raise NotImplementedError

    def test_differential_fold_bmp_png_pnm_pam_and_contracts(self) -> None:
        wd = self.work_dir()
        srgb_icc = make_srgb_icc()

        # 1) Independent oracle cross-check on .fold v3: Pillow PNG -> FOLD -> rfi_ref.decode_image
        bmp_bytes, rgb_src = make_pillow_bmp_rgb8(19, 13, seed=7)
        bmp_in = wd / "src_19x13.bmp"
        fold_out = wd / "out_19x13.fold"
        bmp_in.write_bytes(bmp_bytes)
        rc, out, err = self.run_cli(
            ["cli", "convert", "bmp", "rasterfold", self.to_exe_path(bmp_in),
             self.to_exe_path(fold_out), "preserve", "0", "0", "0"]
        )
        self.assertEqual(rc, 0, out + err)
        fold_bytes = fold_out.read_bytes()
        ow, oh, orgb = _rfi_ref.decode_image(fold_bytes)
        self.assertEqual((ow, oh), (19, 13))
        self.assertEqual(orgb, rgb_src)

        # Decode .fold via adapter dump and compare with rgb_src
        fold_dump = wd / "fold.rfbd"
        rc, out, err = self.run_cli(
            ["cli", "dump", "rasterfold", self.to_exe_path(fold_out),
             self.to_exe_path(fold_dump)]
        )
        self.assertEqual(rc, 0, out + err)
        fd = unpack_rfbd(fold_dump.read_bytes())
        self.assertEqual((fd["width"], fd["height"], fd["sample"]), (19, 13, SAMPLE_RGB8))
        self.assertEqual(fd["pixels"], rgb_src)

        # 2) Valid .fold v3 fixture (128x64, within RF_MAX_DIM/RF_MAX_PIXELS) rejected under
        # tighter limits_override BEFORE core decode/inspect (PEAK_ALLOC=0 in fresh process).
        w128, h64 = 128, 64
        raw128 = bytes(((i * 29 + 3) & 0xFF) for i in range(w128 * h64 * 3))
        rfbe_128 = wd / "in128.rfbe"
        fold_128 = wd / "valid128x64.fold"
        rfbe_128.write_bytes(pack_rfbe(w128, h64, SAMPLE_RGB8, ALPHA_NONE, 8, raw128))
        rc, out, err = self.run_cli(
            ["cli", "encode", "rasterfold", self.to_exe_path(rfbe_128),
             self.to_exe_path(fold_128), "preserve"]
        )
        self.assertEqual(rc, 0, out + err)
        # Re-verify encoded .fold with oracle
        self.assertEqual(_rfi_ref.decode_image(fold_128.read_bytes()), (w128, h64, raw128))

        limit_dump = wd / "limit_unused.rfbd"
        rc, out, err = self.run_cli(
            ["cli", "dump", "rasterfold", self.to_exe_path(fold_128),
             self.to_exe_path(limit_dump), "64", "0"]
        )
        self.assertNotEqual(rc, 0)
        kv = parse_kv(out)
        self.assertEqual(kv.get("STATUS"), "limit-exceeded", out)
        self.assertEqual(kv.get("PEAK_ALLOC"), "0", out)
        self.assertFalse(limit_dump.exists())

        limit_out = wd / "limit_unused.bmp"
        rc, out, err = self.run_cli(
            ["cli", "convert", "rasterfold", "bmp", self.to_exe_path(fold_128),
             self.to_exe_path(limit_out), "preserve", "0", "0", "4096"]
        )
        self.assertNotEqual(rc, 0)
        kv = parse_kv(out)
        self.assertEqual(kv.get("STATUS"), "limit-exceeded", out)
        self.assertEqual(kv.get("PEAK_ALLOC"), "0", out)
        self.assertFalse(limit_out.exists())

        # 3) BMP GRAY8, RGB8, RGBA8 (with A==0 non-zero RGB) differential verification
        g_bmp, g_pix = make_pillow_bmp_gray8(11, 7, seed=11)
        g_bmp_p = wd / "gray.bmp"
        g_bmp_dump = wd / "gray_bmp.rfbd"
        g_bmp_p.write_bytes(g_bmp)
        rc, out, err = self.run_cli(
            ["cli", "dump", "bmp", self.to_exe_path(g_bmp_p), self.to_exe_path(g_bmp_dump)]
        )
        self.assertEqual(rc, 0, out + err)
        gd = unpack_rfbd(g_bmp_dump.read_bytes())
        self.assertEqual((gd["sample"], gd["pixels"]), (SAMPLE_GRAY8, g_pix))

        rgba_w, rgba_h = 9, 5
        rgba_pix = bytearray(
            ((x * 17 + y * 31 + c * 59) & 0xFF)
            for y in range(rgba_h)
            for x in range(rgba_w)
            for c in range(4)
        )
        # Force A==0 with distinct RGB at (2, 1)
        off0 = (1 * rgba_w + 2) * 4
        rgba_pix[off0 : off0 + 4] = b"\x12\x34\x56\x00"
        rgba_rfbe = wd / "rgba.rfbe"
        rgba_bmp = wd / "out_rgba.bmp"
        rgba_rfbe.write_bytes(
            pack_rfbe(rgba_w, rgba_h, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, bytes(rgba_pix))
        )
        rc, out, err = self.run_cli(
            ["cli", "encode", "bmp", self.to_exe_path(rgba_rfbe),
             self.to_exe_path(rgba_bmp), "preserve"]
        )
        self.assertEqual(rc, 0, out + err)
        # Verify RGBA8 BMP in Pillow too!
        with Image.open(rgba_bmp) as im:
            self.assertEqual(im.mode, "RGBA")
            self.assertEqual(im.size, (rgba_w, rgba_h))
            self.assertEqual(im.tobytes(), bytes(rgba_pix))

        # Verify pure-Python decode_bmp on RGB8 encoded BMP
        rgb8_rfbe = wd / "rgb8.rfbe"
        rgb8_bmp = wd / "out_rgb8.bmp"
        rgb8_rfbe.write_bytes(pack_rfbe(19, 13, SAMPLE_RGB8, ALPHA_NONE, 8, rgb_src))
        rc, out, err = self.run_cli(
            ["cli", "encode", "bmp", self.to_exe_path(rgb8_rfbe),
             self.to_exe_path(rgb8_bmp), "preserve"]
        )
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(_ref.decode_bmp(rgb8_bmp.read_bytes()), (19, 13, 3, rgb_src))

        # 4) PNG: GRAY8, GRAY16, RGB8, RGBA8, RGB16, RGBA16, iCCP preserve/strip + 4 eXIf outcomes + 2 budget routes
        rgba16_w, rgba16_h = 7, 5
        rgba16_words = [
            ((x * 1031 + y * 2053 + c * 4099) & 0xFFFF)
            for y in range(rgba16_h)
            for x in range(rgba16_w)
            for c in range(4)
        ]
        rgba16_words[(1 * rgba16_w + 1) * 4 : (1 * rgba16_w + 1) * 4 + 4] = [
            0x1234, 0xABCD, 0x5678, 0x0000
        ]
        rgba16_bytes = b"".join(struct.pack("<H", v) for v in rgba16_words)
        rgba16_rfbe = wd / "rgba16.rfbe"
        rgba16_png = wd / "out_rgba16.png"
        rgba16_rfbe.write_bytes(
            pack_rfbe(rgba16_w, rgba16_h, SAMPLE_RGBA16, ALPHA_STRAIGHT, 16,
                      rgba16_bytes, icc=srgb_icc)
        )
        rc, out, err = self.run_cli(
            ["cli", "encode", "png", self.to_exe_path(rgba16_rfbe),
             self.to_exe_path(rgba16_png), "preserve"]
        )
        self.assertEqual(rc, 0, out + err)
        # Pure-Python PNG decoder independently reconstructs 16-bit RGBA including A==0 RGB!
        pw, ph, pch, pbps, pbytes = _ref.decode_png(rgba16_png.read_bytes())
        self.assertEqual((pw, ph, pch, pbps), (rgba16_w, rgba16_h, 4, 16))
        self.assertEqual(pbytes, rgba16_bytes)

        # Decode RGBA16 PNG back through adapter and verify ICC & pixels match
        rgba16_dump = wd / "rgba16.rfbd"
        rc, out, err = self.run_cli(
            ["cli", "dump", "png", self.to_exe_path(rgba16_png),
             self.to_exe_path(rgba16_dump)]
        )
        self.assertEqual(rc, 0, out + err)
        rd16 = unpack_rfbd(rgba16_dump.read_bytes())
        self.assertEqual(rd16["icc"], srgb_icc)
        self.assertEqual(rd16["pixels"], rgba16_bytes)

        # Strip ICC on encode
        rgba16_strip_png = wd / "out_rgba16_strip.png"
        rc, out, err = self.run_cli(
            ["cli", "encode", "png", self.to_exe_path(rgba16_rfbe),
             self.to_exe_path(rgba16_strip_png), "strip"]
        )
        self.assertEqual(rc, 0, out + err)
        rgba16_strip_dump = wd / "rgba16_strip.rfbd"
        rc, out, err = self.run_cli(
            ["cli", "dump", "png", self.to_exe_path(rgba16_strip_png),
             self.to_exe_path(rgba16_strip_dump)]
        )
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(unpack_rfbd(rgba16_strip_dump.read_bytes())["icc"], b"")

        # 4 eXIf RGB8 PNG orientation outcomes via rfc_scheduler (convert)
        base_png = _ref.encode_png(19, 13, 2, 8, rgb_src)
        exif1_png = wd / "exif1.png"
        exif6_png = wd / "exif6.png"
        exif1_png.write_bytes(png_insert_chunk_before_idat(base_png, b"eXIf", make_exif_orientation(1)))
        exif6_png.write_bytes(png_insert_chunk_before_idat(base_png, b"eXIf", make_exif_orientation(6)))

        o1_out = wd / "exif1_out.png"
        rc, out, err = self.run_cli(
            ["cli", "convert", "png", "png", self.to_exe_path(exif1_png),
             self.to_exe_path(o1_out), "preserve", "0", "0", "0"]
        )
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(parse_kv(out).get("META_OUTCOME"), "preserved")

        o6_fail = wd / "exif6_fail.png"
        rc, out, err = self.run_cli(
            ["cli", "convert", "png", "png", self.to_exe_path(exif6_png),
             self.to_exe_path(o6_fail), "preserve", "0", "0", "0"]
        )
        self.assertNotEqual(rc, 0)
        self.assertEqual(parse_kv(out).get("STATUS"), "metadata-policy")
        self.assertFalse(o6_fail.exists())

        o6_ack = wd / "exif6_ack.png"
        rc, out, err = self.run_cli(
            ["cli", "convert", "png", "png", self.to_exe_path(exif6_png),
             self.to_exe_path(o6_ack), "preserve", str(META_ITEM_ORIENTATION), "0", "0"]
        )
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(parse_kv(out).get("META_OUTCOME"), "unsupported-items-named")

        o6_strip = wd / "exif6_strip.png"
        rc, out, err = self.run_cli(
            ["cli", "convert", "png", "png", self.to_exe_path(exif6_png),
             self.to_exe_path(o6_strip), "strip", "0", "0", "0"]
        )
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(parse_kv(out).get("META_OUTCOME"), "stripped-explicitly")

        # Dual PNG low-budget RFC_ERR_OUT_OF_MEMORY routes
        rgb64 = bytes(((i * 37 + 11) & 0xFF) for i in range(64 * 64 * 3))
        png64_rgb8 = wd / "png64_rgb8.png"
        png64_rgb8.write_bytes(_ref.encode_png(64, 64, 2, 8, rgb64))
        rc, out, err = self.run_cli(
            ["cli", "budget-png-decode", self.to_exe_path(png64_rgb8), "20480"]
        )
        self.assertNotEqual(rc, 0)
        kv = parse_kv(out)
        self.assertEqual(kv.get("STATUS"), "out-of-memory", out)
        self.assertGreater(int(kv.get("REJECTED", "0")), 0, out)
        self.assertEqual(kv.get("CURR_ALLOC"), "0", out)

        rgb64_16 = b"".join(struct.pack("<H", (i * 257) & 0xFFFF) for i in range(64 * 64 * 3))
        png64_rgb16 = wd / "png64_rgb16.png"
        png64_rgb16.write_bytes(_ref.encode_png(64, 64, 2, 16, rgb64_16))
        rc, out, err = self.run_cli(
            ["cli", "budget-png-decode", self.to_exe_path(png64_rgb16), "28000"]
        )
        self.assertNotEqual(rc, 0)
        kv = parse_kv(out)
        self.assertEqual(kv.get("STATUS"), "out-of-memory", out)
        self.assertGreater(int(kv.get("REJECTED", "0")), 0, out)
        self.assertEqual(kv.get("CURR_ALLOC"), "0", out)

        # 5) PNM (P1 ASCII, P4 Pillow, P5 12-bit, P6 16-bit) & PAM (P7 RGB_ALPHA 16-bit)
        for ascii_p1 in (True, False):
            pbm_bytes, pbm_gray8 = make_pillow_pbm(11, 5, ascii_p1=ascii_p1)
            pbm_p = wd / f"pbm_{'p1' if ascii_p1 else 'p4'}.pbm"
            pbm_dump = wd / f"pbm_{'p1' if ascii_p1 else 'p4'}.rfbd"
            pbm_p.write_bytes(pbm_bytes)
            rc, out, err = self.run_cli(
                ["cli", "dump", "pnm", self.to_exe_path(pbm_p), self.to_exe_path(pbm_dump)]
            )
            self.assertEqual(rc, 0, out + err)
            pd = unpack_rfbd(pbm_dump.read_bytes())
            self.assertEqual((pd["sample"], pd["bps"], pd["pixels"]), (SAMPLE_GRAY8, 1, pbm_gray8))

        # Pure-Python P5 4-bit (maxval 15) left-alignment check
        raw4 = bytes(range(16)) + bytes(16 - i - 1 for i in range(4))
        left4 = bytes(v << 4 for v in raw4)
        p5_4 = _ref.encode_pnm(5, 4, 1, 15, raw4)
        p5_4_p = wd / "p5_4bit.pgm"
        p5_4_dump = wd / "p5_4bit.rfbd"
        p5_4_p.write_bytes(p5_4)
        rc, out, err = self.run_cli(
            ["cli", "dump", "pnm", self.to_exe_path(p5_4_p), self.to_exe_path(p5_4_dump)]
        )
        self.assertEqual(rc, 0, out + err)
        d4 = unpack_rfbd(p5_4_dump.read_bytes())
        self.assertEqual((d4["sample"], d4["bps"], d4["pixels"]), (SAMPLE_GRAY8, 4, left4))

        # Encode back to PNM from left4 and verify pure-Python reference decoder sees maxval 15 raw4
        p5_enc_rfbe = wd / "p5_4bit.rfbe"
        p5_enc_out = wd / "p5_4bit_out.pgm"
        p5_enc_rfbe.write_bytes(pack_rfbe(5, 4, SAMPLE_GRAY8, ALPHA_NONE, 4, left4))
        rc, out, err = self.run_cli(
            ["cli", "encode", "pnm", self.to_exe_path(p5_enc_rfbe),
             self.to_exe_path(p5_enc_out), "preserve"]
        )
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(_ref.decode_pnm(p5_enc_out.read_bytes()), (5, 4, 1, 15, raw4))

        # PAM P7 RGB_ALPHA 16-bit differential check
        pam_out = wd / "out_rgba16.pam"
        rc, out, err = self.run_cli(
            ["cli", "encode", "pam", self.to_exe_path(rgba16_rfbe),
             self.to_exe_path(pam_out), "strip"]
        )
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(
            _ref.decode_pam(pam_out.read_bytes()),
            (rgba16_w, rgba16_h, 4, 65535, "RGB_ALPHA", rgba16_bytes),
        )

        # Regression: 16-bit storage with bits_per_sample <= 8 refused in PNM and PAM
        g16_low_rfbe = wd / "g16_low.rfbe"
        g16_low_out = wd / "g16_low_unused.out"
        g16_low_rfbe.write_bytes(
            pack_rfbe(1, 1, SAMPLE_GRAY16, ALPHA_NONE, 8, struct.pack("<H", 0xFF00))
        )
        for fmt_name in ("pnm", "pam"):
            rc, out, err = self.run_cli(
                ["cli", "encode", fmt_name, self.to_exe_path(g16_low_rfbe),
                 self.to_exe_path(g16_low_out), "preserve"]
            )
            self.assertNotEqual(rc, 0, out + err)
            self.assertEqual(parse_kv(out).get("STATUS"), "unsupported-sample", out)
            self.assertFalse(g16_low_out.exists())

        # Regression: ICC-bearing PNG exceeding limits_override.max_width refuses cleanly
        icc_lim_out = wd / "icc_lim_unused.png"
        rc, out, err = self.run_cli(
            ["cli", "convert", "png", "png", self.to_exe_path(rgba16_png),
             self.to_exe_path(icc_lim_out), "preserve", "0", "4", "0"]
        )
        self.assertNotEqual(rc, 0, out + err)
        self.assertEqual(parse_kv(out).get("STATUS"), "limit-exceeded", out)
        self.assertFalse(icc_lim_out.exists())

    def test_seeded_mutations_and_malformed_refusals(self) -> None:
        wd = self.work_dir()
        rng = random.Random(20261116)
        _, rgb = make_pillow_bmp_rgb8(8, 8, seed=99)
        rfbe = wd / "f_base.rfbe"
        fold_p = wd / "f_base.fold"
        rfbe.write_bytes(pack_rfbe(8, 8, SAMPLE_RGB8, ALPHA_NONE, 8, rgb))
        rc, out, err = self.run_cli(
            ["cli", "encode", "rasterfold", self.to_exe_path(rfbe),
             self.to_exe_path(fold_p), "preserve"]
        )
        self.assertEqual(rc, 0, out + err)

        valid_fixtures = {
            "rasterfold": fold_p.read_bytes(),
            "bmp": make_pillow_bmp_rgb8(8, 8, seed=99)[0],
            "png": _ref.encode_png(8, 8, 2, 8, rgb),
            "pnm": _ref.encode_pnm(8, 8, 3, 255, rgb),
            "pam": _ref.encode_pam(8, 8, 3, 255, "RGB", rgb),
        }

        dump_out = wd / "fuzz.rfbd"
        for fmt, valid in valid_fixtures.items():
            # Systematic truncations (0, 1, half, -1 byte)
            for tlen in (0, 1, min(16, len(valid) - 1), len(valid) // 2, len(valid) - 1):
                fpath = wd / f"trunc_{fmt}_{tlen}.bin"
                fpath.write_bytes(valid[:tlen])
                if dump_out.exists():
                    dump_out.unlink()
                rc, _, _ = self.run_cli(
                    ["cli", "dump", fmt, self.to_exe_path(fpath), self.to_exe_path(dump_out)]
                )
                self.assertIn(rc, (0, 2), f"{fmt} trunc len={tlen} crashed rc={rc}")
                self.assertNotEqual(rc, 0, f"{fmt} trunc len={tlen} unexpectedly succeeded")

            # Seeded single-byte and trailing-byte mutations
            for idx in range(4):
                mut = bytearray(valid)
                pos = rng.randrange(len(mut))
                mut[pos] ^= rng.randrange(1, 256)
                if idx == 3:
                    mut.extend(b"\xAB\xCD")
                fpath = wd / f"mut_{fmt}_{idx}.bin"
                fpath.write_bytes(bytes(mut))
                if dump_out.exists():
                    dump_out.unlink()
                rc, _, _ = self.run_cli(
                    ["cli", "dump", fmt, self.to_exe_path(fpath), self.to_exe_path(dump_out)]
                )
                self.assertIn(rc, (0, 2), f"{fmt} mut idx={idx} crashed rc={rc}")


class HostBasicTests(BasicAdapterVerificationMixin, unittest.TestCase):
    host_exe: Path | None = None
    _tmp: tempfile.TemporaryDirectory[str] | None = None
    _wd: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_BASIC_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_BASIC_EXE not configured")
        cls.host_exe = Path(raw)
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_basic_host_")
        cls._wd = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._tmp is not None:
            cls._tmp.cleanup()

    def work_dir(self) -> Path:
        assert self._wd is not None
        return self._wd

    def to_exe_path(self, p: Path) -> str:
        return str(p)

    def run_cli(self, args: list[str], timeout: float = 30.0) -> tuple[int, str, str]:
        assert self.host_exe is not None
        res = _ref.run_process_tree(
            [str(self.host_exe), *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return res.returncode, res.stdout or "", res.stderr or ""


class WindowsBasicTests(BasicAdapterVerificationMixin, unittest.TestCase):
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_BASIC_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_basic.exe")
        if not cand.is_file():
            if os.environ.get("RFC_REQUIRE_WIN_BASIC") == "1":
                raise AssertionError(f"missing Windows rfc_test_basic.exe: {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_basic.exe not built")
        cls.win_exe = cand

        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_basic_win_")
            cls._py_temp = Path(cls._win_temp)
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_basic.exe")
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_basic_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True,
                text=True,
                timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(
                ["wslpath", "-u", cls._win_temp],
                capture_output=True,
                text=True,
                timeout=15.0,
            )
            assert wp.returncode == 0
            cls._py_temp = Path(wp.stdout.strip())
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_basic.exe")

    @classmethod
    def tearDownClass(cls) -> None:
        if not cls._win_temp:
            return
        if os.name == "nt":
            shutil.rmtree(cls._win_temp, ignore_errors=True)
        else:
            _ref.run_process_tree(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    f"if (Test-Path {ps_quote(cls._win_temp)}) {{ Remove-Item -LiteralPath {ps_quote(cls._win_temp)} -Recurse -Force }}",
                ],
                capture_output=True,
                timeout=30.0,
            )

    def work_dir(self) -> Path:
        assert self._py_temp is not None
        return self._py_temp

    def to_exe_path(self, p: Path) -> str:
        rel = p.relative_to(self.work_dir())
        return self._win_temp.rstrip("\\/") + "\\" + str(rel).replace("/", "\\")

    def run_cli(self, args: list[str], timeout: float = 30.0) -> tuple[int, str, str]:
        if os.name == "nt":
            exe = str(self.work_dir() / "rfc_test_basic.exe")
            env = dict(os.environ)
            env["PATH"] = f"{os.environ.get('SystemRoot', r'C:\\Windows')}\\System32;{os.environ.get('SystemRoot', r'C:\\Windows')}"
            res = _ref.run_process_tree(
                [exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True, timeout=timeout
            )
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_basic.exe")
        cmd = (
            "$env:PATH = \"$env:SystemRoot\\System32;$env:SystemRoot\"; "
            f"& {exe_ps} {arg_ps}; exit $LASTEXITCODE"
        )
        res = _ref.run_process_tree(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", cmd],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return res.returncode, res.stdout or "", res.stderr or ""


if __name__ == "__main__":
    unittest.main()
