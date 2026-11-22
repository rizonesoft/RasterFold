#!/usr/bin/env python3
"""Differential, oracle, colorspace, and mutation tests for D10 T02 s8 (QOI).

Drives host C11 test_qoi (if RFC_HOST_QOI_EXE is set) and isolated native
Windows x64 bin/converter/windows-x64/rfc_test_qoi.exe (on NTFS under
restricted System32 PATH), cross-checking:
  - Independent clean-room pure-Python QOI specification oracle
    (tests/converter/reference.py encode_qoi / decode_qoi_ex) against the
    bundled libqoi C11 adapter in both directions (independent-decoder
    coverage reported separately from same-provider verification)
  - RGB8, RGBA8 (including A == 0 exact RGB preservation), padded strides,
    and both QOI colorspaces (0 = sRGB, 1 = linear) with emitted header checks
  - Named malformed fixtures refused with zero pixel allocation under both
    metadata=preserve and metadata=strip alongside valid controls
  - Seeded random truncation and bit-flip mutation fuzzing (never crash/hang)
"""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import random
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def _load_mod(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module {path}")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


_ref = _load_mod("_rfc_ref", ROOT / "tests" / "converter" / "reference.py")

SAMPLE_RGB8 = _ref.SAMPLE_RGB8
SAMPLE_RGBA8 = _ref.SAMPLE_RGBA8
ALPHA_NONE = _ref.ALPHA_NONE
ALPHA_STRAIGHT = _ref.ALPHA_STRAIGHT

COVERAGE: list[tuple[str, str]] = []


def parse_kv(stdout: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in stdout.strip().splitlines():
        for tok in line.strip().split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                out[k] = v
    return out


def pack_rfqe(
    width: int,
    height: int,
    sample: int,
    alpha: int,
    bps: int,
    stride: int,
    colorspace: int,
    pixels: bytes,
) -> bytes:
    hdr = struct.pack(
        "<4s8I",
        b"RFQE",
        width,
        height,
        sample,
        alpha,
        bps,
        stride,
        colorspace,
        len(pixels),
    )
    return hdr + pixels


def unpack_rfqd(blob: bytes) -> dict[str, object]:
    if len(blob) < 36 or blob[:4] != b"RFQD":
        raise AssertionError(f"invalid RFQD header len={len(blob)}")
    magic, w, h, sf, am, bps, stride, cs, pay_len = struct.unpack_from("<4s8I", blob, 0)
    assert magic == b"RFQD"
    assert len(blob) == 36 + pay_len
    return {
        "width": w,
        "height": h,
        "sample": sf,
        "alpha": am,
        "bps": bps,
        "stride": stride,
        "colorspace": cs,
        "pixels": blob[36:],
    }


def make_rgb_pixels(w: int, h: int) -> bytes:
    out = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            off = (y * w + x) * 3
            out[off] = (x * 41 + y * 19) & 0xFF
            out[off + 1] = (x * 13 + y * 57 + 5) & 0xFF
            out[off + 2] = ((x * 73) ^ (y * 23)) & 0xFF
    return bytes(out)


def make_rgba_pixels(w: int, h: int) -> bytes:
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            off = (y * w + x) * 4
            if (x + y) % 4 == 0:
                out[off] = (23 + x * 17) & 0xFF
                out[off + 1] = (149 + y * 11) & 0xFF
                out[off + 2] = (233 - x * 7) & 0xFF
                out[off + 3] = 0  # fully transparent with non-zero RGB
            else:
                out[off] = (x * 31 + y * 7) & 0xFF
                out[off + 1] = (x * 11 + y * 43) & 0xFF
                out[off + 2] = (x * 5 + y * 67) & 0xFF
                out[off + 3] = 1 + ((x * 19 + y * 29) % 254)
    return bytes(out)


def pad_rows(pixels: bytes, w: int, h: int, bpp: int, extra: int) -> tuple[int, bytes]:
    row_b = w * bpp
    stride = row_b + extra
    out = bytearray()
    for y in range(h):
        out.extend(pixels[y * row_b : (y + 1) * row_b])
        out.extend(bytes((0xC3 ^ (y + k)) & 0xFF for k in range(extra)))
    return stride, bytes(out)


def ps_quote(val: str) -> str:
    return "'" + val.replace("'", "''") + "'"


class QoiAdapterBase:
    target_label = "base"

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        raise NotImplementedError

    def work_dir(self) -> Path:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        raise NotImplementedError

    def test_differential_independent_oracle_rgb_rgba_stride_and_colorspace(self) -> None:
        wd = self.work_dir()
        w, h = 19, 13
        cases = [
            ("rgb8_srgb_tight", SAMPLE_RGB8, ALPHA_NONE, 3, 0, 0, make_rgb_pixels(w, h)),
            ("rgba8_srgb_tight", SAMPLE_RGBA8, ALPHA_STRAIGHT, 4, 0, 0, make_rgba_pixels(w, h)),
            ("rgb8_linear_padded", SAMPLE_RGB8, ALPHA_NONE, 3, 11, 1, make_rgb_pixels(w, h)),
            ("rgba8_linear_padded", SAMPLE_RGBA8, ALPHA_STRAIGHT, 4, 9, 1, make_rgba_pixels(w, h)),
        ]
        for name, sf, am, ch, pad, cs, packed_px in cases:
            stride, raw_in = pad_rows(packed_px, w, h, ch, pad)
            rfqe = wd / f"{name}.rfqe"
            qoi_out = wd / f"{name}.qoi"
            rfqe.write_bytes(pack_rfqe(w, h, sf, am, 8, stride, cs, raw_in))

            # Direction 1: adapter encodes -> independent pure-Python oracle decodes.
            rc, out, err = self.run_cli(["encode", self.to_exe_path(rfqe), self.to_exe_path(qoi_out), "preserve"])
            self.assertEqual(rc, 0, f"{name} encode failed: {out} {err}")
            kv = parse_kv(out)
            self.assertEqual(kv.get("status"), "ok")
            self.assertEqual(int(kv.get("colorspace", "-1")), cs)
            qoi_bytes = qoi_out.read_bytes()
            self.assertEqual(qoi_bytes[13], cs)
            ow, oh, och, ocs, opx = _ref.decode_qoi_ex(qoi_bytes)
            self.assertEqual((ow, oh, och, ocs), (w, h, ch, cs))
            self.assertEqual(opx, packed_px)

            # Direction 2: independent pure-Python oracle encodes -> adapter decodes.
            oracle_qoi = _ref.encode_qoi(w, h, ch, packed_px, colorspace=cs)
            qoi_in = wd / f"{name}_oracle.qoi"
            rfqd_out = wd / f"{name}_oracle.rfqd"
            qoi_in.write_bytes(oracle_qoi)
            rc, out, err = self.run_cli(["decode", self.to_exe_path(qoi_in), self.to_exe_path(rfqd_out), "preserve"])
            self.assertEqual(rc, 0, f"{name} decode failed: {out} {err}")
            dec = unpack_rfqd(rfqd_out.read_bytes())
            self.assertEqual(dec["width"], w)
            self.assertEqual(dec["height"], h)
            self.assertEqual(dec["sample"], sf)
            self.assertEqual(dec["colorspace"], cs)
            self.assertEqual(dec["pixels"], packed_px)

        COVERAGE.append((self.target_label, "independent-pure-python-oracle (RGB8, RGBA8 A==0, padded strides, sRGB+linear)"))

    def test_unsupported_colorimetry_preserve_refusal_and_strip(self) -> None:
        wd = self.work_dir()
        w, h = 4, 4
        px = make_rgb_pixels(w, h)
        rfqe = wd / "srgb_chunk.rfqe"
        qoi_out = wd / "srgb_chunk.qoi"
        # cs=2 in RFQE injects PNG sRGB chunk colorimetry, which QOI cannot carry.
        rfqe.write_bytes(pack_rfqe(w, h, SAMPLE_RGB8, ALPHA_NONE, 8, w * 3, 2, px))
        rc, out, _err = self.run_cli(["encode", self.to_exe_path(rfqe), self.to_exe_path(qoi_out), "preserve"])
        self.assertNotEqual(rc, 0)
        self.assertEqual(parse_kv(out).get("status"), "metadata-policy")
        self.assertFalse(qoi_out.exists())

        rc, out, err = self.run_cli(["encode", self.to_exe_path(rfqe), self.to_exe_path(qoi_out), "strip"])
        self.assertEqual(rc, 0, f"strip encode failed: {out} {err}")
        self.assertEqual(qoi_out.read_bytes()[13], 0)

    def test_named_malformed_fixtures_refused_zero_alloc_and_mutations(self) -> None:
        wd = self.work_dir()
        valid = _ref.encode_qoi(4, 3, 4, make_rgba_pixels(4, 3), colorspace=0)
        # Valid control passes both oracle and adapter.
        _ref.decode_qoi_ex(valid)
        ctrl_in = wd / "ctrl.qoi"
        ctrl_out = wd / "ctrl.rfqd"
        ctrl_in.write_bytes(valid)
        rc, out, err = self.run_cli(["decode", self.to_exe_path(ctrl_in), self.to_exe_path(ctrl_out), "preserve"])
        self.assertEqual(rc, 0, f"control failed: {out} {err}")

        named_bad = [
            ("trunc_hdr", valid[:10], "truncated-input"),
            ("bad_magic", b"qoiX" + valid[4:], "invalid-argument"),
            ("zero_w", valid[:4] + b"\x00\x00\x00\x00" + valid[8:], "invalid-argument"),
            ("bad_ch", valid[:12] + bytes([5]) + valid[13:], "invalid-argument"),
            ("bad_cs", valid[:13] + bytes([2]) + valid[14:], "invalid-argument"),
            ("bad_end", valid[:-1] + b"\x00", "truncated-input"),
            ("trunc_rgb", b"qoif\x00\x00\x00\x01\x00\x00\x00\x01\x03\x00\xfe\x01\x02\x00\x00\x00\x00\x00\x00\x00\x01", "truncated-input"),
            ("trunc_rgba", b"qoif\x00\x00\x00\x01\x00\x00\x00\x01\x04\x00\xff\x01\x02\x03\x00\x00\x00\x00\x00\x00\x00\x01", "truncated-input"),
            ("excess_run", b"qoif\x00\x00\x00\x02\x00\x00\x00\x01\x03\x00\xc2\x00\x00\x00\x00\x00\x00\x00\x01", "invalid-argument"),
            ("consec_index", b"qoif\x00\x00\x00\x02\x00\x00\x00\x01\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01", "invalid-argument"),
            ("trailing", b"qoif\x00\x00\x00\x01\x00\x00\x00\x01\x03\x00\xc0\x00\x00\x00\x00\x00\x00\x00\x00\x01", "invalid-argument"),
        ]
        for tag, blob, exp_status in named_bad:
            with self.assertRaises(ValueError, msg=f"oracle accepted malformed {tag}"):
                _ref.decode_qoi_ex(blob)
            p_in = wd / f"bad_{tag}.qoi"
            p_out = wd / f"bad_{tag}.rfqd"
            p_in.write_bytes(blob)
            for mode in ("preserve", "strip"):
                rc, out, _err = self.run_cli(["decode", self.to_exe_path(p_in), self.to_exe_path(p_out), mode])
                self.assertNotEqual(rc, 0, f"{tag} ({mode}) unexpectedly succeeded")
                kv = parse_kv(out)
                self.assertEqual(kv.get("status"), exp_status, f"{tag} ({mode}): {out}")
                self.assertEqual(kv.get("alloc_calls"), "0", f"{tag} ({mode}) allocated before refusal: {out}")

        # Seeded random mutation fuzzing (never crash or hang).
        rng = random.Random(20261122)
        for idx in range(40):
            mut = bytearray(valid)
            kind = idx % 4
            if kind == 0:
                cut = rng.randint(0, len(mut) - 1)
                mut = mut[:cut]
            elif kind == 1:
                pos = rng.randint(0, len(mut) - 1)
                mut[pos] ^= rng.randint(1, 255)
            elif kind == 2:
                pos = rng.randint(0, len(mut))
                mut[pos:pos] = bytes([rng.randint(0, 255)])
            else:
                if len(mut) > 1:
                    pos = rng.randint(0, len(mut) - 1)
                    del mut[pos]
            p_in = wd / f"mut_{idx}.qoi"
            p_out = wd / f"mut_{idx}.rfqd"
            p_in.write_bytes(bytes(mut))
            rc, _out, _err = self.run_cli(["decode", self.to_exe_path(p_in), self.to_exe_path(p_out), "preserve"], timeout=20.0)
            self.assertIn(rc, (0, 1), f"mutation {idx} crashed with rc={rc}")


class HostQoiTests(QoiAdapterBase, unittest.TestCase):
    target_label = "linux-x64"
    host_exe: Path | None = None
    _tmp: tempfile.TemporaryDirectory[str] | None = None
    _wd: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_QOI_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_QOI_EXE not configured")
        cls.host_exe = Path(raw)
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_qoi_host_")
        cls._wd = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._tmp is not None:
            cls._tmp.cleanup()

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        assert self.host_exe is not None
        res = _ref.run_process_tree(
            [str(self.host_exe), *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return res.returncode, res.stdout or "", res.stderr or ""

    def work_dir(self) -> Path:
        assert self._wd is not None
        return self._wd

    def to_exe_path(self, p: Path) -> str:
        return str(p)


class WinQoiTests(QoiAdapterBase, unittest.TestCase):
    target_label = "windows-x64"
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_QOI_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_qoi.exe")
        req = os.environ.get("RFC_REQUIRE_WIN_QOI", "").strip() == "1"
        if not cand.is_file():
            if req:
                raise AssertionError(f"RFC_REQUIRE_WIN_QOI=1 but missing {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_qoi.exe not built")
        cls.win_exe = cand
        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_qoi_win_")
            cls._py_temp = Path(cls._win_temp)
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_qoi_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True, text=True, timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                if req:
                    raise AssertionError("powershell.exe unavailable while RFC_REQUIRE_WIN_QOI=1")
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(["wslpath", "-u", cls._win_temp], capture_output=True, text=True, timeout=15.0)
            if wp.returncode != 0 or not (wp.stdout or "").strip():
                if req:
                    raise AssertionError("wslpath failed while RFC_REQUIRE_WIN_QOI=1")
                raise unittest.SkipTest("wslpath unavailable for Windows NTFS tests")
            cls._py_temp = Path(wp.stdout.strip())
        shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_qoi.exe")

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

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        if os.name == "nt":
            exe = str(self.work_dir() / "rfc_test_qoi.exe")
            env = dict(os.environ)
            root = os.environ.get("SystemRoot", r"C:\Windows")
            env["PATH"] = f"{root}\\System32;{root}"
            res = _ref.run_process_tree(
                [exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True, timeout=timeout,
            )
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_qoi.exe")
        cmd = (
            f"$env:PATH = \"$env:SystemRoot\\System32;$env:SystemRoot\"; Set-Location {ps_quote(self._win_temp)}; "
            f"& {exe_ps} {arg_ps}; exit $LASTEXITCODE"
        )
        res = _ref.run_process_tree(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", cmd],
            cwd=ROOT, capture_output=True, text=True, timeout=timeout,
        )
        return res.returncode, res.stdout or "", res.stderr or ""

    def work_dir(self) -> Path:
        assert self._py_temp is not None
        return self._py_temp

    def to_exe_path(self, p: Path) -> str:
        rel = p.relative_to(self.work_dir())
        return self._win_temp.rstrip("\\/") + "\\" + str(rel).replace("/", "\\")


if __name__ == "__main__":
    result = unittest.main(verbosity=2, exit=False)
    for target, note in COVERAGE:
        print(f"qoi-coverage: target={target} {note}")
    sys.exit(0 if result.result.wasSuccessful() else 1)
