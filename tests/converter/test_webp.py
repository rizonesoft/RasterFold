#!/usr/bin/env python3
"""Differential, oracle, metadata, budget, and mutation tests for D10 T02 s2 (WebP).

Drives host C11 test_webp (if RFC_HOST_WEBP_EXE is set) and isolated native
Windows x64 bin/converter/windows-x64/rfc_test_webp.exe (on NTFS under
restricted System32 PATH), cross-checking:
  - Registry schema validation for WebP metadata_items ([icc, orientation, exif, xmp])
  - Lossless RGB8, RGBA8 (A == 0 exact RGB preservation), and all-opaque RGBA8
  - Independent clean-room FFmpeg libavcodec WebP decoder verification +
    shared-upstream-library Pillow cross-check
  - Lossy VP8 import & animated WebP refusal
  - ICCP / EXIF / XMP preserve, strip, and cross-format PNG/RASTERFOLD policy jobs
  - Limits, budget OOM enforcement, and seeded random truncation/mutation fuzzing
"""
from __future__ import annotations

import importlib.util
import io
import json
import os
from pathlib import Path
import random
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
_cc = _load_mod("_rfc_check_conv", ROOT / "scripts" / "check-converter.py")

SAMPLE_RGB8 = _ref.SAMPLE_RGB8
SAMPLE_RGBA8 = _ref.SAMPLE_RGBA8
ALPHA_NONE = _ref.ALPHA_NONE
ALPHA_STRAIGHT = _ref.ALPHA_STRAIGHT

META_ITEM_ORIENTATION = 2
META_ITEM_EXIF = 8
META_ITEM_XMP = 16
PNG_UNSUPPORTED_FROM_WEBP_META = META_ITEM_ORIENTATION | META_ITEM_EXIF | META_ITEM_XMP


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


def pack_rfwe(
    width: int,
    height: int,
    sample: int,
    alpha: int,
    bps: int,
    pixels: bytes,
    orientation: int = 1,
    icc: bytes = b"",
    exif: bytes = b"",
    xmp: bytes = b"",
) -> bytes:
    hdr = struct.pack(
        "<4s11I",
        b"RFWE",
        width,
        height,
        sample,
        alpha,
        bps,
        0,
        orientation,
        len(icc),
        len(exif),
        len(xmp),
        len(pixels),
    )
    return hdr + icc + exif + xmp + pixels


def unpack_rfwd(blob: bytes) -> dict[str, object]:
    if len(blob) < 48 or blob[:4] != b"RFWD":
        raise AssertionError(f"invalid RFWD dump header len={len(blob)}")
    (
        magic,
        w,
        h,
        sf,
        am,
        bps,
        row_b,
        ori,
        icc_len,
        exif_len,
        xmp_len,
        pay_len,
    ) = struct.unpack_from("<4s11I", blob, 0)
    assert magic == b"RFWD"
    off = 48
    icc = blob[off : off + icc_len]
    off += icc_len
    exif = blob[off : off + exif_len]
    off += exif_len
    xmp = blob[off : off + xmp_len]
    off += xmp_len
    assert off + pay_len == len(blob)
    assert pay_len == row_b * h
    return {
        "width": w,
        "height": h,
        "sample": sf,
        "alpha": am,
        "bps": bps,
        "row_bytes": row_b,
        "orientation": ori,
        "icc": icc,
        "exif": exif,
        "xmp": xmp,
        "pixels": blob[off:],
    }


def make_srgb_icc() -> bytes:
    prof = ImageCms.createProfile("sRGB")
    return ImageCms.ImageCmsProfile(prof).tobytes()


def make_exif_be_orientation(orientation: int) -> bytes:
    return struct.pack(
        ">2sHIHHHIHHI",
        b"MM",
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


def make_rgba_exact_pixels(w: int, h: int, seed: int, all_opaque: bool = False) -> bytes:
    out = bytearray()
    for y in range(h):
        for x in range(w):
            v = (x * 31 + y * 57 + seed * 19) & 0xFF
            if all_opaque:
                a = 255
            elif (x + y) % 4 == 0:
                a = 0
            elif (x + y) % 4 == 1:
                a = 123
            else:
                a = 255
            out.extend((v, (v ^ 0x5A) & 0xFF, (v * 3 + 7) & 0xFF, a))
    return bytes(out)


def make_rgb_pixels(w: int, h: int, seed: int) -> bytes:
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out.append((x * 17 + y * 29 + seed) & 0xFF)
            out.append((x * 41 + y * 13 + seed * 3) & 0xFF)
            out.append((x * 67 + y * 7 + seed * 5) & 0xFF)
    return bytes(out)


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


class WebPRegistrySchemaTests(unittest.TestCase):
    def test_validate_registry_live_and_negative_metadata_items(self) -> None:
        self.assertEqual(_cc.validate_registry(), 0)
        base = json.loads((ROOT / "converter" / "codecs.json").read_text(encoding="utf-8"))
        webp_entry = next(f for f in base["formats"] if f["format"] == "webp")
        self.assertEqual(webp_entry["availability"], "usable")
        self.assertEqual(
            webp_entry["metadata_items"],
            ["icc", "orientation", "exif", "xmp"],
        )

        with tempfile.TemporaryDirectory(prefix="rfc_webp_reg_") as td:
            for label, bad_items in (
                ("duplicate", ["icc", "icc"]),
                ("order", ["exif", "icc"]),
                ("unknown", ["icc", "iptc"]),
            ):
                mut = json.loads(json.dumps(base))
                mut_webp = next(f for f in mut["formats"] if f["format"] == "webp")
                mut_webp["metadata_items"] = bad_items
                p = Path(td) / f"codecs_{label}.json"
                p.write_text(json.dumps(mut), encoding="utf-8")
                buf = io.StringIO()
                old_err = sys.stderr
                sys.stderr = buf
                try:
                    rc = _cc.validate_registry(str(p))
                finally:
                    sys.stderr = old_err
                self.assertNotEqual(rc, 0, f"expected failure for {label}: {bad_items}")


class WebPAdapterVerificationMixin:
    def run_cli(self, args: list[str], timeout: float = 30.0) -> tuple[int, str, str]:
        raise NotImplementedError

    def work_dir(self) -> Path:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        raise NotImplementedError

    def test_differential_lossless_lossy_and_independent_oracles(self) -> None:
        wd = self.work_dir()
        ff_info = _ref.probe_ffmpeg_webp_decoder()
        self.assertTrue(
            ff_info["available"] and ff_info["independent"],
            f"expected clean-room FFmpeg WebP decoder: {ff_info}",
        )

        # 1) Lossless RGB8 + 1v4 thread determinism + independent FFmpeg check + Pillow cross-check
        rgb_w, rgb_h = 23, 17
        rgb_px = make_rgb_pixels(rgb_w, rgb_h, 11)
        rfwe_rgb = wd / "rgb.rfwe"
        rfwe_rgb.write_bytes(pack_rfwe(rgb_w, rgb_h, SAMPLE_RGB8, ALPHA_NONE, 8, rgb_px))
        webp_t1 = wd / "rgb_t1.webp"
        webp_t4 = wd / "rgb_t4.webp"

        rc, out1, err1 = self.run_cli(
            ["cli", "encode", "webp", self.to_exe_path(rfwe_rgb), self.to_exe_path(webp_t1), "preserve", "1"]
        )
        self.assertEqual(rc, 0, out1 + err1)
        rc, out4, err4 = self.run_cli(
            ["cli", "encode", "webp", self.to_exe_path(rfwe_rgb), self.to_exe_path(webp_t4), "preserve", "4"]
        )
        self.assertEqual(rc, 0, out4 + err4)
        self.assertEqual(webp_t1.read_bytes(), webp_t4.read_bytes(), "1 vs 4 thread WebP output differed")

        # Independent clean-room oracle: FFmpeg libavcodec WebP decoder
        ff_rgb = _ref.decode_webp_ffmpeg(webp_t1, pix_fmt="rgb24")
        self.assertEqual(ff_rgb, rgb_px)
        # Shared-upstream-library cross-check: Pillow libwebp
        with Image.open(webp_t1) as im:
            self.assertEqual(im.format, "WEBP")
            self.assertEqual((im.width, im.height, im.mode), (rgb_w, rgb_h, "RGB"))
            self.assertEqual(im.tobytes(), rgb_px)

        dump_rgb = wd / "rgb.rfwd"
        rc, out_d, err_d = self.run_cli(
            ["cli", "dump", "webp", self.to_exe_path(webp_t1), self.to_exe_path(dump_rgb)]
        )
        self.assertEqual(rc, 0, out_d + err_d)
        d_rgb = unpack_rfwd(dump_rgb.read_bytes())
        self.assertEqual(d_rgb["sample"], SAMPLE_RGB8)
        self.assertEqual(d_rgb["pixels"], rgb_px)

        # 2) Lossless RGBA8 with A == 0 non-zero RGB exactness + all-opaque RGBA8
        for label, all_opaque in (("a0_exact", False), ("all_opaque", True)):
            w, h = 19, 13
            px = make_rgba_exact_pixels(w, h, 31 if not all_opaque else 47, all_opaque=all_opaque)
            rfwe_p = wd / f"{label}.rfwe"
            webp_p = wd / f"{label}.webp"
            rfwd_p = wd / f"{label}.rfwd"
            rfwe_p.write_bytes(pack_rfwe(w, h, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, px))
            rc, eo, ee = self.run_cli(
                ["cli", "encode", "webp", self.to_exe_path(rfwe_p), self.to_exe_path(webp_p), "preserve"]
            )
            self.assertEqual(rc, 0, eo + ee)
            rc, do, de = self.run_cli(
                ["cli", "dump", "webp", self.to_exe_path(webp_p), self.to_exe_path(rfwd_p)]
            )
            self.assertEqual(rc, 0, do + de)
            dr = unpack_rfwd(rfwd_p.read_bytes())
            self.assertEqual(dr["sample"], SAMPLE_RGBA8)
            self.assertEqual(dr["alpha"], ALPHA_STRAIGHT)
            self.assertEqual(dr["pixels"], px)
            with Image.open(webp_p) as im:
                self.assertEqual(im.mode, "RGBA")
                self.assertEqual(im.tobytes(), px)
            ff_rgba = _ref.decode_webp_ffmpeg(webp_p, pix_fmt="rgba")
            self.assertEqual(len(ff_rgba), len(px))
            # FFmpeg libavcodec WebP decodes opaque pixels cleanly and lossless RGBA identically
            self.assertEqual(ff_rgba, px)

        # 3) Lossy VP8 (RGB8) and VP8+ALPH (RGBA8) import from Pillow
        for mode, sf in (("RGB", SAMPLE_RGB8), ("RGBA", SAMPLE_RGBA8)):
            lossy_src = wd / f"lossy_{mode.lower()}.webp"
            w, h = 24, 16
            raw = make_rgb_pixels(w, h, 5) if mode == "RGB" else make_rgba_exact_pixels(w, h, 5)
            im = Image.frombytes(mode, (w, h), raw)
            im.save(lossy_src, format="WEBP", lossless=False, quality=80)
            with Image.open(lossy_src) as dec_im:
                expected_lossy = dec_im.convert(mode).tobytes()
            dump_l = wd / f"lossy_{mode.lower()}.rfwd"
            rc, lo, le = self.run_cli(
                ["cli", "dump", "webp", self.to_exe_path(lossy_src), self.to_exe_path(dump_l)]
            )
            self.assertEqual(rc, 0, lo + le)
            dl = unpack_rfwd(dump_l.read_bytes())
            self.assertEqual(dl["sample"], sf)
            self.assertEqual(dl["pixels"], expected_lossy)
            # Confirm independent FFmpeg decodes the same lossy WebP cleanly to matching dimensions/byte count
            ff_l = _ref.decode_webp_ffmpeg(
                lossy_src, pix_fmt=("rgb24" if mode == "RGB" else "rgba")
            )
            self.assertEqual(len(ff_l), len(expected_lossy))

        # 4) Animated Pillow WebP refusal (RFC_ERR_UNSUPPORTED_COMBINATION)
        anim_p = wd / "anim.webp"
        f0 = Image.new("RGBA", (12, 12), (20, 40, 60, 255))
        f1 = Image.new("RGBA", (12, 12), (80, 100, 120, 200))
        f0.save(anim_p, format="WEBP", save_all=True, append_images=[f1], duration=50, loop=0)
        rc, po, _ = self.run_cli(["cli", "probe", "webp", self.to_exe_path(anim_p)])
        self.assertEqual(rc, 2)
        self.assertEqual(parse_kv(po)["STATUS"], "unsupported-combination")
        self.assertEqual(parse_kv(po)["LIVE"], "0")
        rc, do, _ = self.run_cli(
            ["cli", "dump", "webp", self.to_exe_path(anim_p), self.to_exe_path(wd / "anim_none.rfwd")]
        )
        self.assertEqual(rc, 2)
        self.assertEqual(parse_kv(do)["STATUS"], "unsupported-combination")

    def test_metadata_preserve_strip_exif_xmp_and_cross_format(self) -> None:
        wd = self.work_dir()
        icc = make_srgb_icc()
        exif = make_exif_be_orientation(6)
        xmp = b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF/></x:xmpmeta>'
        w, h = 10, 8
        px = make_rgb_pixels(w, h, 99)

        rfwe_meta = wd / "full_meta.rfwe"
        rfwe_meta.write_bytes(
            pack_rfwe(
                w,
                h,
                SAMPLE_RGB8,
                ALPHA_NONE,
                8,
                px,
                orientation=6,
                icc=icc,
                exif=exif,
                xmp=xmp,
            )
        )
        webp_full = wd / "full_meta.webp"
        rc, eo, ee = self.run_cli(
            ["cli", "encode", "webp", self.to_exe_path(rfwe_meta), self.to_exe_path(webp_full), "preserve"]
        )
        self.assertEqual(rc, 0, eo + ee)

        rc, po, pe = self.run_cli(["cli", "probe", "webp", self.to_exe_path(webp_full)])
        self.assertEqual(rc, 0, po + pe)
        pkv = parse_kv(po)
        self.assertEqual(pkv["ICC_LEN"], str(len(icc)))
        self.assertEqual(pkv["EXIF_LEN"], str(len(exif)))
        self.assertEqual(pkv["XMP_LEN"], str(len(xmp)))
        self.assertEqual(pkv["ORIENTATION"], "6")
        self.assertEqual(pkv["LIVE"], "0")

        with Image.open(webp_full) as im:
            self.assertEqual(im.info.get("icc_profile"), icc)
            self.assertEqual(im.info.get("exif"), exif)
            self.assertEqual(im.info.get("xmp"), xmp)

        # metadata=strip produces bare VP8L
        webp_strip = wd / "strip_meta.webp"
        rc, so, se = self.run_cli(
            ["cli", "encode", "webp", self.to_exe_path(rfwe_meta), self.to_exe_path(webp_strip), "strip"]
        )
        self.assertEqual(rc, 0, so + se)
        self.assertEqual(webp_strip.read_bytes()[12:16], b"VP8L")
        rc, spo, _ = self.run_cli(["cli", "probe", "webp", self.to_exe_path(webp_strip)])
        self.assertEqual(rc, 0)
        spkv = parse_kv(spo)
        self.assertEqual(spkv["ICC_LEN"], "0")
        self.assertEqual(spkv["EXIF_LEN"], "0")
        self.assertEqual(spkv["XMP_LEN"], "0")
        self.assertEqual(spkv["ORIENTATION"], "0")

        # Cross-format: WebP(ICC+EXIF+XMP+Ori) -> PNG refuses Ori+EXIF+XMP (26),
        # partial ack (XMP only) still refuses, full ack=26 succeeds
        out_png_refuse = wd / "refused.png"
        rc, co, _ = self.run_cli(
            [
                "cli", "convert", "webp", "png",
                self.to_exe_path(webp_full), self.to_exe_path(out_png_refuse),
                "preserve", "0", "0", "0",
            ]
        )
        self.assertEqual(rc, 2)
        ckv = parse_kv(co)
        self.assertEqual(ckv["STATUS"], "metadata-policy")
        self.assertEqual(int(ckv["UNSUPPORTED_ITEMS"]), PNG_UNSUPPORTED_FROM_WEBP_META)
        self.assertFalse(out_png_refuse.exists())

        rc, co_part, _ = self.run_cli(
            [
                "cli", "convert", "webp", "png",
                self.to_exe_path(webp_full), self.to_exe_path(out_png_refuse),
                "preserve", str(META_ITEM_XMP), "0", "0",
            ]
        )
        self.assertEqual(rc, 2)
        self.assertEqual(parse_kv(co_part)["STATUS"], "metadata-policy")
        self.assertFalse(out_png_refuse.exists())

        out_png_ok = wd / "acked.png"
        rc, co2, ce2 = self.run_cli(
            [
                "cli", "convert", "webp", "png",
                self.to_exe_path(webp_full), self.to_exe_path(out_png_ok),
                "preserve", str(PNG_UNSUPPORTED_FROM_WEBP_META), "0", "0",
            ]
        )
        self.assertEqual(rc, 0, co2 + ce2)
        self.assertTrue(out_png_ok.is_file())

        # PNG(iCCP + eXIf Ori=6) -> WebP preserve + RASTERFOLD strip verified with rfi_ref oracle
        base_png_buf = io.BytesIO()
        Image.frombytes("RGB", (w, h), px).save(base_png_buf, format="PNG", icc_profile=icc)
        png_with_exif = png_insert_chunk_before_idat(base_png_buf.getvalue(), b"eXIf", exif)
        in_png = wd / "src_icc_exif.png"
        in_png.write_bytes(png_with_exif)
        out_webp = wd / "from_png.webp"
        rc, po2, pe2 = self.run_cli(
            [
                "cli", "convert", "png", "webp",
                self.to_exe_path(in_png), self.to_exe_path(out_webp),
                "preserve", "0", "0", "0",
            ]
        )
        self.assertEqual(rc, 0, po2 + pe2)
        self.assertEqual(parse_kv(po2)["META_OUTCOME"], "preserved")

        out_fold = wd / "from_webp.fold"
        rc, fo, fe = self.run_cli(
            [
                "cli", "convert", "webp", "rasterfold",
                self.to_exe_path(out_webp), self.to_exe_path(out_fold),
                "strip", "0", "0", "0",
            ]
        )
        self.assertEqual(rc, 0, fo + fe)
        ow, oh, orgb = _rfi_ref.decode_image(out_fold.read_bytes())
        self.assertEqual((ow, oh), (w, h))
        self.assertEqual(bytes(orgb), px)

    def test_limits_budget_and_seeded_malformed_fuzz(self) -> None:
        wd = self.work_dir()
        w, h = 16, 12
        px = make_rgba_exact_pixels(w, h, 77)
        icc = make_srgb_icc()
        rfwe_p = wd / "lim.rfwe"
        rfwe_p.write_bytes(pack_rfwe(w, h, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, px, icc=icc))
        webp_mux = wd / "lim_mux.webp"
        rc, eo, ee = self.run_cli(
            ["cli", "encode", "webp", self.to_exe_path(rfwe_p), self.to_exe_path(webp_mux), "preserve"]
        )
        self.assertEqual(rc, 0, eo + ee)

        # Limits override width=8
        rc, lo, _ = self.run_cli(["cli", "probe", "webp", self.to_exe_path(webp_mux), "8", "0"])
        self.assertEqual(rc, 2)
        self.assertEqual(parse_kv(lo)["STATUS"], "limit-exceeded")
        self.assertEqual(parse_kv(lo)["LIVE"], "0")

        # Low-budget decode OOM
        rc, bo, _ = self.run_cli(["cli", "budget-decode", self.to_exe_path(webp_mux), "16384"])
        self.assertEqual(rc, 2)
        bkv = parse_kv(bo)
        self.assertEqual(bkv["STATUS"], "out-of-memory")
        self.assertGreater(int(bkv["REJECTED"]), 0)
        self.assertEqual(bkv["CURR_ALLOC"], "0")

        # Also prepare bare lossless VP8L and lossy VP8 for seeded mutations
        webp_bare = wd / "lim_bare.webp"
        rc, _, _ = self.run_cli(
            ["cli", "encode", "webp", self.to_exe_path(rfwe_p), self.to_exe_path(webp_bare), "strip"]
        )
        self.assertEqual(rc, 0)
        webp_lossy = wd / "lim_lossy.webp"
        Image.frombytes("RGB", (w, h), make_rgb_pixels(w, h, 7)).save(
            webp_lossy, format="WEBP", lossless=False, quality=75
        )

        rng = random.Random(0x57454250)
        dump_out = wd / "fuzz_dump.rfwd"
        for label, valid_path in (
            ("mux", webp_mux),
            ("bare", webp_bare),
            ("lossy", webp_lossy),
        ):
            valid = valid_path.read_bytes()
            trunc_pts = sorted({0, 3, 8, 11, 12, 19, max(0, len(valid) - 1), len(valid) // 2})
            for tlen in trunc_pts:
                fp = wd / f"trunc_{label}_{tlen}.webp"
                fp.write_bytes(valid[:tlen])
                if dump_out.exists():
                    dump_out.unlink()
                rc, _, _ = self.run_cli(
                    ["cli", "dump", "webp", self.to_exe_path(fp), self.to_exe_path(dump_out)]
                )
                self.assertEqual(rc, 2, f"{label} trunc len={tlen} returned rc={rc}")

            for idx in range(5):
                mut = bytearray(valid)
                if idx == 4:
                    mut.extend(b"\xCA\xFE")
                else:
                    pos = rng.randrange(len(mut))
                    mut[pos] ^= rng.randrange(1, 256)
                fp = wd / f"mut_{label}_{idx}.webp"
                fp.write_bytes(bytes(mut))
                if dump_out.exists():
                    dump_out.unlink()
                rc, _, _ = self.run_cli(
                    ["cli", "dump", "webp", self.to_exe_path(fp), self.to_exe_path(dump_out)]
                )
                self.assertIn(rc, (0, 2), f"{label} mut idx={idx} crashed rc={rc}")


class HostWebPTests(WebPAdapterVerificationMixin, unittest.TestCase):
    host_exe: Path | None = None
    _tmp: tempfile.TemporaryDirectory[str] | None = None
    _wd: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_WEBP_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_WEBP_EXE not configured")
        cls.host_exe = Path(raw)
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_webp_host_")
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

    def test_actual_libwebp_peak_within_budget(self) -> None:
        assert self.host_exe is not None
        wd = self.work_dir()
        env = dict(os.environ)
        use_interposer = False
        if sys.platform.startswith("linux"):
            cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
            if not cc:
                raise AssertionError("Linux C compiler unavailable for LD_PRELOAD HWM interposer")
            so_c = wd / "webp_hwm_interpose.c"
            so_path = wd / "libwebp_hwm_interpose.so"
            so_c.write_text(
                """#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void (*real_free)(void *) = NULL;
static int (*real_posix_memalign)(void **, size_t, size_t) = NULL;

static char boot_buf[32768];
static size_t boot_used = 0;
static volatile int resolving = 0;
static __thread int in_dlsym = 0;
static uint64_t live_bytes = 0;
static uint64_t hwm_bytes = 0;

static int is_boot_ptr(const void *p) {
    uintptr_t a = (uintptr_t)p;
    uintptr_t lo = (uintptr_t)boot_buf;
    return a >= lo && a < lo + sizeof(boot_buf);
}

static void update_hwm(uint64_t cur) {
    uint64_t prev = __atomic_load_n(&hwm_bytes, __ATOMIC_RELAXED);
    while (cur > prev &&
           !__atomic_compare_exchange_n(&hwm_bytes, &prev, cur, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void add_live(size_t sz) {
    uint64_t cur = __atomic_add_fetch(&live_bytes, (uint64_t)sz, __ATOMIC_RELAXED);
    update_hwm(cur);
}

static void sub_live(size_t sz) {
    uint64_t prev = __atomic_load_n(&live_bytes, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t next = (prev >= (uint64_t)sz) ? (prev - (uint64_t)sz) : 0u;
        if (__atomic_compare_exchange_n(&live_bytes, &prev, next, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            break;
        }
    }
}

static void ensure_init(void) {
    if (__atomic_load_n(&real_free, __ATOMIC_ACQUIRE) != NULL) return;
    if (in_dlsym) return;
    while (__atomic_exchange_n(&resolving, 1, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&real_free, __ATOMIC_ACQUIRE) != NULL) return;
    }
    if (__atomic_load_n(&real_free, __ATOMIC_RELAXED) == NULL) {
        in_dlsym = 1;
        void *(*m)(size_t) = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
        void *(*c)(size_t, size_t) = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
        void *(*r)(void *, size_t) = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
        int (*pm)(void **, size_t, size_t) =
            (int (*)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
        void (*f)(void *) = (void (*)(void *))dlsym(RTLD_NEXT, "free");
        in_dlsym = 0;
        real_malloc = m;
        real_calloc = c;
        real_realloc = r;
        real_posix_memalign = pm;
        __atomic_store_n(&real_free, f, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&resolving, 0, __ATOMIC_RELEASE);
}

void *malloc(size_t sz) {
    ensure_init();
    if (!real_malloc || in_dlsym) {
        size_t a = (sz + 15u) & ~(size_t)15u;
        size_t off = __atomic_fetch_add(&boot_used, a ? a : 16u, __ATOMIC_RELAXED);
        return (off + a <= sizeof(boot_buf)) ? (boot_buf + off) : NULL;
    }
    void *p = real_malloc(sz);
    if (p) add_live(malloc_usable_size(p));
    return p;
}

void *calloc(size_t n, size_t sz) {
    ensure_init();
    if (!real_calloc || in_dlsym) {
        size_t tot = n * sz;
        size_t a = (tot + 15u) & ~(size_t)15u;
        size_t off = __atomic_fetch_add(&boot_used, a ? a : 16u, __ATOMIC_RELAXED);
        if (off + a <= sizeof(boot_buf)) {
            memset(boot_buf + off, 0, tot);
            return boot_buf + off;
        }
        return NULL;
    }
    void *p = real_calloc(n, sz);
    if (p) add_live(malloc_usable_size(p));
    return p;
}

void *realloc(void *p, size_t sz) {
    ensure_init();
    if (!real_realloc || is_boot_ptr(p)) {
        void *np = malloc(sz);
        if (np && p && is_boot_ptr(p)) memcpy(np, p, sz < 1024u ? sz : 1024u);
        return np;
    }
    size_t old_sz = p ? malloc_usable_size(p) : 0u;
    void *np = real_realloc(p, sz);
    if (np) {
        if (old_sz) sub_live(old_sz);
        add_live(malloc_usable_size(np));
    } else if (sz == 0u && old_sz) {
        sub_live(old_sz);
    }
    return np;
}

int posix_memalign(void **memptr, size_t align, size_t sz) {
    ensure_init();
    if (!real_posix_memalign) return 12;
    int rc = real_posix_memalign(memptr, align, sz);
    if (rc == 0 && *memptr) add_live(malloc_usable_size(*memptr));
    return rc;
}

void free(void *p) {
    if (!p || is_boot_ptr(p)) return;
    ensure_init();
    if (!real_free) return;
    size_t old_sz = malloc_usable_size(p);
    real_free(p);
    if (old_sz) sub_live(old_sz);
}

__attribute__((destructor)) static void dump_hwm(void) {
    const char *path = getenv("RFC_HWM_OUT");
    if (!path || !*path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%llu\\n",
                     (unsigned long long)__atomic_load_n(&hwm_bytes, __ATOMIC_RELAXED));
    if (n > 0) (void)write(fd, buf, (size_t)n);
    close(fd);
}
""",
                encoding="utf-8",
            )
            cres = _ref.run_process_tree(
                [cc, "-shared", "-fPIC", "-O2", str(so_c), "-o", str(so_path), "-ldl", "-pthread"],
                cwd=wd,
                capture_output=True,
                text=True,
                timeout=30.0,
            )
            self.assertEqual(cres.returncode, 0, f"compile interposer failed:\n{cres.stderr}")
            env["LD_PRELOAD"] = str(so_path)
            use_interposer = True

        def run_budget_peak(op: str, in_p: Path, out_p: str, threads: int, tag: str) -> tuple[int, int]:
            run_env = dict(env)
            hwm_file = wd / f"hwm_{tag}.txt"
            if use_interposer:
                if hwm_file.exists():
                    hwm_file.unlink()
                run_env["RFC_HWM_OUT"] = str(hwm_file)
            res = _ref.run_process_tree(
                [str(self.host_exe), "cli", "budget-peak", op, str(in_p), out_p, str(threads)],
                cwd=ROOT,
                env=run_env,
                capture_output=True,
                text=True,
                timeout=60.0,
            )
            self.assertEqual(res.returncode, 0, f"{tag} failed:\n{res.stdout}\n{res.stderr}")
            kv = parse_kv(res.stdout or "")
            self.assertEqual(kv.get("STATUS"), "ok", res.stdout)
            self.assertEqual(kv.get("CURR_ALLOC"), "0", res.stdout)
            budget_peak = int(kv["PEAK_BUDGET"])
            actual_peak = 0
            if use_interposer:
                actual_peak = int(hwm_file.read_text(encoding="utf-8").strip())
                self.assertGreater(actual_peak, 0, tag)
                self.assertLessEqual(
                    actual_peak,
                    budget_peak,
                    f"{tag}: actual OS heap peak {actual_peak} > PEAK_BUDGET {budget_peak}",
                )
            return actual_peak, budget_peak

        vp8l_paths: dict[int, Path] = {}
        vp8a_paths: dict[int, Path] = {}
        for dim in (256, 512):
            rng = random.Random(101 + dim)
            raw_rgb = bytearray()
            for y in range(dim):
                for x in range(dim):
                    raw_rgb.append(((x * 13 + y * 29) ^ (rng.randrange(256) if ((x ^ y) & 1) else 0)) & 0xFF)
                    raw_rgb.append(((x * 41 ^ y * 17) + (rng.randrange(128) if (x & 2) else 0)) & 0xFF)
                    raw_rgb.append(((x * 7 + y * 53) ^ (rng.randrange(64))) & 0xFF)
            rfwe_p = wd / f"peak_{dim}.rfwe"
            rfwe_p.write_bytes(
                pack_rfwe(dim, dim, SAMPLE_RGB8, ALPHA_NONE, 8, bytes(raw_rgb))
            )
            wp_out = wd / f"peak_vp8l_{dim}.webp"
            vp8l_paths[dim] = wp_out
            _, b1 = run_budget_peak("encode", rfwe_p, str(wp_out), 1, f"enc_{dim}_t1")
            if dim == 512:
                _, b4 = run_budget_peak("encode", rfwe_p, "-", 4, f"enc_{dim}_t4")
                self.assertGreater(b4, b1)
            run_budget_peak("decode", wp_out, "-", 1, f"dec_vp8l_{dim}")

            # Lossy VP8 + ALPH decode peak measurement via Pillow or ffmpeg libwebp
            rgba_bytes = make_rgba_exact_pixels(dim, dim, seed=dim // 16)
            vp8a_out = wd / f"peak_vp8a_{dim}.webp"
            try:
                bio = io.BytesIO()
                Image.frombytes("RGBA", (dim, dim), rgba_bytes).save(
                    bio, format="WEBP", lossless=False, quality=75
                )
                vp8a_out.write_bytes(bio.getvalue())
            except Exception:
                ffmpeg = _ref.find_ffmpeg()
                png_in = wd / f"peak_rgba_{dim}.png"
                Image.frombytes("RGBA", (dim, dim), rgba_bytes).save(png_in, format="PNG")
                fres = _ref.run_process_tree(
                    [ffmpeg, "-y", "-i", str(png_in), "-c:v", "libwebp", "-lossless", "0", "-q:v", "75", str(vp8a_out)],
                    cwd=wd,
                    capture_output=True,
                    text=True,
                    timeout=30.0,
                )
                self.assertEqual(fres.returncode, 0, fres.stderr)
            self.assertIn(b"ALPH", vp8a_out.read_bytes())
            self.assertIn(b"VP8 ", vp8a_out.read_bytes())
            vp8a_paths[dim] = vp8a_out
            run_budget_peak("decode", vp8a_out, "-", 1, f"dec_vp8a_{dim}")


class WindowsWebPTests(WebPAdapterVerificationMixin, unittest.TestCase):
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_WEBP_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_webp.exe")
        if not cand.is_file():
            if os.environ.get("RFC_REQUIRE_WIN_WEBP") == "1":
                raise AssertionError(f"missing Windows rfc_test_webp.exe: {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_webp.exe not built")
        cls.win_exe = cand

        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_webp_win_")
            cls._py_temp = Path(cls._win_temp)
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_webp.exe")
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_webp_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True,
                text=True,
                timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_WEBP") == "1":
                    raise AssertionError(
                        "powershell.exe unavailable while RFC_REQUIRE_WIN_WEBP=1"
                    )
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(
                ["wslpath", "-u", cls._win_temp],
                capture_output=True,
                text=True,
                timeout=15.0,
            )
            if wp.returncode != 0 or not (wp.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_WEBP") == "1":
                    raise AssertionError(
                        "wslpath failed while RFC_REQUIRE_WIN_WEBP=1"
                    )
                raise unittest.SkipTest("wslpath unavailable for Windows NTFS tests")
            cls._py_temp = Path(wp.stdout.strip())
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_webp.exe")

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
            exe = str(self.work_dir() / "rfc_test_webp.exe")
            env = dict(os.environ)
            env["PATH"] = f"{os.environ.get('SystemRoot', r'C:\\Windows')}\\System32;{os.environ.get('SystemRoot', r'C:\\Windows')}"
            res = _ref.run_process_tree(
                [exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True, timeout=timeout
            )
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_webp.exe")
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


class WindowsRequirementGuardTests(unittest.TestCase):
    def test_require_win_webp_turns_bridge_failure_into_failure(self) -> None:
        from types import SimpleNamespace
        from unittest import mock

        class _Probe(WindowsWebPTests):
            pass

        with tempfile.TemporaryDirectory(prefix="rfc_req_guard_") as td:
            fake_exe = Path(td) / "rfc_test_webp.exe"
            fake_exe.write_bytes(b"MZ")
            env_patch = {
                "RFC_WIN_WEBP_EXE": str(fake_exe),
                "RFC_REQUIRE_WIN_WEBP": "1",
            }
            if os.name == "nt":
                with mock.patch.dict(
                    os.environ,
                    {
                        "RFC_WIN_WEBP_EXE": str(Path(td) / "missing.exe"),
                        "RFC_REQUIRE_WIN_WEBP": "1",
                    },
                ):
                    with self.assertRaises(AssertionError):
                        _Probe.setUpClass()
                return

            with mock.patch.dict(os.environ, env_patch):
                with mock.patch.object(
                    _ref,
                    "run_process_tree",
                    return_value=SimpleNamespace(returncode=1, stdout="", stderr="ps failed"),
                ):
                    with self.assertRaises(AssertionError):
                        _Probe.setUpClass()

                with mock.patch.object(
                    _ref,
                    "run_process_tree",
                    side_effect=[
                        SimpleNamespace(returncode=0, stdout="C:\\Temp\\probe\r\n", stderr=""),
                        SimpleNamespace(returncode=1, stdout="", stderr="wslpath failed"),
                    ],
                ):
                    with self.assertRaises(AssertionError):
                        _Probe.setUpClass()


if __name__ == "__main__":
    unittest.main()
