#!/usr/bin/env python3
"""Build CLI, shared library and static library with repository-local tools."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('linux-x64', 'windows-x64', 'android-arm64', 'android-x64')
COMMON = ('platform', 'api', 'crc32', 'bmp', 'zlib', 'png', 'fold', 'cm', 'pal')


def target_artifacts(target: str) -> tuple[str, ...]:
    if target == 'windows-x64':
        return (
            'bin/rasterfold.exe',
            'bin/rasterfold.dll',
            'lib/librasterfold.a',
            'lib/librasterfold.dll.a',
            'lib/rasterfold.lib',
            'symbols/rasterfold.pdb',
            'symbols/rasterfold-dll.pdb',
            'include/rasterfold.h',
        )
    if target == 'linux-x64':
        return (
            'bin/rasterfold',
            'lib/librasterfold.so.1',
            'lib/librasterfold.a',
            'lib/librasterfold.so',
            'include/rasterfold.h',
        )
    if target in ('android-arm64', 'android-x64'):
        return (
            'bin/rasterfold',
            'lib/librasterfold.so',
            'lib/librasterfold.a',
            'include/rasterfold.h',
        )
    raise ValueError(f'unknown target: {target}')


def shell_command(args: list[str]) -> str:
    return subprocess.list2cmdline(args) if os.name == 'nt' else shlex.join(args)


def ninja_path(path: Path) -> str:
    return path.as_posix().replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def base_x64_flags() -> list[str]:
    return ['-march=x86-64-v2', '-mtune=generic']


def target_toolchain(target: str) -> tuple[Path, str]:
    if target.startswith('android-'):
        tools = ROOT / 'tools/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin'
        arch = 'aarch64' if target == 'android-arm64' else 'x86_64'
        api = json.loads((ROOT / 'toolchain.json').read_text())['android']['min_api']
        triple = f'{arch}-linux-android{api}'
    else:
        tools = ROOT / ('tools/llvm-mingw/bin' if os.name == 'nt' else 'tools/llvm-mingw-linux/bin')
        triple = 'x86_64-w64-mingw32' if target == 'windows-x64' else 'x86_64-linux-gnu'
    return tools, triple


def compile_flags(target: str, portable: bool = False) -> list[str]:
    windows = target == 'windows-x64'
    _tools, triple = target_toolchain(target)
    flags = [f'--target={triple}', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-fno-builtin', '-fno-ident', '-fvisibility=hidden']
    if target != 'android-arm64':
        flags += base_x64_flags()
    if portable:
        flags += ['-DRF_PORTABLE_SIMD']
    if windows:
        flags += ['-gcodeview']
    flags += ['-ffreestanding', '-fno-stack-protector'] if windows else ['-fPIC', '-pthread']
    return flags


def lib_sources_for(target: str) -> tuple[str, ...]:
    return COMMON + (('mem', 'sys') if target == 'windows-x64' else ('sys_posix',))


PGO_TRACKED_SOURCES = (
    'include/rasterfold.h',
    'src/cm.c', 'src/pal.c', 'src/fold.c', 'src/bmp.c', 'src/png.c', 'src/zlib.c', 'src/crc32.c',
    'src/api.c', 'src/main.c', 'src/mem.c', 'src/sys.c',
    'src/sys_posix.c', 'src/platform.c', 'src/rasterfold.def',
    'src/rf.h', 'src/rc.h', 'src/simd.h',
)


def pgo_source_hashes() -> dict[str, str]:
    rels = set(PGO_TRACKED_SOURCES)
    for folder in ('include', 'src'):
        for p in sorted((ROOT / folder).glob('*')):
            if p.is_file() and p.suffix in ('.c', '.h', '.def'):
                rels.add(p.relative_to(ROOT).as_posix())
    return {rel: hashlib.sha256((ROOT / rel).read_bytes()).hexdigest() for rel in sorted(rels)}


def write_pgo_exe_provenance(exe: Path, cc: Path) -> Path:
    compiler = subprocess.check_output([str(cc), '--version'], text=True).splitlines()[0]
    meta = {
        'schema': 1,
        'compiler': compiler,
        'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'sources': pgo_source_hashes(),
    }
    meta_path = Path(str(exe.resolve()) + '.json')
    partial = Path(str(meta_path) + '.partial')
    partial.unlink(missing_ok=True)
    partial.write_text(json.dumps(meta, indent=2) + '\n', encoding='utf-8')
    partial.replace(meta_path)
    return meta_path


def validate_pgo_exe_provenance(exe: Path, cc: Path) -> dict[str, object]:
    resolved = exe.resolve()
    if not resolved.is_file() or resolved.stat().st_size == 0:
        raise FileNotFoundError(f'missing PGO executable: {exe}')
    meta_path = Path(str(resolved) + '.json')
    if not meta_path.is_file():
        raise ValueError(f'stale or mismatched PGO executable provenance: missing {meta_path}')
    try:
        meta = json.loads(meta_path.read_text(encoding='utf-8'))
    except Exception as exc:
        raise ValueError(f'stale or mismatched PGO executable provenance in {meta_path}: {exc}') from exc
    actual_sha = hashlib.sha256(resolved.read_bytes()).hexdigest()
    compiler = subprocess.check_output([str(cc), '--version'], text=True).splitlines()[0]
    if meta.get('schema') != 1 or meta.get('exe_sha256') != actual_sha or meta.get('compiler') != compiler:
        raise ValueError(f'stale or mismatched PGO executable provenance in {meta_path}: compiler or binary digest mismatch')
    sources = meta.get('sources')
    if not isinstance(sources, dict):
        raise ValueError(f'stale or mismatched PGO executable provenance in {meta_path}: missing sources table')
    expected_hashes = pgo_source_hashes()
    if set(sources.keys()) != set(expected_hashes.keys()):
        raise ValueError(f'stale or mismatched PGO executable provenance in {meta_path}: tracked source set mismatch; rebuild pgo-gen')
    for rel, current in expected_hashes.items():
        if sources.get(rel) != current:
            raise ValueError(f'stale or mismatched PGO executable provenance for {rel} in {meta_path}; rebuild pgo-gen')
    return meta


def is_consistent_profile_pair(
    prof_path: Path,
    meta_path: Path,
    compiler: str,
    expected_hashes: dict[str, str],
    profdata_tool: Path | None = None,
) -> bool:
    if not prof_path.is_file() or prof_path.stat().st_size == 0 or not meta_path.is_file():
        return False
    try:
        meta = json.loads(meta_path.read_text(encoding='utf-8'))
        actual_sha = hashlib.sha256(prof_path.read_bytes()).hexdigest()
    except Exception:
        return False
    if meta.get('schema') != 1 or meta.get('profdata_sha256') != actual_sha or meta.get('compiler') != compiler:
        return False
    sources = meta.get('sources')
    if not isinstance(sources, dict) or set(sources.keys()) != set(expected_hashes.keys()):
        return False
    if any(sources.get(rel) != current for rel, current in expected_hashes.items()):
        return False
    if profdata_tool is not None:
        proc = subprocess.run([str(profdata_tool), 'show', str(prof_path)], capture_output=True, text=True)
        if proc.returncode != 0:
            return False
    return True


def restore_profile_backup_if_needed(
    resolved: Path,
    meta_path: Path,
    compiler: str,
    expected_hashes: dict[str, str],
    profdata_tool: Path,
) -> None:
    bak_prof = Path(str(resolved) + '.bak')
    bak_meta = Path(str(meta_path) + '.bak')
    for tmp in (Path(str(bak_prof) + '.tmp'), Path(str(bak_meta) + '.tmp')):
        tmp.unlink(missing_ok=True)
    if not (bak_prof.is_file() or bak_meta.is_file()):
        return
    if is_consistent_profile_pair(resolved, meta_path, compiler, expected_hashes, profdata_tool):
        bak_prof.unlink(missing_ok=True)
        bak_meta.unlink(missing_ok=True)
        return
    if is_consistent_profile_pair(bak_prof, bak_meta, compiler, expected_hashes, profdata_tool):
        res_prof = Path(str(resolved) + '.restore')
        res_meta = Path(str(meta_path) + '.restore')
        shutil.copy2(bak_prof, res_prof)
        shutil.copy2(bak_meta, res_meta)
        res_prof.replace(resolved)
        res_meta.replace(meta_path)
    bak_prof.unlink(missing_ok=True)
    bak_meta.unlink(missing_ok=True)


def validate_pgo_profile(profile: Path, tools: Path, suffix: str) -> Path:
    resolved = profile.resolve()
    meta_path = Path(str(resolved) + '.json')
    profdata = tools / ('llvm-profdata' + suffix)
    cc = tools / ('clang' + suffix)
    if not profdata.is_file() or not cc.is_file():
        raise ValueError(f'missing {profdata}; run tools/bootstrap.py')
    compiler = subprocess.check_output([str(cc), '--version'], text=True).splitlines()[0]
    expected_hashes = pgo_source_hashes()
    restore_profile_backup_if_needed(resolved, meta_path, compiler, expected_hashes, profdata)
    if not resolved.is_file() or resolved.stat().st_size == 0:
        raise ValueError(f'missing or empty PGO profile: {profile}')
    proc = subprocess.run([str(profdata), 'show', str(resolved)], capture_output=True, text=True)
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or 'invalid profile').strip().splitlines()[0]
        raise ValueError(f'corrupt or incompatible PGO profile {profile}: {detail}')
    if not meta_path.is_file():
        raise ValueError(f'stale or mismatched PGO profile provenance: missing {meta_path}')
    try:
        meta = json.loads(meta_path.read_text(encoding='utf-8'))
    except Exception as exc:
        raise ValueError(f'stale or mismatched PGO profile provenance in {meta_path}: {exc}') from exc
    actual_sha = hashlib.sha256(resolved.read_bytes()).hexdigest()
    if meta.get('schema') != 1 or meta.get('profdata_sha256') != actual_sha or meta.get('compiler') != compiler:
        raise ValueError(f'stale or mismatched PGO profile provenance in {meta_path}: compiler or profile digest mismatch')
    sources = meta.get('sources')
    if not isinstance(sources, dict):
        raise ValueError(f'stale or mismatched PGO profile provenance in {meta_path}: missing sources table')
    if set(sources.keys()) != set(expected_hashes.keys()):
        raise ValueError(f'stale or mismatched PGO profile provenance in {meta_path}: tracked source set mismatch; regenerate with scripts/pgo.ps1')
    for rel, current in expected_hashes.items():
        if sources.get(rel) != current:
            raise ValueError(f'stale or mismatched PGO profile provenance for {rel} in {meta_path}; regenerate with scripts/pgo.ps1')
    return resolved


def build_pgo_gen(jobs: int) -> None:
    check_product_header()
    suffix = '.exe' if os.name == 'nt' else ''
    tools = ROOT / ('tools/llvm-mingw/bin' if os.name == 'nt' else 'tools/llvm-mingw-linux/bin')
    triple = 'x86_64-w64-mingw32'
    cc = tools / ('clang' + suffix)
    ninja = ROOT / ('tools/ninja/ninja.exe' if os.name == 'nt' else 'tools/ninja-linux/ninja')
    profile_rt = tools.parent / 'lib/clang/23/lib/windows/libclang_rt.profile-x86_64.a'
    for path in (cc, ninja, profile_rt):
        if not path.is_file():
            raise ValueError(f'missing {path}; run tools/bootstrap.py (Windows: tools/bootstrap.ps1)')
    name = 'windows-x64-pgo'
    work, runtime = ROOT / 'build' / name, ROOT / 'bin' / name / 'bin'
    work.mkdir(parents=True, exist_ok=True)
    runtime.mkdir(parents=True, exist_ok=True)
    flags = [f'--target={triple}', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-fno-ident', '-fvisibility=hidden', *base_x64_flags(),
             '-gcodeview', '-fprofile-instr-generate', '-mllvm', '-enable-name-compression=false', '-DRF_PGO_GEN']
    sources = (*COMMON, 'sys', 'main')
    objects = [work / (s + '.o') for s in sources]
    exe = runtime / 'rasterfold.exe'
    lines = ['# Generated by scripts/build.py (pgo-gen); no source of truth lives here.', '']
    for source_name, obj in zip(sources, objects):
        source = ROOT / 'src' / (source_name + '.c')
        rule = 'cc_' + source_name
        args = [str(cc), *flags, '-MMD', '-MF', str(obj) + '.d', '-c', str(source), '-o', str(obj)]
        lines += [f'rule {rule}', '  command = ' + shell_command(args).replace('$', '$$'),
                  f'  depfile = {ninja_path(obj)}.d', '  deps = gcc', f'  description = CC {source_name} (pgo-gen)',
                  f'build {ninja_path(obj)}: {rule} {ninja_path(source)}', '']
    exe_args = [str(cc), f'--target={triple}', '-fuse-ld=lld', *map(str, objects),
                str(profile_rt), '-lkernel32', '-lshell32', '-o', str(exe)]
    lines += ['rule exe', '  command = ' + shell_command(exe_args).replace('$', '$$'),
              f'  description = LINK {exe.name} (pgo-gen)',
              f'build {ninja_path(exe)}: exe ' + ' '.join(map(ninja_path, objects)), '',
              f'default {ninja_path(exe)}']
    manifest = work / 'build.ninja'
    text = '\n'.join(lines) + '\n'
    if not manifest.exists() or manifest.read_text() != text:
        manifest.write_text(text)
    subprocess.run([str(ninja), '-f', str(manifest), '-j', str(jobs)], cwd=ROOT, check=True)
    write_pgo_exe_provenance(exe, cc)
    print(f'built {name}: {exe.relative_to(ROOT).as_posix()}', flush=True)


def check_product_header() -> None:
    import importlib.util as _ilu
    _spec = _ilu.spec_from_file_location('rf_version_tool', Path(__file__).resolve().parent / 'version.py')
    _vt = _ilu.module_from_spec(_spec); _spec.loader.exec_module(_vt)
    problems = _vt.check(ROOT)
    if problems:
        raise SystemExit('build: ' + '; '.join(problems))


def build(target: str, jobs: int, portable: bool, pgo_profile: Path | None = None, lto: bool = False) -> None:
    check_product_header()
    windows = target == 'windows-x64'
    android = target.startswith('android-')
    suffix = '.exe' if os.name == 'nt' else ''
    if os.name == 'nt' and not windows:
        raise ValueError('Linux and Android builds run in Linux/WSL; Windows hosts build windows-x64')
    tools, triple = target_toolchain(target)
    cc, ar = tools / ('clang' + suffix), tools / ('llvm-ar' + suffix)
    ninja = ROOT / ('tools/ninja/ninja.exe' if os.name == 'nt' else 'tools/ninja-linux/ninja')
    for path in (cc, ar, ninja):
        if not path.is_file():
            raise ValueError(f'missing {path}; run tools/bootstrap.py (Windows: tools/bootstrap.ps1)')
    name = target + ('-portable' if portable else '')
    work, out = ROOT / 'build' / name, ROOT / 'bin' / name
    runtime, libraries, headers, symbols = (out / part for part in ('bin', 'lib', 'include', 'symbols'))
    for directory in (work, *( [work / 'lto'] if lto else [] ), runtime, libraries, headers, *([symbols] if windows else [])):
        directory.mkdir(parents=True, exist_ok=True)
    flags = compile_flags(target, portable)
    validated_profile: Path | None = None
    if pgo_profile is not None:
        pgo_tools = ROOT / ('tools/llvm-mingw/bin' if os.name == 'nt' else 'tools/llvm-mingw-linux/bin')
        validated_profile = validate_pgo_profile(pgo_profile, pgo_tools, suffix)
        if windows and not portable:
            flags += [f'-fprofile-instr-use={validated_profile}', '-Wno-profile-instr-unprofiled']
        else:
            validated_profile = None
    link = [str(cc), f'--target={triple}', '-fuse-ld=lld', *(['-flto'] if lto else [])]
    if windows:
        link += ['-nostdlib']
    else:
        link += ['-pthread', '-Wl,-z,relro,-z,now']
    if android:
        link += ['-Wl,-z,max-page-size=16384']
    lib_sources = lib_sources_for(target)
    objects = [work / (name + '.o') for name in lib_sources]
    # Hosted static-library callers supply their CRT memory functions. Keep the
    # freestanding definitions only in our executable and the export-limited DLL.
    archive_objects = [p for p in objects if p.name != 'mem.o'] if windows else objects
    linked_objects = [work / 'lto' / (name + '.o') for name in lib_sources] if lto else objects
    main = (work / 'lto' / 'main.o') if lto else (work / 'main.o')
    exe = runtime / ('rasterfold.exe' if windows else 'rasterfold')
    shared = (runtime if windows else libraries) / ('rasterfold.dll' if windows else 'librasterfold.so' if android else 'librasterfold.so.1')
    archive = libraries / 'librasterfold.a'
    imports = libraries / 'librasterfold.dll.a'
    exe_pdb, dll_pdb = symbols / 'rasterfold.pdb', symbols / 'rasterfold-dll.pdb'
    lines = ['# Generated by scripts/build.py; no source of truth lives here.', '']
    compdb = []
    compile_units: list[tuple[str, Path, list[str], str]] = [
        (source_name, obj, flags, '')
        for source_name, obj in zip(lib_sources, objects)
    ]
    if lto:
        lto_flags = [*flags, '-flto']
        compile_units += [
            (source_name, obj, lto_flags, '_lto')
            for source_name, obj in [*zip(lib_sources, linked_objects), ('main', main)]
        ]
    else:
        compile_units.append(('main', main, flags, ''))
    for source_name, obj, unit_flags, rule_suffix in compile_units:
        source = ROOT / 'src' / (source_name + '.c')
        rule = 'cc_' + source_name + rule_suffix
        args = [str(cc), *unit_flags, '-MMD', '-MF', str(obj) + '.d', '-c', str(source), '-o', str(obj)]
        if not rule_suffix or source_name == 'main':
            compdb.append({
                'directory': str(ROOT),
                'file': str(source),
                'output': str(obj),
                'arguments': [str(cc), *unit_flags, '-c', str(source), '-o', str(obj)],
            })
        obj_implicit = f' | {ninja_path(validated_profile)}' if validated_profile is not None else ''
        lines += [f'rule {rule}', '  command = ' + shell_command(args).replace('$', '$$'),
                  f'  depfile = {ninja_path(obj)}.d', '  deps = gcc', f'  description = CC {source_name}{rule_suffix}',
                  f'build {ninja_path(obj)}: {rule} {ninja_path(source)}{obj_implicit}', '']
    (work / 'compile_commands.json').write_text(json.dumps(compdb, indent=2) + '\n')
    shared_args = [*link, '-shared', *map(str, linked_objects), '-o', str(shared)]
    exe_args = [*link, *map(str, [*linked_objects, main]), '-o', str(exe)]
    if windows:
        shared_args = [str(tools / ('ld.lld' + suffix)), '-flavor', 'link', '/dll', '/noentry',
                       '/def:' + str(ROOT / 'src/rasterfold.def'), '/out:' + str(shared),
                       '/implib:' + str(imports), '/debug:full', '/pdb:' + str(dll_pdb), *map(str, linked_objects),
                       str(tools.parent / 'x86_64-w64-mingw32/lib/libkernel32.a')]
        exe_args += ['-Wl,--entry=rf_start', '-Wl,--subsystem,console', '-Wl,--pdb=' + str(exe_pdb), '-lkernel32', '-lshell32']
    else:
        shared_args += ['-Wl,-soname,' + shared.name]
    extra_dep = ' ' + ninja_path(ROOT / 'src/rasterfold.def') if windows else ''
    for rule, output, args, deps in (
        ('shared', shared, shared_args, linked_objects),
        ('archive', archive, [sys.executable, str(ROOT / 'scripts/build.py'), '--archive', str(ar), str(archive), *map(str, archive_objects)], archive_objects),
        ('exe', exe, exe_args, [*linked_objects, main]),
    ):
        implicit = [imports, dll_pdb] if windows and rule == 'shared' else [exe_pdb] if windows and rule == 'exe' else []
        lines += [f'rule {rule}', '  command = ' + shell_command(args).replace('$', '$$'),
                  f'  description = LINK {output.name}',
                  f'build {ninja_path(output)}' + (' | ' + ' '.join(map(ninja_path, implicit)) if implicit else '') + f': {rule} ' + ' '.join(map(ninja_path, deps)) + (extra_dep if rule == 'shared' else ''), '']
    lines += ['default ' + ' '.join(map(ninja_path, [exe, shared, archive]))]
    manifest = work / 'build.ninja'
    text = '\n'.join(lines) + '\n'
    if not manifest.exists() or manifest.read_text() != text:
        manifest.write_text(text)
    subprocess.run([str(ninja), '-f', str(manifest), '-j', str(jobs)], cwd=ROOT, check=True)
    artifacts = [exe, shared, archive]
    if not windows and not android:
        alias = libraries / 'librasterfold.so'
        if alias.is_symlink() or alias.exists():
            alias.unlink()
        alias.symlink_to(shared.name)
        artifacts.append(alias)
    if windows:
        msvc = libraries / 'rasterfold.lib'
        subprocess.run([str(tools / ('llvm-dlltool' + suffix)), '-m', 'i386:x86-64', '-d',
                        str(ROOT / 'src/rasterfold.def'), '-l', str(msvc)], check=True)
        artifacts += [imports, msvc, exe_pdb, dll_pdb]
    header = headers / 'rasterfold.h'
    shutil.copy2(ROOT / 'include/rasterfold.h', header)
    artifacts.append(header)
    emitted = tuple(p.relative_to(out).as_posix() for p in artifacts)
    if emitted != target_artifacts(target):
        raise ValueError(f'emitted artifacts {emitted} do not match target_artifacts({target!r})')
    compiler = subprocess.check_output([str(cc), '--version'], text=True).splitlines()[0]
    import importlib.util as _ilu
    _spec = _ilu.spec_from_file_location('rf_version_tool', Path(__file__).resolve().parent / 'version.py')
    _vt = _ilu.module_from_spec(_spec); _spec.loader.exec_module(_vt)
    _revision, _dirty = _vt.git_state(ROOT)
    (out / 'build.json').write_text(json.dumps({
        'schema': 2, 'target': target, 'triple': triple, 'portable_simd': portable, 'compiler': compiler,
        'format_version': 3, 'api_version': 1,
        'product_version': _vt.read_version(ROOT), 'revision': _revision, 'dirty': _dirty,
        'artifacts': {p.relative_to(out).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts},
    }, indent=2) + '\n')
    print(f'built {name}: CLI, shared and static libraries ({compiler})', flush=True)
    import importlib.util as _ilu2
    _spec2 = _ilu2.spec_from_file_location('rf_gen_compdb', Path(__file__).resolve().parent / 'gen-compdb.py')
    _gc = _ilu2.module_from_spec(_spec2); _spec2.loader.exec_module(_gc)
    _gc.write_root()


def main() -> int:
    if len(sys.argv) > 4 and sys.argv[1] == '--archive':
        # ar replaces matching members but retains obsolete ones. Always
        # assemble a fresh archive, then publish it after ar succeeds.
        ar, dest, *objects = sys.argv[2:]
        temp = Path(dest + '.partial')
        temp.unlink(missing_ok=True)
        subprocess.run([ar, 'rcs', str(temp), *objects], check=True)
        temp.replace(dest)
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', action='append', choices=(*TARGETS, 'all', 'pgo-gen'))
    parser.add_argument('--jobs', '-j', type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument('--portable', action='store_true', help='exercise the ARM arithmetic path on x64')
    parser.add_argument('--pgo-gen', action='store_true', help='build bin/windows-x64-pgo/bin/rasterfold.exe with -fprofile-instr-generate')
    parser.add_argument('--pgo-use', nargs='?', const='build/pgo/rasterfold.profdata', default=os.environ.get('RF_PGO_PROFILE'),
                        help='compile with -fprofile-instr-use=<profdata>')
    parser.add_argument('--lto', action='store_true', help='compile and link CLI and shared libraries with -flto')
    args = parser.parse_args()
    try:
        if args.jobs < 1:
            raise ValueError('jobs must be positive')
        if args.pgo_gen or (args.target and 'pgo-gen' in args.target):
            build_pgo_gen(args.jobs)
            remaining = [t for t in (args.target or []) if t != 'pgo-gen']
            if not remaining:
                return 0
            args.target = remaining
        targets = args.target or ['windows-x64' if os.name == 'nt' else 'linux-x64']
        if 'all' in targets:
            targets = list(TARGETS)
        pgo_path = Path(args.pgo_use) if args.pgo_use else None
        for target in dict.fromkeys(targets):
            build(target, args.jobs, args.portable, pgo_profile=pgo_path, lto=args.lto)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f'build: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
