"""Pinned converter bundle, PE import, licensing, and restricted-PATH Windows tests."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
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
        if os.name == 'nt':
            tmp = Path(tempfile.mkdtemp(prefix='rfc_smoke_'))
            try:
                copied = tmp / 'rfc_bundle_smoke.exe'
                shutil.copy2(SMOKE_EXE, copied)
                env = {'SystemRoot': os.environ.get('SystemRoot', r'C:\Windows'),
                       'PATH': r'C:\Windows\System32'}
                proc = subprocess.run([str(copied)], cwd=tmp, env=env, capture_output=True, text=True, timeout=60)
            finally:
                shutil.rmtree(tmp)
        else:
            win_exe = subprocess.check_output(['wslpath', '-w', str(SMOKE_EXE)], text=True).strip()
            ps_script = (
                "$ErrorActionPreference='Stop'; "
                "$tmp=Join-Path ([IO.Path]::GetTempPath()) ('rfc_smoke_'+[guid]::NewGuid().ToString('N')); "
                "New-Item -ItemType Directory -Path $tmp | Out-Null; "
                "try { "
                f"Copy-Item -LiteralPath '{win_exe}' -Destination (Join-Path $tmp 'rfc_bundle_smoke.exe'); "
                "$env:PATH=\"$env:SystemRoot\\System32\"; "
                "Set-Location $tmp; "
                "& (Join-Path $tmp 'rfc_bundle_smoke.exe'); "
                "exit $LASTEXITCODE "
                "} finally { Set-Location $env:SystemRoot; Remove-Item -Recurse -Force $tmp }"
            )
            proc = subprocess.run(
                ['powershell.exe', '-NoProfile', '-Command', ps_script],
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=60,
            )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('BUNDLE_SMOKE PASS', proc.stdout)
        for prov in EXPECTED_SMOKE_PROVIDERS:
            self.assertIn(f'PASS provider={prov}', proc.stdout)

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

        build_converter = _load_build_converter()
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            (td_path / 'tools').mkdir()
            (td_path / 'converter').mkdir()
            shutil.copy2(ROOT / 'toolchain.json', td_path / 'toolchain.json')
            shutil.copy2(ROOT / 'tools/bootstrap.py', td_path / 'tools/bootstrap.py')
            shutil.copytree(ROOT / 'converter/licenses', td_path / 'converter/licenses')
            shutil.copy2(
                ROOT / 'converter/THIRD-PARTY-NOTICES.txt',
                td_path / 'converter/THIRD-PARTY-NOTICES.txt',
            )
            shutil.copy2(ROOT / 'converter/sbom.spdx.json', td_path / 'converter/sbom.spdx.json')
            mutated = json.loads(json.dumps(vendor))
            mutated['packages'][0]['version'] = '999.0.0'
            (td_path / 'converter/vendor.json').write_text(json.dumps(mutated), encoding='utf-8')
            old_root = build_converter.ROOT
            try:
                build_converter.ROOT = td_path
                self.assertEqual(build_converter.check_notices(), 1)
            finally:
                build_converter.ROOT = old_root


if __name__ == '__main__':
    unittest.main()
