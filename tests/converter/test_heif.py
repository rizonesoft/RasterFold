#!/usr/bin/env python3
"""HEIF/HEIC (D10 T02 s7) system-codec tests: shared-provider coverage only.

There is no independent HEVC decoder in this repository: the converter
decodes HEIF and HEIC through the Windows Imaging Component and the system
HEVC decoder, and these tests check that path against itself and against the
pattern the fixtures were encoded from, reporting the coverage as
shared-provider. Host Linux (RFC_HOST_HEIF_EXE): the named refusal of a
build without the system codec. Isolated Windows x64 (rfc_test_heif.exe on
NTFS under a System32-only PATH): WIC-written fixtures decoded by the adapter
and by a second fresh WIC decode, seeded mutations each decoded in a fresh
process (never a crash or hang), and the decode's peak private bytes, in a
fresh process over a no-op baseline, against the adapter's reservation.
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
    sys.modules[name] = mod  # dataclasses resolve annotations through sys.modules
    spec.loader.exec_module(mod)
    return mod


_ref = _load_mod("_rfc_ref", ROOT / "tests" / "converter" / "reference.py")

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


def box(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", 8 + len(payload)) + kind + payload


def fullbox(kind: bytes, payload: bytes, version: int = 0) -> bytes:
    return box(kind, bytes([version, 0, 0, 0]) + payload)


def minimal_heic(w: int = 32, h: int = 24) -> bytes:
    """A structurally valid single-picture HEIC (no coded data)."""
    hdlr = fullbox(b"hdlr", b"\0\0\0\0pict" + b"\0" * 12 + b"\0")
    pitm = fullbox(b"pitm", struct.pack(">H", 1))
    infe = fullbox(b"infe", struct.pack(">HH", 1, 0) + b"hvc1\0", version=2)
    iinf = fullbox(b"iinf", struct.pack(">H", 1) + infe)
    ispe = fullbox(b"ispe", struct.pack(">II", w, h))
    ipco = box(b"ipco", ispe)
    ipma = fullbox(b"ipma", struct.pack(">IHBB", 1, 1, 1, 0x81))
    iprp = box(b"iprp", ipco + ipma)
    meta = fullbox(b"meta", hdlr + pitm + iinf + iprp)
    ftyp = box(b"ftyp", b"heic" + b"\0\0\0\0" + b"mif1heic")
    return ftyp + meta + box(b"mdat", b"\0\0\0\0")


class HeifAdapterBase(unittest.TestCase):
    """Shared helpers; concrete subclasses supply the executable."""

    __test__ = False

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


class HostHeifTests(HeifAdapterBase):
    __test__ = True
    host_exe: Path | None = None
    _tmp: tempfile.TemporaryDirectory[str] | None = None
    _wd: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_HEIF_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_HEIF_EXE not configured")
        cls.host_exe = Path(raw)
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_heif_host_")
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

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        assert self.host_exe is not None
        res = _ref.run_process_tree([str(self.host_exe), *args], cwd=ROOT, capture_output=True, text=True,
                                    timeout=timeout)
        return res.returncode, res.stdout or "", res.stderr or ""

    def test_named_refusal_without_the_system_codec(self) -> None:
        src = self.work_dir() / "x.heic"
        src.write_bytes(minimal_heic())
        rc, out, err = self.run_cli(["cli", "dump", str(src), str(self.work_dir() / "x.raw")])
        self.assertEqual(rc, 2, out + err)
        self.assertEqual(parse_kv(out)["STATUS"], "no-adapter")


class WindowsHeifTests(HeifAdapterBase):
    __test__ = True
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_HEIF_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_heif.exe")
        req = os.environ.get("RFC_REQUIRE_WIN_HEIF") == "1"
        if not cand.is_file():
            if req:
                raise AssertionError(f"missing Windows rfc_test_heif.exe: {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_heif.exe not built")
        cls.win_exe = cand
        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_heif_win_")
            cls._py_temp = Path(cls._win_temp)
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_heif_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True, text=True, timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                if req:
                    raise AssertionError("powershell.exe unavailable while RFC_REQUIRE_WIN_HEIF=1")
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(["wslpath", "-u", cls._win_temp], capture_output=True, text=True, timeout=15.0)
            if wp.returncode != 0 or not (wp.stdout or "").strip():
                if req:
                    raise AssertionError("wslpath failed while RFC_REQUIRE_WIN_HEIF=1")
                raise unittest.SkipTest("wslpath unavailable for Windows NTFS tests")
            cls._py_temp = Path(wp.stdout.strip())
        shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_heif.exe")

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

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        if os.name == "nt":
            exe = str(self.work_dir() / "rfc_test_heif.exe")
            env = dict(os.environ)
            root = os.environ.get("SystemRoot", r"C:\Windows")
            env["PATH"] = f"{root}\\System32;{root}"
            res = _ref.run_process_tree([exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True,
                                        timeout=timeout)
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_heif.exe")
        cmd = (f"$env:PATH = \"$env:SystemRoot\\System32;$env:SystemRoot\"; Set-Location {ps_quote(self._win_temp)}; "
               f"& {exe_ps} {arg_ps}; exit $LASTEXITCODE")
        res = _ref.run_process_tree(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", cmd],
                                    cwd=ROOT, capture_output=True, text=True, timeout=timeout)
        return res.returncode, res.stdout or "", res.stderr or ""

    def make(self, name: str, w: int, h: int, orientation: int = 0) -> Path:
        p = self.work_dir() / name
        self.cli_ok(["cli", "make-heic", self.to_exe_path(p), str(w), str(h), str(orientation)])
        return p

    def test_shared_provider_sample_equality(self) -> None:
        for w, h, o in ((64, 48, 0), (130, 77, 0), (48, 32, 6), (48, 32, 2), (48, 32, 3)):
            src = self.make(f"eq_{w}x{h}_{o}.heic", w, h, o)
            a = self.work_dir() / "a.raw"
            b = self.work_dir() / "b.raw"
            kv = self.cli_ok(["cli", "dump", self.to_exe_path(src), self.to_exe_path(a)])
            kw = self.cli_ok(["cli", "wic-dump", self.to_exe_path(src), self.to_exe_path(b)])
            self.assertEqual((kv["WIDTH"], kv["HEIGHT"]), (kw["WIDTH"], kw["HEIGHT"]))
            self.assertEqual(a.read_bytes(), b.read_bytes(), src.name)
            COVERAGE.append((src.name, "shared-provider"))
        # the HEIF row: the same picture rebranded as generic HEIF (mif1)
        src = self.make("eq_heif.heic", 64, 48)
        data = bytearray(src.read_bytes())
        ftyp = struct.unpack(">I", data[:4])[0]
        for i in range(8, ftyp, 4):
            if data[i:i + 4] in (b"heic", b"heix"):
                data[i:i + 4] = b"mif1"
        heif = self.work_dir() / "eq.heif"
        heif.write_bytes(bytes(data))
        a = self.work_dir() / "ha.raw"
        b = self.work_dir() / "hb.raw"
        self.cli_ok(["cli", "dump", self.to_exe_path(heif), self.to_exe_path(a), "heif"])
        self.cli_ok(["cli", "wic-dump", self.to_exe_path(heif), self.to_exe_path(b)])
        self.assertEqual(a.read_bytes(), b.read_bytes(), "heif")
        COVERAGE.append((heif.name, "shared-provider"))

    def test_seeded_mutations_never_crash_or_hang(self) -> None:
        base = self.make("mut_base.heic", 64, 48).read_bytes()
        rng = random.Random(0x4E1F)
        seen = {"ok": 0, "refused": 0}
        for i in range(40):
            m = bytearray(base)
            if i % 3 == 0:
                m = m[: rng.randrange(8, len(m))]
            else:
                for _ in range(1 + rng.randrange(6)):
                    m[rng.randrange(8, len(m))] = rng.randrange(256)
            p = self.work_dir() / "mut.heic"
            p.write_bytes(bytes(m))
            rc, out, err = self.run_cli(["cli", "dump", self.to_exe_path(p), self.to_exe_path(self.work_dir() / "m.raw")],
                                        timeout=30.0)
            self.assertIn(rc, (0, 2), f"mutation {i}: rc={rc}\n{out}\n{err}")
            seen["ok" if rc == 0 else "refused"] += 1
        self.assertGreater(seen["refused"], 0)
        print(f"heif mutations: {seen}")

    def test_decode_peak_within_reservation(self) -> None:
        for w, h in ((1024, 768), (2048, 1536)):
            src = self.make(f"peak_{w}x{h}.heic", w, h)
            base = self.cli_ok(["cli", "heif-peak", "noop", self.to_exe_path(src)], timeout=120.0)
            dec = self.cli_ok(["cli", "heif-peak", "decode", self.to_exe_path(src)], timeout=120.0)
            growth = int(dec["PEAK"]) - int(base["PEAK"])
            reserve = int(dec["RESERVE"])  # the budget's peak: the adapter's own reservation and frame
            print(f"heif decode peak {w}x{h}: growth {growth} of the adapter's budgeted {reserve} ({growth / reserve:.3f})")
            self.assertLessEqual(growth, reserve, f"{w}x{h}")


class LinkMatcherTests(unittest.TestCase):
    """The no-HEVC link check recognises the ordinary linker spellings."""

    def test_hevc_link_names(self) -> None:
        cc = _load_mod("_rfc_check_conv", ROOT / "scripts" / "check-converter.py")
        for tok in ("libx265.a", "-lx265", "libkvazaar.a", "/opt/lib/libde265.a", "-lheif",
                    "libheif.dll.a", "x265.lib", "libopenhevc.a", "-l:libx265.a", "-l:/opt/libde265.a"):
            self.assertIsNotNone(cc._hevc_link_name(tok), tok)
        for tok in ("rfc_test_heif.exe", "heif_scan.c.obj", "librfc_engine.a", "-lole32", "libavif.a",
                    "libjxl.a"):
            self.assertIsNone(cc._hevc_link_name(tok), tok)

    def test_linker_reported_inputs_catch_flag_and_search_path_archives(self) -> None:
        """An HEVC-named archive reached only through -Wl, flags and a library
        search path, with no member extracted, is still among the inputs lld
        reports reading, so the check sees it however the command spells it."""
        cc = _load_mod("_rfc_check_conv", ROOT / "scripts" / "check-converter.py")
        tc = ROOT / "tools" / "llvm-mingw-linux" / "bin"
        clang, ar = tc / "x86_64-w64-mingw32-clang", tc / "llvm-ar"
        if not clang.exists() or not ar.exists():
            self.skipTest("the pinned llvm-mingw toolchain is not provisioned")
        with tempfile.TemporaryDirectory() as td:
            t = Path(td)
            (t / "unused.c").write_text("int x265_unused(void) { return 1; }\n")
            (t / "main.c").write_text("int main(void) { return 0; }\n")
            obj = _ref.run_process_tree([str(clang), "-c", str(t / "unused.c"), "-o", str(t / "unused.o")],
                                       capture_output=True, timeout=120.0)
            self.assertEqual(obj.returncode, 0, obj.stderr)
            lib = t / "deps"
            lib.mkdir()
            ok = _ref.run_process_tree([str(ar), "rcs", str(lib / "libx265.a"), str(t / "unused.o")],
                                      capture_output=True, timeout=60.0)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            for spelling in (["-Wl,-L" + str(lib), "-Wl,-lx265"], ["-Wl,-L" + str(lib), "-Wl,-l:libx265.a"]):
                link = _ref.run_process_tree([str(clang), str(t / "main.c"), "-o", str(t / "m.exe"), *spelling,
                                             "-Wl,--verbose"], cwd=td, capture_output=True, timeout=300.0)
                self.assertEqual(link.returncode, 0, link.stderr[-400:])
                files = cc._linker_inputs_from_verbose(link.stdout + "\n" + link.stderr, td)
                self.assertEqual(cc._hevc_among_inputs(files), str(lib / "libx265.a"), spelling)
            clean = _ref.run_process_tree([str(clang), str(t / "main.c"), "-o", str(t / "c.exe"), "-Wl,--verbose"],
                                         cwd=td, capture_output=True, timeout=300.0)
            self.assertIsNone(cc._hevc_among_inputs(
                cc._linker_inputs_from_verbose(clean.stdout + "\n" + clean.stderr, td)))


def load_tests(loader: unittest.TestLoader, tests: unittest.TestSuite, pattern: str | None) -> unittest.TestSuite:
    suite = unittest.TestSuite()
    for cls in (LinkMatcherTests, HostHeifTests, WindowsHeifTests):
        suite.addTests(loader.loadTestsFromTestCase(cls))
    return suite


def tearDownModule() -> None:
    if COVERAGE:
        print(f"heif coverage: {{'shared-provider': {len(COVERAGE)}}}")


if __name__ == "__main__":
    unittest.main()
