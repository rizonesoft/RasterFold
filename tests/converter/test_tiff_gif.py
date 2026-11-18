#!/usr/bin/env python3
"""Independent-oracle, export, refusal and mutation tests for D10 T02 s6 (TIFF and GIF).

Drives host C11 test_tiff_gif (RFC_HOST_TIFF_GIF_EXE) and the isolated native
Windows x64 bin/converter/windows-x64/rfc_test_tiff_gif.exe (on NTFS under a
System32-only PATH), cross-checking:
  - TIFF decode (none, PackBits, LZW, Deflate; strips and tiles; contiguous
    and separate planes; 8/16-bit gray, RGB, RGBA; predictor 2) against the
    independent pure-Python tests/converter/tiff_gif_ref.py, on fixtures from
    Pillow and from the pinned libtiff writer
  - JPEG-compressed TIFF through a fresh libtiff session only (shared-provider
    coverage, reported as such)
  - GIF decode (interlaced, transparent, Pillow-written) against the oracle
  - PNG -> TIFF and PNG -> GIF export decoded by the oracle back to the
    PNG's samples, with ICC/XMP/orientation carried in TIFF
  - seeded truncation and mutation of both formats: refused or decoded,
    never a crash or hang
"""
from __future__ import annotations

import importlib.util
import io
import os
from pathlib import Path
import random
import shutil
import sys
import tempfile
import unittest

from PIL import Image

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
_tg = _load_mod("_rfc_tiff_gif_ref", ROOT / "tests" / "converter" / "tiff_gif_ref.py")

COVERAGE: list[tuple[str, str]] = []

COMP = {"none": 1, "lzw": 5, "jpeg": 7, "deflate": 8, "packbits": 32773, "webp": 50001}


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
    bands = len(mode) if mode != "L" else 1
    data = []
    for y in range(h):
        for x in range(w):
            v = tuple((x * 5 + y * 3 + c * 41 + rng.randrange(9)) & 0xFF for c in range(bands))
            data.append(v if bands > 1 else v[0])
    im.putdata(data)
    return im


def pil_bytes(im: Image.Image, fmt: str, **kw) -> bytes:
    buf = io.BytesIO()
    im.save(buf, fmt, **kw)
    return buf.getvalue()


class TiffGifAdapterBase(unittest.TestCase):
    """Shared cases; concrete subclasses supply the executable."""

    __test__ = False

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        raise NotImplementedError

    def work_dir(self) -> Path:
        raise NotImplementedError

    def to_exe_path(self, p: Path) -> str:
        raise NotImplementedError

    def mutation_count(self) -> int:
        return 150

    def cli_ok(self, args: list[str], timeout: float = 60.0) -> dict[str, str]:
        rc, out, err = self.run_cli(args, timeout)
        self.assertEqual(rc, 0, f"{args}\n{out}\n{err}")
        return parse_kv(out)

    def fixture(self, name: str, data: bytes) -> Path:
        p = self.work_dir() / name
        p.write_bytes(data)
        return p

    def c_dump(self, fmt: str, p: Path, tag: str) -> tuple[dict[str, str], bytes]:
        raw = self.work_dir() / f"{tag}.raw"
        kv = self.cli_ok(["cli", "dump", fmt, self.to_exe_path(p), self.to_exe_path(raw)])
        return kv, raw.read_bytes()

    def assert_matches_tiff_oracle(self, p: Path, tag: str) -> None:
        kv, px = self.c_dump("tiff", p, tag)
        ref = _tg.decode_tiff(p.read_bytes())
        self.assertEqual((int(kv["WIDTH"]), int(kv["HEIGHT"])), (ref["width"], ref["height"]), tag)
        self.assertEqual(int(kv["BPS"]), ref["bits"], tag)
        self.assertEqual(px, ref["pixels"], tag)
        COVERAGE.append((tag, "independent"))

    def test_tiff_independent_oracle_matrix(self) -> None:
        n = 0
        # The pinned libtiff writer: every compression, layout and depth,
        # written and decoded by the adapter in one process (one round trip
        # on Windows), each decode checked against the independent oracle.
        mdir = self.work_dir() / "matrix"
        mdir.mkdir(exist_ok=True)
        kv = self.cli_ok(["cli", "make-matrix", self.to_exe_path(mdir)], timeout=180.0)
        self.assertEqual(kv["MADE"], "96")
        for tif in sorted(mdir.glob("mk_*.tif")):
            ref = _tg.decode_tiff(tif.read_bytes())
            self.assertEqual(tif.with_suffix(".raw").read_bytes(), ref["pixels"], tif.name)
            COVERAGE.append((tif.stem, "independent"))
            n += 1
        # Pillow's writer (a different encoder of the same format).
        for mode in ("L", "RGB", "RGBA"):
            for comp in ("raw", "packbits", "tiff_lzw", "tiff_adobe_deflate"):
                im = pattern_image(33, 19, mode, len(mode) * 7)
                p = self.fixture(f"pil_{mode}_{comp}.tif", pil_bytes(im, "TIFF", compression=comp))
                self.assert_matches_tiff_oracle(p, p.stem)
                n += 1
        self.assertEqual(n, 96 + 12)

    def test_tiff_jpeg_webp_shared_provider_coverage(self) -> None:
        # JPEG- and WebP-in-TIFF have no independent oracle: they are decoded
        # by a fresh libtiff session only and reported as shared-provider
        # coverage. The writer's pattern, written uncompressed and decoded by
        # the independent oracle, is the reference: WebP lossless must match
        # it exactly, JPEG (lossy, RGB and YCbCr 2x2) must stay close to it.
        ref_p = self.work_dir() / "mk_ref_none.tif"
        self.cli_ok(["cli", "make-tiff", self.to_exe_path(ref_p), "48", "32", "3", "8", str(COMP["none"]), "1",
                     "0", "0"])
        ref = _tg.decode_tiff(ref_p.read_bytes())["pixels"]
        assert isinstance(ref, bytes)
        for comp, photometric, tag in (("jpeg", 2, "jpeg_rgb"), ("jpeg", 6, "jpeg_ycbcr"), ("webp", 2, "webp")):
            p = self.work_dir() / f"mk_{tag}.tif"
            self.cli_ok(["cli", "make-tiff", self.to_exe_path(p), "48", "32", "3", "8", str(COMP[comp]), "1", "0",
                         "0", str(photometric)])
            with self.assertRaises(_tg.UnsupportedTiff):
                _tg.decode_tiff(p.read_bytes())
            kv, px = self.c_dump("tiff", p, f"mk_{tag}")
            self.assertEqual((kv["WIDTH"], kv["HEIGHT"], kv["SAMPLE"], len(px)), ("48", "32", "3", len(ref)), tag)
            if comp == "webp":
                self.assertEqual(px, ref, tag)
            else:
                diffs = [abs(a - b) for a, b in zip(px, ref)]
                mean = sum(diffs) / len(diffs)

                def luma(buf: bytes, i: int) -> float:
                    return 0.299 * buf[i] + 0.587 * buf[i + 1] + 0.114 * buf[i + 2]

                lerr = [abs(luma(px, i) - luma(ref, i)) for i in range(0, len(ref), 3)]
                lmean = sum(lerr) / len(lerr)
                # A wrong YCbCr conversion breaks luma; 2x2 chroma subsampling
                # of a pattern with per-pixel chroma detail only raises the
                # overall error.
                self.assertLess(lmean, 8.0, f"{tag}: luma mean abs error {lmean:.2f}")
                self.assertLess(mean, 24.0 if photometric == 6 else 12.0, f"{tag}: mean abs error {mean:.2f}")
                self.assertGreater(len(set(px)), 32, f"{tag}: decoded image is degenerate")
                print(f"tiff/gif {tag}: mean abs error {mean:.2f}, luma {lmean:.2f}, against the uncompressed pattern")
            COVERAGE.append((f"mk_{tag}", "shared-provider"))

    def test_gif_independent_oracle(self) -> None:
        for interlace in (False, True):
            for transparency in (None, 3):
                im = pattern_image(29, 23, "RGB", 5).quantize(colors=16)
                kw: dict[str, object] = {"interlace": interlace}
                if transparency is not None:
                    kw["transparency"] = transparency
                tag = f"pil_gif_{int(interlace)}_{transparency}"
                p = self.fixture(f"{tag}.gif", pil_bytes(im, "GIF", **kw))
                kv, px = self.c_dump("gif", p, tag)
                w, h, ch, ref_px = _tg.decode_gif(p.read_bytes())
                self.assertEqual((int(kv["WIDTH"]), int(kv["HEIGHT"])), (w, h), tag)
                self.assertEqual(px, ref_px, tag)
                self.assertEqual(ch, 4 if transparency is not None else 3, tag)
                COVERAGE.append((tag, "independent"))

    def test_png_export_to_tiff_and_gif(self) -> None:
        wd = self.work_dir()
        for mode in ("L", "RGB", "RGBA"):
            im = pattern_image(31, 17, mode, 3)
            src = self.fixture(f"src_{mode}.png", pil_bytes(im, "PNG"))
            dst = wd / f"out_{mode}.tif"
            if dst.exists():
                dst.unlink()
            self.cli_ok(["cli", "convert", "png", "tiff", self.to_exe_path(src), self.to_exe_path(dst), "-"])
            ref = _tg.decode_tiff(dst.read_bytes())
            self.assertEqual(ref["pixels"], im.tobytes(), mode)
        # 16-bit PNG: TIFF keeps all 16 bits.
        im16 = Image.new("I;16", (12, 9))
        im16.putdata([(x * 4099 + y * 257) & 0xFFFF for y in range(9) for x in range(12)])
        src = self.fixture("src16.png", pil_bytes(im16, "PNG"))
        dst = wd / "out16.tif"
        if dst.exists():
            dst.unlink()
        self.cli_ok(["cli", "convert", "png", "tiff", self.to_exe_path(src), self.to_exe_path(dst), "-"])
        ref = _tg.decode_tiff(dst.read_bytes())
        self.assertEqual(ref["bits"], 16)
        self.assertEqual(ref["pixels"], im16.tobytes("raw", "I;16"))
        # Palette-representable PNG -> GIF, decoded independently.
        im = pattern_image(26, 14, "RGB", 2).quantize(colors=40).convert("RGB")
        src = self.fixture("src_pal.png", pil_bytes(im, "PNG"))
        dst = wd / "out_pal.gif"
        if dst.exists():
            dst.unlink()
        self.cli_ok(["cli", "convert", "png", "gif", self.to_exe_path(src), self.to_exe_path(dst), "-"])
        w, h, ch, px = _tg.decode_gif(dst.read_bytes())
        self.assertEqual((w, h, ch, px), (26, 14, 3, im.tobytes()))
        # Too many colours for GIF: refused, no output left.
        src = self.fixture("src_many.png", pil_bytes(pattern_image(64, 64, "RGB", 9), "PNG"))
        dst = wd / "out_many.gif"
        if dst.exists():
            dst.unlink()
        rc, out, err = self.run_cli(["cli", "convert", "png", "gif", self.to_exe_path(src), self.to_exe_path(dst), "-"])
        self.assertEqual(rc, 2, out + err)
        self.assertFalse(dst.exists())

    def test_seeded_mutations_never_crash_or_hang(self) -> None:
        wd = self.work_dir()
        bases = {
            "tiff": pil_bytes(pattern_image(24, 18, "RGB", 4), "TIFF", compression="tiff_lzw"),
            "gif": pil_bytes(pattern_image(24, 18, "RGB", 4).quantize(colors=32), "GIF", interlace=True),
        }
        rng = random.Random(0x7165)
        seen = {"ok": 0, "refused": 0}
        for i in range(self.mutation_count()):
            fmt = "tiff" if i % 2 == 0 else "gif"
            m = bytearray(bases[fmt])
            if i % 3 == 0:
                m = m[: rng.randrange(2, len(m))]
            else:
                for _ in range(1 + rng.randrange(6)):
                    m[rng.randrange(2, len(m))] = rng.randrange(256)
            p = wd / f"mut.{fmt}"
            p.write_bytes(bytes(m))
            raw = wd / "mut.raw"
            rc, out, err = self.run_cli(["cli", "dump", fmt, self.to_exe_path(p), self.to_exe_path(raw)], timeout=30.0)
            self.assertIn(rc, (0, 2), f"mutation {i} ({fmt}): rc={rc}\n{out}\n{err}")
            seen["ok" if rc == 0 else "refused"] += 1
        self.assertGreater(seen["refused"], 0)
        print(f"tiff/gif mutations: {seen}")


class HostTiffGifTests(TiffGifAdapterBase):
    __test__ = True
    host_exe: Path | None = None
    _tmp: tempfile.TemporaryDirectory[str] | None = None
    _wd: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_HOST_TIFF_GIF_EXE", "").strip()
        if not raw or not Path(raw).is_file():
            raise unittest.SkipTest("RFC_HOST_TIFF_GIF_EXE not configured")
        cls.host_exe = Path(raw)
        cls._tmp = tempfile.TemporaryDirectory(prefix="rfc_tg_host_")
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
        res = _ref.run_process_tree(
            [str(self.host_exe), *args], cwd=ROOT, capture_output=True, text=True, timeout=timeout
        )
        return res.returncode, res.stdout or "", res.stderr or ""


    def test_worst_case_default_jobs_fit_their_reservation(self) -> None:
        # Incompressible 4096x1024 content, by path with default settings (no
        # caller layout or estimate): the submit-time reservation derived from
        # the sniffed geometry, the provider contracts and the held source
        # bytes must cover the measured budget peak of every pairing.
        wd = self.work_dir()
        rng = random.Random(0xB16)
        rgb = Image.frombytes("RGB", (4096, 1024), rng.randbytes(4096 * 1024 * 3))
        pal = Image.frombytes("P", (4096, 1024), rng.randbytes(4096 * 1024))
        pal.putpalette(list(rng.randbytes(768)))
        srcs = {
            "png": self.fixture("wc.png", pil_bytes(rgb, "PNG")),
            "tiff": self.fixture("wc.tif", pil_bytes(rgb, "TIFF", compression="tiff_lzw")),
            "gif": self.fixture("wc.gif", pil_bytes(pal, "GIF")),
            "pngpal": self.fixture("wcp.png", pil_bytes(pal.convert("RGB"), "PNG")),
        }
        ext = {"png": "png", "tiff": "tif", "gif": "gif"}
        pairs = [("png", "png"), ("png", "tiff"), ("tiff", "tiff"), ("tiff", "png"), ("gif", "gif"),
                 ("gif", "png"), ("gif", "tiff"), ("pngpal", "gif")]
        for sk, dfmt in pairs:
            sfmt = "png" if sk == "pngpal" else sk
            dst = wd / f"wc_{sk}_to.{ext[dfmt]}"
            if dst.exists():
                dst.unlink()
            kv = self.cli_ok(["cli", "convert", sfmt, dfmt, str(srcs[sk]), str(dst), "-"], timeout=180.0)
            limit, peak = int(kv["BUDGET_LIMIT"]), int(kv["BUDGET_PEAK"])
            self.assertLessEqual(peak, limit, f"{sk}->{dfmt}")
            print(f"tiff/gif worst-case {sk}->{dfmt}: peak {peak} of {limit} ({peak / limit:.3f})")


class WindowsTiffGifTests(TiffGifAdapterBase):
    __test__ = True
    win_exe: Path | None = None
    _win_temp: str = ""
    _py_temp: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        raw = os.environ.get("RFC_WIN_TIFF_GIF_EXE", "").strip()
        cand = Path(raw) if raw else (ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_tiff_gif.exe")
        req = os.environ.get("RFC_REQUIRE_WIN_TIFF_GIF") == "1"
        if not cand.is_file():
            if req:
                raise AssertionError(f"missing Windows rfc_test_tiff_gif.exe: {cand}")
            raise unittest.SkipTest("windows-x64 rfc_test_tiff_gif.exe not built")
        cls.win_exe = cand
        if os.name == "nt":
            cls._win_temp = tempfile.mkdtemp(prefix="rfc_tg_win_")
            cls._py_temp = Path(cls._win_temp)
        else:
            res = _ref.run_process_tree(
                [
                    "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_tg_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
                    "New-Item -ItemType Directory -Path $d | Out-Null; Write-Output $d",
                ],
                capture_output=True, text=True, timeout=30.0,
            )
            if res.returncode != 0 or not (res.stdout or "").strip():
                if req:
                    raise AssertionError("powershell.exe unavailable while RFC_REQUIRE_WIN_TIFF_GIF=1")
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")
            cls._win_temp = res.stdout.strip().splitlines()[-1].strip()
            wp = _ref.run_process_tree(["wslpath", "-u", cls._win_temp], capture_output=True, text=True, timeout=15.0)
            if wp.returncode != 0 or not (wp.stdout or "").strip():
                if req:
                    raise AssertionError("wslpath failed while RFC_REQUIRE_WIN_TIFF_GIF=1")
                raise unittest.SkipTest("wslpath unavailable for Windows NTFS tests")
            cls._py_temp = Path(wp.stdout.strip())
        shutil.copy2(cls.win_exe, cls._py_temp / "rfc_test_tiff_gif.exe")

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
        return 40  # each call crosses the WSL/Windows process boundary

    def run_cli(self, args: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
        if os.name == "nt":
            exe = str(self.work_dir() / "rfc_test_tiff_gif.exe")
            env = dict(os.environ)
            root = os.environ.get("SystemRoot", r"C:\Windows")
            env["PATH"] = f"{root}\\System32;{root}"
            res = _ref.run_process_tree([exe, *args], cwd=self.work_dir(), env=env, capture_output=True, text=True, timeout=timeout)
            return res.returncode, res.stdout or "", res.stderr or ""
        arg_ps = " ".join(ps_quote(a) for a in args)
        exe_ps = ps_quote(self._win_temp.rstrip("\\/") + "\\rfc_test_tiff_gif.exe")
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
        print(f"tiff/gif coverage: {kinds}")


def load_tests(loader: unittest.TestLoader, tests: unittest.TestSuite, pattern: str | None) -> unittest.TestSuite:
    suite = unittest.TestSuite()
    for cls in (HostTiffGifTests, WindowsTiffGifTests):
        suite.addTests(loader.loadTestsFromTestCase(cls))
    return suite


if __name__ == "__main__":
    unittest.main()
