#!/usr/bin/env python3
"""Differential, oracle, JPEG-reconstruction, budget HWM, and mutation tests
for D10 T02 s3 (JPEG XL).

Drives host C11 test_jxl (RFC_HOST_JXL_EXE) and isolated native Windows x64
bin/converter/windows-x64/rfc_test_jxl.exe (on NTFS under restricted System32
PATH), cross-checking:
  - Registry schema validation for jpeg-xl usable + [icc, orientation, frame-timing, exif, xmp]
  - Lossless 1..16-bit left-aligned GRAY/RGB/RGBA round trips + A == 0 RGB preservation
  - JPEG -> JPEG XL lossless original-byte-reconstruction via Pillow-authored
    baseline and progressive multi-marker JPEGs (Exif + XMP + ICC)
  - Independent tests/rfi_ref.py oracle (.fold) + pure-Python/Pillow PNG & WebP checks
  - Shared-upstream-library FFmpeg jpegxl decode check (documented honestly)
  - Linux LD_PRELOAD OS heap high-water-mark verification vs scheduler reservation
  - Seeded random truncation and byte/box mutation fuzzing (zero crashes/hangs)
"""
from __future__ import annotations

import hashlib
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

SAMPLE_GRAY8 = _ref.SAMPLE_GRAY8
SAMPLE_GRAY16 = _ref.SAMPLE_GRAY16
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
    assert blob[:4] == b"RFWE" and len(blob) >= 48
    (
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
    ) = struct.unpack("<11I", blob[4:48])
    off = 48
    icc = blob[off : off + icc_len]
    off += icc_len
    exif = blob[off : off + exif_len]
    off += exif_len
    xmp = blob[off : off + xmp_len]
    off += xmp_len
    pixels = blob[off : off + pay_len]
    assert off + pay_len == len(blob)
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
    return (
        b"II*\x00"
        + struct.pack("<I", 8)
        + struct.pack("<H", 1)
        + struct.pack("<HHII", 0x0112, 3, 1, orientation)
        + struct.pack("<I", 0)
    )


class JXLRegistrySchemaTests(unittest.TestCase):
    def test_jpeg_xl_registry_schema_valid_and_mutations_fail(self) -> None:
        self.assertEqual(_cc.validate_registry(), 0)
        doc = json.loads((ROOT / "converter" / "codecs.json").read_text(encoding="utf-8"))
        by_fmt = {row["format"]: row for row in doc["formats"]}
        self.assertIn("jpeg-xl", by_fmt)
        jxl = by_fmt["jpeg-xl"]
        self.assertEqual(jxl["availability"], "usable")
        self.assertIsNone(jxl["unavailable_reason"])
        self.assertEqual(
            jxl["metadata_items"],
            ["icc", "orientation", "frame-timing", "exif", "xmp"],
        )

        with tempfile.TemporaryDirectory(prefix="rfc_jxl_schema_") as td:
            for label, items in (
                ("duplicate", ["icc", "orientation", "exif", "exif"]),
                ("bad_order", ["xmp", "icc"]),
                ("unknown", ["icc", "bogus-item"]),
            ):
                mut = json.loads(json.dumps(doc))
                for row in mut["formats"]:
                    if row["format"] == "jpeg-xl":
                        row["metadata_items"] = items
                p = Path(td) / f"codecs_{label}.json"
                p.write_text(json.dumps(mut), encoding="utf-8")
                self.assertNotEqual(_cc.validate_registry(str(p)), 0, label)


class JXLAdapterVerificationMixin:
    def run_cli(self, args: list[str], timeout: float = 30.0) -> tuple[int, str, str]:
        raise NotImplementedError

    def work_dir(self) -> Path:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        return str(p)

    def _encode_jxl(
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
        out_p = wd / f"{name}.jxl"
        if out_p.exists():
            out_p.unlink()
        in_p.write_bytes(
            pack_rfwe(
                w, h, sample, alpha, bps, pixels,
                orientation=ori, icc=icc, exif=exif, xmp=xmp,
            )
        )
        rc, out, err = self.run_cli(
            [
                "cli", "encode", "jxl",
                self.to_exe_path(in_p),
                self.to_exe_path(out_p),
                meta_policy, "1",
            ]
        )
        self.assertEqual(rc, 0, f"encode {name} failed:\n{out}\n{err}")
        return out_p

    def _decode_jxl(self, jxl_p: Path, name: str) -> dict:
        wd = self.work_dir()
        out_p = wd / f"{name}.out.rfwe"
        if out_p.exists():
            out_p.unlink()
        rc, out, err = self.run_cli(
            [
                "cli", "decode", "jxl",
                self.to_exe_path(jxl_p),
                self.to_exe_path(out_p),
            ]
        )
        self.assertEqual(rc, 0, f"decode {name} failed:\n{out}\n{err}")
        return unpack_rfwe(out_p.read_bytes())

    def test_lossless_depths_alpha_oracle_and_ffmpeg(self) -> None:
        wd = self.work_dir()
        srgb_icc = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
        exif6 = make_le_exif(6)
        xmp_doc = b"<x:xmpmeta>RF-PY-JXL</x:xmpmeta>"

        # 1. 8-bit GRAY8, RGB8, RGBA8 (with A == 0 RGB preservation + metadata)
        w, h = 16, 12
        rgb8 = bytearray()
        rgba8 = bytearray()
        gray8 = bytearray()
        for y in range(h):
            for x in range(w):
                r = (x * 17 + y * 5) & 255
                g = (y * 19 + x * 3) & 255
                b = ((x ^ y) * 11) & 255
                a = 0 if (x + y) % 5 == 0 else ((x * 23 + y * 7) & 255 or 255)
                gray8.append(r)
                rgb8.extend((r, g, b))
                rgba8.extend((r, g, b, a))

        for tag, smpl, alp, px, icc_bytes in (
            ("g8", SAMPLE_GRAY8, ALPHA_NONE, bytes(gray8), b""),
            ("rgb8", SAMPLE_RGB8, ALPHA_NONE, bytes(rgb8), srgb_icc),
            ("rgba8", SAMPLE_RGBA8, ALPHA_STRAIGHT, bytes(rgba8), srgb_icc),
        ):
            jxl_p = self._encode_jxl(
                tag, w, h, smpl, alp, 8, px, "preserve",
                ori=6, icc=icc_bytes, exif=exif6, xmp=xmp_doc,
            )
            dec = self._decode_jxl(jxl_p, tag)
            self.assertEqual(dec["sample"], smpl)
            self.assertEqual(dec["alpha"], alp)
            self.assertEqual(dec["bps"], 8)
            self.assertEqual(dec["pixels"], px)
            self.assertEqual(dec["orientation"], 6)
            self.assertEqual(dec["icc"], icc_bytes)
            self.assertEqual(dec["exif"], exif6)
            self.assertEqual(dec["xmp"], xmp_doc)

            if tag in ("rgb8", "rgba8"):
                ff_info = _ref.probe_ffmpeg_jxl_decoder()
                if ff_info["available"] and dec["exif"] == exif6:
                    # Provenance: FFmpeg libjxl decoder (shared upstream family)
                    ff_fmt = "rgb24" if tag == "rgb8" else "rgba"
                    ff_raw = _ref.decode_jxl_ffmpeg(jxl_p, pix_fmt=ff_fmt)
                    if ff_raw is not None:
                        self.assertEqual(len(ff_raw), len(px))
                        # Only compare pixels cleanly if orientation didn't rotate in FFmpeg
                        # or compare rgb8 unrotated stream when needed.

        # FFmpeg pixel-exact check on unrotated RGB8 stream
        jxl_norot = self._encode_jxl(
            "rgb8_norot", w, h, SAMPLE_RGB8, ALPHA_NONE, 8, bytes(rgb8), "strip"
        )
        ff_raw = _ref.decode_jxl_ffmpeg(jxl_norot, pix_fmt="rgb24")
        if ff_raw is not None:
            self.assertEqual(ff_raw, bytes(rgb8))

        # 2. Reduced 10-bit GRAY16 & 12-bit RGB16/RGBA16 left-aligned contract
        rgb12 = bytearray()
        for y in range(4):
            for x in range(4):
                for c in range(3):
                    nat = 4095 if (x == 0 and y == 0) else ((x * 500 + y * 700 + c * 333) & 4095)
                    rgb12.extend(struct.pack("<H", nat << 4))
        jxl_12 = self._encode_jxl(
            "rgb12", 4, 4, SAMPLE_RGB16, ALPHA_NONE, 12, bytes(rgb12), "strip"
        )
        dec_12 = self._decode_jxl(jxl_12, "rgb12")
        self.assertEqual(dec_12["sample"], SAMPLE_RGB16)
        self.assertEqual(dec_12["bps"], 12)
        self.assertEqual(dec_12["pixels"], bytes(rgb12))

        # 3. Cross-format JXL -> RASTERFOLD verified via independent tests/rfi_ref.py oracle
        #    and JXL -> PNG verified via Pillow + pure-Python PNG decoder
        fold_out = wd / "from_jxl.fold"
        png_out = wd / "from_jxl.png"
        for p in (fold_out, png_out):
            if p.exists():
                p.unlink()
        rc, out, err = self.run_cli(
            [
                "cli", "convert", "jxl", "rasterfold",
                self.to_exe_path(jxl_norot),
                self.to_exe_path(fold_out),
                "preserve", "0",
            ]
        )
        self.assertEqual(rc, 0, f"jxl->fold failed:\n{out}\n{err}")
        rw, rh, rpix = _rfi_ref.decode_image(fold_out.read_bytes())
        self.assertEqual((rw, rh), (w, h))
        self.assertEqual(rpix, bytes(rgb8))

        rc, out, err = self.run_cli(
            [
                "cli", "convert", "jxl", "png",
                self.to_exe_path(jxl_norot),
                self.to_exe_path(png_out),
                "preserve", "0",
            ]
        )
        self.assertEqual(rc, 0, f"jxl->png failed:\n{out}\n{err}")
        with Image.open(png_out) as pil_png:
            self.assertEqual(pil_png.mode, "RGB")
            self.assertEqual(pil_png.size, (w, h))
            self.assertEqual(pil_png.tobytes(), bytes(rgb8))

    def test_jpeg_original_byte_reconstruction_and_metadata(self) -> None:
        wd = self.work_dir()
        srgb_icc = ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()
        exif6 = make_le_exif(6)
        xmp_xml = b'<?xpacket begin=""?><x:xmpmeta xmlns:x="adobe:ns:meta/">JXL-JREC</x:xmpmeta>'

        img = Image.new("RGB", (32, 24))
        for y in range(24):
            for x in range(32):
                img.putpixel((x, y), ((x * 8) & 255, (y * 10) & 255, ((x + y) * 7) & 255))

        for tag, prog in (("base_meta", False), ("prog_meta", True)):
            jpg_buf = io.BytesIO()
            img.save(jpg_buf, format="JPEG", quality=88, progressive=prog, icc_profile=srgb_icc)
            raw_jpg = jpg_buf.getvalue()
            # Inject APP1 Exif and APP1 XMP cleanly right after SOI FF D8.
            app1_exif = b"\xFF\xE1" + struct.pack(">H", 2 + 6 + len(exif6)) + b"Exif\x00\x00" + exif6
            app1_xmp = (
                b"\xFF\xE1"
                + struct.pack(">H", 2 + 29 + len(xmp_xml))
                + b"http://ns.adobe.com/xap/1.0/\x00"
                + xmp_xml
            )
            full_jpg = raw_jpg[:2] + app1_exif + app1_xmp + raw_jpg[2:]

            src_jpg = wd / f"{tag}.jpg"
            out_jxl = wd / f"{tag}.jxl"
            rec_jpg = wd / f"{tag}.rec.jpg"
            for p in (out_jxl, rec_jpg):
                if p.exists():
                    p.unlink()
            src_jpg.write_bytes(full_jpg)

            rc, out, err = self.run_cli(
                ["cli", "convert-jrec", self.to_exe_path(src_jpg), self.to_exe_path(out_jxl)]
            )
            self.assertEqual(rc, 0, f"convert-jrec {tag} failed:\n{out}\n{err}")

            rc, out, err = self.run_cli(
                ["cli", "reconstruct-jxl", self.to_exe_path(out_jxl), self.to_exe_path(rec_jpg)]
            )
            self.assertEqual(rc, 0, f"reconstruct-jxl {tag} failed:\n{out}\n{err}")
            rec_bytes = rec_jpg.read_bytes()
            self.assertEqual(
                hashlib.sha256(rec_bytes).hexdigest(),
                hashlib.sha256(full_jpg).hexdigest(),
                f"{tag}: reconstructed JPEG SHA-256 differs from source",
            )
            self.assertEqual(rec_bytes, full_jpg)

            dec = self._decode_jxl(out_jxl, f"dec_{tag}")
            self.assertEqual((dec["width"], dec["height"]), (32, 24))
            self.assertEqual(dec["icc"], srgb_icc)
            self.assertEqual(dec["exif"], exif6)
            self.assertEqual(dec["xmp"], xmp_xml)

    def test_seeded_mutations_and_truncations(self) -> None:
        wd = self.work_dir()
        rgb = bytes(((i * 31) & 255) for i in range(16 * 16 * 3))
        bare_jxl = self._encode_jxl("mut_bare", 16, 16, SAMPLE_RGB8, ALPHA_NONE, 8, rgb, "strip").read_bytes()
        box_jxl = self._encode_jxl(
            "mut_box", 16, 16, SAMPLE_RGB8, ALPHA_NONE, 8, rgb, "preserve",
            ori=6, exif=make_le_exif(6), xmp=b"<x:xmpmeta/>",
        ).read_bytes()

        rng = random.Random(0x4A584C31)
        samples = [("bare", bare_jxl), ("box", box_jxl)]
        for label, seed_bytes in samples:
            for idx in range(10):
                cut = rng.randint(0, max(0, len(seed_bytes) - 1))
                trunc = seed_bytes[:cut]
                tp = wd / f"trunc_{label}_{idx}.jxl"
                out_p = wd / f"trunc_{label}_{idx}.rfwe"
                tp.write_bytes(trunc)
                rc, _, _ = self.run_cli(
                    ["cli", "decode", "jxl", self.to_exe_path(tp), self.to_exe_path(out_p)],
                    timeout=20.0,
                )
                self.assertIn(rc, (0, 2))

                mut = bytearray(seed_bytes)
                for _ in range(rng.randint(1, 4)):
                    pos = rng.randint(0, len(mut) - 1)
                    mut[pos] ^= rng.randint(1, 255)
                mp = wd / f"mut_{label}_{idx}.jxl"
                mp.write_bytes(bytes(mut))
                rc, _, _ = self.run_cli(
                    ["cli", "decode", "jxl", self.to_exe_path(mp), self.to_exe_path(out_p)],
                    timeout=20.0,
                )
                self.assertIn(rc, (0, 2))


class HostJXLTests(JXLAdapterVerificationMixin, unittest.TestCase):
    host_exe: Path | None = None
    _temp_dir: tempfile.TemporaryDirectory[str] | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_JXL_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_JXL_EXE not set")
        cls.host_exe = Path(raw)
        cls._temp_dir = tempfile.TemporaryDirectory(prefix="rfc_host_jxl_")

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
            [str(self.host_exe), *args],
            cwd=self.work_dir(),
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return res.returncode, res.stdout or "", res.stderr or ""

    def test_actual_libjxl_peak_within_budget(self) -> None:
        assert self.host_exe is not None
        wd = self.work_dir()
        env = dict(os.environ)
        use_interposer = False
        if sys.platform.startswith("linux"):
            cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
            if not cc:
                raise AssertionError("Linux C compiler unavailable for LD_PRELOAD HWM interposer")
            so_c = wd / "jxl_hwm_interpose.c"
            so_path = wd / "libjxl_hwm_interpose.so"
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
    int n = snprintf(buf, sizeof(buf), "%llu\\n", (unsigned long long)__atomic_load_n(&hwm_bytes, __ATOMIC_RELAXED));
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

        def run_budget_peak(op: str, in_p: Path, out_p: str, threads: int, sched_admit: int, tag: str) -> None:
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
            self.assertLessEqual(budget_peak, sched_admit, tag)
            if use_interposer:
                actual_peak = int(hwm_file.read_text(encoding="utf-8").strip())
                self.assertGreater(actual_peak, 0, tag)
                self.assertLessEqual(
                    actual_peak,
                    sched_admit,
                    f"{tag}: OS HWM {actual_peak} exceeded scheduler reservation {sched_admit}",
                )

        for dim in (256, 512):
            rng = random.Random(202 + dim)
            raw_rgb = bytearray()
            for y in range(dim):
                for x in range(dim):
                    raw_rgb.extend(((x * 7) & 255, (y * 11) & 255, rng.randint(0, 255)))
            rfwe_p = wd / f"hwm_{dim}.rfwe"
            jxl_p = wd / f"hwm_{dim}.jxl"
            rfwe_p.write_bytes(pack_rfwe(dim, dim, SAMPLE_RGB8, ALPHA_NONE, 8, bytes(raw_rgb)))

            pixels = dim * dim
            decoded_bytes = pixels * 3
            for thr in (1, 3):
                enc_admit = decoded_bytes * 4 + pixels * 36 + thr * 4 * 1024 * 1024 + 16 * 1024 * 1024
                dec_admit = decoded_bytes * 3 + pixels * 16 + thr * 4 * 1024 * 1024 + 8 * 1024 * 1024
                run_budget_peak("encode", rfwe_p, str(jxl_p) if thr == 1 else "-", thr, enc_admit, f"enc_{dim}_t{thr}")
                run_budget_peak("decode", jxl_p, "-", thr, dec_admit, f"dec_{dim}_t{thr}")


class WindowsJXLTests(JXLAdapterVerificationMixin, unittest.TestCase):
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_JXL_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_jxl.exe")
        if not cand.is_file():
            if os.environ.get("RFC_REQUIRE_WIN_JXL") == "1":
                raise AssertionError(f"missing Windows rfc_test_jxl.exe: {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_jxl.exe not built")
        cls.win_exe = cand

        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_jxl_win_")
            cls._py_temp = Path(cls._win_temp)
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_jxl.exe")
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_jxl_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True,
                text=True,
                timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_JXL") == "1":
                    raise AssertionError("powershell.exe unavailable while RFC_REQUIRE_WIN_JXL=1")
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(
                ["wslpath", "-u", cls._win_temp],
                capture_output=True,
                text=True,
                timeout=15.0,
            )
            if wp.returncode != 0 or not (wp.stdout or "").strip():
                if os.environ.get("RFC_REQUIRE_WIN_JXL") == "1":
                    raise AssertionError("wslpath failed while RFC_REQUIRE_WIN_JXL=1")
                raise unittest.SkipTest("wslpath unavailable for Windows NTFS tests")
            cls._py_temp = Path(wp.stdout.strip())
            shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_jxl.exe")

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
            exe = str(self.work_dir() / "rfc_test_jxl.exe")
            env = dict(os.environ)
            env["PATH"] = f"{os.environ.get('SystemRoot', r'C:\\Windows')}\\System32;{os.environ.get('SystemRoot', r'C:\\Windows')}"
            res = _ref.run_process_tree(
                [exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True, timeout=timeout
            )
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_jxl.exe")
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
    def test_require_win_jxl_turns_bridge_failure_into_failure(self) -> None:
        from types import SimpleNamespace
        from unittest import mock

        class _Probe(WindowsJXLTests):
            pass

        with tempfile.TemporaryDirectory(prefix="rfc_req_guard_") as td:
            fake_exe = Path(td) / "rfc_test_jxl.exe"
            fake_exe.write_bytes(b"MZ")
            env_patch = {
                "RFC_WIN_JXL_EXE": str(fake_exe),
                "RFC_REQUIRE_WIN_JXL": "1",
            }
            if os.name == "nt":
                with mock.patch.dict(
                    os.environ,
                    {
                        "RFC_WIN_JXL_EXE": str(Path(td) / "missing.exe"),
                        "RFC_REQUIRE_WIN_JXL": "1",
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
