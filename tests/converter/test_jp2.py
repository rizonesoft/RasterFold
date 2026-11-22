"""Differential, malformed, and mutation tests for JPEG 2000 (D10 T02 s9).

Reports three verification tracks separately:
1. Independent native FFmpeg jpeg2000 decoder (-c:v jpeg2000) sample equality.
2. Shared-upstream-library Pillow openjp2 cross-check (explicitly labeled
   shared-upstream-library because Pillow links OpenJPEG).
3. Same-provider fresh bundled OpenJPEG 2.5.4 session decode.
"""

from __future__ import annotations

import io
import os
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image

import reference as _ref

ROOT = Path(__file__).resolve().parents[2]

SAMPLE_GRAY8 = 1
SAMPLE_GRAY16 = 2
SAMPLE_RGB8 = 3
SAMPLE_RGBA8 = 4
SAMPLE_RGB16 = 5
SAMPLE_RGBA16 = 6

ALPHA_NONE = 0
ALPHA_OPAQUE = 1
ALPHA_STRAIGHT = 2

JP2_SIG = b"\x00\x00\x00\x0cjP  \r\n\x87\n"


def parse_kv(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for ln in text.splitlines():
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def pack_rfqd(
    w: int,
    h: int,
    fmt: int,
    alpha: int,
    bits: int,
    stride: int,
    data: bytes,
    icc: bytes = b"",
) -> bytes:
    return (
        b"RFQD"
        + struct.pack("<IIIIIIII", w, h, fmt, alpha, bits, stride, len(data), len(icc))
        + data
        + icc
    )


def unpack_rfqd(blob: bytes) -> tuple[int, int, int, int, int, int, bytes, bytes]:
    if len(blob) < 36 or blob[:4] != b"RFQD":
        raise ValueError("bad RFQD")
    w, h, fmt, alpha, bits, stride, dlen, ilen = struct.unpack_from("<IIIIIIII", blob, 4)
    data = blob[36 : 36 + dlen]
    icc = blob[36 + dlen : 36 + dlen + ilen]
    return w, h, fmt, alpha, bits, stride, data, icc


def find_jp2_box(blob: bytes, tag: bytes) -> bytes | None:
    if len(blob) < 12 or blob[:12] != JP2_SIG:
        return None
    off = 12
    while off + 8 <= len(blob):
        lbox = struct.unpack_from(">I", blob, off)[0]
        tbox = blob[off + 4 : off + 8]
        total = (len(blob) - off) if lbox == 0 else lbox
        if total < 8 or off + total > len(blob):
            return None
        if tbox == tag:
            return blob[off + 8 : off + total]
        off += total
    return None


def find_sub_box(payload: bytes, tag: bytes) -> bytes | None:
    off = 0
    while off + 8 <= len(payload):
        lbox = struct.unpack_from(">I", payload, off)[0]
        tbox = payload[off + 4 : off + 8]
        total = (len(payload) - off) if lbox == 0 else lbox
        if total < 8 or off + total > len(payload):
            return None
        if tbox == tag:
            return payload[off + 8 : off + total]
        off += total
    return None


def cod_is_reversible(jp2_bytes: bytes) -> bool:
    cs = find_jp2_box(jp2_bytes, b"jp2c")
    if not cs or len(cs) < 6 or cs[:4] != b"\xff\x4f\xff\x51":
        return False
    lsiz = struct.unpack_from(">H", cs, 4)[0]
    pos = 4 + lsiz
    while pos + 4 <= len(cs):
        if cs[pos] != 0xFF:
            return False
        m = cs[pos + 1]
        if m == 0x90:
            break
        ml = struct.unpack_from(">H", cs, pos + 2)[0]
        if m == 0x52 and ml >= 12:
            return cs[pos + 13] == 1
        pos += 2 + ml
    return False


def _find_ffmpeg_with_jpeg2000() -> str | None:
    candidates: list[str] = []
    for name in ("ffmpeg", "ffmpeg.exe"):
        found = shutil.which(name)
        if found:
            candidates.append(found)
    winget = "/mnt/c/Users/DerickPayne/AppData/Local/Microsoft/WinGet/Links/ffmpeg.exe"
    if os.path.isfile(winget):
        candidates.append(winget)
    for exe in candidates:
        res = _ref.run_process_tree(
            [exe, "-v", "error", "-decoders"],
            timeout=15.0,
            capture_output=True,
            text=True,
        )
        if res.returncode == 0 and " jpeg2000 " in res.stdout:
            return exe
    return None


def _to_ffmpeg_path(exe: str, p: Path) -> str:
    s = os.fspath(p)
    if os.name != "nt" and exe.lower().endswith(".exe"):
        wp = _ref.run_process_tree(
            ["wslpath", "-w", s], timeout=5.0, capture_output=True, text=True
        )
        if wp.returncode != 0:
            raise RuntimeError(f"wslpath failed for {s}")
        return wp.stdout.strip()
    return s


def _run_ffmpeg_jpeg2000(exe: str, jp2_path: Path, pix_fmt: str) -> bytes:
    target = _to_ffmpeg_path(exe, jp2_path)
    cmd = [
        exe,
        "-v",
        "error",
        "-c:v",
        "jpeg2000",
        "-i",
        target,
        "-f",
        "rawvideo",
        "-pix_fmt",
        pix_fmt,
        "-",
    ]
    res = _ref.run_process_tree(cmd, timeout=30.0, capture_output=True, text=False)
    if res.returncode != 0:
        raise RuntimeError(
            f"ffmpeg jpeg2000 failed ({res.returncode}): "
            f"{res.stderr.decode(errors='replace')}"
        )
    return res.stdout


class Jp2AdapterMixin:
    TARGET_LABEL = "linux-x64"
    _work_dir: Path | None = None

    def run_cli(self, args: list[str]) -> tuple[int, str, str]:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        return str(p)

    def _wd(self) -> Path:
        assert self._work_dir is not None
        return self._work_dir

    def test_differential_independent_ffmpeg_and_shared_pillow_samples_and_icc(self) -> None:
        wd = self._wd()
        ffmpeg = _find_ffmpeg_with_jpeg2000()
        self.assertIsNotNone(ffmpeg, "independent native FFmpeg jpeg2000 decoder required")
        assert ffmpeg is not None

        icc_blob = b"\x00\x00\x00\x20test\x04\x30\x00\x00mntrRGB XYZ acsp\x01\x02\x03\x04"

        w, h = 13, 11
        gray8 = bytes(((x * 23 + y * 41) & 0xFF) for y in range(h) for x in range(w))
        gray16 = b"".join(
            struct.pack("<H", (x * 4093 + y * 2053) & 0xFFFF)
            for y in range(h)
            for x in range(w)
        )
        rgb8 = bytes(
            ((x * 37 + y * 19 + c * 67) & 0xFF)
            for y in range(h)
            for x in range(w)
            for c in range(3)
        )
        rgba8_arr = bytearray(
            ((x * 31 + y * 17 + c * 59) & 0xFF) if c < 3 else (0 if (x == 0 and y == 0) else 200)
            for y in range(h)
            for x in range(w)
            for c in range(4)
        )
        # Pixel (0,0) has A==0 with distinct non-zero RGB
        rgba8_arr[0:4] = bytes([0xDE, 0xAD, 0xBE, 0x00])
        rgba8 = bytes(rgba8_arr)

        rgb16 = b"".join(
            struct.pack("<H", (x * 3001 + y * 1997 + c * 7919) & 0xFFFF)
            for y in range(h)
            for x in range(w)
            for c in range(3)
        )
        rgba16_vals: list[int] = []
        for y in range(h):
            for x in range(w):
                if x == 0 and y == 0:
                    rgba16_vals.extend([0x1234, 0x5678, 0x9ABC, 0x0000])
                else:
                    rgba16_vals.extend(
                        [
                            (x * 2503 + y * 997) & 0xFFFF,
                            (x * 1117 + y * 3571) & 0xFFFF,
                            (x * 4001 + y * 1609) & 0xFFFF,
                            0xC000,
                        ]
                    )
        rgba16 = struct.pack(f"<{len(rgba16_vals)}H", *rgba16_vals)

        cases = [
            ("gray8", SAMPLE_GRAY8, ALPHA_NONE, 8, w, gray8, b"", "gray"),
            ("gray16", SAMPLE_GRAY16, ALPHA_NONE, 16, w * 2, gray16, b"", "gray16le"),
            ("rgb8", SAMPLE_RGB8, ALPHA_NONE, 8, w * 3, rgb8, b"", "rgb24"),
            ("rgba8_icc", SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, w * 4, rgba8, icc_blob, "rgba"),
            ("rgb16", SAMPLE_RGB16, ALPHA_NONE, 16, w * 6, rgb16, b"", "rgb48le"),
            ("rgba16", SAMPLE_RGBA16, ALPHA_STRAIGHT, 16, w * 8, rgba16, b"", "rgba64le"),
        ]

        for name, fmt, alpha, bits, stride, pixels, icc, ff_fmt in cases:
            p_in = wd / f"{name}.rfqd"
            p_jp2 = wd / f"{name}.jp2"
            p_dec = wd / f"{name}_dec.rfqd"
            p_in.write_bytes(pack_rfqd(w, h, fmt, alpha, bits, stride, pixels, icc))

            rc, out, err = self.run_cli(
                ["encode", self.to_exe_path(p_in), self.to_exe_path(p_jp2), "preserve"]
            )
            self.assertEqual(rc, 0, f"encode {name} failed: {out}\n{err}")
            jp2_bytes = p_jp2.read_bytes()
            self.assertTrue(cod_is_reversible(jp2_bytes), f"{name}: COD not 5-3 reversible")

            if name == "rgba8_icc":
                jp2h = find_jp2_box(jp2_bytes, b"jp2h")
                self.assertIsNotNone(jp2h)
                assert jp2h is not None
                colr = find_sub_box(jp2h, b"colr")
                cdef = find_sub_box(jp2h, b"cdef")
                self.assertIsNotNone(colr, "rgba8_icc missing colr box")
                self.assertIsNotNone(cdef, "rgba8_icc missing cdef box")
                assert colr is not None and cdef is not None
                self.assertEqual(colr[0], 2)
                self.assertEqual(colr[3:], icc_blob)
                self.assertEqual(struct.unpack_from(">H", cdef, 0)[0], 4)
                # Channel 3: cn=3, typ=1 (straight alpha), asoc=0
                self.assertEqual(struct.unpack_from(">HHH", cdef, 2 + 3 * 6), (3, 1, 0))

            # 1. Independent native FFmpeg jpeg2000 decode check.
            ff_raw = _run_ffmpeg_jpeg2000(ffmpeg, p_jp2, ff_fmt)
            self.assertEqual(ff_raw, pixels, f"{name}: independent FFmpeg jpeg2000 mismatch")

            # 2. Shared-library Pillow openjp2 cross-check for 8-bit modes.
            # (Pillow's Jpeg2KImagePlugin does not populate info["icc_profile"] from
            # colr boxes; ICC bytes are verified directly in the JP2 colr box above
            # and via fresh decode below.)
            if bits == 8:
                with Image.open(io.BytesIO(jp2_bytes)) as pim:
                    self.assertEqual(pim.size, (w, h))
                    self.assertEqual(pim.tobytes(), pixels, f"{name}: Pillow openjp2 mismatch")

            # 3. Same-provider fresh bundled OpenJPEG decode check.
            rc, out, err = self.run_cli(
                ["decode", self.to_exe_path(p_jp2), self.to_exe_path(p_dec), "preserve"]
            )
            self.assertEqual(rc, 0, f"decode {name} failed: {out}\n{err}")
            dw, dh, dfmt, dalpha, dbits, _dstride, dpix, dicc = unpack_rfqd(p_dec.read_bytes())
            self.assertEqual((dw, dh, dfmt, dalpha, dbits), (w, h, fmt, alpha, bits))
            self.assertEqual(dpix, pixels)
            self.assertEqual(dicc, icc)

        # Reduced-precision 12-bit left-aligned MSB check (Plan #5):
        # 0xFFF0 valid -> FFmpeg decodes native 12-bit 4095 (0xFFF0 in rgb48le or 0x0FFF),
        # 0xFFF1 invalid -> refused with invalid-argument.
        p12_ok = wd / "rgb12_ok.rfqd"
        p12_jp2 = wd / "rgb12_ok.jp2"
        p12_vals = [0xFFF0, 0x8000, 0x0010, 0x0000] * 3  # 2x2 RGB12
        p12_ok.write_bytes(
            pack_rfqd(2, 2, SAMPLE_RGB16, ALPHA_NONE, 12, 12, struct.pack("<12H", *p12_vals))
        )
        rc, out, err = self.run_cli(
            ["encode", self.to_exe_path(p12_ok), self.to_exe_path(p12_jp2), "preserve"]
        )
        self.assertEqual(rc, 0, f"12-bit valid encode failed: {out}\n{err}")
        cs12 = find_jp2_box(p12_jp2.read_bytes(), b"jp2c")
        self.assertIsNotNone(cs12)
        assert cs12 is not None
        self.assertEqual(cs12[42], 11, "12-bit SIZ Ssiz must equal 11 (12-bit unsigned)")
        ff12 = struct.unpack("<12H", _run_ffmpeg_jpeg2000(ffmpeg, p12_jp2, "rgb48le"))
        # FFmpeg scales 12-bit native v to rgb48le via (v << 4) | (v >> 8); verify native v == p12_vals[i] >> 4.
        for got, exp_shifted in zip(ff12, p12_vals):
            self.assertEqual(got >> 4, exp_shifted >> 4)

        p12_bad = wd / "rgb12_bad.rfqd"
        bad_vals = list(p12_vals)
        bad_vals[0] = 0xFFF1  # non-zero low padding bit
        p12_bad.write_bytes(
            pack_rfqd(2, 2, SAMPLE_RGB16, ALPHA_NONE, 12, 12, struct.pack("<12H", *bad_vals))
        )
        rc, out, _err = self.run_cli(
            ["encode", self.to_exe_path(p12_bad), self.to_exe_path(wd / "bad12.jp2"), "preserve"]
        )
        self.assertNotEqual(rc, 0)
        self.assertEqual(parse_kv(out).get("status"), "invalid-argument")

        print(
            f"jp2-coverage: target={self.TARGET_LABEL} "
            "independent-ffmpeg-jpeg2000 (GRAY8/16, RGB8/16, RGBA8+ICC+cdef, RGBA16, RGB12); "
            "shared-provider-pillow-openjp2 (8-bit); same-provider-fresh-decode"
        )

    def test_named_malformed_two_tier_and_seeded_mutations(self) -> None:
        wd = self._wd()
        # Generate a valid 8x8 RGB8 JP2 and a valid 8x8 RGBA8 JP2.
        w, h = 8, 8
        rgb8 = bytes(((i * 19 + 7) & 0xFF) for i in range(w * h * 3))
        p_in = wd / "ctrl.rfqd"
        p_jp2 = wd / "ctrl.jp2"
        p_in.write_bytes(pack_rfqd(w, h, SAMPLE_RGB8, ALPHA_NONE, 8, w * 3, rgb8))
        rc, out, err = self.run_cli(
            ["encode", self.to_exe_path(p_in), self.to_exe_path(p_jp2), "preserve"]
        )
        self.assertEqual(rc, 0, f"ctrl encode failed: {out}\n{err}")
        valid_jp2 = p_jp2.read_bytes()
        cs_off = valid_jp2.index(b"jp2c") + 4

        rgba8 = bytes(((i * 13 + 3) & 0xFF) for i in range(w * h * 4))
        p_in4 = wd / "ctrl4.rfqd"
        p_jp2_4 = wd / "ctrl4.jp2"
        p_in4.write_bytes(pack_rfqd(w, h, SAMPLE_RGBA8, ALPHA_STRAIGHT, 8, w * 4, rgba8))
        rc, _, _ = self.run_cli(
            ["encode", self.to_exe_path(p_in4), self.to_exe_path(p_jp2_4), "preserve"]
        )
        self.assertEqual(rc, 0)
        bare_4ch = find_jp2_box(p_jp2_4.read_bytes(), b"jp2c")
        self.assertIsNotNone(bare_4ch)
        assert bare_4ch is not None

        # Tier-1: structural / representation / limit failures must precede pixel allocation (peak_alloc == 0).
        signed_jp2 = bytearray(valid_jp2)
        signed_jp2[cs_off + 42] |= 0x80
        sub_jp2 = bytearray(valid_jp2)
        sub_jp2[cs_off + 43] = 2

        tier1 = [
            ("bad_sig", b"NOPE" + valid_jp2[4:], "invalid-argument"),
            ("trunc_hdr", valid_jp2[:16], "truncated-input"),
            ("signed_ssiz", bytes(signed_jp2), "unsupported-sample"),
            ("subsampled", bytes(sub_jp2), "unsupported-sample"),
            ("bare_4ch_j2k", bare_4ch, "unsupported-combination"),
        ]
        for tag, blob, exp_status in tier1:
            p_b = wd / f"t1_{tag}.jp2"
            p_o = wd / f"t1_{tag}.rfqd"
            p_b.write_bytes(blob)
            for mode in ("preserve", "strip"):
                if p_o.exists():
                    p_o.unlink()
                rc, out, _ = self.run_cli(
                    ["decode", self.to_exe_path(p_b), self.to_exe_path(p_o), mode]
                )
                self.assertNotEqual(rc, 0, f"{tag} ({mode}) succeeded unexpectedly")
                kv = parse_kv(out)
                self.assertEqual(kv.get("status"), exp_status, f"{tag} ({mode}): {out}")
                self.assertEqual(kv.get("provider_calls"), "0", f"{tag} ({mode}) entered provider before refusal: {out}")
                self.assertEqual(kv.get("peak_budget"), "0", f"{tag} ({mode}) reserved budget before refusal: {out}")
                self.assertEqual(kv.get("peak_alloc"), "0", f"{tag} ({mode}) allocated before refusal: {out}")
                self.assertEqual(kv.get("live_budget"), "0", f"{tag} ({mode}) leaked budget: {out}")
                self.assertEqual(kv.get("frame_alloc_calls"), "0", f"{tag} ({mode}) allocated output frame: {out}")
                self.assertEqual(kv.get("out_frames"), "0")
                self.assertFalse(p_o.exists())

        # Tier-2: valid framing with truncated/corrupt tile payload fails within budget,
        # releases reservation to 0, allocates no output frame, leaves source intact.
        trunc_tile = bytearray(valid_jp2[:-6])
        struct.pack_into(">I", trunc_tile, cs_off - 8, len(trunc_tile) - (cs_off - 8))
        p_t2 = wd / "t2_corrupt_tile.jp2"
        p_t2_out = wd / "t2_corrupt_tile.rfqd"
        p_t2.write_bytes(bytes(trunc_tile))
        for mode in ("preserve", "strip"):
            if p_t2_out.exists():
                p_t2_out.unlink()
            rc, out, _ = self.run_cli(
                ["decode", self.to_exe_path(p_t2), self.to_exe_path(p_t2_out), mode]
            )
            self.assertNotEqual(rc, 0, f"t2 ({mode}) succeeded unexpectedly")
            kv = parse_kv(out)
            peak_b = int(kv.get("peak_budget", "0"))
            self.assertGreater(peak_b, 0, f"t2 ({mode}) did not reserve provider budget: {out}")
            self.assertLessEqual(peak_b, 128 * 1024 * 1024, f"t2 ({mode}) exceeded budget: {out}")
            self.assertEqual(kv.get("live_budget"), "0", f"t2 ({mode}) leaked budget: {out}")
            self.assertEqual(kv.get("frame_alloc_calls"), "0", f"t2 ({mode}) allocated output frame: {out}")
            self.assertEqual(kv.get("out_frames"), "0")
            self.assertFalse(p_t2_out.exists())
            self.assertEqual(p_t2.read_bytes(), bytes(trunc_tile))

        # Seeded mutation fuzzing (24 deterministic mutations): never crash or hang.
        rng = random.Random(20261122)
        for idx in range(24):
            buf = bytearray(valid_jp2)
            op = idx % 3
            if op == 0:
                pos = rng.randrange(len(buf))
                buf[pos] ^= rng.randrange(1, 256)
            elif op == 1:
                cut = rng.randrange(1, len(buf))
                buf = buf[:cut]
            else:
                for _ in range(3):
                    pos = rng.randrange(len(buf))
                    buf[pos] = rng.randrange(256)
            pm = wd / f"mut_{idx}.jp2"
            po = wd / f"mut_{idx}.rfqd"
            pm.write_bytes(bytes(buf))
            rc, out, _ = self.run_cli(
                ["decode", self.to_exe_path(pm), self.to_exe_path(po), "preserve"]
            )
            self.assertIn(rc, (0, 2), f"mutation {idx} crashed rc={rc}: {out}")
            self.assertEqual(parse_kv(out).get("live_budget"), "0")


class HostJp2Tests(Jp2AdapterMixin, unittest.TestCase):
    TARGET_LABEL = "linux-x64"

    @classmethod
    def setUpClass(cls) -> None:
        exe = os.environ.get("RFC_HOST_JP2_EXE")
        if not exe or not os.path.isfile(exe):
            raise unittest.SkipTest("RFC_HOST_JP2_EXE not set")
        cls._exe = exe
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_jp2_host_")
        cls._work_dir = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "_tmp"):
            cls._tmp.cleanup()

    def run_cli(self, args: list[str]) -> tuple[int, str, str]:
        res = _ref.run_process_tree(
            [self._exe, *args], timeout=30.0, capture_output=True, text=True
        )
        return res.returncode, res.stdout, res.stderr


class WinJp2Tests(Jp2AdapterMixin, unittest.TestCase):
    TARGET_LABEL = "windows-x64"

    @classmethod
    def setUpClass(cls) -> None:
        exe = os.environ.get("RFC_WIN_JP2_EXE")
        require = os.environ.get("RFC_REQUIRE_WIN_JP2") == "1"
        if not exe or not os.path.isfile(exe):
            if require:
                raise AssertionError("RFC_WIN_JP2_EXE missing while RFC_REQUIRE_WIN_JP2=1")
            raise unittest.SkipTest("RFC_WIN_JP2_EXE not set")
        ps = shutil.which("powershell.exe")
        if not ps:
            if require:
                raise AssertionError("powershell.exe unavailable while RFC_REQUIRE_WIN_JP2=1")
            raise unittest.SkipTest("powershell.exe unavailable")
        cls._ps = ps
        wp = _ref.run_process_tree(
            ["wslpath", "-w", exe], timeout=10.0, capture_output=True, text=True
        )
        if wp.returncode != 0:
            raise AssertionError(f"wslpath failed for {exe}")
        cls._win_exe = wp.stdout.strip()
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_jp2_win_")
        cls._work_dir = Path(cls._tmp.name)
        wd_wp = _ref.run_process_tree(
            ["wslpath", "-w", str(cls._work_dir)], timeout=10.0, capture_output=True, text=True
        )
        if wd_wp.returncode != 0:
            raise AssertionError("wslpath failed for work_dir")
        cls._win_wd = wd_wp.stdout.strip()

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "_tmp"):
            cls._tmp.cleanup()

    def to_exe_path(self, p: Path) -> str:
        rel = p.relative_to(self._wd())
        return f"{self._win_wd}\\{rel}"

    def run_cli(self, args: list[str]) -> tuple[int, str, str]:
        quoted = ", ".join("'" + a.replace("'", "''") + "'" for a in args)
        ps_cmd = (
            "$env:PATH = 'C:\\Windows\\System32'; "
            f"& '{self._win_exe}' @({quoted}); "
            "exit $LASTEXITCODE"
        )
        res = _ref.run_process_tree(
            [self._ps, "-NoProfile", "-NonInteractive", "-Command", ps_cmd],
            timeout=45.0,
            capture_output=True,
            text=True,
        )
        return res.returncode, res.stdout, res.stderr


if __name__ == "__main__":
    unittest.main()
