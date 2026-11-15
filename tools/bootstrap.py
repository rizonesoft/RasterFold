#!/usr/bin/env python3
"""Install the pinned Linux-host toolchain inside this checkout. Stdlib only."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import platform
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_MOVING_REFS = {
    'main', 'master', 'head', 'latest', 'trunk', 'dev', 'develop', 'nightly', 'edge',
}
HUNK_RE = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(1024 * 1024):
            h.update(block)
    return h.hexdigest()


def _validate_symlink_in_tree(root_res: Path, link_path: Path, rel_posix: str, target: str) -> str:
    if not target or '\\' in target:
        raise ValueError(f'vendor source symlink escapes root: {rel_posix} -> {target!r}')
    target_norm = target.replace('\\', '/')
    parts = PurePosixPath(target_norm).parts
    if target_norm.startswith('/') or (parts and ':' in parts[0]):
        raise ValueError(f'vendor source symlink escapes root: {rel_posix} -> {target!r}')
    parent_rel = PurePosixPath(rel_posix).parent
    joined = (parent_rel / target_norm).as_posix() if parent_rel != PurePosixPath('.') else target_norm
    norm = posixpath.normpath(joined)
    if norm == '..' or norm.startswith('../'):
        raise ValueError(f'vendor source symlink escapes root: {rel_posix} -> {target!r}')
    if not (link_path.parent / target).resolve(strict=False).is_relative_to(root_res):
        raise ValueError(f'vendor source symlink escapes root: {rel_posix} -> {target!r}')
    return target_norm


def tree_digest(root: Path) -> str:
    if root.is_symlink():
        raise ValueError(f'vendor source symlink escapes root: {root}')
    h = hashlib.sha256()
    root_res = root.resolve()
    entries: list[tuple[str, bytes]] = []
    for dirpath, dirnames, filenames in os.walk(root_res, topdown=True, followlinks=False):
        cur = Path(dirpath)
        kept_dirs: list[str] = []
        for d in dirnames:
            p = cur / d
            rel = p.relative_to(root_res).as_posix()
            if p.is_symlink():
                if rel == 'PINNED.json':
                    raise ValueError('vendor source PINNED.json must not be a symlink')
                t_norm = _validate_symlink_in_tree(root_res, p, rel, os.readlink(p))
                entries.append((rel, f'L\0{rel}\0{t_norm}\n'.encode('utf-8')))
            else:
                kept_dirs.append(d)
        dirnames[:] = kept_dirs
        for fname in filenames:
            p = cur / fname
            rel = p.relative_to(root_res).as_posix()
            if rel == 'PINNED.json':
                if p.is_symlink():
                    raise ValueError('vendor source PINNED.json must not be a symlink')
                continue
            if p.is_symlink():
                t_norm = _validate_symlink_in_tree(root_res, p, rel, os.readlink(p))
                entries.append((rel, f'L\0{rel}\0{t_norm}\n'.encode('utf-8')))
            elif p.is_file():
                entries.append((rel, f'F\0{rel}\0{p.stat().st_size}\0{digest(p)}\n'.encode('utf-8')))
    entries.sort(key=lambda item: item[0])
    for _rel, rec in entries:
        h.update(rec)
    return h.hexdigest()


def _ensure_safe_member_name(name: str) -> None:
    if not name or name.startswith(('/', '\\')) or '\\' in name:
        raise ValueError(f'archive member escapes extraction root: {name}')
    parts = PurePosixPath(name).parts
    if '..' in parts or (parts and ':' in parts[0]):
        raise ValueError(f'archive member escapes extraction root: {name}')


def _safe_extract(archive: Path, stage: Path) -> None:
    stage_res = stage.resolve()
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            for entry in z.infolist():
                _ensure_safe_member_name(entry.filename)
                path = stage / entry.filename
                if not path.resolve().is_relative_to(stage_res):
                    raise ValueError('archive member escapes extraction root')
                mode = entry.external_attr >> 16
                if stat.S_ISLNK(mode):
                    target = z.read(entry).decode('utf-8')
                    if Path(target).is_absolute() or not (path.parent / target).resolve().is_relative_to(stage_res):
                        raise ValueError('archive symlink escapes extraction root')
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.symlink_to(target)
                    continue
                z.extract(entry, stage)
                if mode & 0o111:
                    path.chmod(path.stat().st_mode | 0o111)
        return
    try:
        with tarfile.open(archive) as t:
            for member in t.getmembers():
                _ensure_safe_member_name(member.name)
                dest = stage / member.name
                if not dest.resolve().is_relative_to(stage_res):
                    raise ValueError('archive member escapes extraction root')
                if member.issym() or member.islnk():
                    link_base = stage_res if member.islnk() else dest.parent
                    if Path(member.linkname).is_absolute() or not (link_base / member.linkname).resolve().is_relative_to(stage_res):
                        raise ValueError('archive symlink escapes extraction root')
            t.extractall(stage, filter='data')
    except (tarfile.TarError, OSError) as exc:
        raise ValueError(f'archive member escapes extraction root: {exc}') from exc


def _fetch_verified_file(entry: dict, cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    archive_name = entry.get('archive') or entry['url'].rsplit('/', 1)[-1]
    if not archive_name or '/' in archive_name or '\\' in archive_name or '..' in archive_name:
        raise ValueError(f'invalid archive name: {archive_name!r}')
    archive = cache / archive_name
    if not archive.exists():
        partial = archive.with_name(archive.name + '.partial')
        with urllib.request.urlopen(entry['url'], timeout=90) as response, partial.open('wb') as out:
            shutil.copyfileobj(response, out, 1024 * 1024)
        partial.replace(archive)
    if archive.stat().st_size != entry['bytes'] or digest(archive) != entry['sha256']:
        raise ValueError(f'archive checksum or size mismatch: {archive}')
    return archive


def _safe_target_in_root(root: Path, rel_path: str) -> Path:
    if not rel_path or rel_path.startswith(('/', '\\')) or '\\' in rel_path:
        raise ValueError(f'patch path escapes source root: {rel_path!r}')
    parts = PurePosixPath(rel_path).parts
    if '..' in parts or (parts and ':' in parts[0]):
        raise ValueError(f'patch path escapes source root: {rel_path!r}')
    root_res = root.resolve()
    candidate = root_res / rel_path
    if not candidate.resolve().is_relative_to(root_res):
        raise ValueError(f'patch path escapes source root: {rel_path!r}')
    cur = root_res
    for part in parts:
        cur = cur / part
        if cur.is_symlink():
            raise ValueError(f'patch refuses symlink target: {rel_path!r}')
    return candidate


def apply_unified_patch(root: Path, patch_text: str, strip: int = 1) -> None:
    lines = patch_text.splitlines(keepends=True)
    i = 0
    applied = 0
    while i < len(lines):
        line = lines[i]
        if not line.startswith('--- '):
            i += 1
            continue
        old_raw = line[4:].split('\t', 1)[0].rstrip('\r\n')
        if i + 1 >= len(lines) or not lines[i + 1].startswith('+++ '):
            raise ValueError(f'malformed patch header near line {i + 1}')
        new_raw = lines[i + 1][4:].split('\t', 1)[0].rstrip('\r\n')
        i += 2
        if new_raw == '/dev/null':
            raise ValueError('patch file deletion unsupported')
        parts = PurePosixPath(new_raw).parts
        if len(parts) <= strip:
            raise ValueError(f'patch path shorter than strip={strip}: {new_raw}')
        rel = '/'.join(parts[strip:])
        target = _safe_target_in_root(root, rel)
        if old_raw == '/dev/null':
            src_lines: list[str] = []
        else:
            if not target.is_file():
                raise ValueError(f'patch target missing: {rel}')
            src_lines = target.read_text(encoding='utf-8').splitlines(keepends=True)
        out_lines: list[str] = []
        cursor = 0
        while i < len(lines) and lines[i].startswith('@@ '):
            m = HUNK_RE.match(lines[i])
            if not m:
                raise ValueError(f'malformed patch hunk near line {i + 1}: {lines[i]!r}')
            old_start = int(m.group(1))
            old_len = int(m.group(2)) if m.group(2) is not None else 1
            i += 1
            target_idx = 0 if (old_start == 0 and old_len == 0) else old_start - 1
            if target_idx < cursor or target_idx > len(src_lines):
                raise ValueError(f'patch hunk out of bounds in {rel} at line {old_start}')
            out_lines.extend(src_lines[cursor:target_idx])
            cursor = target_idx
            seen_old = 0
            while i < len(lines) and not lines[i].startswith(('@@ ', '--- ', 'diff ')):
                hline = lines[i]
                if hline.startswith(' '):
                    if cursor >= len(src_lines) or src_lines[cursor].rstrip('\r\n') != hline[1:].rstrip('\r\n'):
                        raise ValueError(f'patch context mismatch in {rel} at source line {cursor + 1}')
                    out_lines.append(src_lines[cursor])
                    cursor += 1
                    seen_old += 1
                elif hline.startswith('-'):
                    if cursor >= len(src_lines) or src_lines[cursor].rstrip('\r\n') != hline[1:].rstrip('\r\n'):
                        raise ValueError(f'patch deletion mismatch in {rel} at source line {cursor + 1}')
                    cursor += 1
                    seen_old += 1
                elif hline.startswith('+'):
                    out_lines.append(hline[1:])
                elif hline.startswith('\\'):
                    pass
                else:
                    raise ValueError(f'unexpected patch line in {rel}: {hline!r}')
                i += 1
            if seen_old != old_len:
                raise ValueError(f'patch old line count mismatch in {rel}: expected {old_len}, got {seen_old}')
        out_lines.extend(src_lines[cursor:])
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(''.join(out_lines), encoding='utf-8', newline='\n')
        applied += 1
    if applied == 0:
        raise ValueError('patch contained zero file hunks')


def _is_moving_ref(value: str) -> bool:
    low = value.strip().lower()
    if low in FORBIDDEN_MOVING_REFS:
        return True
    if low.startswith('refs/heads/') or '/heads/' in low:
        return True
    for marker in ('/archive/main.', '/archive/master.', '/archive/head.',
                   '/tarball/main', '/tarball/master', '/zipball/main', '/zipball/master'):
        if marker in low:
            return True
    return False


def validate_vendor_package(pkg: dict) -> None:
    if not isinstance(pkg, dict):
        raise ValueError('vendor package entry must be an object')
    for key in ('id', 'version', 'repo', 'url', 'archive', 'sha256', 'license', 'license_file'):
        if not isinstance(pkg.get(key), str) or not pkg[key].strip():
            raise ValueError(f'vendor package missing {key}: {pkg.get("id")!r}')
    if not pkg.get('bundled', False):
        raise ValueError(f'vendor package {pkg["id"]} cannot be marked unbundled inside packages')
    if pkg.get('branch'):
        raise ValueError(f'vendor package {pkg["id"]} forbids moving branch ref: {pkg["branch"]!r}')
    commit = pkg.get('commit')
    if not isinstance(commit, str) or not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError(f'vendor package {pkg["id"]} requires a 40-char lowercase commit hex: {commit!r}')
    tag = pkg.get('tag')
    if tag is not None:
        if not isinstance(tag, str) or not tag.strip() or _is_moving_ref(tag):
            raise ValueError(f'vendor package {pkg["id"]} refuses moving tag/branch ref: {tag!r}')
    if _is_moving_ref(pkg['url']) or _is_moving_ref(pkg['repo']):
        raise ValueError(f'vendor package {pkg["id"]} URL uses a moving branch reference: {pkg["url"]}')
    if not isinstance(pkg.get('bytes'), int) or pkg['bytes'] <= 0:
        raise ValueError(f'vendor package {pkg["id"]} invalid byte size')
    if not re.fullmatch(r'[0-9a-f]{64}', pkg['sha256']):
        raise ValueError(f'vendor package {pkg["id"]} invalid sha256')
    for patch in pkg.get('patches', []):
        for key in ('name', 'url', 'archive', 'sha256'):
            if not isinstance(patch.get(key), str) or not patch[key].strip():
                raise ValueError(f'vendor package {pkg["id"]} patch missing {key}')
        if not re.fullmatch(r'[0-9a-f]{64}', patch['sha256']):
            raise ValueError(f'vendor package {pkg["id"]} patch invalid sha256')
        if not isinstance(patch.get('bytes'), int) or patch['bytes'] <= 0:
            raise ValueError(f'vendor package {pkg["id"]} patch invalid bytes')
        if not isinstance(patch.get('strip', 1), int) or patch.get('strip', 1) < 0:
            raise ValueError(f'vendor package {pkg["id"]} patch invalid strip count')
        if _is_moving_ref(patch['url']):
            raise ValueError(f'vendor package {pkg["id"]} patch URL uses moving ref: {patch["url"]}')


def validate_vendor_manifest(manifest: dict) -> None:
    if not isinstance(manifest, dict) or manifest.get('schema') != 1:
        raise ValueError('converter/vendor.json schema must be 1')
    packages = manifest.get('packages')
    if not isinstance(packages, list) or not packages:
        raise ValueError('converter/vendor.json packages must be non-empty')
    seen_ids: set[str] = set()
    for pkg in packages:
        validate_vendor_package(pkg)
        if pkg['id'] in seen_ids:
            raise ValueError(f'duplicate vendor package id: {pkg["id"]}')
        for dep in pkg.get('deps', []):
            if dep not in seen_ids:
                raise ValueError(f'vendor package {pkg["id"]} dep {dep!r} must appear earlier in topological order')
        seen_ids.add(pkg['id'])
    for sys_entry in manifest.get('system_codecs', []):
        if sys_entry.get('bundled') or sys_entry.get('hevc_bundled') or sys_entry.get('hevc_source_or_binary_present'):
            raise ValueError('system codecs must not bundle HEVC source or binaries')
    runtimes = manifest.get('runtime_libraries')
    if not isinstance(runtimes, list) or not runtimes:
        raise ValueError('converter/vendor.json runtime_libraries must list statically linked toolchain runtimes')
    rt_ids: set[str] = set()
    for rt in runtimes:
        for key in ('id', 'name', 'version', 'toolchain_component', 'repo', 'upstream_repo', 'tag', 'license', 'license_file', 'notes'):
            if not isinstance(rt.get(key), str) or not rt[key].strip():
                raise ValueError(f'runtime library entry missing {key}: {rt.get("id")!r}')
        if _is_moving_ref(rt['tag']) or _is_moving_ref(rt['repo']) or _is_moving_ref(rt['upstream_repo']):
            raise ValueError(f'runtime library {rt["id"]} forbids moving branch ref')
        if not isinstance(rt.get('libraries'), list) or not rt['libraries']:
            raise ValueError(f'runtime library {rt["id"]} must name statically linked archives')
        if rt['id'] in rt_ids:
            raise ValueError(f'duplicate runtime library id: {rt["id"]}')
        rt_ids.add(rt['id'])
    missing_rt = {'llvm-runtime', 'mingw-w64-runtime', 'winpthreads'} - rt_ids
    if missing_rt:
        raise ValueError(f'converter/vendor.json runtime_libraries missing required entries: {sorted(missing_rt)}')


def verify(component: dict, tools: Path) -> None:
    dest = tools / component['dir']
    receipt = json.loads((dest / 'PINNED.json').read_text())
    if receipt['sha256'] != component['sha256'] or receipt['url'] != component['url']:
        raise ValueError('installation receipt disagrees with manifest')
    if set(receipt['probes']) != set(component.get('probes', [component['probe']])):
        raise ValueError('installation receipt omits or adds verification probes')
    for name, expected in receipt['probes'].items():
        if digest(dest / name) != expected:
            raise ValueError(f'installed probe changed: {name}')
    out = subprocess.check_output([str(dest / component['probe']), *component['versionArgs']],
                                  stderr=subprocess.STDOUT, text=True, timeout=30)
    if component.get('versionMatch'):
        m = re.search(component['versionMatch'], out, re.MULTILINE)
        if not m or m.group(1) != component['expectVersion']:
            raise ValueError('installed compiler/tool reports the wrong version')
    elif component['expectVersion'] not in out:
        raise ValueError('installed compiler/tool reports the wrong version')
    if component.get('versionFile') and component['versionText'] not in (dest / component['versionFile']).read_text():
        raise ValueError('SDK revision disagrees with manifest')


def _build_autotools(source: Path, dest_prefix: Path) -> None:
    env = dict(os.environ)
    env.pop('DESTDIR', None)
    configure = source / 'configure'
    if not configure.is_file():
        raise ValueError('autotools source missing configure script')
    subprocess.run(['sh', './configure', f'--prefix={dest_prefix}'], cwd=source, env=env, check=True)
    jobs = str(max(1, min(os.cpu_count() or 4, 8)))
    subprocess.run(['make', f'-j{jobs}'], cwd=source, env=env, check=True)
    subprocess.run(['make', 'install'], cwd=source, env=env, check=True)


def install(component: dict, tools: Path) -> None:
    dest = tools / component['dir']
    if dest.exists():
        verify(component, tools)
        return
    cache = tools / 'downloads'
    archive = _fetch_verified_file(component, cache)
    with tempfile.TemporaryDirectory(prefix='install-', dir=cache) as temp:
        stage = Path(temp)
        _safe_extract(archive, stage)
        children = [c for c in stage.iterdir()]
        source = children[0] if len(children) == 1 and children[0].is_dir() else stage
        if component.get('build') == 'autotools':
            built = stage / 'built'
            built.mkdir()
            _build_autotools(source, built)
            source = built
        probes = {name: digest(source / name) for name in component.get('probes', [component['probe']])}
        (source / 'PINNED.json').write_text(json.dumps({
            'url': component['url'], 'sha256': component['sha256'], 'probes': probes,
        }, indent=2) + '\n')
        staged_tools = stage / 'verified'
        staged_tools.mkdir()
        package = staged_tools / component['dir']
        if source == stage:
            package.mkdir()
            for child in list(source.iterdir()):
                if child != staged_tools:
                    shutil.move(str(child), package / child.name)
        else:
            source.rename(package)
        verify(component, staged_tools)
        package.rename(dest)


def verify_vendor_package(pkg: dict, converter_src: Path) -> None:
    validate_vendor_package(pkg)
    dest = converter_src / pkg['id']
    receipt = json.loads((dest / 'PINNED.json').read_text())
    expected_patches = [{'name': p['name'], 'sha256': p['sha256']} for p in pkg.get('patches', [])]
    if (receipt.get('id') != pkg['id']
            or receipt.get('version') != pkg['version']
            or receipt.get('commit') != pkg['commit']
            or receipt.get('url') != pkg['url']
            or receipt.get('sha256') != pkg['sha256']
            or receipt.get('bytes') != pkg['bytes']
            or receipt.get('patches', []) != expected_patches):
        raise ValueError(f'vendor source receipt disagrees with manifest: {pkg["id"]}')
    if receipt.get('tree_sha256') != tree_digest(dest):
        raise ValueError(f'vendor source tree changed: {pkg["id"]}')


def install_vendor_package(pkg: dict, tools: Path, converter_src: Path | None = None) -> None:
    validate_vendor_package(pkg)
    converter_src = converter_src or (tools / 'converter-src')
    converter_src.mkdir(parents=True, exist_ok=True)
    dest = converter_src / pkg['id']
    if dest.exists():
        verify_vendor_package(pkg, converter_src)
        return
    cache = tools / 'downloads'
    archive = _fetch_verified_file(pkg, cache)
    patch_files: list[tuple[dict, Path]] = []
    for patch in pkg.get('patches', []):
        patch_files.append((patch, _fetch_verified_file(patch, cache)))
    with tempfile.TemporaryDirectory(prefix=f'vendor-{pkg["id"]}-', dir=cache) as temp:
        stage = Path(temp)
        _safe_extract(archive, stage)
        children = list(stage.iterdir())
        source = children[0] if len(children) == 1 and children[0].is_dir() else stage
        for patch, patch_path in patch_files:
            raw = patch_path.read_bytes()
            if patch_path.suffix == '.gz':
                raw = gzip.decompress(raw)
            apply_unified_patch(source, raw.decode('utf-8'), strip=patch.get('strip', 1))
        receipt = {
            'id': pkg['id'],
            'version': pkg['version'],
            'commit': pkg['commit'],
            'url': pkg['url'],
            'sha256': pkg['sha256'],
            'bytes': pkg['bytes'],
            'patches': [{'name': p['name'], 'sha256': p['sha256']} for p in pkg.get('patches', [])],
            'tree_sha256': tree_digest(source),
        }
        (source / 'PINNED.json').write_text(json.dumps(receipt, indent=2) + '\n')
        staged_root = stage / 'verified'
        staged_root.mkdir()
        package = staged_root / pkg['id']
        if source == stage:
            package.mkdir()
            for child in list(source.iterdir()):
                if child != staged_root:
                    shutil.move(str(child), package / child.name)
        else:
            source.rename(package)
        verify_vendor_package(pkg, staged_root)
        package.rename(dest)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--converter', action='store_true',
                        help='also install/verify converter-scoped build tools and vendor sources')
    parser.add_argument('--component', action='append', help='install/verify only a named component')
    args = parser.parse_args()
    import sys
    if sys.version_info < (3, 12):
        parser.error('Linux bootstrap requires Python 3.12+ for safe archive extraction')
    if platform.system() != 'Linux' or platform.machine().lower() not in ('x86_64', 'amd64'):
        parser.error('use tools/bootstrap.ps1 on Windows; this bootstrap serves Linux and WSL x64')
    manifest = json.loads((ROOT / 'toolchain.json').read_text())
    components = manifest['hosts']['linux-x86_64']['components']
    names = {c['name'] for c in components}
    if args.component and set(args.component) - names:
        parser.error('unknown component')
    failures = 0
    for component in components:
        if args.component:
            if component['name'] not in args.component:
                continue
        elif component.get('scope') == 'converter' and not args.converter:
            continue
        try:
            (verify if args.verify else install)(component, ROOT / 'tools')
            print(f"OK {component['name']} {component['expectVersion']} ({component['dir']})", flush=True)
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            failures += 1
            print(f"FAIL {component['name']}: {exc}", flush=True)
    if args.converter and not args.component:
        vendor_path = ROOT / 'converter/vendor.json'
        if not vendor_path.is_file():
            print('FAIL converter/vendor.json missing', flush=True)
            return 1
        try:
            vendor_manifest = json.loads(vendor_path.read_text(encoding='utf-8'))
            validate_vendor_manifest(vendor_manifest)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(f'FAIL converter/vendor.json: {exc}', flush=True)
            return 1
        converter_src = ROOT / 'tools/converter-src'
        for pkg in vendor_manifest['packages']:
            try:
                if args.verify:
                    verify_vendor_package(pkg, converter_src)
                else:
                    install_vendor_package(pkg, ROOT / 'tools', converter_src)
                print(f"OK vendor:{pkg['id']} {pkg['version']}", flush=True)
            except (OSError, ValueError, KeyError) as exc:
                failures += 1
                print(f"FAIL vendor:{pkg['id']}: {exc}", flush=True)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
