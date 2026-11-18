#!/usr/bin/env python3
"""Independent-oracle, optimization, metadata, refusal and mutation tests for D10 T02 s5 (JPEG).

Drives host C11 test_jpeg (RFC_HOST_JPEG_EXE) and the isolated native Windows
x64 bin/converter/windows-x64/rfc_test_jpeg.exe (on NTFS under a System32-only
PATH), cross-checking:
  - quantized DCT coefficients and pinned-decoder pixels against the
    independent pure-Python tests/converter/jpeg_ref.py (baseline, extended,
    progressive Huffman; 4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1, gray, restarts)
  - coefficient-exact JPEG -> JPEG optimization: identical coefficients by the
    independent oracle, strictly smaller output for non-optimized input, the
    not-smaller fallback, and the photo-pack size report
  - arithmetic-coded JPEG through a fresh libjpeg-turbo session only
    (shared-provider coverage, reported as such)
  - ICC/EXIF/XMP/orientation preserve and strip, JPEG -> PNG pixel export
  - lossy-write refusals, CMYK and multi-image refusals, existing-destination
    safety, seeded truncation and mutation, and (host) the LD_PRELOAD heap
    high-water mark against the adapter's budget accounting
"""
from __future__ import annotations

import importlib.util
import io
import json
import math
import os
from pathlib import Path
import random
import shutil
import struct
import sys
import tempfile
import unittest

from PIL import Image, ImageCms

ROOT = Path(__file__).resolve().parents[2]


def _load_mod(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod  # dataclasses resolve annotations through sys.modules
    spec.loader.exec_module(mod)
    return mod


_ref = _load_mod("_rfc_ref", ROOT / "tests" / "converter" / "reference.py")
_jref = _load_mod("_rfc_jpeg_ref", ROOT / "tests" / "converter" / "jpeg_ref.py")
_cc = _load_mod("_rfc_check_conv", ROOT / "scripts" / "check-converter.py")

META_ITEM_ORIENTATION = 2
META_ITEM_EXIF = 8
META_ITEM_XMP = 16

# Coverage ledger written to stdout so the gate log records which fixtures
# had an independent oracle and which only a shared-provider check.
COVERAGE: list[tuple[str, str]] = []


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


def pattern_image(w: int, h: int, mode: str, seed: int) -> Image.Image:
    rng = random.Random(seed)
    im = Image.new(mode, (w, h))
    px = []
    for y in range(h):
        for x in range(w):
            n = rng.randrange(24)
            if mode == "L":
                px.append((x * 7 + y * 13 + (x * y) % 23 + n) & 255)
            else:
                px.append((((x * 7 + y * 3) + n) & 255, (x * x + y * 5) & 255, (((x ^ y) * 11) + n) & 255))
    im.putdata(px)
    return im


def pillow_jpeg(im: Image.Image, **kw) -> bytes:
    b = io.BytesIO()
    im.save(b, "JPEG", **kw)
    return b.getvalue()


def parse_coeff_dump(blob: bytes) -> dict:
    if blob[:4] != b"RFJC":
        raise AssertionError("bad coefficient dump")
    o = 4

    def u32() -> int:
        nonlocal o
        v = struct.unpack_from("<I", blob, o)[0]
        o += 4
        return v

    w, h, prog, arith, base, nc = (u32() for _ in range(6))
    qt = {}
    for t in range(4):
        pop = u32()
        vals = [u32() for _ in range(64)]
        if pop:
            qt[t] = vals
    comps = []
    for _ in range(nc):
        cid, hs, vs, tq, dc, ac, bw, bh = (u32() for _ in range(8))
        cnt = bw * bh * 64
        coefs = list(struct.unpack_from(f"<{cnt}h", blob, o))
        o += cnt * 2
        comps.append({"id": cid, "h": hs, "v": vs, "tq": tq, "dc": dc, "ac": ac, "bw": bw, "bh": bh, "coefs": coefs})
    assert o == len(blob)
    return {"w": w, "h": h, "progressive": prog, "arith": arith, "baseline": base, "comps": comps, "qt": qt}


def assert_dump_matches_oracle(tc: unittest.TestCase, dump: dict, ref, label: str) -> None:
    tc.assertEqual((dump["w"], dump["h"]), (ref.width, ref.height), label)
    tc.assertEqual(bool(dump["progressive"]), ref.progressive, label)
    tc.assertEqual(len(dump["comps"]), len(ref.components), label)
    for ci, (dc, rc) in enumerate(zip(dump["comps"], ref.components)):
        tc.assertEqual((dc["id"], dc["h"], dc["v"], dc["tq"]), (rc.cid, rc.h, rc.v, rc.tq), f"{label} comp {ci}")
        tc.assertEqual((dc["bw"], dc["bh"]), (rc.blocks_w, rc.blocks_h), f"{label} comp {ci} blocks")
        tc.assertEqual((dc["dc"], dc["ac"]), (rc.dc_tbl, rc.ac_tbl), f"{label} comp {ci} selectors")
        tc.assertEqual(dump["qt"][dc["tq"]], ref.qtables[rc.tq], f"{label} comp {ci} quant")
        flat = []
        for by in range(rc.blocks_h):
            for bx in range(rc.blocks_w):
                flat.extend(rc.coefs[by * rc.alloc_w + bx])
        tc.assertEqual(dc["coefs"], flat, f"{label} comp {ci} coefficients")


def assert_same_coefficients(tc: unittest.TestCase, a, b, label: str) -> None:
    """Independent coefficient-exactness: two oracle decodes agree."""
    tc.assertEqual((a.width, a.height, a.progressive), (b.width, b.height, b.progressive), label)
    tc.assertEqual(len(a.components), len(b.components), label)
    for ca, cb in zip(a.components, b.components):
        tc.assertEqual((ca.cid, ca.h, ca.v, ca.tq, ca.blocks_w, ca.blocks_h), (cb.cid, cb.h, cb.v, cb.tq, cb.blocks_w, cb.blocks_h), label)
        tc.assertEqual(a.qtables[ca.tq], b.qtables[cb.tq], label)
        for by in range(ca.blocks_h):
            for bx in range(ca.blocks_w):
                tc.assertEqual(ca.coefs[by * ca.alloc_w + bx], cb.coefs[by * cb.alloc_w + bx], f"{label} block {by},{bx}")


class JpegRegistryAndOracleTests(unittest.TestCase):
    def test_registry_row_and_provenance(self) -> None:
        self.assertEqual(_cc.validate_registry(), 0)
        base = json.loads((ROOT / "converter" / "codecs.json").read_text(encoding="utf-8"))
        row = next(f for f in base["formats"] if f["format"] == "jpeg")
        self.assertEqual(row["availability"], "usable")
        self.assertEqual(row["modes"], {"read": True, "write": True})
        self.assertEqual(row["native_samples"], ["gray8", "rgb8"])
        self.assertEqual(row["metadata_items"], ["icc", "orientation", "exif", "xmp"])
        ex = next(s for s in row["settings"] if s["id"] == "exactness")
        self.assertEqual(ex["default"], "coefficient-exact")
        self.assertEqual(ex["allowed"], ["pixel-exact", "byte-identical", "coefficient-exact"])
        prov = _ref.validate_provenance_table()["jpeg"]
        self.assertEqual(prov["coverage_state"], "executable-in-d10-t02-s5")
        self.assertEqual(prov["reference_decoder_kind"], "independent-pure-python")
        self.assertFalse(prov["shares_c_library_with_converter"])

    def test_oracle_self_check_against_libjpeg(self) -> None:
        # Pillow shares libjpeg-turbo with the converter, so this validates the
        # oracle itself, never the adapter.
        for mode, sub, prog, (w, h) in (
            ("RGB", 0, False, (37, 29)), ("RGB", 1, True, (40, 17)), ("RGB", 2, False, (5, 3)),
            ("RGB", 2, True, (64, 33)), ("L", 0, True, (31, 19)),
        ):
            kw = {"quality": 85, "progressive": prog}
            if mode == "RGB":
                kw["subsampling"] = sub
            data = pillow_jpeg(pattern_image(w, h, mode, 3), **kw)
            with Image.open(io.BytesIO(data)) as im:
                im.load()
                exp = im.tobytes()
            self.assertEqual(_jref.decode_pixels(data)[3], exp, (mode, sub, prog, w, h))


class JpegAdapterMixin:
    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        raise NotImplementedError

    def work_dir(self) -> Path:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        raise NotImplementedError

    def cli_ok(self, args: list[str], timeout: float = 60.0) -> dict[str, str]:
        rc, out, err = self.run_cli(args, timeout)
        self.assertEqual(rc, 0, f"{args}\n{out}\n{err}")
        return parse_kv(out)

    def fixture(self, name: str, data: bytes) -> Path:
        p = self.work_dir() / name
        p.write_bytes(data)
        return p

    def c_coeffs(self, p: Path, tag: str) -> dict:
        dump = self.work_dir() / f"{tag}.rfjc"
        self.cli_ok(["cli", "coeffs", self.to_exe_path(p), self.to_exe_path(dump)])
        return parse_coeff_dump(dump.read_bytes())

    def c_pixels(self, p: Path, tag: str) -> bytes:
        raw = self.work_dir() / f"{tag}.raw"
        self.cli_ok(["cli", "dump", self.to_exe_path(p), self.to_exe_path(raw)])
        return raw.read_bytes()

    def convert(self, src: Path, dst: Path, sfmt: str = "jpeg", dfmt: str = "jpeg",
                exactness: str = "-", meta: str = "-", ack: int = 0) -> tuple[int, dict[str, str]]:
        if dst.exists():
            dst.unlink()
        rc, out, err = self.run_cli(
            ["cli", "convert", sfmt, dfmt, self.to_exe_path(src), self.to_exe_path(dst), exactness, meta, str(ack)]
        )
        return rc, parse_kv(out + "\n" + err)

    def huffman_fixtures(self) -> list[tuple[str, bytes]]:
        fx = []
        for mode, sub, prog, (w, h), rst in (
            ("RGB", 0, False, (37, 29), 0), ("RGB", 1, False, (48, 32), 0), ("RGB", 2, False, (61, 45), 2),
            ("RGB", 2, True, (64, 48), 0), ("RGB", 0, True, (33, 21), 0), ("L", 0, False, (30, 22), 0),
            ("L", 0, True, (45, 40), 0),
        ):
            kw = {"quality": 88, "progressive": prog, "optimize": False}
            if mode == "RGB":
                kw["subsampling"] = sub
            if rst:
                kw["restart_marker_blocks"] = rst
            fx.append((f"pil_{mode}_{sub}_{int(prog)}_{w}x{h}", pillow_jpeg(pattern_image(w, h, mode, w + h), **kw)))
        # Custom geometries: 4:4:0, 4:1:1, and a mixed case where Cb is
        # vertically and Cr horizontally full-resolution against a 2x2 luma.
        for tag, hv, factors in (
            ("440", "12,11,11", [(1, 2), (1, 1), (1, 1)]),
            ("411", "41,11,11", [(4, 1), (1, 1), (1, 1)]),
            ("mixed", "22,12,21", [(2, 2), (1, 2), (2, 1)]),
        ):
            p = self.work_dir() / f"mk_{tag}.jpg"
            self.cli_ok(["cli", "make", self.to_exe_path(p), "53", "39", "3", hv, "90", "0", "0", "0", "0", "7"])
            data = p.read_bytes()
            ref = _jref.decode_coefficients(data)
            self.assertEqual([(c.h, c.v) for c in ref.components], factors, tag)
            fx.append((f"mk_{tag}", data))
        # Extended sequential (SOF1): the same Huffman 8-bit stream under SOF1.
        base = pillow_jpeg(pattern_image(42, 30, "RGB", 9), quality=90, optimize=False)
        sof1 = base.replace(b"\xff\xc0", b"\xff\xc1", 1)
        self.assertEqual(_jref.decode_coefficients(sof1).sof, 0xC1)
        fx.append(("sof1", sof1))
        return fx

    def test_independent_coefficients_pixels_and_optimization(self) -> None:
        wd = self.work_dir()
        for label, data in self.huffman_fixtures():
            src = self.fixture(f"{label}.jpg", data)
            ref = _jref.decode_coefficients(data)
            assert_dump_matches_oracle(self, self.c_coeffs(src, label), ref, label)
            self.assertEqual(self.c_pixels(src, label), _jref.decode_pixels(data)[3], f"{label} pixels")
            dst = wd / f"{label}.opt.jpg"
            rc, kv = self.convert(src, dst)
            self.assertEqual(rc, 0, kv)
            self.assertEqual(kv.get("EXACTNESS"), "coefficient-exact", kv)
            out = dst.read_bytes()
            out_ref = _jref.decode_coefficients(out)
            assert_same_coefficients(self, ref, out_ref, f"{label} optimized")
            # Huffman selectors as the scans that use them declare them.
            self.assertEqual(
                [(c.dc_tbl, c.ac_tbl) for c in ref.components],
                [(c.dc_tbl, c.ac_tbl) for c in out_ref.components],
                f"{label} selectors",
            )
            self.assertEqual(_jref.decode_pixels(out)[3], _jref.decode_pixels(data)[3], f"{label} optimized pixels")
            if ref.progressive:
                # libjpeg always optimizes progressive Huffman tables, so a
                # progressive source never grows and may not shrink.
                self.assertLessEqual(len(out), len(data), label)
            else:
                self.assertLess(len(out), len(data), f"{label}: non-optimized Huffman input must shrink")
                self.assertNotEqual(out, data, label)
            COVERAGE.append((label, "independent-pure-python"))
            # The optimized output is already optimal: the fallback publishes it unchanged.
            again = wd / f"{label}.again.jpg"
            rc, kv = self.convert(dst, again)
            self.assertEqual(rc, 0, kv)
            self.assertEqual(again.read_bytes(), out, f"{label}: not-smaller fallback")

    def test_arithmetic_shared_provider_coverage(self) -> None:
        wd = self.work_dir()
        for prog in (0, 1):
            src = wd / f"arith_{prog}.jpg"
            self.cli_ok(["cli", "make", self.to_exe_path(src), "47", "35", "3", "22,11,11", "85", str(prog), "1", "0", "0", "9"])
            data = src.read_bytes()
            with self.assertRaises(_jref.UnsupportedJpeg):
                _jref.decode_coefficients(data)
            dst = wd / f"arith_{prog}.opt.jpg"
            rc, kv = self.convert(src, dst)
            self.assertEqual(rc, 0, kv)
            a = self.c_coeffs(src, f"arith_src_{prog}")
            b = self.c_coeffs(dst, f"arith_dst_{prog}")
            self.assertEqual(a["arith"], 1)
            self.assertEqual(a, b, "arithmetic coefficients via a fresh shared-provider session")
            self.assertLessEqual(len(dst.read_bytes()), len(data))
            COVERAGE.append((f"arith_prog{prog}", "shared-provider-fresh-session"))

    def test_metadata_preserve_strip_and_png_export(self) -> None:
        wd = self.work_dir()
        icc = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
        exif = Image.Exif()
        exif[0x0112] = 6
        xmp = b'<x:xmpmeta xmlns:x="adobe:ns:meta/">rasterfold</x:xmpmeta>'
        data = pillow_jpeg(pattern_image(40, 28, "RGB", 11), quality=90, icc_profile=icc, exif=exif.tobytes(), xmp=xmp, optimize=False)
        src = self.fixture("meta.jpg", data)
        kv = self.cli_ok(["cli", "probe", self.to_exe_path(src)])
        self.assertEqual(int(kv["ICC_LEN"]), len(icc))
        self.assertEqual(int(kv["XMP_LEN"]), len(xmp))
        self.assertEqual(kv["ORIENTATION"], "6")
        pix = _jref.decode_pixels(data)[3]
        keep = wd / "meta.keep.jpg"
        rc, kv = self.convert(src, keep, meta="preserve")
        self.assertEqual(rc, 0, kv)
        kk = self.cli_ok(["cli", "probe", self.to_exe_path(keep)])
        self.assertEqual((kk["ICC_LEN"], kk["XMP_LEN"], kk["ORIENTATION"]), (str(len(icc)), str(len(xmp)), "6"))
        self.assertEqual(_jref.decode_pixels(keep.read_bytes())[3], pix)
        strip = wd / "meta.strip.jpg"
        rc, kv = self.convert(src, strip, meta="strip")
        self.assertEqual(rc, 0, kv)
        self.assertEqual(kv.get("META_OUTCOME"), "stripped-explicitly", kv)
        ks = self.cli_ok(["cli", "probe", self.to_exe_path(strip)])
        self.assertEqual((ks["ICC_LEN"], ks["EXIF_LEN"], ks["XMP_LEN"], ks["ORIENTATION"]), ("0", "0", "0", "1"))
        self.assertEqual(_jref.decode_pixels(strip.read_bytes())[3], pix)
        # JPEG -> PNG: pixel-exact by default; PNG cannot carry EXIF/XMP/orientation.
        png = wd / "meta.png"
        rc, kv = self.convert(src, png, dfmt="png")
        self.assertNotEqual(rc, 0)
        self.assertFalse(png.exists())
        rc, kv = self.convert(src, png, dfmt="png", ack=META_ITEM_EXIF | META_ITEM_XMP | META_ITEM_ORIENTATION)
        self.assertEqual(rc, 0, kv)
        self.assertEqual(kv.get("EXACTNESS"), "pixel-exact", kv)
        w, h, channels, bd, native = _ref.decode_png(png.read_bytes())
        self.assertEqual((w, h, channels, bd), (40, 28, 3, 8))
        self.assertEqual(native, pix, "PNG export equals the independent pinned-decoder pixels")

    def test_refusals_defaults_and_file_safety(self) -> None:
        wd = self.work_dir()
        data = pillow_jpeg(pattern_image(24, 16, "RGB", 5), quality=80)
        src = self.fixture("ref.jpg", data)
        dst = wd / "ref.out.jpg"
        rc, kv = self.convert(src, dst, exactness="pixel-exact")
        self.assertEqual(kv.get("SUBMIT"), "unsupported-combination", kv)
        self.assertFalse(dst.exists())
        png_src = self.fixture("ref.png", _ref.encode_png(4, 4, 2, 8, bytes(48)))
        rc, kv = self.convert(png_src, dst, sfmt="png")
        self.assertEqual(kv.get("SUBMIT"), "unsupported-combination", kv)
        self.assertFalse(dst.exists())
        # Existing destination survives untouched.
        dst.write_bytes(b"keep-me")
        rc, out, err = self.run_cli(["cli", "convert", "jpeg", "jpeg", self.to_exe_path(src), self.to_exe_path(dst), "-", "-", "0"])
        self.assertNotEqual(rc, 0)
        self.assertEqual(dst.read_bytes(), b"keep-me")
        self.assertEqual(src.read_bytes(), data)
        # CMYK, concatenated images and truncation.
        cmyk = self.fixture("cmyk.jpg", pillow_jpeg(Image.new("CMYK", (8, 8), (1, 2, 3, 4))))
        rc, out, _ = self.run_cli(["cli", "probe", self.to_exe_path(cmyk)])
        self.assertEqual(parse_kv(out)["STATUS"], "unsupported-sample")
        cat = self.fixture("cat.jpg", data + data)
        rc, out, _ = self.run_cli(["cli", "probe", self.to_exe_path(cat)])
        self.assertEqual(parse_kv(out)["STATUS"], "unsupported-combination")
        trunc = self.fixture("trunc.jpg", data[: len(data) // 2])
        rc, out, _ = self.run_cli(["cli", "probe", self.to_exe_path(trunc)])
        self.assertEqual(parse_kv(out)["STATUS"], "truncated-input")

    def mutation_count(self) -> int:
        return 300

    def test_seeded_mutations_never_crash_or_hang(self) -> None:
        wd = self.work_dir()
        base = pillow_jpeg(pattern_image(32, 24, "RGB", 2), quality=75, progressive=True)
        rng = random.Random(0x5EED)
        seen = {"ok": 0, "refused": 0}
        for i in range(self.mutation_count()):
            m = bytearray(base)
            kind = i % 3
            if kind == 0:
                m = m[: rng.randrange(2, len(m))]
            else:
                for _ in range(1 + rng.randrange(6)):
                    m[rng.randrange(2, len(m))] = rng.randrange(256)
            p = wd / "mut.jpg"
            p.write_bytes(bytes(m))
            raw = wd / "mut.raw"
            rc, out, err = self.run_cli(["cli", "dump", self.to_exe_path(p), self.to_exe_path(raw)], timeout=30.0)
            self.assertIn(rc, (0, 2), f"mutation {i}: rc={rc}\n{out}\n{err}")
            seen["ok" if rc == 0 else "refused"] += 1
        self.assertGreater(seen["refused"], 0)
        print(f"jpeg mutations: {seen}")


class HostJpegTests(JpegAdapterMixin, unittest.TestCase):
    host_exe: Path | None = None
    _tmp: tempfile.TemporaryDirectory[str] | None = None
    _wd: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_JPEG_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_JPEG_EXE not configured")
        cls.host_exe = Path(raw)
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_jpeg_host_")
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

    def run_cli(self, args: list[str], timeout: float = 60.0, env: dict[str, str] | None = None) -> tuple[int, str, str]:
        assert self.host_exe is not None
        res = _ref.run_process_tree(
            [str(self.host_exe), *args], cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout
        )
        return res.returncode, res.stdout or "", res.stderr or ""

    def test_photo_pack_optimization_report(self) -> None:
        wd = self.work_dir()
        ratios = []
        total_in = total_out = 0
        for png in sorted((ROOT / "testdata" / "photos").glob("*.png")):
            with Image.open(png) as im:
                data = pillow_jpeg(im.convert("RGB"), quality=90, optimize=False)
            src = self.fixture(f"photo_{png.stem}.jpg", data)
            dst = wd / f"photo_{png.stem}.opt.jpg"
            rc, kv = self.convert(src, dst)
            self.assertEqual(rc, 0, kv)
            out = dst.read_bytes()
            self.assertLess(len(out), len(data), png.name)
            self.assertEqual(self.c_coeffs(src, f"p_{png.stem}_a"), self.c_coeffs(dst, f"p_{png.stem}_b"), png.name)
            ratios.append(len(out) / len(data))
            total_in += len(data)
            total_out += len(out)
        self.assertEqual(len(ratios), 10)
        geo = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
        print(
            f"jpeg photo-pack optimization: total {total_in} -> {total_out} bytes "
            f"(ratio {total_out / total_in:.4f}), geomean {geo:.4f}, worst {max(ratios):.4f}, best {min(ratios):.4f}"
        )

    def test_heap_peak_within_budget_accounting(self) -> None:
        if not sys.platform.startswith("linux"):
            self.skipTest("LD_PRELOAD heap accounting is Linux-only")
        wd = self.work_dir()
        so = _ref.build_hwm_interposer(wd)
        big_markers = wd / "bigmk.jpg"
        im = pattern_image(16, 16, "RGB", 4)
        data = pillow_jpeg(im, quality=85, comment=b"m" * 60000)
        # Pillow writes one COM; add thirteen more 60 KB COM segments after SOI.
        seg = b"\xff\xfe" + struct.pack(">H", 60002) + b"c" * 60000
        big_markers.write_bytes(data[:2] + seg * 13 + data[2:])
        prog = self.fixture("prog512.jpg", pillow_jpeg(pattern_image(512, 512, "RGB", 8), quality=85, progressive=True))
        def measure(op: str, path: Path) -> tuple[int, int]:
            hwm = wd / f"hwm_{op}_{path.stem}.txt"
            env = dict(os.environ, LD_PRELOAD=str(so), RFC_HWM_OUT=str(hwm))
            rc, out, err = self.run_cli(["cli", "budget-peak", op, str(path), str(256 << 20)], env=env)
            self.assertEqual(rc, 0, out + err)
            kv = parse_kv(out)
            self.assertEqual(kv["CURR_ALLOC"], "0", out)
            return int(hwm.read_text(encoding="utf-8").strip()), int(kv["PEAK_BUDGET"])

        for op, path in (("optimize", big_markers), ("optimize", prog), ("decode", prog), ("coeffs", prog)):
            # The CLI's own footprint (input file buffer, registry, stdio) is
            # measured, not assumed: the same process with no adapter call.
            baseline, _ = measure("noop", path)
            actual, budget_peak = measure(op, path)
            self.assertLessEqual(
                actual - baseline, budget_peak,
                f"{op} {path.name}: adapter heap {actual - baseline} (os {actual} - baseline {baseline}) > budget {budget_peak}",
            )
            print(f"jpeg heap peak {op} {path.name}: os={actual} baseline={baseline} budget={budget_peak}")
            # A budget below the reservation refuses with nothing left allocated.
            rc, out, _ = self.run_cli(["cli", "budget-peak", op, str(path), str(budget_peak // 4)])
            self.assertEqual(rc, 2, out)
            kv = parse_kv(out)
            self.assertIn(kv["STATUS"], ("out-of-memory", "limit-exceeded"), out)
            self.assertEqual(kv["CURR_ALLOC"], "0", out)


class WindowsJpegTests(JpegAdapterMixin, unittest.TestCase):
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_JPEG_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_jpeg.exe")
        if not cand.is_file():
            if os.environ.get("RFC_REQUIRE_WIN_JPEG") == "1":
                raise AssertionError(f"missing Windows rfc_test_jpeg.exe: {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_jpeg.exe not built")
        cls.win_exe = cand
        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_jpeg_win_")
            cls._py_temp = Path(cls._win_temp)
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_jpeg_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True, text=True, timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_JPEG") == "1":
                    raise AssertionError("powershell.exe unavailable while RFC_REQUIRE_WIN_JPEG=1")
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(["wslpath", "-u", cls._win_temp], capture_output=True, text=True, timeout=15.0)
            if wp.returncode != 0 or not (wp.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_JPEG") == "1":
                    raise AssertionError("wslpath failed while RFC_REQUIRE_WIN_JPEG=1")
                raise unittest.SkipTest("wslpath unavailable for Windows NTFS tests")
            cls._py_temp = Path(wp.stdout.strip())
        shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_jpeg.exe")

    @classmethod
    def tearDownClass(cls) -> None:
        if not cls._win_temp:
            return
        if os.name == "nt":
            shutil.rmtree(cls._win_temp, ignore_errors=True)
        else:
            _ref.run_process_tree(
                [
                    "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    f"if (Test-Path {ps_quote(cls._win_temp)}) {{ Remove-Item -LiteralPath {ps_quote(cls._win_temp)} -Recurse -Force }}",
                ],
                capture_output=True, timeout=30.0,
            )

    def work_dir(self) -> Path:
        assert self._py_temp is not None
        return self._py_temp

    def to_exe_path(self, p: Path) -> str:
        rel = p.relative_to(self.work_dir())
        return self._win_temp.rstrip("\\/") + "\\" + str(rel).replace("/", "\\")

    def mutation_count(self) -> int:
        return 45  # each call crosses the WSL/Windows process boundary

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        if os.name == "nt":
            exe = str(self.work_dir() / "rfc_test_jpeg.exe")
            env = dict(os.environ)
            root = os.environ.get("SystemRoot", r"C:\Windows")
            env["PATH"] = f"{root}\\System32;{root}"
            res = _ref.run_process_tree([exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True, timeout=timeout)
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_jpeg.exe")
        cmd = f"$env:PATH = \"$env:SystemRoot\\System32;$env:SystemRoot\"; Set-Location {ps_quote(self._win_temp)}; & {exe_ps} {arg_ps}; exit $LASTEXITCODE"
        res = _ref.run_process_tree(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", cmd],
            cwd=ROOT, capture_output=True, text=True, timeout=timeout,
        )
        return res.returncode, res.stdout or "", res.stderr or ""


def tearDownModule() -> None:
    if COVERAGE:
        kinds: dict[str, int] = {}
        for _, k in COVERAGE:
            kinds[k] = kinds.get(k, 0) + 1
        print(f"jpeg coverage: {kinds}")


if __name__ == "__main__":
    unittest.main()
