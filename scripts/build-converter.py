#!/usr/bin/env python3
"""Build pinned Windows converter codec dependencies and generate legal notices/SBOM."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('windows-x64',)
SPDX_CREATED = '2026-11-15T00:00:00Z'

WRAPPER_PACKAGES = {'lcms2', 'giflib', 'qoi', 'libraw'}


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding='utf-8'))


def load_bootstrap():
    spec = importlib.util.spec_from_file_location('rf_bootstrap', ROOT / 'tools/bootstrap.py')
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def host_tool_paths(toolchain: dict, bootstrap=None, tools_dir: Path | None = None) -> dict:
    if bootstrap is None:
        bootstrap = load_bootstrap()
    tools_root = tools_dir if tools_dir is not None else (ROOT / 'tools')
    if sys.platform == 'linux':
        bin_ext = ''
        llvm_dir = tools_root / 'llvm-mingw-linux'
        ninja_exe = tools_root / 'ninja-linux/ninja'
        cmake_exe = tools_root / 'cmake-linux/bin/cmake'
        nasm_exe = tools_root / 'nasm-linux/bin/nasm'
        required_names = ('llvm-mingw-linux', 'ninja-linux', 'cmake-linux', 'nasm-linux')
        host_components = {c['name']: c for c in toolchain['hosts']['linux-x86_64']['components']}
    elif os.name == 'nt':
        bin_ext = '.exe'
        llvm_dir = tools_root / 'llvm-mingw'
        ninja_exe = tools_root / 'ninja/ninja.exe'
        cmake_exe = tools_root / 'cmake/bin/cmake.exe'
        nasm_exe = tools_root / 'nasm/nasm.exe'
        required_names = ('llvm-mingw', 'ninja', 'cmake', 'nasm')
        host_components = {c['name']: c for c in toolchain['components']}
    else:
        raise RuntimeError(f'unsupported host platform: {sys.platform}')

    tool_receipts: dict[str, dict] = {}
    for cname in required_names:
        comp = host_components.get(cname)
        if comp is None:
            raise KeyError(f'missing toolchain component definition for {cname!r}')
        try:
            bootstrap.verify(comp, tools_root)
        except Exception as exc:
            raise RuntimeError(
                f'converter host tool verification failed for {cname}: {exc} '
                '(run python3 tools/bootstrap.py --converter on Linux or '
                'powershell -File tools\\bootstrap.ps1 -Converter on Windows)'
            ) from exc
        receipt = load_json(tools_root / comp['dir'] / 'PINNED.json')
        tool_receipts[cname] = {
            'url': receipt['url'],
            'sha256': receipt['sha256'],
            'probes': receipt['probes'],
        }

    tools = {
        'llvm_bin': llvm_dir / 'bin',
        'llvm_sysroot': llvm_dir / 'x86_64-w64-mingw32',
        'ninja': ninja_exe,
        'cmake': cmake_exe,
        'nasm': nasm_exe,
        'bin_ext': bin_ext,
        'host_components': host_components,
        'tool_receipts': tool_receipts,
    }
    for key in ('ninja', 'cmake', 'nasm'):
        if not tools[key].is_file():
            raise FileNotFoundError(
                f'missing converter host tool {tools[key]} '
                '(run tools/bootstrap.py --converter on Linux or tools\\bootstrap.ps1 -Converter on Windows)'
            )
    if not (tools['llvm_bin'] / f'x86_64-w64-mingw32-clang{bin_ext}').is_file():
        raise FileNotFoundError(f'missing llvm-mingw compiler in {tools["llvm_bin"]}')
    return tools


def ensure_rasterfold_static(target: str) -> Path:
    subprocess.run([sys.executable, str(ROOT / 'scripts/build.py'), '--target', target], cwd=ROOT, check=True)
    lib_path = ROOT / 'bin' / target / 'lib' / 'librasterfold.a'
    if not lib_path.is_file():
        raise FileNotFoundError(f'expected RasterFold static library missing: {lib_path}')
    return lib_path


def _run_checked(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print('+', ' '.join(cmd), flush=True)
    subprocess.run(cmd, cwd=cwd or ROOT, env=env, check=True)


def _wrapper_hash(pkg_id: str) -> str:
    if pkg_id not in WRAPPER_PACKAGES:
        return ''
    wrapper_dir = ROOT / 'converter/cmake' / pkg_id
    if not wrapper_dir.is_dir():
        raise FileNotFoundError(f'missing wrapper CMake directory: {wrapper_dir}')
    h = hashlib.sha256()
    for p in sorted(wrapper_dir.rglob('*')):
        if p.is_file():
            rel = p.relative_to(wrapper_dir).as_posix()
            h.update(f'{rel}\0{hashlib.sha256(p.read_bytes()).hexdigest()}\n'.encode('utf-8'))
    return h.hexdigest()


def _package_stamp(
    pkg: dict,
    receipt: dict,
    toolchain_hash: str,
    extra_args: list[str] | None = None,
    wrapper_hash: str = '',
    dep_stamps: dict[str, str] | None = None,
) -> str:
    payload = {
        'id': pkg['id'],
        'version': pkg['version'],
        'commit': pkg['commit'],
        'sha256': pkg['sha256'],
        'tree_sha256': receipt['tree_sha256'],
        'patches': pkg.get('patches', []),
        'receipt_patches': receipt.get('patches', []),
        'cmake_options': pkg.get('cmake_options', []),
        'extra_cmake_args': list(extra_args or []),
        'wrapper_hash': wrapper_hash,
        'deps': pkg.get('deps', []),
        'dep_stamps': dict(dep_stamps or {}),
        'toolchain_hash': toolchain_hash,
    }
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode('utf-8')).hexdigest()


def _extra_cmake_args(pkg_id: str, prefix: Path) -> list[str]:
    pfx = prefix.as_posix()
    args: list[str] = []
    if pkg_id in ('libpng', 'libtiff', 'libraw'):
        zlib_a = prefix / 'lib' / 'libz.a'
        args.extend([
            f'-DZLIB_ROOT={pfx}',
            f'-DZLIB_INCLUDE_DIR={pfx}/include',
            f'-DZLIB_LIBRARY={zlib_a.as_posix()}',
        ])
    if pkg_id == 'libjxl':
        args.extend([
            f'-DLCMS2_INCLUDE_DIR={pfx}/include',
            f'-DLCMS2_LIBRARIES={(prefix / "lib" / "liblcms2.a").as_posix()}',
            f'-DBROTLI_INCLUDE_DIR={pfx}/include',
            f'-DBROTLICOMMON_LIBRARY={(prefix / "lib" / "libbrotlicommon.a").as_posix()}',
            f'-DBROTLIDEC_LIBRARY={(prefix / "lib" / "libbrotlidec.a").as_posix()}',
            f'-DBROTLIENC_LIBRARY={(prefix / "lib" / "libbrotlienc.a").as_posix()}',
        ])
    if pkg_id == 'libavif':
        sharpyuv_inc = prefix / 'include' / 'webp'
        args.extend([
            f'-DAOM_INCLUDE_DIR={pfx}/include',
            f'-DAOM_LIBRARY={(prefix / "lib" / "libaom.a").as_posix()}',
            f'-DLIBSHARPYUV_INCLUDE_DIR={sharpyuv_inc.as_posix()}',
            f'-DLIBSHARPYUV_LIBRARY={(prefix / "lib" / "libsharpyuv.a").as_posix()}',
        ])
    if pkg_id == 'libwebp':
        args.extend([
            '-DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON',
            '-DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON',
            '-DCMAKE_DISABLE_FIND_PACKAGE_TIFF=ON',
            '-DCMAKE_DISABLE_FIND_PACKAGE_GIF=ON',
        ])
    if pkg_id == 'libjpeg-turbo':
        # libjpeg-turbo configures src/spng before find_package(ZLIB), so spng-static
        # misses deps-prefix/include unless passed explicitly when WITH_SYSTEM_ZLIB=ON.
        args.append(f'-DCMAKE_C_FLAGS=-march=x86-64-v2 -isystem {pfx}/include')
    if pkg_id == 'libtiff':
        args.extend([
            '-Dtiff-cxx=OFF',
            f'-DJPEG_INCLUDE_DIR={pfx}/include',
            f'-DJPEG_LIBRARY={(prefix / "lib" / "libjpeg.a").as_posix()}',
            f'-DDeflate_INCLUDE_DIR={pfx}/include',
            f'-DDeflate_LIBRARY={(prefix / "lib" / "libdeflate.a").as_posix()}',
            f'-DWebP_INCLUDE_DIR={pfx}/include',
            f'-DWebP_LIBRARY={(prefix / "lib" / "libwebp.a").as_posix()}',
        ])
    return args


def _refresh_archive_alias(src: Path, alias: Path) -> None:
    if not src.is_file():
        return
    if alias.exists() or alias.is_symlink():
        if alias.exists() and not alias.is_symlink() and os.path.samefile(src, alias):
            return
        alias.unlink()
    shutil.copy2(src, alias)


def _post_install_fixups(pkg_id: str, prefix: Path) -> None:
    lib_dir = prefix / 'lib'
    if pkg_id == 'zlib':
        # zlib 1.4 CMake names static archives libzs.a on MinGW unless normalized.
        _refresh_archive_alias(lib_dir / 'libzs.a', lib_dir / 'libz.a')
    elif pkg_id == 'libpng':
        _refresh_archive_alias(lib_dir / 'libpng16.a', lib_dir / 'libpng.a')


def build_target(target: str, clean: bool = False) -> Path:
    if target not in TARGETS:
        raise ValueError(f'unsupported converter target {target!r}; supported: {TARGETS}')
    vendor = load_json(ROOT / 'converter/vendor.json')
    toolchain = load_json(ROOT / 'toolchain.json')
    bootstrap = load_bootstrap()
    bootstrap.validate_vendor_manifest(vendor)

    tools = host_tool_paths(toolchain, bootstrap)
    rf_lib = ensure_rasterfold_static(target)

    converter_src_root = ROOT / 'tools/converter-src'
    build_root = ROOT / 'build/converter' / target
    if clean and build_root.exists():
        shutil.rmtree(build_root)
    deps_prefix = build_root / 'deps-prefix'
    stamps_dir = build_root / 'deps-stamps'
    deps_build_root = build_root / 'deps-build'
    smoke_build = build_root / 'app'
    deps_prefix.mkdir(parents=True, exist_ok=True)
    stamps_dir.mkdir(parents=True, exist_ok=True)
    deps_build_root.mkdir(parents=True, exist_ok=True)

    toolchain_file = ROOT / 'converter/cmake/mingw-w64-x86_64.cmake'
    common_args = [
        '-G', 'Ninja',
        f'-DCMAKE_MAKE_PROGRAM={tools["ninja"].as_posix()}',
        f'-DCMAKE_TOOLCHAIN_FILE={toolchain_file.as_posix()}',
        f'-DRF_LLVM_MINGW_BIN={tools["llvm_bin"].as_posix()}',
        f'-DRF_LLVM_MINGW_SYSROOT={tools["llvm_sysroot"].as_posix()}',
        f'-DRF_NASM_EXECUTABLE={tools["nasm"].as_posix()}',
        f'-DRF_DEPS_PREFIX={deps_prefix.as_posix()}',
        f'-DCMAKE_PREFIX_PATH={deps_prefix.as_posix()}',
        f'-DCMAKE_INSTALL_PREFIX={deps_prefix.as_posix()}',
        '-DCMAKE_INSTALL_LIBDIR=lib',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
        '-DCMAKE_POSITION_INDEPENDENT_CODE=OFF',
    ]

    tc_hash_input = json.dumps(
        {
            'toolchain_cmake': hashlib.sha256(toolchain_file.read_bytes()).hexdigest(),
            'tool_receipts': tools['tool_receipts'],
            'common_args': common_args,
            'target': target,
        },
        sort_keys=True,
    ).encode('utf-8')
    toolchain_hash = hashlib.sha256(tc_hash_input).hexdigest()

    env = dict(os.environ)
    env.pop('PKG_CONFIG_PATH', None)
    env['PKG_CONFIG_LIBDIR'] = ''
    env['PATH'] = f'{tools["llvm_bin"]}{os.pathsep}{tools["nasm"].parent}{os.pathsep}{env.get("PATH", "")}'

    resolved_stamps: dict[str, str] = {}
    rebuilt_any = False

    for pkg in vendor['packages']:
        pid = pkg['id']
        bootstrap.verify_vendor_package(pkg, converter_src_root)
        src_dir = converter_src_root / pid
        receipt = load_json(src_dir / 'PINNED.json')
        extra_args = _extra_cmake_args(pid, deps_prefix)
        w_hash = _wrapper_hash(pid)
        dep_stamps = {dep: resolved_stamps[dep] for dep in pkg.get('deps', [])}
        want_stamp = _package_stamp(
            pkg,
            receipt,
            toolchain_hash,
            extra_args=extra_args,
            wrapper_hash=w_hash,
            dep_stamps=dep_stamps,
        )
        stamp_file = stamps_dir / f'{pid}.stamp'
        if stamp_file.is_file() and stamp_file.read_text(encoding='utf-8').strip() == want_stamp:
            resolved_stamps[pid] = want_stamp
            print(f'ok dep:{pid} {pkg["version"]} (cached)', flush=True)
            continue

        rebuilt_any = True
        bdir = deps_build_root / pid
        if bdir.exists():
            shutil.rmtree(bdir)
        cmake_src = (ROOT / 'converter/cmake' / pid) if pid in WRAPPER_PACKAGES else src_dir
        cfg_cmd = [
            str(tools['cmake']),
            '-S', cmake_src.as_posix(),
            '-B', bdir.as_posix(),
            *common_args,
            *pkg.get('cmake_options', []),
            *extra_args,
        ]
        if pid in WRAPPER_PACKAGES:
            cfg_cmd.append(f'-DVENDOR_SOURCE_DIR={src_dir.as_posix()}')

        _run_checked(cfg_cmd, env=env)
        _run_checked([str(tools['cmake']), '--build', bdir.as_posix(), '--parallel'], env=env)
        _run_checked([str(tools['cmake']), '--install', bdir.as_posix()], env=env)
        _post_install_fixups(pid, deps_prefix)
        stamp_file.write_text(want_stamp + '\n', encoding='utf-8')
        resolved_stamps[pid] = want_stamp
        print(f'built dep:{pid} {pkg["version"]}', flush=True)

    # Configure and build converter/CMakeLists.txt -> bin/converter/<target>/rfc_bundle_smoke.exe
    out_exe = ROOT / 'bin/converter' / target / 'rfc_bundle_smoke.exe'
    if rebuilt_any and out_exe.is_file():
        out_exe.unlink()
    app_cfg = [
        str(tools['cmake']),
        '-S', (ROOT / 'converter').as_posix(),
        '-B', smoke_build.as_posix(),
        *common_args,
        f'-DRF_LIBRASTERFOLD={rf_lib.as_posix()}',
        f'-DRF_CONVERTER_BIN_DIR={(ROOT / "bin/converter" / target).as_posix()}',
    ]
    _run_checked(app_cfg, env=env)
    _run_checked([str(tools['cmake']), '--build', smoke_build.as_posix(), '--parallel'], env=env)
    if not out_exe.is_file():
        raise FileNotFoundError(f'expected smoke binary missing: {out_exe}')
    print(f'built converter bundle smoke: {out_exe.relative_to(ROOT).as_posix()}', flush=True)
    return out_exe


def _spdx_id(name: str) -> str:
    return 'SPDXRef-' + re.sub(r'[^A-Za-z0-9.-]+', '-', name)


def render_notices_and_sbom() -> tuple[str, str]:
    vendor = load_json(ROOT / 'converter/vendor.json')
    toolchain = load_json(ROOT / 'toolchain.json')
    bootstrap = load_bootstrap()
    bootstrap.validate_vendor_manifest(vendor)

    win_tools = {c['name']: c for c in toolchain['components']}
    lin_tools = {c['name']: c for c in toolchain['hosts']['linux-x86_64']['components']}

    lines: list[str] = [
        'RasterFold Converter Third-Party Notices and Source Retrieval',
        '============================================================',
        '',
        'Generated deterministically from converter/vendor.json, toolchain.json, and converter/licenses/*.txt.',
        f'Target platform: {vendor["target"]} (all codec libraries statically linked into native binaries).',
        '',
        'System-Provided Codecs (Not Bundled)',
        '----------------------------------',
    ]
    for sys_entry in vendor.get('system_codecs', []):
        fmts = ', '.join(sys_entry['formats'])
        lines.append(
            f'- {fmts}: {sys_entry["provider"]} '
            f'(read={sys_entry["modes"]["read"]}, write={sys_entry["modes"]["write"]}, '
            f'bundled={sys_entry["bundled"]}, hevc_bundled={sys_entry["hevc_bundled"]}, '
            f'decision={sys_entry["decision_doc"]})'
        )
    lines.extend([
        '',
        'Converter-Scoped Build Tools (Not Shipped in Converter Binaries)',
        '----------------------------------------------------------------',
    ])
    for tool_name in vendor['build_tools']['required_converter_tools']:
        wc = win_tools[tool_name]
        lc = lin_tools.get(f'{tool_name}-linux')
        lines.append(f'- {tool_name} {wc["version"]} (scope={wc.get("scope", "core")}):')
        lines.append(f'  Windows URL: {wc["url"]}')
        lines.append(f'  Windows SHA-256: {wc["sha256"]} ({wc["bytes"]} bytes)')
        if lc:
            lines.append(f'  Linux URL: {lc["url"]}')
            lines.append(f'  Linux SHA-256: {lc["sha256"]} ({lc["bytes"]} bytes)')
    for omitted, reason in vendor['build_tools'].get('omitted_tools', {}).items():
        lines.append(f'- {omitted} (omitted): {reason}')

    lines.extend([
        '',
        'Bundled Static Libraries (Alphabetical Index)',
        '---------------------------------------------',
    ])
    pkgs_sorted = sorted(vendor['packages'], key=lambda p: p['id'])
    for pkg in pkgs_sorted:
        fmts = ', '.join(pkg['formats']) if pkg['formats'] else 'transitive dependency'
        lines.append(
            f'- {pkg["id"]} {pkg["version"]} ({pkg["license"]}) [{pkg["role"]}: {fmts}]'
        )

    runtimes_sorted = sorted(vendor.get('runtime_libraries', []), key=lambda r: r['id'])
    lines.extend([
        '',
        'Statically Linked Toolchain Runtime Libraries (llvm-mingw)',
        '----------------------------------------------------------',
    ])
    for rt in runtimes_sorted:
        lines.append(
            f'- {rt["id"]} {rt["version"]} ({rt["license"]}) [runtime: {", ".join(rt["libraries"])}]'
        )

    lines.append('')
    for pkg in pkgs_sorted:
        lic_path = ROOT / pkg['license_file']
        if not lic_path.is_file():
            raise FileNotFoundError(f'missing license file {pkg["license_file"]} for {pkg["id"]}')
        lic_text = lic_path.read_text(encoding='utf-8').replace('\r\n', '\n').rstrip() + '\n'
        lic_sha = hashlib.sha256(lic_path.read_bytes()).hexdigest()
        lines.extend([
            '=' * 76,
            f'Package: {pkg["name"]} ({pkg["id"]}) {pkg["version"]}',
            f'Role: {pkg["role"]} | Formats: {", ".join(pkg["formats"]) if pkg["formats"] else "none (transitive)"} | Owner: {pkg["owner"]}',
            f'SPDX License: {pkg["license"]}' + (f' (upstream dual: {pkg["license_expression_upstream"]})' if pkg.get('license_expression_upstream') else ''),
            f'Upstream Repo: {pkg["repo"]}',
            f'Pinned Tag: {pkg.get("tag") or "(untagged commit)"}',
            f'Pinned Commit: {pkg["commit"]}',
            f'Source Archive: {pkg["url"]}',
            f'Archive File: {pkg["archive"]} ({pkg["bytes"]} bytes)',
            f'Archive SHA-256: {pkg["sha256"]}',
        ])
        if pkg.get('patches'):
            for patch in pkg['patches']:
                lines.append(
                    f'Applied Patch: {patch["name"]} | URL: {patch["url"]} | '
                    f'SHA-256: {patch["sha256"]} ({patch["bytes"]} bytes, strip=-p{patch.get("strip", 1)})'
                )
        else:
            lines.append('Applied Patches: none')
        feats = pkg.get('features', {})
        lines.append(
            f'Build Features: simd="{feats.get("simd", "")}"; '
            f'nasm={feats.get("nasm", False)}; threads={feats.get("threads", False)}'
        )
        lines.append(
            'Retrieval & Verification: run python3 tools/bootstrap.py --converter '
            'or powershell -File tools\\bootstrap.ps1 -Converter; '
            f'build via python3 scripts/build-converter.py --target {vendor["target"]}'
        )
        lines.append(f'License Text File: {pkg["license_file"]} (sha256:{lic_sha})')
        lines.append('-' * 76)
        lines.append(lic_text.rstrip())
        lines.append('')

    for rt in runtimes_sorted:
        lic_path = ROOT / rt['license_file']
        if not lic_path.is_file():
            raise FileNotFoundError(f'missing license file {rt["license_file"]} for {rt["id"]}')
        lic_text = lic_path.read_text(encoding='utf-8').replace('\r\n', '\n').rstrip() + '\n'
        lic_sha = hashlib.sha256(lic_path.read_bytes()).hexdigest()
        wc = win_tools[rt['toolchain_component']]
        lc = lin_tools.get(rt.get('toolchain_host_component', ''))
        lines.extend([
            '=' * 76,
            f'Runtime: {rt["name"]} ({rt["id"]}) {rt["version"]}',
            f'Statically Linked Archives: {", ".join(rt["libraries"])}',
            f'SPDX License: {rt["license"]}',
            f'Distribution Repo: {rt["repo"]} (tag {rt["tag"]})',
            f'Upstream Project Repo: {rt["upstream_repo"]}',
            f'Windows Toolchain Archive: {wc["url"]}',
            f'Windows Toolchain SHA-256: {wc["sha256"]} ({wc["bytes"]} bytes)',
        ])
        if lc:
            lines.append(f'Linux Cross-Toolchain Archive: {lc["url"]}')
            lines.append(f'Linux Cross-Toolchain SHA-256: {lc["sha256"]} ({lc["bytes"]} bytes)')
        lines.extend([
            f'Notes: {rt["notes"]}',
            f'License Text File: {rt["license_file"]} (sha256:{lic_sha})',
            '-' * 76,
            lic_text.rstrip(),
            '',
        ])

    notices_text = '\n'.join(lines).rstrip() + '\n'

    # SPDX 2.3 SBOM
    spdx_packages: list[dict] = [
        {
            'SPDXID': 'SPDXRef-Package-rasterfold-converter',
            'name': 'rasterfold-converter',
            'versionInfo': '1.0-dev',
            'supplier': 'Organization: Rizone Technologies',
            'downloadLocation': 'NOASSERTION',
            'filesAnalyzed': False,
            'licenseConcluded': 'MIT',
            'licenseDeclared': 'MIT',
            'comment': 'Windows x64 native lossless image converter static bundle.',
        }
    ]
    relationships: list[dict] = [
        {
            'spdxElementId': 'SPDXRef-DOCUMENT',
            'relationshipType': 'DESCRIBES',
            'relatedSpdxElement': 'SPDXRef-Package-rasterfold-converter',
        }
    ]
    for tool_name in vendor['build_tools']['required_converter_tools']:
        wc = win_tools[tool_name]
        tid = _spdx_id(f'tool-{tool_name}')
        spdx_packages.append({
            'SPDXID': tid,
            'name': tool_name,
            'versionInfo': wc['version'],
            'downloadLocation': wc['url'],
            'filesAnalyzed': False,
            'checksums': [{'algorithm': 'SHA256', 'checksumValue': wc['sha256']}],
            'licenseConcluded': 'NOASSERTION',
            'licenseDeclared': 'NOASSERTION',
            'comment': f'Converter build tool ({wc.get("note", "")}).',
        })
        relationships.append({
            'spdxElementId': tid,
            'relationshipType': 'BUILD_TOOL_OF',
            'relatedSpdxElement': 'SPDXRef-Package-rasterfold-converter',
        })

    for pkg in pkgs_sorted:
        pid = _spdx_id(f'pkg-{pkg["id"]}')
        lic_file = ROOT / pkg['license_file']
        lic_text = lic_file.read_text(encoding='utf-8').replace('\r\n', '\n').rstrip()
        pkg_obj: dict = {
            'SPDXID': pid,
            'name': pkg['id'],
            'versionInfo': pkg['version'],
            'downloadLocation': pkg['url'],
            'filesAnalyzed': False,
            'checksums': [{'algorithm': 'SHA256', 'checksumValue': pkg['sha256']}],
            'licenseConcluded': pkg['license'],
            'licenseDeclared': pkg.get('license_expression_upstream') or pkg['license'],
            'externalRefs': [
                {
                    'referenceCategory': 'OTHER',
                    'referenceType': 'vcs',
                    'referenceLocator': f'{pkg["repo"]}@{pkg["commit"]}',
                }
            ],
            'comment': (
                f'role={pkg["role"]}; owner={pkg["owner"]}; '
                f'license_file={pkg["license_file"]}; '
                f'license_excerpt={lic_text.splitlines()[0] if lic_text else ""}'
            ),
        }
        if pkg.get('patches'):
            pkg_obj['sourceInfo'] = 'Patched with: ' + '; '.join(
                f'{p["name"]} (sha256:{p["sha256"]})' for p in pkg['patches']
            )
        spdx_packages.append(pkg_obj)
        relationships.append({
            'spdxElementId': 'SPDXRef-Package-rasterfold-converter',
            'relationshipType': 'STATIC_LINK',
            'relatedSpdxElement': pid,
        })
        for dep in pkg.get('deps', []):
            relationships.append({
                'spdxElementId': pid,
                'relationshipType': 'DEPENDS_ON',
                'relatedSpdxElement': _spdx_id(f'pkg-{dep}'),
            })

    for rt in runtimes_sorted:
        rid = _spdx_id(f'runtime-{rt["id"]}')
        wc = win_tools[rt['toolchain_component']]
        lc = lin_tools.get(rt.get('toolchain_host_component', ''))
        lic_file = ROOT / rt['license_file']
        lic_text = lic_file.read_text(encoding='utf-8').replace('\r\n', '\n').rstrip()
        rt_obj: dict = {
            'SPDXID': rid,
            'name': rt['id'],
            'versionInfo': rt['version'],
            'downloadLocation': wc['url'],
            'filesAnalyzed': False,
            'checksums': [{'algorithm': 'SHA256', 'checksumValue': wc['sha256']}],
            'licenseConcluded': rt['license'],
            'licenseDeclared': rt['license'],
            'externalRefs': [
                {
                    'referenceCategory': 'OTHER',
                    'referenceType': 'vcs',
                    'referenceLocator': rt['upstream_repo'],
                }
            ],
            'comment': (
                f'runtime_archives={",".join(rt["libraries"])}; '
                f'toolchain={rt["toolchain_component"]}; '
                f'license_file={rt["license_file"]}; '
                f'license_excerpt={lic_text.splitlines()[0] if lic_text else ""}'
            ),
        }
        if lc:
            rt_obj['sourceInfo'] = f'Linux cross-toolchain archive: {lc["url"]} (sha256:{lc["sha256"]})'
        spdx_packages.append(rt_obj)
        relationships.append({
            'spdxElementId': 'SPDXRef-Package-rasterfold-converter',
            'relationshipType': 'STATIC_LINK',
            'relatedSpdxElement': rid,
        })

    sbom_doc = {
        'spdxVersion': 'SPDX-2.3',
        'dataLicense': 'CC0-1.0',
        'SPDXID': 'SPDXRef-DOCUMENT',
        'name': 'RasterFold-Converter-Bundled-Codecs',
        'documentNamespace': 'https://rasterfold.invalid/spdx/converter-bundle-schema1',
        'creationInfo': {
            'created': SPDX_CREATED,
            'creators': ['Tool: RasterFold-build-converter'],
            'licenseListVersion': '3.24',
        },
        'packages': spdx_packages,
        'relationships': relationships,
    }
    sbom_text = json.dumps(sbom_doc, indent=2) + '\n'
    return notices_text, sbom_text


def write_notices() -> None:
    notices_text, sbom_text = render_notices_and_sbom()
    (ROOT / 'converter/THIRD-PARTY-NOTICES.txt').write_text(notices_text, encoding='utf-8', newline='\n')
    (ROOT / 'converter/sbom.spdx.json').write_text(sbom_text, encoding='utf-8', newline='\n')
    print('wrote converter/THIRD-PARTY-NOTICES.txt and converter/sbom.spdx.json', flush=True)


def check_notices() -> int:
    want_notices, want_sbom = render_notices_and_sbom()
    notices_path = ROOT / 'converter/THIRD-PARTY-NOTICES.txt'
    sbom_path = ROOT / 'converter/sbom.spdx.json'
    if not notices_path.is_file():
        print('FAIL missing converter/THIRD-PARTY-NOTICES.txt (run --write-notices)', file=sys.stderr)
        return 1
    if not sbom_path.is_file():
        print('FAIL missing converter/sbom.spdx.json (run --write-notices)', file=sys.stderr)
        return 1
    if notices_path.read_text(encoding='utf-8') != want_notices:
        print('FAIL converter/THIRD-PARTY-NOTICES.txt is out of date (run --write-notices)', file=sys.stderr)
        return 1
    if sbom_path.read_text(encoding='utf-8') != want_sbom:
        print('FAIL converter/sbom.spdx.json is out of date (run --write-notices)', file=sys.stderr)
        return 1
    print('notices-sbom: current', flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', default='windows-x64', choices=['windows-x64', 'all'],
                        help='converter target to build (Windows x64 only)')
    parser.add_argument('--clean', action='store_true', help='remove build/converter/<target> before building')
    parser.add_argument('--write-notices', action='store_true',
                        help='regenerate converter/THIRD-PARTY-NOTICES.txt and converter/sbom.spdx.json')
    parser.add_argument('--check-notices', action='store_true',
                        help='verify converter/THIRD-PARTY-NOTICES.txt and converter/sbom.spdx.json are current')
    args = parser.parse_args()

    if args.check_notices:
        return check_notices()
    if args.write_notices:
        write_notices()
        return 0

    targets = list(TARGETS) if args.target == 'all' else [args.target]
    for tgt in targets:
        build_target(tgt, clean=args.clean)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
