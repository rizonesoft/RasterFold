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


class NoHevcArtifactProofTests(unittest.TestCase):
    """Class regression for the shipped-artifact, dependency-prefix, and lld --reproduce no-HEVC proof."""

    def test_shipped_binaries_prefix_and_repro_links_catch_hevc(self) -> None:
        cc = _load_mod("_rfc_check_conv", ROOT / "scripts" / "check-converter.py")
        tc = ROOT / "tools" / "llvm-mingw-linux" / "bin"
        clang, ar, strip = tc / "x86_64-w64-mingw32-clang", tc / "llvm-ar", tc / "llvm-strip"
        if not clang.exists() or not ar.exists() or not strip.exists():
            self.skipTest("the pinned llvm-mingw toolchain is not provisioned")
        with tempfile.TemporaryDirectory() as td:
            t = Path(td)
            (t / "ok.c").write_text("int neutral_fn(void) { return 0; }\nint main(void) { return neutral_fn(); }\n")
            (t / "sym_only.c").write_text(
                "int x265_encoder_open(void) { return 1; }\n"
                "int main(void) { return x265_encoder_open() - 1; }\n"
            )
            (t / "str_only.c").write_text(
                "const char *banner(void) { return \"libde265\"; }\n"
                "int main(void) { return banner()[0] == 0; }\n"
            )
            (t / "unused_enc.c").write_text("int x265_encoder_open(void) { return 1; }\n")
            (t / "unused_ok.c").write_text("int neutral_unused(void) { return 0; }\n")
            (t / "dll_hevc.c").write_text(
                "__declspec(dllexport) int de265_new_decoder(void) { return 1; }\n"
            )
            (t / "use_dll.c").write_text(
                "__declspec(dllimport) int de265_new_decoder(void);\n"
                "int main(void) { return de265_new_decoder() - 1; }\n"
            )

            def run_ok(cmd: list[str], cwd: str | None = None) -> None:
                r = _ref.run_process_tree(cmd, cwd=cwd, capture_output=True, timeout=120.0)
                self.assertEqual(r.returncode, 0, r.stderr)

            # 1. Clean shipped executable passes
            clean_bin = t / "bin_clean"
            clean_bin.mkdir()
            run_ok([str(clang), str(t / "ok.c"), "-o", str(clean_bin / "app.exe")])
            msg, bins = cc._no_hevc_in_shipped_binaries(str(clean_bin))
            self.assertIsNone(msg)
            self.assertEqual(bins, [str(clean_bin / "app.exe")])

            # 2a. Unstripped executable defining an HEVC symbol (no string literal) fails via llvm-nm
            sym_bin = t / "bin_sym"
            sym_bin.mkdir()
            run_ok([str(clang), str(t / "sym_only.c"), "-o", str(sym_bin / "app.exe")])
            msg, _ = cc._no_hevc_in_shipped_binaries(str(sym_bin))
            self.assertIsNotNone(msg)
            self.assertIn("x265_encoder_open", msg)

            # 2b. Stripped executable with an HEVC encoder/decoder string fails via binary string scan
            str_bin = t / "bin_str"
            str_bin.mkdir()
            run_ok([str(clang), str(t / "str_only.c"), "-o", str(str_bin / "app.exe")])
            run_ok([str(strip), "--strip-all", str(str_bin / "app.exe")])
            msg, _ = cc._no_hevc_in_shipped_binaries(str(str_bin))
            self.assertIsNotNone(msg)
            self.assertIn("libde265", msg)

            # 3. Shipped executable importing an HEVC symbol from a neutrally named DLL fails on imports
            imp_bin = t / "bin_imp"
            imp_bin.mkdir()
            run_ok([str(clang), "-shared", str(t / "dll_hevc.c"),
                    "-Wl,--out-implib," + str(t / "libneutral.dll.a"),
                    "-o", str(t / "neutral.dll")])
            run_ok([str(clang), str(t / "use_dll.c"), str(t / "libneutral.dll.a"),
                    "-o", str(imp_bin / "app.exe")])
            msg, _ = cc._no_hevc_in_shipped_binaries(str(imp_bin))
            self.assertIsNotNone(msg)
            self.assertIn("de265_new_decoder", msg)

            # 4. Empty shipped directory fails closed
            empty_bin = t / "bin_empty"
            empty_bin.mkdir()
            with self.assertRaises(RuntimeError):
                cc._no_hevc_in_shipped_binaries(str(empty_bin))

            # 5. Dependency prefix: clean passes; neutrally named archive with HEVC content fails;
            #    dangling link and unreadable subtree fail closed
            prefix = t / "deps-prefix"
            (prefix / "lib").mkdir(parents=True)
            run_ok([str(clang), "-c", str(t / "ok.c"), "-o", str(t / "ok.o")])
            run_ok([str(clang), "-c", str(t / "unused_enc.c"), "-o", str(t / "unused_enc.o")])
            run_ok([str(clang), "-c", str(t / "unused_ok.c"), "-o", str(t / "unused_ok.o")])
            run_ok([str(ar), "rcs", str(prefix / "lib" / "libneutral.a"), str(t / "unused_ok.o")])
            msg, _entries, libs = cc._no_hevc_in_dependency_prefix(str(prefix))
            self.assertIsNone(msg)
            self.assertEqual(libs, [str(prefix / "lib" / "libneutral.a")])

            bad_ar = prefix / "lib" / "codec.bin"
            run_ok([str(ar), "rcs", str(bad_ar), str(t / "unused_enc.o")])
            msg, _, _ = cc._no_hevc_in_dependency_prefix(str(prefix))
            self.assertIsNotNone(msg)
            self.assertIn("x265_encoder_open", msg)
            bad_ar.unlink()

            (prefix / "lib" / "dangling.a").symlink_to(t / "nowhere.a")
            with self.assertRaises(RuntimeError):
                cc._no_hevc_in_dependency_prefix(str(prefix))
            (prefix / "lib" / "dangling.a").unlink()

            hidden = prefix / "lib" / "hidden"
            hidden.mkdir()
            run_ok([str(ar), "rcs", str(hidden / "codec.bin"), str(t / "unused_enc.o")])
            hidden.chmod(0)
            try:
                if not os.access(hidden, os.R_OK):
                    with self.assertRaises(RuntimeError):
                        cc._no_hevc_in_dependency_prefix(str(prefix))
            finally:
                hidden.chmod(0o755)
            shutil.rmtree(hidden)

            # 6. lld --reproduce catches external unused archives however spelled:
            #    -Wl,-lx265, -Wl,-l:libx265.a, parenthesized archive codec(test),
            #    symlink-plus-.. path, and a later forbidden alias libde265.a -> first.a
            ext = t / "ext"
            ext.mkdir()
            run_ok([str(ar), "rcs", str(ext / "libx265.a"), str(t / "unused_ok.o")])
            run_ok([str(ar), "rcs", str(ext / "codec(test)"), str(t / "unused_enc.o")])
            (t / "deep" / "inner").mkdir(parents=True)
            run_ok([str(ar), "rcs", str(t / "deep" / "via.a"), str(t / "unused_enc.o")])
            (t / "hop").symlink_to(t / "deep" / "inner", target_is_directory=True)
            run_ok([str(ar), "rcs", str(ext / "first.a"), str(t / "unused_ok.o")])
            (ext / "libde265.a").symlink_to(ext / "first.a")
            run_ok([str(ar), "rcs", str(ext / "version.txt"), str(t / "unused_enc.o")])
            run_ok([str(ar), "rcs", str(ext / "response.txt"), str(t / "unused_enc.o")])
            run_ok([str(clang), "-flto", "-c", str(t / "unused_enc.c"), "-o", str(ext / "lto_enc.o")])

            clean_rep = t / "clean.repro.tar"
            run_ok([str(clang), str(t / "ok.o"), "-o", str(t / "c.exe"), f"-Wl,--reproduce={clean_rep}"])
            msg, n_in = cc._no_hevc_in_repro_tar(str(clean_rep))
            self.assertIsNone(msg)
            self.assertGreater(n_in, 0)

            cases = [
                ("wl_l", ["-Wl,-L" + str(ext), "-Wl,-lx265"], "libx265.a"),
                ("wl_l_colon", ["-Wl,-L" + str(ext), "-Wl,-l:libx265.a"], "libx265.a"),
                ("parens", [str(ext / "codec(test)")], "x265_encoder_open"),
                ("symlink_dotdot", [f"{t}/hop/../via.a"], "x265_encoder_open"),
                ("alias_second", [str(ext / "first.a"), str(ext / "libde265.a")], "libde265.a"),
                ("nested_version", [str(ext / "version.txt")], "x265_encoder_open"),
                ("nested_response", [str(ext / "response.txt")], "x265_encoder_open"),
                ("llvm_bitcode", [str(ext / "lto_enc.o")], "x265_encoder_open"),
            ]
            for label, extra_args, needle in cases:
                rep = t / f"{label}.repro.tar"
                run_ok([str(clang), str(t / "ok.o"), "-o", str(t / f"{label}.exe"),
                        *extra_args, f"-Wl,--reproduce={rep}"])
                msg, _ = cc._no_hevc_in_repro_tar(str(rep))
                self.assertIsNotNone(msg, label)
                self.assertIn(needle, msg, label)


def load_tests(loader: unittest.TestLoader, tests: unittest.TestSuite, pattern: str | None) -> unittest.TestSuite:
    suite = unittest.TestSuite()
    for cls in (NoHevcArtifactProofTests, HostHeifTests, WindowsHeifTests):
        suite.addTests(loader.loadTestsFromTestCase(cls))
    return suite


def tearDownModule() -> None:
    if COVERAGE:
        print(f"heif coverage: {{'shared-provider': {len(COVERAGE)}}}")


if __name__ == "__main__":
    unittest.main()
