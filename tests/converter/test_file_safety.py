#!/usr/bin/env python3
"""End-to-end host (POSIX) and Windows NTFS file-safety suite for D10 T01 s5.

Uses tests/converter/reference.py::run_process_tree for all child executions
and tests/rfi_ref.py to independently verify .fold output bytes.
"""
from __future__ import annotations

import atexit
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def _load_mod(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module from {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_ref = _load_mod("_rfc_ref", ROOT / "tests" / "converter" / "reference.py")
_rfi_ref = _load_mod("_rf_oracle", ROOT / "tests" / "rfi_ref.py")

_TEMP_COMPILE_DIRS: list[tempfile.TemporaryDirectory[str]] = []


def _cleanup_compile_dirs() -> None:
    while _TEMP_COMPILE_DIRS:
        d = _TEMP_COMPILE_DIRS.pop()
        d.cleanup()


atexit.register(_cleanup_compile_dirs)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_bmp_rgb(width: int, height: int, seed: int) -> tuple[bytes, bytes]:
    rgb = bytearray(width * height * 3)
    row_stride = (width * 3 + 3) & ~3
    pix_bytes = bytearray(row_stride * height)
    for y in range(height):
        dst_row_off = (height - 1 - y) * row_stride
        for x in range(width):
            r = (x * 29 + y * 13 + seed) & 0xFF
            g = (x * 11 + y * 37 + seed * 3) & 0xFF
            b = (x * 43 + y * 7 + seed * 5) & 0xFF
            i = (y * width + x) * 3
            rgb[i] = r
            rgb[i + 1] = g
            rgb[i + 2] = b
            pix_bytes[dst_row_off + x * 3] = b
            pix_bytes[dst_row_off + x * 3 + 1] = g
            pix_bytes[dst_row_off + x * 3 + 2] = r

    total_size = 54 + len(pix_bytes)
    hdr = struct.pack(
        "<2sIHHI", b"BM", total_size, 0, 0, 54
    ) + struct.pack(
        "<IIIHHIIIIII",
        40,
        width,
        height,
        1,
        24,
        0,
        len(pix_bytes),
        2835,
        2835,
        0,
        0,
    )
    return hdr + bytes(pix_bytes), bytes(rgb)


def parse_kv_line(output: str) -> dict[str, str]:
    lines = [ln.strip() for ln in output.splitlines() if ln.strip()]
    if not lines:
        return {}
    kv: dict[str, str] = {}
    for tok in lines[-1].split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            kv[k] = v
    return kv


def resolve_host_exe() -> Path:
    env_exe = os.environ.get("RFC_HOST_FILE_SAFETY_EXE", "")
    if env_exe and Path(env_exe).is_file():
        return Path(env_exe)

    if os.name == "nt":
        win_exe = ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_file_safety.exe"
        if not win_exe.is_file():
            res = _ref.run_process_tree(
                [sys.executable, str(ROOT / "scripts" / "build-converter.py"), "--target", "windows-x64"],
                cwd=ROOT,
                capture_output=True,
                timeout=300.0,
            )
            if res.returncode != 0:
                raise RuntimeError(f"build-converter failed:\n{res.stdout}\n{res.stderr}")
        return win_exe

    rf_lib = ROOT / "bin" / "linux-x64" / "lib" / "librasterfold.a"
    if not rf_lib.is_file():
        res = _ref.run_process_tree(
            [sys.executable, str(ROOT / "scripts" / "build.py"), "--target", "linux-x64"],
            cwd=ROOT,
            capture_output=True,
            timeout=180.0,
        )
        if res.returncode != 0:
            raise RuntimeError(f"build.py linux-x64 failed:\n{res.stdout}\n{res.stderr}")

    tmp = tempfile.TemporaryDirectory(prefix="rfc_fs_host_bin_")
    _TEMP_COMPILE_DIRS.append(tmp)
    exe = Path(tmp.name) / "test_file_safety"
    cc = os.environ.get("CC", "cc")
    src_dir = ROOT / "converter" / "src"
    sources = [
        src_dir / "registry.c",
        src_dir / "limits.c",
        src_dir / "verify.c",
        src_dir / "metadata.c",
        src_dir / "preview.c",
        src_dir / "input.c",
        src_dir / "output.c",
        src_dir / "metrics.c",
        src_dir / "job.c",
        src_dir / "scheduler.c",
        ROOT / "tests" / "converter" / "test_file_safety.c",
    ]
    cmd = [
        cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pthread",
        "-I",
        str(ROOT / "converter" / "include"),
        "-I",
        str(ROOT / "include"),
        *(str(s) for s in sources),
        str(rf_lib),
        "-lm",
        "-pthread",
        "-o",
        str(exe),
    ]
    res = _ref.run_process_tree(cmd, cwd=ROOT, capture_output=True, timeout=120.0)
    if res.returncode != 0:
        raise RuntimeError(f"host compile failed:\n{res.stdout}\n{res.stderr}")
    return exe


def resolve_win_exe() -> Path | None:
    env_exe = os.environ.get("RFC_WIN_FILE_SAFETY_EXE", "")
    if env_exe and Path(env_exe).is_file():
        return Path(env_exe)
    candidate = ROOT / "bin" / "converter" / "windows-x64" / "rfc_test_file_safety.exe"
    if candidate.is_file():
        return candidate
    return None


def ps_quote(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


class TestHostFileSafety(unittest.TestCase):
    host_exe: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.host_exe = resolve_host_exe()

    def _run_host(self, *args: str) -> tuple[int, dict[str, str], str]:
        res = _ref.run_process_tree(
            [str(self.host_exe), *args],
            cwd=ROOT,
            capture_output=True,
            timeout=60.0,
        )
        out = (res.stdout or "") + "\n" + (res.stderr or "")
        return res.returncode, parse_kv_line(res.stdout or ""), out

    def test_existing_target_and_overwrite_flag_refusal(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rfc_py_exist_") as td:
            d = Path(td)
            dst = d / "target.fold"
            sentinel = b"EXISTING-FILE-MUST-STAY-INTACT-0123456789"
            dst.write_bytes(sentinel)
            before = sha256_file(dst)

            rc, kv, out = self._run_host("export-refuse", "-", str(dst), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "destination-exists", out)
            self.assertEqual(kv.get("published"), "0", out)
            self.assertEqual(sha256_file(dst), before)

            rc2, kv2, out2 = self._run_host("export-refuse", "-", str(dst), "1")
            self.assertNotEqual(rc2, 0, out2)
            self.assertEqual(kv2.get("name"), "invalid-argument", out2)
            self.assertEqual(sha256_file(dst), before)

            fresh = d / "fresh_with_flag.fold"
            rc3, kv3, out3 = self._run_host("export-refuse", "-", str(fresh), "1")
            self.assertNotEqual(rc3, 0, out3)
            self.assertEqual(kv3.get("name"), "invalid-argument", out3)
            self.assertFalse(fresh.exists())
            self.assertEqual([p.name for p in d.iterdir()], ["target.fold"])

    def test_same_file_and_aliases(self) -> None:
        if os.name == "nt":
            self.skipTest("POSIX symlink/hardlink tests covered separately on Windows")
        with tempfile.TemporaryDirectory(prefix="rfc_py_alias_") as td:
            d = Path(td)
            bmp_bytes, _ = make_bmp_rgb(8, 8, seed=17)
            src = d / "input.bmp"
            src.write_bytes(bmp_bytes)
            src_hash = sha256_file(src)

            # Same path input/output alias via pipeline-fold
            rc, kv, out = self._run_host("pipeline-fold", str(src), str(src), "0", "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "path-alias", out)
            self.assertEqual(sha256_file(src), src_hash)

            # Hard-link alias
            hlink = d / "hard_alias.fold"
            os.link(src, hlink)
            rc, kv, out = self._run_host("export-refuse", str(src), str(hlink), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "path-alias", out)
            self.assertEqual(sha256_file(src), src_hash)

            # Symlink alias (dest -> src, and src_link -> src checked directly)
            sym = d / "sym_alias.fold"
            os.symlink(src, sym)
            rc, kv, out = self._run_host("export-refuse", str(src), str(sym), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "path-alias", out)
            self.assertEqual(sha256_file(src), src_hash)

            src_sym = d / "src_sym.bmp"
            os.symlink(src, src_sym)
            rc, kv, out = self._run_host("export-refuse", str(src_sym), str(src), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "path-alias", out)
            self.assertEqual(sha256_file(src), src_hash)

            # Dangling symlink destination
            dangling = d / "dangling.fold"
            os.symlink(d / "does_not_exist.fold", dangling)
            rc, kv, out = self._run_host("export-refuse", "-", str(dangling), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "path-alias", out)
            self.assertFalse((d / "does_not_exist.fold").exists())

            # Symlinked parent directory alias
            real_sub = d / "real_sub"
            real_sub.mkdir()
            sub_src = real_sub / "img.bmp"
            sub_src.write_bytes(bmp_bytes)
            sub_hash = sha256_file(sub_src)
            sym_sub = d / "sym_sub"
            os.symlink(real_sub, sym_sub)
            rc, kv, out = self._run_host(
                "pipeline-fold", str(sub_src), str(sym_sub / "img.bmp"), "0", "0"
            )
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "path-alias", out)
            self.assertEqual(sha256_file(sub_src), sub_hash)

    def test_permission_denial_and_truncated_streams(self) -> None:
        if os.name == "nt":
            self.skipTest("chmod permission test is POSIX-only")
        with tempfile.TemporaryDirectory(prefix="rfc_py_perm_") as td:
            d = Path(td)
            bmp_bytes, _ = make_bmp_rgb(8, 8, seed=41)

            if os.geteuid() != 0:
                ro_dir = d / "ro_dir"
                ro_dir.mkdir()
                src = d / "in.bmp"
                src.write_bytes(bmp_bytes)
                ro_dst = ro_dir / "out.fold"
                os.chmod(ro_dir, 0o555)
                try:
                    rc, kv, out = self._run_host("pipeline-fold", str(src), str(ro_dst), "0", "0")
                    self.assertNotEqual(rc, 0, out)
                    self.assertEqual(kv.get("name"), "permission-denied", out)
                    self.assertFalse(ro_dst.exists())
                finally:
                    os.chmod(ro_dir, 0o755)

            # Truncated BMP, PNG, JPEG, and .fold streams refused safely
            trunc_bmp = d / "trunc.bmp"
            trunc_bmp.write_bytes(bmp_bytes[:-8])
            out_fold = d / "never.fold"
            rc, kv, out = self._run_host("pipeline-fold", str(trunc_bmp), str(out_fold), "0", "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "truncated-input", out)
            self.assertFalse(out_fold.exists())

            trunc_png = d / "trunc.png"
            trunc_png.write_bytes(b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00")
            rc, kv, out = self._run_host("read-check", str(trunc_png), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "truncated-input", out)

            trunc_jpg = d / "trunc.jpg"
            trunc_jpg.write_bytes(b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01")
            rc, kv, out = self._run_host("read-check", str(trunc_jpg), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "truncated-input", out)

            trunc_fold = d / "trunc.fold"
            full_fold = _rfi_ref.encode_image(8, 8, bytes((i * 17) & 0xFF for i in range(8 * 8 * 3)))
            trunc_fold.write_bytes(full_fold[:-4])
            rc, kv, out = self._run_host("read-check", str(trunc_fold), "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "truncated-input", out)

            # POSIX FIFO with no writer must return promptly instead of hanging
            fifo = d / "pipe.fifo"
            os.mkfifo(fifo)
            rc, kv, out = self._run_host("read-check", str(fifo), "0")
            self.assertNotEqual(rc, 0, out)

            # POSIX trailing slash destination must be refused
            rc, kv, out = self._run_host("export-refuse", "-", str(d) + "/", "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "invalid-argument", out)

    def test_posix_rlimit_fsize_disk_full_cleanup(self) -> None:
        if os.name == "nt":
            self.skipTest("RLIMIT_FSIZE is POSIX-only")
        with tempfile.TemporaryDirectory(prefix="rfc_py_rlim_") as td:
            d = Path(td)
            dst = d / "out.fold"
            foreign1 = d / "out.fold.partial"
            foreign2 = d / "out.fold.rfc-partial.999999.1.1.1"
            foreign_data = b"FOREIGN-PARTIAL-MUST-SURVIVE"
            foreign1.write_bytes(foreign_data)
            foreign2.write_bytes(foreign_data)
            h1 = sha256_file(foreign1)
            h2 = sha256_file(foreign2)

            helper = (
                "import os, resource, signal, sys;"
                "signal.signal(signal.SIGXFSZ, signal.SIG_IGN);"
                "resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096));"
                "os.execv(sys.argv[1], sys.argv[1:])"
            )
            res = _ref.run_process_tree(
                [sys.executable, "-c", helper, str(self.host_exe), "write-fault", str(dst), "65536", "0"],
                cwd=ROOT,
                capture_output=True,
                timeout=60.0,
            )
            out = (res.stdout or "") + "\n" + (res.stderr or "")
            kv = parse_kv_line(res.stdout or "")
            self.assertNotEqual(res.returncode, 0, out)
            self.assertEqual(kv.get("name"), "disk-full", out)
            self.assertEqual(kv.get("published"), "0", out)
            self.assertEqual(kv.get("cleaned"), "1", out)
            self.assertFalse(dst.exists())
            self.assertEqual(sha256_file(foreign1), h1)
            self.assertEqual(sha256_file(foreign2), h2)
            remaining = sorted(p.name for p in d.iterdir())
            self.assertEqual(
                remaining,
                ["out.fold.partial", "out.fold.rfc-partial.999999.1.1.1"],
            )

    def test_sniff_script_refusal_disagreement_and_fold_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rfc_py_rt_") as td:
            d = Path(td)
            # SVG disguised as .png and extensionless SVG must both be refused
            svg_body = b'<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>'
            svg_png = d / "evil.png"
            svg_png.write_bytes(svg_body)
            rc, kv, out = self._run_host("sniff", str(svg_png))
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "format-disagreement", out)
            self.assertEqual(kv.get("disagree"), "1", out)
            self.assertEqual(kv.get("content"), "0", out)

            svg_noext = d / "evil_svg"
            svg_noext.write_bytes(svg_body)
            rc, kv, out = self._run_host("sniff", str(svg_noext))
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "unknown-format", out)
            self.assertEqual(kv.get("content"), "0", out)

            # Real BMP disguised as .png
            bmp_bytes, orig_rgb = make_bmp_rgb(16, 12, seed=53)
            trick_png = d / "trick.png"
            trick_png.write_bytes(bmp_bytes)
            trick_hash = sha256_file(trick_png)

            out_refused = d / "refused.fold"
            rc, kv, out = self._run_host("pipeline-fold", str(trick_png), str(out_refused), "0", "0")
            self.assertNotEqual(rc, 0, out)
            self.assertEqual(kv.get("name"), "format-disagreement", out)
            self.assertFalse(out_refused.exists())

            out_ok = d / "allowed.fold"
            rc, kv, out = self._run_host("pipeline-fold", str(trick_png), str(out_ok), "1", "0")
            self.assertEqual(rc, 0, out)
            self.assertEqual(kv.get("disagree"), "1", out)
            self.assertEqual(kv.get("published"), "1", out)
            self.assertEqual(sha256_file(trick_png), trick_hash)
            fold_blob = out_ok.read_bytes()
            self.assertTrue(fold_blob.startswith(_rfi_ref.MAGIC))
            dec_w, dec_h, dec_rgb = _rfi_ref.decode_image(fold_blob)
            self.assertEqual((dec_w, dec_h), (16, 12))
            self.assertEqual(dec_rgb, orig_rgb)

            # Standard .bmp -> .fold round-trip with before/after hash preservation & no temp leftovers
            real_bmp = d / "photo.bmp"
            real_bmp.write_bytes(bmp_bytes)
            real_hash = sha256_file(real_bmp)
            final_fold = d / "photo.fold"
            rc, kv, out = self._run_host("pipeline-fold", str(real_bmp), str(final_fold), "0", "0")
            self.assertEqual(rc, 0, out)
            self.assertEqual(kv.get("published"), "1", out)
            self.assertEqual(kv.get("cleaned"), "1", out)
            self.assertEqual(sha256_file(real_bmp), real_hash)
            w2, h2, rgb2 = _rfi_ref.decode_image(final_fold.read_bytes())
            self.assertEqual((w2, h2), (16, 12))
            self.assertEqual(rgb2, orig_rgb)
            partials = [p.name for p in d.iterdir() if ".rfc-partial." in p.name]
            self.assertEqual(partials, [])
            print(
                f" [sha256-posix src={real_hash} trick={trick_hash} fold={sha256_file(final_fold)}]",
                end="",
                flush=True,
            )


class TestWindowsNtfsFileSafety(unittest.TestCase):
    win_exe: Path | None = None
    ps_ok: bool = False

    @classmethod
    def setUpClass(cls) -> None:
        cls.win_exe = resolve_win_exe()
        if cls.win_exe is None:
            raise unittest.SkipTest("windows-x64 rfc_test_file_safety.exe not built")
        if os.name == "nt":
            cls.ps_ok = True
        else:
            res = _ref.run_process_tree(
                ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", "[IO.Path]::GetTempPath()"],
                capture_output=True,
                timeout=30.0,
            )
            cls.ps_ok = res.returncode == 0 and bool((res.stdout or "").strip())
            if not cls.ps_ok:
                raise unittest.SkipTest("powershell.exe unavailable for Windows NTFS tests")

    def _run_ps(self, script: str, timeout: float = 90.0) -> str:
        res = _ref.run_process_tree(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script],
            cwd=ROOT,
            capture_output=True,
            timeout=timeout,
        )
        if res.returncode != 0:
            raise AssertionError(f"PowerShell failed (rc={res.returncode}):\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")
        return res.stdout or ""

    def _win_to_py_path(self, win_path: str) -> Path:
        if os.name == "nt":
            return Path(win_path)
        res = _ref.run_process_tree(["wslpath", "-u", win_path], capture_output=True, timeout=15.0)
        if res.returncode != 0:
            raise RuntimeError(f"wslpath -u failed for {win_path}: {res.stderr}")
        return Path((res.stdout or "").strip())

    def test_ntfs_aliases_acl_diskfull_and_fold_roundtrip(self) -> None:
        assert self.win_exe is not None
        win_exe_for_ps = str(self.win_exe)
        if os.name != "nt":
            wres = _ref.run_process_tree(["wslpath", "-w", str(self.win_exe)], capture_output=True, timeout=15.0)
            self.assertEqual(wres.returncode, 0, wres.stderr)
            win_exe_for_ps = (wres.stdout or "").strip()

        win_temp_dir = self._run_ps(
            "$d = Join-Path ([IO.Path]::GetTempPath()) ('rfc_ntfs_' + [Guid]::NewGuid().ToString('N').Substring(0,8)); "
            "New-Item -ItemType Directory -Path $d | Out-Null; "
            "Write-Output $d"
        ).strip().splitlines()[-1].strip()

        py_temp_dir = self._win_to_py_path(win_temp_dir)
        ro_win_dir = f"{win_temp_dir}\\ro_acl"
        try:
            exe_dst = py_temp_dir / "rfc_test_file_safety.exe"
            shutil.copy2(self.win_exe, exe_dst)
            bmp_bytes, orig_rgb = make_bmp_rgb(16, 16, seed=79)

            src_bmp = py_temp_dir / "src.bmp"
            src_bmp.write_bytes(bmp_bytes)
            src_hash = sha256_file(src_bmp)

            existing = py_temp_dir / "existing.fold"
            existing.write_bytes(b"WIN-SENTINEL-0123456789")
            exist_hash = sha256_file(existing)

            foreign_partial = py_temp_dir / "disk.fold.partial"
            foreign_partial.write_bytes(b"WIN-FOREIGN-PARTIAL")
            fp_hash = sha256_file(foreign_partial)

            # Create NTFS hard link, NTFS junction directory, and ACL-denied dir inside Windows temp
            ps_script = f"""
$ErrorActionPreference = 'Stop'
$root = {ps_quote(win_temp_dir)}
$exe = Join-Path $root 'rfc_test_file_safety.exe'
$src = Join-Path $root 'src.bmp'
$exist = Join-Path $root 'existing.fold'
$hlink = Join-Path $root 'hard_alias.fold'
cmd.exe /c mklink /H "$hlink" "$src" | Out-Null
if ($LASTEXITCODE -ne 0) {{ throw 'mklink /H failed' }}

$realSub = Join-Path $root 'real_sub'
New-Item -ItemType Directory -Path $realSub | Out-Null
Copy-Item $src (Join-Path $realSub 'img.bmp')
$juncSub = Join-Path $root 'junc_sub'
cmd.exe /c mklink /J "$juncSub" "$realSub" | Out-Null
if ($LASTEXITCODE -ne 0) {{ throw 'mklink /J failed' }}

$roDir = Join-Path $root 'ro_acl'
New-Item -ItemType Directory -Path $roDir | Out-Null
icacls "$roDir" /deny "*S-1-1-0:(OI)(CI)(W)" | Out-Null
if ($LASTEXITCODE -ne 0) {{ throw 'icacls /deny failed' }}

$env:PATH = "$env:SystemRoot\\System32;$env:SystemRoot"

function Invoke-Fs([string]$tag, [string[]]$a) {{
    $out = & $exe @a 2>&1
    $rc = $LASTEXITCODE
    Write-Output ("CASE=" + $tag + " RC=" + $rc + " OUT=" + ($out -join ' '))
}}

Invoke-Fs 'exist0' @('export-refuse', '-', $exist, '0')
Invoke-Fs 'exist1' @('export-refuse', '-', $exist, '1')
Invoke-Fs 'same' @('pipeline-fold', $src, $src, '0', '0')
Invoke-Fs 'hardlink' @('export-refuse', $src, $hlink, '0')
Invoke-Fs 'junction' @('pipeline-fold', (Join-Path $realSub 'img.bmp'), (Join-Path $juncSub 'img.bmp'), '0', '0')
Invoke-Fs 'con' @('export-refuse', '-', (Join-Path $root 'CON.fold'), '0')
Invoke-Fs 'nulspace' @('export-refuse', '-', (Join-Path $root 'NUL .fold'), '0')
Invoke-Fs 'conparent' @('export-refuse', '-', (Join-Path $root 'CON\\out.fold'), '0')
Invoke-Fs 'acl' @('pipeline-fold', $src, (Join-Path $roDir 'out.fold'), '0', '0')
Invoke-Fs 'diskfull' @('write-fault', (Join-Path $root 'disk.fold'), '65536', '1')
Invoke-Fs 'roundtrip' @('pipeline-fold', $src, (Join-Path $root 'out.fold'), '0', '0')
"""
            raw = self._run_ps(ps_script, timeout=120.0)
            cases: dict[str, tuple[int, dict[str, str], str]] = {}
            for line in raw.splitlines():
                if line.startswith("CASE="):
                    parts = line.split(" OUT=", 1)
                    head = parts[0]
                    body = parts[1] if len(parts) > 1 else ""
                    hparts = head.split()
                    tag = hparts[0].split("=", 1)[1]
                    rc = int(hparts[1].split("=", 1)[1])
                    cases[tag] = (rc, parse_kv_line(body), body)

            self.assertNotEqual(cases["exist0"][0], 0, raw)
            self.assertEqual(cases["exist0"][1].get("name"), "destination-exists", raw)
            self.assertNotEqual(cases["exist1"][0], 0, raw)
            self.assertEqual(cases["exist1"][1].get("name"), "invalid-argument", raw)
            self.assertEqual(sha256_file(existing), exist_hash)

            self.assertNotEqual(cases["same"][0], 0, raw)
            self.assertEqual(cases["same"][1].get("name"), "path-alias", raw)
            self.assertEqual(cases["hardlink"][1].get("name"), "path-alias", raw)
            self.assertEqual(cases["junction"][1].get("name"), "path-alias", raw)
            self.assertEqual(cases["con"][1].get("name"), "invalid-argument", raw)
            self.assertEqual(cases["nulspace"][1].get("name"), "invalid-argument", raw)
            self.assertEqual(cases["conparent"][1].get("name"), "invalid-argument", raw)
            self.assertEqual(cases["acl"][1].get("name"), "permission-denied", raw)
            self.assertEqual(sha256_file(src_bmp), src_hash)

            self.assertNotEqual(cases["diskfull"][0], 0, raw)
            self.assertEqual(cases["diskfull"][1].get("name"), "disk-full", raw)
            self.assertEqual(cases["diskfull"][1].get("published"), "0", raw)
            self.assertEqual(cases["diskfull"][1].get("cleaned"), "1", raw)
            self.assertFalse((py_temp_dir / "disk.fold").exists())
            self.assertEqual(sha256_file(foreign_partial), fp_hash)

            self.assertEqual(cases["roundtrip"][0], 0, raw)
            self.assertEqual(cases["roundtrip"][1].get("published"), "1", raw)
            out_fold = py_temp_dir / "out.fold"
            self.assertTrue(out_fold.is_file())
            fold_bytes = out_fold.read_bytes()
            self.assertTrue(fold_bytes.startswith(_rfi_ref.MAGIC))
            w, h, dec_rgb = _rfi_ref.decode_image(fold_bytes)
            self.assertEqual((w, h), (16, 16))
            self.assertEqual(dec_rgb, orig_rgb)

            partials = [p.name for p in py_temp_dir.iterdir() if ".rfc-partial." in p.name]
            self.assertEqual(partials, [])
            print(
                f" [sha256-ntfs src={src_hash} exist={exist_hash} foreign={fp_hash} fold={sha256_file(out_fold)}]",
                end="",
                flush=True,
            )
        finally:
            junc_win_dir = f"{win_temp_dir}\\junc_sub"
            self._run_ps(
                f"if (Test-Path {ps_quote(junc_win_dir)}) {{ cmd.exe /c rmdir {ps_quote(junc_win_dir)} | Out-Null }}; "
                f"if (Test-Path {ps_quote(ro_win_dir)}) {{ "
                f"icacls {ps_quote(ro_win_dir)} /remove:d '*S-1-1-0' | Out-Null; "
                f"icacls {ps_quote(ro_win_dir)} /reset /T | Out-Null }}; "
                f"if (Test-Path {ps_quote(win_temp_dir)}) {{ "
                f"Remove-Item -LiteralPath {ps_quote(win_temp_dir)} -Recurse -Force }}"
            )
            _ = win_exe_for_ps


if __name__ == "__main__":
    unittest.main()
