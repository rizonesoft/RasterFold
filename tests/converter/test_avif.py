#!/usr/bin/env python3
"""Differential, oracle, clean-room libdav1d, budget HWM, and mutation tests
for D10 T02 s4 (AVIF).

Drives host C11 test_avif (RFC_HOST_AVIF_EXE) and isolated native Windows x64
bin/converter/windows-x64/rfc_test_avif.exe (on NTFS under restricted System32
PATH), cross-checking:
  - Registry schema validation for avif usable + [icc, orientation, exif, xmp]
  - Provenance validation (executable-in-d10-t02-s4)
  - Lossless 8/10/12-bit RGB & RGBA round trips (including all-opaque RGBA8/RGBA16
    alpha retention, Exif\\0\\0 + TIFF, ICC, XMP, native irot/imir orientations)
  - Independent clean-room FFmpeg (libavformat + VideoLAN libdav1d) decoding of
    converter outputs and converter import of FFmpeg yuv420p/yuv422p/yuv444p/gbrp/gray
    AVIF streams + animated AVIF refusal
  - Independent tests/rfi_ref.py .fold oracle + non-independent Pillow cross-check
  - Same-format AVIF -> AVIF exactness=byte-identical copy + non-ASCII filenames
  - Linux LD_PRELOAD OS heap high-water-mark verification vs scheduler reservation
  - Seeded random truncation and byte/box mutation fuzzing (zero crashes/hangs)
"""
from __future__ import annotations

import base64
import hashlib
import importlib.util
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
import unittest

from PIL import Image, ImageCms, features as pil_features

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
SAMPLE_RGB16 = _ref.SAMPLE_RGB16
SAMPLE_RGBA16 = _ref.SAMPLE_RGBA16
ALPHA_NONE = _ref.ALPHA_NONE
ALPHA_STRAIGHT = _ref.ALPHA_STRAIGHT


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
    stride = (len(pixels) // height) if height > 0 else 0
    hdr = struct.pack(
        "<4s11I",
        b"RFWE",
        width,
        height,
        sample,
        alpha,
        bps,
        stride,
        orientation,
        len(icc),
        len(exif),
        len(xmp),
        len(pixels),
    )
    return hdr + icc + exif + xmp + pixels


def unpack_rfwe(blob: bytes) -> dict:
    if len(blob) < 48 or blob[:4] != b"RFWE":
        raise ValueError("invalid RFWE header")
    (
        _magic,
        w,
        h,
        sample,
        alpha,
        bps,
        stride,
        ori,
        icc_len,
        exif_len,
        xmp_len,
        pay_len,
    ) = struct.unpack_from("<4s11I", blob, 0)
    off = 48
    icc = blob[off : off + icc_len]
    off += icc_len
    exif = blob[off : off + exif_len]
    off += exif_len
    xmp = blob[off : off + xmp_len]
    off += xmp_len
    pixels = blob[off : off + pay_len]
    if off + pay_len != len(blob):
        raise ValueError("RFWE size mismatch")
    return {
        "width": w,
        "height": h,
        "sample": sample,
        "alpha": alpha,
        "bps": bps,
        "stride": stride,
        "orientation": ori,
        "icc": icc,
        "exif": exif,
        "xmp": xmp,
        "pixels": pixels,
    }


def make_le_exif(orientation: int) -> bytes:
    buf = bytearray(26)
    buf[0:2] = b"II"
    struct.pack_into("<HIH", buf, 2, 42, 8, 1)
    struct.pack_into("<HHII", buf, 10, 0x0112, 3, 1, orientation)
    return bytes(buf)


def _to_ffmpeg_path(exe: str, p: Path) -> str:
    target = os.fspath(p)
    if os.name != "nt" and exe.lower().endswith(".exe"):
        wp = _ref.run_process_tree(
            ["wslpath", "-w", target],
            timeout=5.0,
            capture_output=True,
            text=True,
        )
        if wp.returncode != 0 or not wp.stdout.strip():
            raise RuntimeError(f"wslpath failed for {target}")
        return wp.stdout.strip()
    return target


def _run_ffmpeg_libdav1d(exe: str, avif_path: Path, pix_fmt: str, extra_args: list[str] | None = None) -> bytes:
    target = _to_ffmpeg_path(exe, avif_path)
    cmd = [exe, "-v", "error", "-c:v", "libdav1d", "-i", target, *(extra_args or []), "-f", "rawvideo", "-pix_fmt", pix_fmt, "-"]
    res = _ref.run_process_tree(cmd, timeout=30.0, capture_output=True, text=False)
    if res.returncode != 0:
        raise RuntimeError(f"ffmpeg libdav1d failed ({res.returncode}): {res.stderr.decode(errors='replace')}")
    return res.stdout


class AVIFSchemaAndProvenanceTests(unittest.TestCase):
    def test_codecs_json_schema_and_provenance(self) -> None:
        codecs_path = ROOT / "converter" / "codecs.json"
        self.assertEqual(_cc.validate_registry(str(codecs_path)), 0)
        doc = json.loads(codecs_path.read_text(encoding="utf-8"))
        by_fmt = {row["format"]: row for row in doc["formats"]}
        avif = by_fmt["avif"]
        self.assertEqual(avif["availability"], "usable")
        self.assertIsNone(avif["unavailable_reason"])
        self.assertIsNone(avif["extension_owner"])
        self.assertEqual(avif["native_samples"], ["rgb8", "rgba8", "rgb16", "rgba16"])
        self.assertEqual(avif["metadata_items"], ["icc", "orientation", "exif", "xmp"])

        by_prov = _ref.validate_provenance_table()
        self.assertEqual(by_prov["avif"]["coverage_state"], "executable-in-d10-t02-s4")
        self.assertEqual(by_prov["avif"]["reference_decoder_kind"], "shared-upstream-library")
        self.assertFalse(by_prov["avif"]["claims_independent_decoder"])

        with tempfile.TemporaryDirectory(prefix="rfc_avif_schema_") as td:
            for label, items in (
                ("duplicate", ["icc", "orientation", "exif", "exif"]),
                ("bad_order", ["xmp", "icc"]),
                ("unknown", ["icc", "bogus-item"]),
            ):
                mut = json.loads(json.dumps(doc))
                for row in mut["formats"]:
                    if row["format"] == "avif":
                        row["metadata_items"] = items
                p = Path(td) / f"codecs_{label}.json"
                p.write_text(json.dumps(mut), encoding="utf-8")
                self.assertNotEqual(_cc.validate_registry(str(p)), 0, label)


class AVIFAdapterVerificationMixin:
    def run_cli(self, args: list[str], timeout: float = 30.0) -> tuple[int, str, str]:
        raise NotImplementedError

    def work_dir(self) -> Path:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        return str(p)

    def _encode_avif(
        self,
        name: str,
        w: int,
        h: int,
        sample: int,
        alpha: int,
        bps: int,
        pixels: bytes,
        meta_policy: str = "preserve",
        ori: int = 1,
        icc: bytes = b"",
        exif: bytes = b"",
        xmp: bytes = b"",
    ) -> Path:
        wd = self.work_dir()
        in_p = wd / f"{name}.rfwe"
        out_p = wd / f"{name}.avif"
        if out_p.exists():
            out_p.unlink()
        in_p.write_bytes(
            pack_rfwe(w, h, sample, alpha, bps, pixels, orientation=ori, icc=icc, exif=exif, xmp=xmp)
        )
        rc, out, err = self.run_cli(
            ["cli", "encode", self.to_exe_path(in_p), self.to_exe_path(out_p), meta_policy, "1"]
        )
        self.assertEqual(rc, 0, f"encode {name} failed:\n{out}\n{err}")
        return out_p

    def _decode_avif(self, avif_p: Path, name: str) -> dict:
        wd = self.work_dir()
        out_p = wd / f"{name}.out.rfwe"
        if out_p.exists():
            out_p.unlink()
        rc, out, err = self.run_cli(
            ["cli", "decode", self.to_exe_path(avif_p), self.to_exe_path(out_p), "1"]
        )
        self.assertEqual(rc, 0, f"decode {name} failed:\n{out}\n{err}")
        return unpack_rfwe(out_p.read_bytes())

    def test_lossless_depths_opaque_alpha_and_metadata(self) -> None:
        wd = self.work_dir()
        srgb_icc = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
        pref_exif = b"Exif\x00\x00" + make_le_exif(6)
        xmp_doc = b"<x:xmpmeta>RF-PY-AVIF</x:xmpmeta>"

        w, h = 14, 10
        rgb8 = bytearray()
        rgba8 = bytearray()
        opaque8 = bytearray()
        for y in range(h):
            for x in range(w):
                r = (x * 19 + y * 7) & 255
                g = (y * 23 + x * 5) & 255
                b = ((x ^ y) * 13) & 255
                a = 255 if ((x + y) % 4 == 0) else (0 if (x % 3 == 0) else ((x * 29 + y * 11) & 255))
                rgb8.extend((r, g, b))
                rgba8.extend((r, g, b, a))
                opaque8.extend((r, g, b, 255))

        avif_rgb8 = self._encode_avif(
            "rgb8_meta", w, h, SAMPLE_RGB8, ALPHA_NONE, 8, bytes(rgb8),
            "preserve", ori=6, icc=srgb_icc, exif=pref_exif, xmp=xmp_doc,
        )
        dec_rgb8 = self._decode_avif(avif_rgb8, "rgb8_meta")
        self.assertEqual((dec_rgb8["width"], dec_rgb8["height"], dec_rgb8["sample"], dec_rgb8["bps"]), (w, h, SAMPLE_RGB8, 8))
        self.assertEqual(dec_rgb8["pixels"], bytes(rgb8))
        self.assertEqual(dec_rgb8["orientation"], 6)
        self.assertEqual(dec_rgb8["icc"], srgb_icc)
        self.assertEqual(dec_rgb8["exif"], pref_exif)
        self.assertEqual(dec_rgb8["xmp"], xmp_doc)

        # Stripped variant removes all metadata and orientation -> NORMAL (1).
        avif_strip = self._encode_avif(
            "rgb8_strip", w, h, SAMPLE_RGB8, ALPHA_NONE, 8, bytes(rgb8),
            "strip", ori=6, icc=srgb_icc, exif=pref_exif, xmp=xmp_doc,
        )
        dec_strip = self._decode_avif(avif_strip, "rgb8_strip")
        self.assertEqual(dec_strip["pixels"], bytes(rgb8))
        self.assertEqual(dec_strip["orientation"], 1)
        self.assertEqual(dec_strip["icc"], b"")
        self.assertEqual(dec_strip["exif"], b"")
        self.assertEqual(dec_strip["xmp"], b"")

        # Variable RGBA8 + all-opaque RGBA8 retain alpha plane and RGBA8 layout.
        for label, px in (("rgba8_var", bytes(rgba8)), ("rgba8_opaque", bytes(opaque8))):
            p_av = self._encode_avif(label, w, h, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, px, "strip")
            d_av = self._decode_avif(p_av, label)
            self.assertEqual(d_av["sample"], SAMPLE_RGBA8)
            self.assertEqual(d_av["alpha"], ALPHA_STRAIGHT)
            self.assertEqual(d_av["pixels"], px)

        # 10-bit and 12-bit left-aligned RGBA16 (including all-opaque 12-bit RGBA16).
        for bps in (10, 12):
            shift = 16 - bps
            maxv = (1 << bps) - 1
            px16 = bytearray()
            for y in range(6):
                for x in range(8):
                    for c in range(3):
                        v = (x * 73 + y * 139 + c * 47) & maxv
                        px16.extend(struct.pack("<H", v << shift))
                    px16.extend(struct.pack("<H", maxv << shift))
            p16 = self._encode_avif(f"rgba{bps}", 8, 6, SAMPLE_RGBA16, ALPHA_STRAIGHT, bps, bytes(px16), "strip")
            d16 = self._decode_avif(p16, f"rgba{bps}")
            self.assertEqual(d16["sample"], SAMPLE_RGBA16)
            self.assertEqual(d16["alpha"], ALPHA_STRAIGHT)
            self.assertEqual(d16["bps"], bps)
            self.assertEqual(d16["pixels"], bytes(px16))

        # Non-ASCII filename test.
        non_ascii = self._encode_avif(
            "café_avif_π", 8, 8, SAMPLE_RGB8, ALPHA_NONE, 8, bytes(rgb8[: 8 * 8 * 3]), "strip"
        )
        dec_na = self._decode_avif(non_ascii, "café_avif_π")
        self.assertEqual(dec_na["pixels"], bytes(rgb8[: 8 * 8 * 3]))

        # Cross-format AVIF -> RasterFold via independent tests/rfi_ref.py oracle.
        fold_out = wd / "from_avif.fold"
        if fold_out.exists():
            fold_out.unlink()
        rc, out, err = self.run_cli(
            ["cli", "convert", "avif", "rasterfold", self.to_exe_path(avif_strip), self.to_exe_path(fold_out), "strip", "pixel-exact"]
        )
        self.assertEqual(rc, 0, f"avif->fold failed:\n{out}\n{err}")
        rw, rh, rpix = _rfi_ref.decode_image(fold_out.read_bytes())
        self.assertEqual((rw, rh), (w, h))
        self.assertEqual(rpix, bytes(rgb8))

        # And RasterFold -> AVIF round-trip.
        back_avif = wd / "from_fold.avif"
        if back_avif.exists():
            back_avif.unlink()
        rc, out, err = self.run_cli(
            ["cli", "convert", "rasterfold", "avif", self.to_exe_path(fold_out), self.to_exe_path(back_avif), "strip", "pixel-exact"]
        )
        self.assertEqual(rc, 0, f"fold->avif failed:\n{out}\n{err}")
        dec_back = self._decode_avif(back_avif, "from_fold")
        self.assertEqual(dec_back["pixels"], bytes(rgb8))

        # Multi-tile 2x2 edge-cropped grid RGBA8 import (117x113 -> 64x64 + edges).
        gw, gh = 117, 113
        grid_px = bytearray()
        for gy in range(gh):
            for gx in range(gw):
                grid_px.extend(
                    (
                        (gx * 9 + gy * 3) & 255,
                        (gx * 5 + gy * 11) & 255,
                        ((gx ^ gy) * 7) & 255,
                        255 if ((gx + gy) % 4 == 0) else ((gx * 13 + gy) & 255),
                    )
                )
        grid_in = wd / "grid_117x113.rfwe"
        grid_avif = wd / "grid_117x113.avif"
        grid_in.write_bytes(pack_rfwe(gw, gh, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, bytes(grid_px)))
        rc, out, err = self.run_cli(["cli", "encode-grid", self.to_exe_path(grid_in), self.to_exe_path(grid_avif)])
        self.assertEqual(rc, 0, f"encode-grid failed:\n{out}\n{err}")
        dec_grid = self._decode_avif(grid_avif, "grid_117x113")
        self.assertEqual((dec_grid["width"], dec_grid["height"]), (gw, gh))
        self.assertEqual(dec_grid["sample"], SAMPLE_RGBA8)
        self.assertEqual(dec_grid["alpha"], ALPHA_STRAIGHT)
        self.assertEqual(dec_grid["pixels"], bytes(grid_px))

    def test_cleanroom_ffmpeg_and_pillow_crosscheck(self) -> None:
        wd = self.work_dir()
        ff_info = _ref.probe_ffmpeg_avif_decoder()
        self.assertTrue(
            ff_info["available"] and ff_info["independent"],
            f"expected clean-room FFmpeg libdav1d decoder: {ff_info}",
        )
        ff_exe = ff_info["executable"]

        w, h = 16, 12
        rgb8 = bytearray()
        rgba8 = bytearray()
        opaque8 = bytearray()
        for y in range(h):
            for x in range(w):
                r = (x * 17 + y * 9) & 255
                g = (x * 5 + y * 21) & 255
                b = ((x ^ y) * 15) & 255
                a = 255 if ((x + y) % 3 == 0) else 96
                rgb8.extend((r, g, b))
                rgba8.extend((r, g, b, a))
                opaque8.extend((r, g, b, 255))

        # 1. Clean-room libdav1d decode of converter-written 8-bit RGB & RGBA AVIFs.
        av_rgb = self._encode_avif("ff_rgb8", w, h, SAMPLE_RGB8, ALPHA_NONE, 8, bytes(rgb8), "strip")
        dav_rgb = _run_ffmpeg_libdav1d(ff_exe, av_rgb, "rgb24")
        self.assertEqual(dav_rgb, bytes(rgb8))

        for label, px in (("ff_rgba8", bytes(rgba8)), ("ff_opaque8", bytes(opaque8))):
            av_a = self._encode_avif(label, w, h, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, px, "strip")
            dav_rgb_main = _run_ffmpeg_libdav1d(ff_exe, av_a, "rgb24", ["-map", "0:v:0"])
            self.assertEqual(len(dav_rgb_main), w * h * 3)
            # Check primary RGB channels match source RGB bytes exactly.
            exp_rgb = bytearray()
            for i in range(w * h):
                exp_rgb.extend(px[i * 4 : i * 4 + 3])
            self.assertEqual(dav_rgb_main, bytes(exp_rgb))
            # Check auxiliary alpha track via libdav1d stream 0:v:1!
            dav_alpha = _run_ffmpeg_libdav1d(ff_exe, av_a, "gray", ["-map", "0:v:1"])
            self.assertEqual(dav_alpha, bytes(px[3::4]))

        # Non-independent Pillow shared-libavif check (documents shared upstream library).
        if pil_features.check("avif"):
            with Image.open(av_rgb) as pil_im:
                self.assertEqual(pil_im.mode, "RGB")
                self.assertEqual(pil_im.size, (w, h))
                self.assertEqual(pil_im.tobytes(), bytes(rgb8))
            av_op = wd / "ff_opaque8.avif"
            with Image.open(av_op) as pil_op:
                self.assertEqual(pil_op.mode, "RGBA")
                self.assertEqual(pil_op.tobytes(), bytes(opaque8))

        # 2. Clean-room libdav1d decode of converter 10-bit RGB AVIF.
        px10 = bytearray()
        exp10_le = bytearray()
        for y in range(6):
            for x in range(8):
                for c in range(3):
                    nat = (x * 101 + y * 193 + c * 61) & 0x3FF
                    px10.extend(struct.pack("<H", nat << 6))
                    exp10_le.extend(struct.pack("<H", nat))
        av_10 = self._encode_avif("ff_rgb10", 8, 6, SAMPLE_RGB16, ALPHA_NONE, 10, bytes(px10), "strip")
        dav_10 = _run_ffmpeg_libdav1d(ff_exe, av_10, "rgb48le", ["-strict", "unofficial"])
        # ffmpeg scales 10-bit -> 16-bit for rgb48le (shifted left by 6 or replicated); mask top 10 bits!
        self.assertEqual(len(dav_10), 8 * 6 * 3 * 2)
        for idx in range(0, len(dav_10), 2):
            got_v = struct.unpack_from("<H", dav_10, idx)[0] >> 6
            want_v = struct.unpack_from("<H", exp10_le, idx)[0]
            self.assertEqual(got_v, want_v)

        # 3. Synthesize external AVIFs via clean-room FFmpeg (yuv420p, yuv422p, yuv444p, gbrp, gray,
        #    and animated multi-frame .avif using non-aom AV1 encoder when available) and test import/refusal.
        enc_res = _ref.run_process_tree([ff_exe, "-hide_banner", "-encoders"], timeout=10.0, capture_output=True, text=True)
        ff_encoders = enc_res.stdout or ""
        av1_enc = None
        for cand_enc in ("libsvtav1", "librav1e", "libaom-av1"):
            if cand_enc in ff_encoders:
                av1_enc = cand_enc
                break
        self.assertIsNotNone(av1_enc, "expected AV1 encoder in ffmpeg for external stream generation")
        assert av1_enc is not None
        for pix_tag, ff_pf, sub_fmt, enc_use in (
            ("ff_yuv420p", "yuv420p", "yuv420p", av1_enc),
            ("ff_yuv422p", "yuv422p", "yuv422p", "libaom-av1" if av1_enc == "libsvtav1" else av1_enc),
            ("ff_yuv444p", "yuv444p", "yuv444p", "libaom-av1"),
            ("ff_gbrp", "gbrp", "gbrp", "libaom-av1"),
            ("ff_gray", "gray", "gray", "libaom-av1"),
        ):
            src_png = wd / f"{pix_tag}_src.png"
            ext_p = wd / f"{pix_tag}.avif"
            im_in = Image.new("L" if ff_pf == "gray" else "RGB", (16, 12))
            for yy in range(12):
                for xx in range(16):
                    if ff_pf == "gray":
                        im_in.putpixel((xx, yy), (xx * 13 + yy * 7) & 255)
                    else:
                        im_in.putpixel((xx, yy), ((xx * 13) & 255, (yy * 17) & 255, ((xx + yy) * 9) & 255))
            im_in.save(src_png)
            ff_cmd = [
                ff_exe, "-y", "-v", "error", "-i", _to_ffmpeg_path(ff_exe, src_png),
                "-c:v", enc_use, "-pix_fmt", ff_pf,
                *(["-vf", "setparams=colorspace=bt709"] if ff_pf == "gray" else []),
                "-f", "avif", _to_ffmpeg_path(ff_exe, ext_p),
            ]
            f_out = _ref.run_process_tree(ff_cmd, timeout=30.0, capture_output=True, text=True)
            self.assertEqual(f_out.returncode, 0, f"ffmpeg synthesize {sub_fmt} failed: {f_out.stderr}")
            dec_ext = self._decode_avif(ext_p, pix_tag)
            self.assertEqual((dec_ext["width"], dec_ext["height"]), (16, 12))
            self.assertEqual(dec_ext["sample"], SAMPLE_RGB8)

            if pix_tag == "ff_yuv420p":
                bi_out = wd / f"{pix_tag}_bi.avif"
                if bi_out.exists():
                    bi_out.unlink()
                rc, out, err = self.run_cli(
                    ["cli", "convert", "avif", "avif", self.to_exe_path(ext_p), self.to_exe_path(bi_out), "preserve", "byte-identical"]
                )
                self.assertEqual(rc, 0, f"byte-identical failed:\n{out}\n{err}")
                self.assertEqual(bi_out.read_bytes(), ext_p.read_bytes())

        # Multi-frame animated AVIF synthesized cleanly with FFmpeg must refuse.
        anim_p = wd / "anim_ff.avif"
        anim_cmd = [
            ff_exe, "-y", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=8x8:d=0.1:r=25",
            "-frames:v", "3", "-c:v", av1_enc, "-f", "avif", _to_ffmpeg_path(ff_exe, anim_p),
        ]
        a_res = _ref.run_process_tree(anim_cmd, timeout=30.0, capture_output=True, text=True)
        self.assertEqual(a_res.returncode, 0, f"ffmpeg anim.avif failed: {a_res.stderr}")
        rc, out, _ = self.run_cli(["cli", "probe", self.to_exe_path(anim_p)])
        self.assertEqual(rc, 2)
        self.assertIn("STATUS=unsupported-combination", out)

    def test_seeded_mutations_and_truncations(self) -> None:
        wd = self.work_dir()
        rgb = bytes(((i * 31) & 255) for i in range(16 * 16 * 3))
        seed_avif = self._encode_avif(
            "mut_seed", 16, 16, SAMPLE_RGB8, ALPHA_NONE, 8, rgb,
            "preserve", ori=6, exif=b"Exif\x00\x00" + make_le_exif(6), xmp=b"<x:xmpmeta/>",
        ).read_bytes()

        rng = random.Random(0x41564946)
        for idx in range(16):
            cut = rng.randint(0, max(0, len(seed_avif) - 1))
            tp = wd / f"trunc_{idx}.avif"
            out_p = wd / f"trunc_{idx}.rfwe"
            tp.write_bytes(seed_avif[:cut])
            rc, _, _ = self.run_cli(
                ["cli", "decode", self.to_exe_path(tp), self.to_exe_path(out_p)],
                timeout=20.0,
            )
            self.assertIn(rc, (0, 2))

            mut = bytearray(seed_avif)
            for _ in range(rng.randint(1, 4)):
                pos = rng.randint(0, len(mut) - 1)
                mut[pos] ^= rng.randint(1, 255)
            mp = wd / f"mut_{idx}.avif"
            mp.write_bytes(bytes(mut))
            rc, _, _ = self.run_cli(
                ["cli", "decode", self.to_exe_path(mp), self.to_exe_path(out_p)],
                timeout=20.0,
            )
            self.assertIn(rc, (0, 2))


class HostAVIFTests(AVIFAdapterVerificationMixin, unittest.TestCase):
    host_exe: Path | None = None
    _temp_dir: tempfile.TemporaryDirectory[str] | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_AVIF_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_AVIF_EXE not set")
        cls.host_exe = Path(raw)
        cls._temp_dir = tempfile.TemporaryDirectory(prefix="rfc_host_avif_")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._temp_dir is not None:
            cls._temp_dir.cleanup()

    def work_dir(self) -> Path:
        assert self._temp_dir is not None
        return Path(self._temp_dir.name)

    def run_cli(self, args: list[str], timeout: float = 30.0) -> tuple[int, str, str]:
        assert self.host_exe is not None
        res = _ref.run_process_tree(
            [str(self.host_exe), *args], cwd=ROOT, capture_output=True, text=True, timeout=timeout
        )
        return res.returncode, res.stdout or "", res.stderr or ""


class HostAVIFMallocHWMTests(unittest.TestCase):
    host_exe: Path | None = None
    _temp_dir: tempfile.TemporaryDirectory[str] | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_AVIF_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_AVIF_EXE not set")
        cls.host_exe = Path(raw)
        cls._temp_dir = tempfile.TemporaryDirectory(prefix="rfc_avif_hwm_")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._temp_dir is not None:
            cls._temp_dir.cleanup()

    def test_os_heap_hwm_bounded_by_scheduler_reservation(self) -> None:
        if not sys.platform.startswith("linux"):
            self.skipTest("LD_PRELOAD heap interposer applies to Linux hosts")
        assert self.host_exe is not None
        assert self._temp_dir is not None
        wd = Path(self._temp_dir.name)

        cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
        if not cc:
            raise AssertionError("Linux C compiler unavailable for LD_PRELOAD HWM interposer")
        so_path = wd / "hwm_interpose.so"
        so_c = wd / "hwm_interpose.c"
        env = dict(os.environ)
        so_c.write_text(
            r"""#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *(*real_malloc)(size_t) = NULL;
static void *(*real_calloc)(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void *(*real_memalign)(size_t, size_t) = NULL;
static void *(*real_aligned_alloc)(size_t, size_t) = NULL;
static int (*real_posix_memalign)(void **, size_t, size_t) = NULL;
static void (*real_free)(void *) = NULL;
static uint8_t boot_buf[262144];
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
           !__atomic_compare_exchange_n(&hwm_bytes, &prev, cur, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void add_live(size_t sz) {
    update_hwm(__atomic_add_fetch(&live_bytes, (uint64_t)sz, __ATOMIC_RELAXED));
}
static void sub_live(size_t sz) {
    uint64_t prev = __atomic_load_n(&live_bytes, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t next = (prev >= (uint64_t)sz) ? (prev - (uint64_t)sz) : 0u;
        if (__atomic_compare_exchange_n(&live_bytes, &prev, next, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
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
        real_malloc = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
        real_calloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
        real_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
        real_memalign = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "memalign");
        real_aligned_alloc = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "aligned_alloc");
        real_posix_memalign = (int (*)(void **, size_t, size_t))dlsym(RTLD_NEXT, "posix_memalign");
        __atomic_store_n(&real_free, (void (*)(void *))dlsym(RTLD_NEXT, "free"), __ATOMIC_RELEASE);
        in_dlsym = 0;
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
        void *p = malloc(tot);
        if (p) memset(p, 0, tot);
        return p;
    }
    void *p = real_calloc(n, sz);
    if (p) add_live(malloc_usable_size(p));
    return p;
}
void *realloc(void *p, size_t sz) {
    ensure_init();
    if (!real_realloc) return NULL;
    if (is_boot_ptr(p)) {
        void *np = real_malloc(sz);
        if (np) { memcpy(np, p, sz); add_live(malloc_usable_size(np)); }
        return np;
    }
    size_t old_sz = p ? malloc_usable_size(p) : 0u;
    void *np = real_realloc(p, sz);
    if (np) { if (old_sz) sub_live(old_sz); add_live(malloc_usable_size(np)); }
    else if (sz == 0u && old_sz) sub_live(old_sz);
    return np;
}
void *memalign(size_t align, size_t sz) {
    ensure_init();
    if (!real_memalign) return NULL;
    void *p = real_memalign(align, sz);
    if (p) add_live(malloc_usable_size(p));
    return p;
}
void *aligned_alloc(size_t align, size_t sz) {
    ensure_init();
    if (!real_aligned_alloc) return NULL;
    void *p = real_aligned_alloc(align, sz);
    if (p) add_live(malloc_usable_size(p));
    return p;
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
    int n = snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)__atomic_load_n(&hwm_bytes, __ATOMIC_RELAXED));
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

        def run_budget_peak(op: str, in_p: Path, out_p: str, threads: int, sched_admit: int, tag: str) -> int:
            run_env = dict(env)
            hwm_file = wd / f"hwm_{tag}.txt"
            if hwm_file.exists():
                hwm_file.unlink()
            run_env["RFC_HWM_OUT"] = str(hwm_file)
            res = _ref.run_process_tree(
                [str(self.host_exe), "cli", "budget-peak", op, str(in_p), out_p, str(threads)],
                cwd=ROOT,
                env=run_env,
                capture_output=True,
                text=True,
                timeout=120.0,
            )
            self.assertEqual(res.returncode, 0, f"{tag} failed:\n{res.stdout}\n{res.stderr}")
            kv = parse_kv(res.stdout or "")
            self.assertEqual(kv.get("STATUS"), "ok", res.stdout)
            self.assertEqual(kv.get("CURR_ALLOC"), "0", res.stdout)
            budget_peak = int(kv["PEAK_BUDGET"])
            self.assertLessEqual(budget_peak, sched_admit, tag)
            self.assertTrue(hwm_file.is_file(), f"{tag}: LD_PRELOAD interposer produced no HWM file")
            actual_peak = int(hwm_file.read_text(encoding="utf-8").strip())
            self.assertGreater(actual_peak, 0, tag)
            self.assertLessEqual(
                actual_peak,
                sched_admit,
                f"{tag}: OS HWM {actual_peak} exceeded scheduler reservation {sched_admit}",
            )
            return actual_peak

        cases = (
            ("rgb8", SAMPLE_RGB8, ALPHA_NONE, 8, 3),
            ("rgba12", SAMPLE_RGBA16, ALPHA_STRAIGHT, 12, 8),
        )
        for dim in (256, 512):
            pixels = dim * dim
            for cname, sample, alpha, bps, bpp in cases:
                rng = random.Random(303 + dim + bps)
                raw_px = bytearray(rng.randbytes(pixels * bpp))
                if bps == 12:
                    raw_px[0::2] = bytes(b & 0xF0 for b in raw_px[0::2])
                rfwe_p = wd / f"hwm_{cname}_{dim}.rfwe"
                avif_p = wd / f"hwm_{cname}_{dim}.avif"
                rfwe_p.write_bytes(pack_rfwe(dim, dim, sample, alpha, bps, bytes(raw_px)))

                decoded_bytes = pixels * bpp
                for thr in (1, 3):
                    enc_admit = (
                        4 * decoded_bytes
                        + 44 * pixels
                        + thr * 8 * 1024 * 1024
                        + 16 * 1024 * 1024
                    )
                    dec_admit = (
                        4 * decoded_bytes
                        + (16 + 8 * thr) * pixels
                        + thr * 8 * 1024 * 1024
                        + 8 * 1024 * 1024
                    )
                    run_budget_peak("encode", rfwe_p, str(avif_p) if thr == 1 else "-", thr, enc_admit, f"enc_{cname}_{dim}_t{thr}")
                    run_budget_peak("decode", avif_p, "-", thr, dec_admit, f"dec_{cname}_{dim}_t{thr}")

        # Verify 2x2 edge-cropped grid decode OS heap HWM <= calibrated reservation.
        gw, gh = 117, 113
        gpix = gw * gh
        gdec = gpix * 4
        g_rfwe = wd / "hwm_grid_117x113.rfwe"
        g_avif = wd / "hwm_grid_117x113.avif"
        g_raw = random.Random(404).randbytes(gdec)
        g_rfwe.write_bytes(pack_rfwe(gw, gh, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, g_raw))
        gres = _ref.run_process_tree(
            [str(self.host_exe), "cli", "encode-grid", str(g_rfwe), str(g_avif)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=60.0,
        )
        self.assertEqual(gres.returncode, 0, f"grid encode failed:\n{gres.stdout}\n{gres.stderr}")
        for thr in (1, 3):
            g_dec_admit = (
                4 * gdec
                + (16 + 8 * thr) * gpix
                + thr * 8 * 1024 * 1024
                + 8 * 1024 * 1024
            )
            run_budget_peak("decode", g_avif, "-", thr, g_dec_admit, f"dec_grid_117x113_t{thr}")


class WindowsAVIFTests(AVIFAdapterVerificationMixin, unittest.TestCase):
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_AVIF_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_avif.exe")
        if not cand.is_file():
            if os.environ.get("RFC_REQUIRE_WIN_AVIF") == "1":
                raise AssertionError(f"missing Windows rfc_test_avif.exe: {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_avif.exe not built")
        cls.win_exe = cand

        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_avif_win_")
            cls._py_temp = Path(cls._win_temp)
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_avif.exe")
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_avif_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True,
                text=True,
                timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_AVIF") == "1":
                    raise AssertionError("powershell.exe unavailable while RFC_REQUIRE_WIN_AVIF=1")
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(
                ["wslpath", "-u", cls._win_temp],
                capture_output=True,
                text=True,
                timeout=15.0,
            )
            if wp.returncode != 0 or not (wp.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_AVIF") == "1":
                    raise AssertionError("wslpath failed while RFC_REQUIRE_WIN_AVIF=1")
                raise unittest.SkipTest("wslpath unavailable for Windows NTFS tests")
            cls._py_temp = Path(wp.stdout.strip())
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_avif.exe")

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
            exe = str(self.work_dir() / "rfc_test_avif.exe")
            env = dict(os.environ)
            env["PATH"] = f"{os.environ.get('SystemRoot', r'C:\\Windows')}\\System32;{os.environ.get('SystemRoot', r'C:\\Windows')}"
            res = _ref.run_process_tree(
                [exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True, timeout=timeout
            )
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_avif.exe")
        cmd = (
            "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); "
            "$env:PATH = \"$env:SystemRoot\\System32;$env:SystemRoot\"; "
            f"& {exe_ps} {arg_ps}; exit $LASTEXITCODE"
        )
        b64_cmd = base64.b64encode(cmd.encode("utf-16-le")).decode("ascii")
        res = _ref.run_process_tree(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", b64_cmd],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return res.returncode, res.stdout or "", res.stderr or ""


class WindowsRequirementGuardTests(unittest.TestCase):
    def test_require_win_avif_turns_bridge_failure_into_failure(self) -> None:
        from types import SimpleNamespace
        from unittest import mock

        class _Probe(WindowsAVIFTests):
            pass

        with tempfile.TemporaryDirectory(prefix="rfc_req_guard_") as td:
            fake_exe = Path(td) / "rfc_test_avif.exe"
            fake_exe.write_bytes(b"MZ")
            env_patch = {
                "RFC_WIN_AVIF_EXE": str(fake_exe),
                "RFC_REQUIRE_WIN_AVIF": "1",
            }
            if os.name == "nt":
                with mock.patch.dict(
                    os.environ,
                    {
                        "RFC_WIN_AVIF_EXE": str(Path(td) / "missing.exe"),
                        "RFC_REQUIRE_WIN_AVIF": "1",
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
