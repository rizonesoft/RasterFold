"""Pinned converter bundle, PE import, licensing, and restricted-PATH Windows tests."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
SMOKE_EXE = ROOT / 'bin/converter/windows-x64/rfc_bundle_smoke.exe'

ALLOWED_SYSTEM_DLLS = frozenset({
    'kernel32.dll',
    'user32.dll',
    'advapi32.dll',
    'shell32.dll',
    'ole32.dll',
    'oleaut32.dll',
    'bcrypt.dll',
    'ws2_32.dll',
    'gdi32.dll',
    'ucrtbase.dll',
    'msvcrt.dll',
    'sechost.dll',
    'shlwapi.dll',
    'rpcrt4.dll',
})

EXPECTED_SMOKE_PROVIDERS = (
    'rfc_registry',
    'rasterfold',
    'zlib',
    'libdeflate',
    'libpng',
    'libwebp',
    'brotli',
    'lcms2',
    'libjxl+highway+jxl_threads+jxl_cms',
    'libavif+libaom',
    'libjpeg-turbo',
    'libtiff',
    'giflib',
    'qoi',
    'openjpeg',
    'openexr+imath+openjph',
    'libraw',
)

FORBIDDEN_HEVC_IDS = frozenset({'x265', 'libde265', 'kvazaar', 'openhevc', 'libheif', 'hevc'})


def _load_bootstrap():
    spec = importlib.util.spec_from_file_location('rf_bootstrap', ROOT / 'tools/bootstrap.py')
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _load_build_converter():
    spec = importlib.util.spec_from_file_location('rf_build_converter', ROOT / 'scripts/build-converter.py')
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_pe64_imports(pe_path: Path) -> list[str]:
    """Parse Import Directory DLL names directly from a PE32+ executable."""
    data = pe_path.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError(f'{pe_path}: missing MZ signature')
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b'PE\0\0':
        raise ValueError(f'{pe_path}: missing PE signature')
    coff_off = e_lfanew + 4
    _machine, num_sections, _ts, _sym, _nsym, opt_size, _chars = struct.unpack_from('<HHIIIHH', data, coff_off)
    opt_off = coff_off + 20
    magic = struct.unpack_from('<H', data, opt_off)[0]
    if magic != 0x20B:
        raise ValueError(f'{pe_path}: expected PE32+ (0x20B), got 0x{magic:x}')
    # In PE32+, NumberOfRvaAndSizes is at opt_off + 108, DataDirectory[1] (Import) at opt_off + 120.
    import_rva, import_size = struct.unpack_from('<II', data, opt_off + 112 + 8)
    if import_rva == 0 or import_size == 0:
        return []

    sec_off = opt_off + opt_size
    sections: list[tuple[int, int, int]] = []
    for idx in range(num_sections):
        sh = sec_off + idx * 40
        vsize, vaddr, rsize, rptr = struct.unpack_from('<IIII', data, sh + 8)
        sections.append((vaddr, max(vsize, rsize), rptr))

    def rva_to_off(rva: int) -> int:
        for vaddr, span, rptr in sections:
            if vaddr <= rva < vaddr + span:
                return rptr + (rva - vaddr)
        raise ValueError(f'{pe_path}: RVA 0x{rva:x} outside PE sections')

    dlls: list[str] = []
    pos = rva_to_off(import_rva)
    while pos + 20 <= len(data):
        orig_first, _ts, _fwd, name_rva, first_thunk = struct.unpack_from('<IIIII', data, pos)
        if orig_first == 0 and name_rva == 0 and first_thunk == 0:
            break
        noff = rva_to_off(name_rva)
        end = data.index(0, noff)
        dlls.append(data[noff:end].decode('ascii', errors='strict').lower())
        pos += 20
    return dlls


def is_allowed_windows_system_dll(name: str) -> bool:
    low = name.strip().lower()
    if low in ALLOWED_SYSTEM_DLLS:
        return True
    if low.startswith('api-ms-win-crt-') and low.endswith('.dll'):
        return True
    return False


def validate_pe_imports(dlls: list[str]) -> None:
    if not dlls:
        raise ValueError('executable imports zero DLLs (expected kernel32.dll)')
    disallowed = [d for d in dlls if not is_allowed_windows_system_dll(d)]
    if disallowed:
        raise ValueError(f'disallowed non-system DLL imports: {disallowed}')


def readobj_imports(pe_path: Path) -> list[str]:
    tool = ROOT / ('tools/llvm-mingw/bin/llvm-readobj.exe' if os.name == 'nt' else 'tools/llvm-mingw-linux/bin/llvm-readobj')
    out = subprocess.check_output([str(tool), '--coff-imports', str(pe_path)], text=True, timeout=30)
    names: list[str] = []
    for line in out.splitlines():
        s = line.strip()
        if s.startswith('Name:'):
            names.append(s.split(':', 1)[1].strip().lower())
    return names


WSL_INTEROP_LAUNCH_FAILURE = 'UtilAcceptVsock'


def run_windows_binary_isolated(
    exe_path: Path,
    args: tuple[str, ...] = (),
    timeout_sec: float = 45.0,
) -> subprocess.CompletedProcess[str]:
    """Run a Windows PE binary in a clean temp dir with System32-only PATH, killing its tree on timeout."""
    res = _run_windows_binary_isolated_once(exe_path, args, timeout_sec)
    # WORKAROUND: under host load the WSL interop bridge intermittently fails
    # to start powershell.exe ("UtilAcceptVsock:281: accept4 failed 110"), a
    # WSL platform fault the repository cannot fix; the child never started
    # (it prints SMOKE_WIN_PID first), so only that signature is relaunched,
    # at most twice.
    for _ in range(2):
        if (os.name == 'nt' or res.returncode == 0 or (res.stdout or '').strip()
                or WSL_INTEROP_LAUNCH_FAILURE not in (res.stderr or '')):
            break
        res = _run_windows_binary_isolated_once(exe_path, args, timeout_sec)
    return res


def _run_windows_binary_isolated_once(
    exe_path: Path,
    args: tuple[str, ...] = (),
    timeout_sec: float = 45.0,
) -> subprocess.CompletedProcess[str]:
    """One isolated run of a Windows PE binary (see run_windows_binary_isolated)."""
    if os.name == 'nt':
        tmp = Path(tempfile.mkdtemp(prefix='rfc_smoke_'))
        copied = tmp / exe_path.name
        try:
            shutil.copy2(exe_path, copied)
            sys_root = os.environ.get('SystemRoot', r'C:\Windows')
            env = {'SystemRoot': sys_root, 'PATH': os.path.join(sys_root, 'System32')}
            creationflags = (
                getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)
                | getattr(subprocess, 'CREATE_NO_WINDOW', 0)
            )
            proc = subprocess.Popen(
                [str(copied), *args],
                cwd=tmp,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                creationflags=creationflags,
            )
            try:
                stdout, stderr = proc.communicate(timeout=timeout_sec)
            except subprocess.TimeoutExpired as exc:
                subprocess.run(
                    ['taskkill', '/F', '/T', '/PID', str(proc.pid)],
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=15,
                )
                out_tail, err_tail = proc.communicate()
                new_exc = subprocess.TimeoutExpired(
                    exc.cmd, timeout_sec, output=out_tail, stderr=err_tail
                )
                setattr(new_exc, 'win_pid', proc.pid)
                raise new_exc from exc
            return subprocess.CompletedProcess([str(copied), *args], proc.returncode, stdout, stderr)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    win_exe = subprocess.check_output(['wslpath', '-w', str(exe_path)], text=True).strip()
    run_id = os.urandom(6).hex()
    ps_script = (
        "$ErrorActionPreference='Stop'; "
        "$tmp=Join-Path ([IO.Path]::GetTempPath()) ('rfc_smoke_' + $env:RFC_RUN_ID); "
        "New-Item -ItemType Directory -Path $tmp | Out-Null; "
        "$p=$null; "
        "try { "
        "$dst=Join-Path $tmp $env:RFC_EXE_NAME; "
        "Copy-Item -LiteralPath $env:RFC_WIN_EXE -Destination $dst; "
        # WSL interop can hand Windows both PATH and Path; .NET then leaves
        # ProcessStartInfo.EnvironmentVariables null. Drop one entry so the
        # child's System32-only PATH below is the only one it receives.
        "[Environment]::SetEnvironmentVariable('PATH', $null, 'Process'); "
        "$psi=[System.Diagnostics.ProcessStartInfo]::new(); "
        "$psi.FileName=$dst; "
        "$argArr=@($env:RFC_ARGS_JSON | ConvertFrom-Json); "
        "$psi.Arguments=(@($argArr | ForEach-Object { if ($_ -match '[\\s\"]') { '\"' + ($_ -replace '\"', '\\\"') + '\"' } else { $_ } }) -join ' '); "
        "$psi.WorkingDirectory=$tmp; "
        "$psi.UseShellExecute=$false; "
        "$psi.CreateNoWindow=$true; "
        "$psi.RedirectStandardOutput=$true; "
        "$psi.RedirectStandardError=$true; "
        "$psi.EnvironmentVariables['PATH']=\"$env:SystemRoot\\System32\"; "
        "$p=[System.Diagnostics.Process]::Start($psi); "
        "[Console]::Out.WriteLine('SMOKE_WIN_PID=' + $p.Id); "
        "[Console]::Out.Flush(); "
        "$outTask=$p.StandardOutput.ReadToEndAsync(); "
        "$errTask=$p.StandardError.ReadToEndAsync(); "
        "$p.WaitForExit(); "
        "[Console]::Out.Write($outTask.GetAwaiter().GetResult()); "
        "[Console]::Error.Write($errTask.GetAwaiter().GetResult()); "
        "exit $p.ExitCode "
        # WORKAROUND: child may exit or briefly hold executable handle between WaitForExit/kill and temp cleanup.
        "} finally { "
        "if ($p -and -not $p.HasExited) { try { $p.Kill() } catch {} } "
        "Set-Location $env:SystemRoot; "
        "if (Test-Path -LiteralPath $tmp) { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -LiteralPath $tmp } "
        "}"
    )
    ps_env = dict(os.environ)
    ps_env['RFC_RUN_ID'] = run_id
    ps_env['RFC_EXE_NAME'] = exe_path.name
    ps_env['RFC_WIN_EXE'] = win_exe
    ps_env['RFC_ARGS_JSON'] = json.dumps(list(args))
    existing_wslenv = ps_env.get('WSLENV', '')
    export_keys = 'RFC_RUN_ID:RFC_EXE_NAME:RFC_WIN_EXE:RFC_ARGS_JSON'
    ps_env['WSLENV'] = f'{existing_wslenv}:{export_keys}' if existing_wslenv else export_keys

    proc = subprocess.Popen(
        ['powershell.exe', '-NoProfile', '-Command', ps_script],
        env=ps_env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    out_chunks: list[str] = []
    err_chunks: list[str] = []
    win_pid_box: list[int | None] = [None]
    pid_ready = threading.Event()

    def _drain_out() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            if win_pid_box[0] is None and line.startswith('SMOKE_WIN_PID='):
                pid_str = line.split('=', 1)[1].strip()
                if pid_str.isdigit():
                    win_pid_box[0] = int(pid_str)
                pid_ready.set()
            else:
                out_chunks.append(line)

    def _drain_err() -> None:
        assert proc.stderr is not None
        for line in proc.stderr:
            err_chunks.append(line)

    t_out = threading.Thread(target=_drain_out, daemon=True)
    t_err = threading.Thread(target=_drain_err, daemon=True)
    t_out.start()
    t_err.start()
    try:
        rc = proc.wait(timeout=timeout_sec)
        t_out.join(timeout=5)
        t_err.join(timeout=5)
        return subprocess.CompletedProcess(proc.args, rc, ''.join(out_chunks), ''.join(err_chunks))
    except subprocess.TimeoutExpired as exc:
        pid_ready.wait(timeout=1.0)
        win_pid = win_pid_box[0]
        if win_pid is not None:
            subprocess.run(
                ['taskkill.exe', '/F', '/T', '/PID', str(win_pid)],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=15,
            )
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=10)
        t_out.join(timeout=2)
        t_err.join(timeout=2)
        subprocess.run(
            [
                'powershell.exe',
                '-NoProfile',
                '-Command',
                f"$t=Join-Path ([IO.Path]::GetTempPath()) 'rfc_smoke_{run_id}'; "
                "if (Test-Path -LiteralPath $t) { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -LiteralPath $t }",
            ],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=15,
        )
        new_exc = subprocess.TimeoutExpired(
            exc.cmd, timeout_sec, output=''.join(out_chunks), stderr=''.join(err_chunks)
        )
        setattr(new_exc, 'win_pid', win_pid)
        raise new_exc from exc
    finally:
        if proc.stdout is not None:
            proc.stdout.close()
        if proc.stderr is not None:
            proc.stderr.close()


class IsolatedRunnerRelaunchTests(unittest.TestCase):
    """The isolated Windows runner relaunches only a WSL interop launch failure."""

    def test_relaunch_only_the_bridge_launch_failure(self) -> None:
        if os.name == 'nt':
            self.skipTest('the WSL interop bridge exists only on the Linux side')
        mod = sys.modules[__name__]
        real = mod._run_windows_binary_isolated_once
        bridge = subprocess.CompletedProcess(['x'], 1, '', '<3>WSL (1 - ) ERROR: UtilAcceptVsock:281: accept4 failed 110\n')
        calls: list[int] = []

        def fake(results):
            def run(exe, args=(), timeout_sec=45.0):
                calls.append(1)
                return results.pop(0)
            return run

        try:
            ok = subprocess.CompletedProcess(['x'], 0, 'SMOKE_WIN_PID=1\nall checks passed', '')
            mod._run_windows_binary_isolated_once = fake([bridge, ok])
            self.assertEqual(mod.run_windows_binary_isolated(Path('x.exe')).returncode, 0)
            self.assertEqual(len(calls), 2)
            calls.clear()
            failed = subprocess.CompletedProcess(['x'], 1, 'SMOKE_WIN_PID=1\nCHECK FAILED', '')
            mod._run_windows_binary_isolated_once = fake([failed, ok])
            self.assertEqual(mod.run_windows_binary_isolated(Path('x.exe')).returncode, 1)
            self.assertEqual(len(calls), 1)  # a test failure is never relaunched
            calls.clear()
            mod._run_windows_binary_isolated_once = fake([bridge, bridge, bridge, ok])
            self.assertEqual(mod.run_windows_binary_isolated(Path('x.exe')).returncode, 1)
            self.assertEqual(len(calls), 3)  # at most two relaunches
        finally:
            mod._run_windows_binary_isolated_once = real


class BundleTests(unittest.TestCase):
    def test_vendor_manifest_contract_and_no_hevc(self):
        vendor = json.loads((ROOT / 'converter/vendor.json').read_text(encoding='utf-8'))
        bootstrap = _load_bootstrap()
        bootstrap.validate_vendor_manifest(vendor)
        self.assertEqual(vendor['target'], 'windows-x64')
        self.assertEqual(vendor['build_tools']['required_converter_tools'], ['cmake', 'nasm'])
        self.assertIn('meson', vendor['build_tools']['omitted_tools'])
        self.assertEqual(len(vendor['packages']), 19)
        ids = {p['id'] for p in vendor['packages']}
        self.assertFalse(ids & FORBIDDEN_HEVC_IDS)
        expected_emb = {
            'libaom-fastfeat',
            'libaom-vector',
            'libaom-x86inc',
            'libavif-libyuv',
            'libjpeg-turbo-spng',
            'openexr-xxhash',
            'openexr-zstd',
        }
        embedded = vendor.get('embedded_components', [])
        self.assertEqual({e['id'] for e in embedded}, expected_emb)

        sys_codecs = vendor['system_codecs']
        self.assertTrue(sys_codecs)
        heif_rows = [s for s in sys_codecs if set(s['formats']) >= {'heif', 'heic'}]
        self.assertEqual(len(heif_rows), 1)
        self.assertFalse(heif_rows[0]['bundled'])
        self.assertFalse(heif_rows[0]['hevc_bundled'])
        self.assertFalse(heif_rows[0]['hevc_source_or_binary_present'])
        self.assertEqual(heif_rows[0]['modes'], {'read': True, 'write': False})

    def test_vendor_covers_codecs_licenses_and_notices(self):
        vendor = json.loads((ROOT / 'converter/vendor.json').read_text(encoding='utf-8'))
        codecs = json.loads((ROOT / 'converter/codecs.json').read_text(encoding='utf-8'))
        covered_formats: set[str] = set()
        for pkg in vendor['packages']:
            lic_path = ROOT / pkg['license_file']
            self.assertTrue(lic_path.is_file(), f'missing license file {pkg["license_file"]}')
            text = lic_path.read_text(encoding='utf-8')
            self.assertGreater(len(text.strip()), 80, f'too short license file: {pkg["license_file"]}')
            self.assertNotIn('\r\n', text, f'CRLF in {pkg["license_file"]}')
            covered_formats.update(pkg.get('formats', []))
        system_formats = {fmt for s in vendor['system_codecs'] for fmt in s['formats']}
        builtin_only = {'rasterfold', 'bmp', 'pnm', 'pam'}
        for row in codecs['formats']:
            fmt = row['format']
            if fmt in builtin_only:
                continue
            if fmt in system_formats:
                self.assertFalse(row['modes']['write'])
                self.assertIn(fmt, {'heif', 'heic'})
                continue
            self.assertIn(fmt, covered_formats, f'format {fmt!r} missing from converter/vendor.json')

        rc = subprocess.run(
            [sys.executable, str(ROOT / 'scripts/build-converter.py'), '--check-notices'],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(rc.returncode, 0, rc.stdout + rc.stderr)
        notices = (ROOT / 'converter/THIRD-PARTY-NOTICES.txt').read_text(encoding='utf-8')
        for pkg in vendor['packages']:
            self.assertIn(pkg['commit'], notices)
            self.assertIn(pkg['sha256'], notices)

        self.assertIn('Independent JPEG Group', notices)
        self.assertRegex(notices, r'this software is based in part on the work of\s+the Independent JPEG Group')

        runtimes = vendor.get('runtime_libraries', [])
        self.assertEqual({r['id'] for r in runtimes}, {'llvm-runtime', 'mingw-w64-runtime', 'winpthreads'})
        for rt in runtimes:
            lic_path = ROOT / rt['license_file']
            self.assertTrue(lic_path.is_file(), f'missing runtime license file {rt["license_file"]}')
            text = lic_path.read_text(encoding='utf-8')
            self.assertGreater(len(text.strip()), 80, f'too short runtime license file: {rt["license_file"]}')
            self.assertNotIn('\r\n', text, f'CRLF in {rt["license_file"]}')
            self.assertIn(rt['id'], notices)
            self.assertIn(rt['version'], notices)
            first_line = next(line.strip() for line in text.splitlines() if line.strip())
            self.assertIn(first_line, notices)

        sbom = json.loads((ROOT / 'converter/sbom.spdx.json').read_text(encoding='utf-8'))
        static_links = {
            (rel['spdxElementId'], rel['relatedSpdxElement'])
            for rel in sbom['relationships']
            if rel['relationshipType'] == 'STATIC_LINK'
        }
        contains_rels = {
            (rel['spdxElementId'], rel['relatedSpdxElement'])
            for rel in sbom['relationships']
            if rel['relationshipType'] == 'CONTAINS'
        }
        for rt in runtimes:
            spdx_id = f'SPDXRef-runtime-{rt["id"]}'
            self.assertIn(('SPDXRef-Package-rasterfold-converter', spdx_id), static_links)

        pkg_by_id = {p['id']: p for p in vendor['packages']}
        embedded = vendor.get('embedded_components', [])
        self.assertEqual(
            {e['id'] for e in embedded},
            {
                'libaom-fastfeat',
                'libaom-vector',
                'libaom-x86inc',
                'libavif-libyuv',
                'libjpeg-turbo-spng',
                'openexr-xxhash',
                'openexr-zstd',
            },
        )
        for emb in embedded:
            lic_path = ROOT / emb['license_file']
            self.assertTrue(lic_path.is_file(), f'missing embedded license file {emb["license_file"]}')
            text = lic_path.read_text(encoding='utf-8')
            self.assertGreater(len(text.strip()), 80, f'too short embedded license file: {emb["license_file"]}')
            self.assertNotIn('\r\n', text, f'CRLF in {emb["license_file"]}')
            host_pkg = pkg_by_id[emb['host_package']]
            self.assertIn(host_pkg['version'], emb['notes'])
            self.assertIn(emb['id'], notices)
            self.assertIn(emb['version'], notices)
            self.assertIn(emb['revision'], notices)
            self.assertIn(emb['subpath'], notices)
            self.assertIn(host_pkg['commit'], notices)
            self.assertIn(host_pkg['sha256'], notices)
            first_line = next(line.strip() for line in text.splitlines() if line.strip())
            self.assertIn(first_line, notices)
            emb_spdx = f'SPDXRef-embedded-{emb["id"]}'
            self.assertIn((f'SPDXRef-pkg-{emb["host_package"]}', emb_spdx), contains_rels)
            self.assertIn(('SPDXRef-Package-rasterfold-converter', emb_spdx), static_links)

        deps_build = ROOT / 'build/converter/windows-x64/deps-build'
        deps_lib = ROOT / 'build/converter/windows-x64/deps-prefix/lib'
        if deps_build.is_dir() and deps_lib.is_dir():
            declared_subpaths = {
                (e['host_package'], '/'.join(Path(e['subpath']).parts[:2]))
                for e in embedded
            }
            compiled_subpaths: set[tuple[str, str]] = set()
            for bn in sorted(deps_build.glob('*/build.ninja')):
                pkg_id = bn.parent.name
                src_marker = f'/{pkg_id}/'
                for line in bn.read_text(encoding='utf-8', errors='replace').splitlines():
                    if not line.startswith('build '):
                        continue
                    target_part, _, rest = line.partition(':')
                    if not target_part.strip().endswith('.obj'):
                        continue
                    if pkg_id == 'libaom' and ('CMakeFiles/webm.dir/' in target_part or 'CMakeFiles/yuv.dir/' in target_part):
                        continue
                    if pkg_id == 'libaom' and '.asm.obj' in target_part:
                        compiled_subpaths.add(('libaom', 'third_party/x86inc'))
                    for seg in ('third_party/', 'thirdparty/', 'external/', 'src/spng'):
                        idx = rest.find(seg)
                        if idx != -1 and src_marker in rest[:idx]:
                            sub_parts = Path(rest[idx:].split()[0]).parts[:2]
                            compiled_subpaths.add((pkg_id, '/'.join(sub_parts)))
            self.assertTrue(compiled_subpaths, 'expected compiled embedded third-party rules in deps-build')
            missing_cov = compiled_subpaths - declared_subpaths
            self.assertFalse(missing_cov, f'compiled embedded third-party subpaths missing from vendor.json: {sorted(missing_cov)}')

            ar_bin = ROOT / ('tools/llvm-mingw/bin/llvm-ar.exe' if os.name == 'nt' else 'tools/llvm-mingw-linux/bin/llvm-ar')
            exr_core = deps_lib / 'libOpenEXRCore-3_5.a'
            if ar_bin.is_file() and exr_core.is_file():
                ar_out = subprocess.check_output([str(ar_bin), 't', str(exr_core)], text=True)
                self.assertIn('zstd_compress.c.obj', ar_out)
                self.assertIn('xxhash.c.obj', ar_out)

    def test_core_codec_build_and_oracle_isolated(self):
        build_py = (ROOT / 'scripts/build.py').read_text(encoding='utf-8')
        oracle_py = (ROOT / 'tests/rfi_ref.py').read_text(encoding='utf-8')
        for marker in ('vendor.json', 'libjxl', 'libavif', 'libraw', 'openexr', 'openjpeg'):
            self.assertNotIn(marker, build_py)
            self.assertNotIn(marker, oracle_py)

    def test_smoke_binary_pe_imports_strictly_system(self):
        self.assertTrue(SMOKE_EXE.is_file(), f'missing {SMOKE_EXE}; run scripts/build-converter.py')
        pe_dlls = parse_pe64_imports(SMOKE_EXE)
        ro_dlls = readobj_imports(SMOKE_EXE)
        self.assertEqual(sorted(set(pe_dlls)), sorted(set(ro_dlls)))
        validate_pe_imports(pe_dlls)

    def test_smoke_binary_clean_dir_restricted_path_windows(self):
        self.assertTrue(SMOKE_EXE.is_file(), f'missing {SMOKE_EXE}; run scripts/build-converter.py')
        proc = run_windows_binary_isolated(SMOKE_EXE, timeout_sec=60.0)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('BUNDLE_SMOKE PASS', proc.stdout)
        for prov in EXPECTED_SMOKE_PROVIDERS:
            self.assertIn(f'PASS provider={prov}', proc.stdout)

    def test_isolated_windows_runner_gives_child_one_system32_path(self):
        # Regression: a Windows environment carrying both PATH and Path (as WSL
        # interop produces) made ProcessStartInfo.EnvironmentVariables null and
        # every isolated run fail before the child started.
        if os.name == 'nt':
            cmd_exe = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'cmd.exe'
        else:
            cmd_exe = Path('/mnt/c/Windows/System32/cmd.exe')
        if not cmd_exe.is_file():
            self.skipTest(f'Windows cmd.exe unavailable at {cmd_exe}')
        proc = run_windows_binary_isolated(cmd_exe, args=('/d', '/c', 'set', 'PATH'), timeout_sec=30.0)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        path_lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip().upper().startswith('PATH=')]
        self.assertEqual(len(path_lines), 1, proc.stdout)
        self.assertTrue(path_lines[0].upper().endswith('\\SYSTEM32'), path_lines[0])

    def test_isolated_windows_runner_kills_process_tree_on_timeout(self):
        if os.name == 'nt':
            ping_exe = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'PING.EXE'
        else:
            ping_exe = Path('/mnt/c/Windows/System32/PING.EXE')
        if not ping_exe.is_file():
            self.skipTest(f'Windows PING.EXE unavailable at {ping_exe}')
        with self.assertRaises(subprocess.TimeoutExpired) as ctx:
            run_windows_binary_isolated(ping_exe, args=('-n', '30', '127.0.0.1'), timeout_sec=1.5)
        win_pid = getattr(ctx.exception, 'win_pid', None)
        self.assertIsNotNone(win_pid, 'expected runner to capture Windows PID before timeout kill')
        tasklist_cmd = 'tasklist.exe' if os.name != 'nt' else 'tasklist'
        chk = subprocess.run(
            [tasklist_cmd, '/FI', f'PID eq {win_pid}', '/NH'],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=15,
            check=True,
        )
        self.assertNotIn(str(win_pid), chk.stdout, f'orphaned Windows child survived timeout: {chk.stdout}')

    def test_build_converter_stamp_cache_fixup_and_tool_verification(self):
        build_converter = _load_build_converter()
        bootstrap = _load_bootstrap()
        toolchain = json.loads((ROOT / 'toolchain.json').read_text(encoding='utf-8'))

        pkg = {
            'id': 'libpng',
            'version': '1.6.59',
            'commit': 'a' * 40,
            'sha256': 'b' * 64,
            'patches': [{'name': 'apng', 'sha256': 'c' * 64}],
            'cmake_options': ['-DPNG_STATIC=ON'],
            'deps': ['zlib'],
        }
        rcpt = {'tree_sha256': 'd' * 64, 'patches': [{'name': 'apng', 'sha256': 'c' * 64}]}
        base = build_converter._package_stamp(
            pkg,
            rcpt,
            'tc-1',
            extra_args=['-DZLIB_ROOT=/pfx'],
            wrapper_hash='wrap-1',
            dep_stamps={'zlib': 'zlib-stamp-1'},
        )
        self.assertNotEqual(
            base,
            build_converter._package_stamp(
                pkg, rcpt, 'tc-1', extra_args=['-DZLIB_ROOT=/pfx'], wrapper_hash='wrap-1', dep_stamps={'zlib': 'zlib-stamp-2'}
            ),
        )
        self.assertNotEqual(
            base,
            build_converter._package_stamp(
                dict(pkg, patches=[{'name': 'apng', 'sha256': 'e' * 64}]),
                rcpt, 'tc-1', extra_args=['-DZLIB_ROOT=/pfx'], wrapper_hash='wrap-1', dep_stamps={'zlib': 'zlib-stamp-1'}
            ),
        )
        self.assertNotEqual(
            base,
            build_converter._package_stamp(
                pkg, rcpt, 'tc-1', extra_args=['-DZLIB_ROOT=/pfx'], wrapper_hash='wrap-2', dep_stamps={'zlib': 'zlib-stamp-1'}
            ),
        )
        self.assertNotEqual(
            base,
            build_converter._package_stamp(
                pkg, rcpt, 'tc-1', extra_args=['-DZLIB_ROOT=/other'], wrapper_hash='wrap-1', dep_stamps={'zlib': 'zlib-stamp-1'}
            ),
        )
        self.assertNotEqual(
            base,
            build_converter._package_stamp(
                pkg, rcpt, 'tc-2', extra_args=['-DZLIB_ROOT=/pfx'], wrapper_hash='wrap-1', dep_stamps={'zlib': 'zlib-stamp-1'}
            ),
        )

        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            lib_dir = td_path / 'lib'
            lib_dir.mkdir()
            (lib_dir / 'libz.a').write_bytes(b'stale-z')
            (lib_dir / 'libzs.a').write_bytes(b'fresh-z')
            (lib_dir / 'libpng.a').write_bytes(b'stale-png')
            (lib_dir / 'libpng16.a').write_bytes(b'fresh-png')
            build_converter._post_install_fixups('zlib', td_path)
            build_converter._post_install_fixups('libpng', td_path)
            self.assertEqual((lib_dir / 'libz.a').read_bytes(), b'fresh-z')
            self.assertEqual((lib_dir / 'libpng.a').read_bytes(), b'fresh-png')

        if os.name != 'nt':
            with tempfile.TemporaryDirectory() as td:
                tools_dir = Path(td)
                host_comps = {c['name']: c for c in toolchain['hosts']['linux-x86_64']['components']}
                for cname in ('llvm-mingw-linux', 'ninja-linux', 'cmake-linux', 'nasm-linux'):
                    comp = host_comps[cname]
                    cdir = tools_dir / comp['dir']
                    probes_map = {}
                    for p_rel in comp.get('probes', [comp['probe']]):
                        p_file = cdir / p_rel
                        p_file.parent.mkdir(parents=True, exist_ok=True)
                        p_file.write_text(f'#!/bin/sh\necho "{comp["expectVersion"]}"\n', encoding='utf-8')
                        p_file.chmod(0o755)
                        probes_map[p_rel] = bootstrap.digest(p_file)
                    if comp.get('versionFile'):
                        vf = cdir / comp['versionFile']
                        vf.parent.mkdir(parents=True, exist_ok=True)
                        vf.write_text(comp['versionText'], encoding='utf-8')
                    (cdir / 'PINNED.json').write_text(
                        json.dumps({'url': comp['url'], 'sha256': comp['sha256'], 'probes': probes_map}),
                        encoding='utf-8',
                    )
                res = build_converter.host_tool_paths(toolchain, bootstrap, tools_dir=tools_dir)
                self.assertIn('cmake-linux', res['tool_receipts'])

                (tools_dir / host_comps['cmake-linux']['dir'] / 'PINNED.json').unlink()
                with self.assertRaisesRegex(RuntimeError, 'cmake-linux'):
                    build_converter.host_tool_paths(toolchain, bootstrap, tools_dir=tools_dir)

                # Restore cmake-linux and tamper a non-version probe in llvm-mingw-linux.
                cc = host_comps['cmake-linux']
                cp = tools_dir / cc['dir']
                c_probes = {p: bootstrap.digest(cp / p) for p in cc.get('probes', [cc['probe']])}
                (cp / 'PINNED.json').write_text(
                    json.dumps({'url': cc['url'], 'sha256': cc['sha256'], 'probes': c_probes}),
                    encoding='utf-8',
                )
                tamper_target = tools_dir / host_comps['llvm-mingw-linux']['dir'] / 'bin/llvm-ar'
                tamper_target.write_text('#!/bin/sh\necho tampered\n', encoding='utf-8')
                with self.assertRaisesRegex(RuntimeError, 'llvm-mingw-linux'):
                    build_converter.host_tool_paths(toolchain, bootstrap, tools_dir=tools_dir)

        # Wrapper build directory survives a cached build_target pass without running CMake.
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            conv_src = td_path / 'tools/converter-src/qoi'
            conv_src.mkdir(parents=True)
            (td_path / 'tools').mkdir(exist_ok=True)
            shutil.copy2(ROOT / 'tools/bootstrap.py', td_path / 'tools/bootstrap.py')
            (td_path / 'converter/cmake/qoi').mkdir(parents=True)
            (td_path / 'converter/licenses').mkdir(parents=True)
            (td_path / 'converter/licenses/qoi.txt').write_text('MIT\n', encoding='utf-8')
            (td_path / 'converter/licenses/llvm-runtime.txt').write_text('Apache\n', encoding='utf-8')
            (td_path / 'converter/licenses/mingw-w64-runtime.txt').write_text('ZPL\n', encoding='utf-8')
            (td_path / 'converter/licenses/winpthreads.txt').write_text('MIT\n', encoding='utf-8')
            (td_path / 'converter/cmake/mingw-w64-x86_64.cmake').write_bytes(b'# tc\n')
            wrapper_cm = td_path / 'converter/cmake/qoi/CMakeLists.txt'
            wrapper_cm.write_text('cmake_minimum_required(VERSION 3.20)\n', encoding='utf-8')
            (conv_src / 'qoi.h').write_text('/* qoi */\n', encoding='utf-8')
            shutil.copy2(ROOT / 'toolchain.json', td_path / 'toolchain.json')
            vendor_real = json.loads((ROOT / 'converter/vendor.json').read_text(encoding='utf-8'))
            qoi_pkg = dict([p for p in vendor_real['packages'] if p['id'] == 'qoi'][0], deps=[], patches=[])
            rcpt_qoi = {
                'id': qoi_pkg['id'],
                'version': qoi_pkg['version'],
                'commit': qoi_pkg['commit'],
                'url': qoi_pkg['url'],
                'sha256': qoi_pkg['sha256'],
                'bytes': qoi_pkg['bytes'],
                'patches': [],
                'tree_sha256': bootstrap.tree_digest(conv_src),
            }
            (conv_src / 'PINNED.json').write_text(json.dumps(rcpt_qoi), encoding='utf-8')
            (td_path / 'converter/vendor.json').write_text(
                json.dumps({
                    'schema': 1,
                    'target': 'windows-x64',
                    'system_codecs': [],
                    'packages': [qoi_pkg],
                    'runtime_libraries': vendor_real['runtime_libraries'],
                }),
                encoding='utf-8',
            )
            fake_tools = {
                'llvm_bin': td_path / 'llvm/bin',
                'llvm_sysroot': td_path / 'llvm/sys',
                'ninja': td_path / 'ninja',
                'cmake': td_path / 'cmake',
                'nasm': td_path / 'nasm',
                'bin_ext': '',
                'host_components': {},
                'tool_receipts': {'cmake': {'url': 'u', 'sha256': 's', 'probes': {}}},
            }
            fake_rf = td_path / 'bin/windows-x64/lib/librasterfold.a'
            fake_rf.parent.mkdir(parents=True)
            fake_rf.write_bytes(b'!<arch>\n')
            smoke_out = td_path / 'bin/converter/windows-x64/rfc_bundle_smoke.exe'
            smoke_out.parent.mkdir(parents=True)
            smoke_out.write_bytes(b'MZ')
            old_root = build_converter.ROOT
            old_htp = build_converter.host_tool_paths
            old_rf = build_converter.ensure_rasterfold_static
            old_run = build_converter._run_checked
            try:
                build_converter.ROOT = td_path
                build_converter.host_tool_paths = lambda tc, bs=None: fake_tools
                build_converter.ensure_rasterfold_static = lambda t: fake_rf
                build_converter._run_checked = lambda cmd, cwd=None, env=None: None
                b_dir = td_path / 'build/converter/windows-x64'
                (b_dir / 'deps-prefix').mkdir(parents=True)
                (b_dir / 'deps-stamps').mkdir(parents=True)
                qoi_bdir = b_dir / 'deps-build/qoi'
                qoi_bdir.mkdir(parents=True)
                marker = qoi_bdir / 'build.ninja'
                marker.write_text('# kept\n', encoding='utf-8')
                common_args = [
                    '-G', 'Ninja',
                    f'-DCMAKE_MAKE_PROGRAM={fake_tools["ninja"].as_posix()}',
                    f'-DCMAKE_TOOLCHAIN_FILE={(td_path / "converter/cmake/mingw-w64-x86_64.cmake").as_posix()}',
                    f'-DRF_LLVM_MINGW_BIN={fake_tools["llvm_bin"].as_posix()}',
                    f'-DRF_LLVM_MINGW_SYSROOT={fake_tools["llvm_sysroot"].as_posix()}',
                    f'-DRF_NASM_EXECUTABLE={fake_tools["nasm"].as_posix()}',
                    f'-DRF_DEPS_PREFIX={(b_dir / "deps-prefix").as_posix()}',
                    f'-DCMAKE_PREFIX_PATH={(b_dir / "deps-prefix").as_posix()}',
                    f'-DCMAKE_INSTALL_PREFIX={(b_dir / "deps-prefix").as_posix()}',
                    '-DCMAKE_INSTALL_LIBDIR=lib',
                    '-DCMAKE_BUILD_TYPE=Release',
                    '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
                    '-DCMAKE_POSITION_INDEPENDENT_CODE=OFF',
                ]
                tc_hash = build_converter.hashlib.sha256(
                    json.dumps(
                        {
                            'toolchain_cmake': build_converter.hashlib.sha256(
                                (td_path / 'converter/cmake/mingw-w64-x86_64.cmake').read_bytes()
                            ).hexdigest(),
                            'tool_receipts': fake_tools['tool_receipts'],
                            'common_args': common_args,
                            'target': 'windows-x64',
                        },
                        sort_keys=True,
                    ).encode('utf-8')
                ).hexdigest()
                want_qoi = build_converter._package_stamp(
                    qoi_pkg,
                    rcpt_qoi,
                    tc_hash,
                    extra_args=build_converter._extra_cmake_args('qoi', b_dir / 'deps-prefix'),
                    wrapper_hash=build_converter._wrapper_hash('qoi'),
                    dep_stamps={},
                )
                (b_dir / 'deps-stamps/qoi.stamp').write_text(want_qoi + '\n', encoding='utf-8')
                cmds_called: list[list[str]] = []
                build_converter._run_checked = lambda cmd, cwd=None, env=None: cmds_called.append(cmd)
                build_converter.build_target('windows-x64')
                self.assertTrue(marker.is_file(), 'deps-build/qoi should survive cache hit')
                self.assertEqual(len(cmds_called), 3, 'provenance generation + app configure+build should run on dep cache hit')
                self.assertIn('gen_converter_provenance.py', ' '.join(cmds_called[0]))
            finally:
                build_converter.ROOT = old_root
                build_converter.host_tool_paths = old_htp
                build_converter.ensure_rasterfold_static = old_rf
                build_converter._run_checked = old_run

    def test_negative_fixtures_rejected_loudly(self):
        bootstrap = _load_bootstrap()
        with self.assertRaisesRegex(ValueError, 'non-system DLL'):
            validate_pe_imports(['kernel32.dll', 'libwinpthread-1.dll'])
        with self.assertRaisesRegex(ValueError, 'non-system DLL'):
            validate_pe_imports(['kernel32.dll', 'libc++.dll'])
        with self.assertRaisesRegex(ValueError, 'non-system DLL'):
            validate_pe_imports(['kernel32.dll', 'libjxl.dll'])

        vendor = json.loads((ROOT / 'converter/vendor.json').read_text(encoding='utf-8'))
        bad_hevc = json.loads(json.dumps(vendor))
        bad_hevc['system_codecs'][0]['hevc_bundled'] = True
        with self.assertRaisesRegex(ValueError, 'HEVC'):
            bootstrap.validate_vendor_manifest(bad_hevc)

        bad_branch = json.loads(json.dumps(vendor))
        bad_branch['packages'][0]['branch'] = 'main'
        with self.assertRaisesRegex(ValueError, 'branch'):
            bootstrap.validate_vendor_manifest(bad_branch)

        bad_emb_host = json.loads(json.dumps(vendor))
        bad_emb_host['embedded_components'][0]['host_package'] = 'missing-pkg'
        with self.assertRaisesRegex(ValueError, 'unknown host_package'):
            bootstrap.validate_vendor_manifest(bad_emb_host)

        bad_emb_rev = json.loads(json.dumps(vendor))
        bad_emb_rev['embedded_components'][0]['revision'] = 'main'
        with self.assertRaisesRegex(ValueError, 'moving branch ref'):
            bootstrap.validate_vendor_manifest(bad_emb_rev)

        bad_emb_lic = json.loads(json.dumps(vendor))
        bad_emb_lic['embedded_components'][0]['license_file'] = '../LICENSE'
        with self.assertRaisesRegex(ValueError, 'invalid license_file'):
            bootstrap.validate_vendor_manifest(bad_emb_lic)

        bad_emb_hevc = json.loads(json.dumps(vendor))
        bad_emb_hevc['embedded_components'][0]['subpath'] = 'third_party/hevc'
        with self.assertRaisesRegex(ValueError, 'HEVC'):
            bootstrap.validate_vendor_manifest(bad_emb_hevc)

        build_converter = _load_build_converter()
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            (td_path / 'tools').mkdir()
            (td_path / 'converter').mkdir()
            shutil.copy2(ROOT / 'toolchain.json', td_path / 'toolchain.json')
            shutil.copy2(ROOT / 'tools/bootstrap.py', td_path / 'tools/bootstrap.py')
            shutil.copy2(ROOT / 'converter/vendor.json', td_path / 'converter/vendor.json')
            shutil.copytree(ROOT / 'converter/licenses', td_path / 'converter/licenses')
            shutil.copy2(
                ROOT / 'converter/THIRD-PARTY-NOTICES.txt',
                td_path / 'converter/THIRD-PARTY-NOTICES.txt',
            )
            shutil.copy2(ROOT / 'converter/sbom.spdx.json', td_path / 'converter/sbom.spdx.json')
            old_root = build_converter.ROOT
            try:
                build_converter.ROOT = td_path
                # Windows CRLF checkout of license/notice files must still pass --check-notices.
                zstd_lic = td_path / 'converter/licenses/openexr-zstd.txt'
                zstd_lic.write_bytes(zstd_lic.read_bytes().replace(b'\n', b'\r\n'))
                notices_td = td_path / 'converter/THIRD-PARTY-NOTICES.txt'
                notices_td.write_bytes(notices_td.read_bytes().replace(b'\n', b'\r\n'))
                self.assertEqual(build_converter.check_notices(), 0)

                mutated_emb = json.loads(json.dumps(vendor))
                mutated_emb['embedded_components'][0]['version'] = '999.0.0'
                (td_path / 'converter/vendor.json').write_text(json.dumps(mutated_emb), encoding='utf-8')
                self.assertEqual(build_converter.check_notices(), 1)

                mutated = json.loads(json.dumps(vendor))
                mutated['packages'][0]['version'] = '999.0.0'
                (td_path / 'converter/vendor.json').write_text(json.dumps(mutated), encoding='utf-8')
                self.assertEqual(build_converter.check_notices(), 1)
            finally:
                build_converter.ROOT = old_root


if __name__ == '__main__':
    unittest.main()
