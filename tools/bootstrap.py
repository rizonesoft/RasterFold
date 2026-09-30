#!/usr/bin/env python3
"""Install the pinned Linux-host toolchain inside this checkout. Stdlib only."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(1024 * 1024):
            h.update(block)
    return h.hexdigest()


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
    if component['expectVersion'] not in out:
        raise ValueError('installed compiler/tool reports the wrong version')
    if component.get('versionFile') and component['versionText'] not in (dest / component['versionFile']).read_text():
        raise ValueError('SDK revision disagrees with manifest')


def install(component: dict, tools: Path) -> None:
    dest = tools / component['dir']
    if dest.exists():
        verify(component, tools)
        return
    cache = tools / 'downloads'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / component['url'].rsplit('/', 1)[-1]
    if not archive.exists():
        partial = archive.with_name(archive.name + '.partial')
        with urllib.request.urlopen(component['url'], timeout=90) as response, partial.open('wb') as out:
            shutil.copyfileobj(response, out, 1024 * 1024)
        partial.replace(archive)
    if archive.stat().st_size != component['bytes'] or digest(archive) != component['sha256']:
        raise ValueError(f'archive checksum or size mismatch: {archive}')
    with tempfile.TemporaryDirectory(prefix='install-', dir=cache) as temp:
        stage = Path(temp)
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as z:
                for entry in z.infolist():
                    path = stage / entry.filename
                    if not path.resolve().is_relative_to(stage.resolve()):
                        raise ValueError('archive member escapes extraction root')
                    mode = entry.external_attr >> 16
                    if stat.S_ISLNK(mode):
                        target = z.read(entry).decode('utf-8')
                        if not (path.parent / target).resolve().is_relative_to(stage.resolve()):
                            raise ValueError('archive symlink escapes extraction root')
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.symlink_to(target)
                        continue
                    z.extract(entry, stage)
                    if mode & 0o111:
                        path.chmod(path.stat().st_mode | 0o111)
        else:
            with tarfile.open(archive) as t:
                t.extractall(stage, filter='data')
        children = list(stage.iterdir())
        source = children[0] if len(children) == 1 and children[0].is_dir() else stage
        probes = {name: digest(source / name) for name in component.get('probes', [component['probe']])}
        (source / 'PINNED.json').write_text(json.dumps({
            'url': component['url'], 'sha256': component['sha256'], 'probes': probes,
        }, indent=2) + '\n')
        # Verify before publishing the installation; a failed extraction never
        # replaces a usable existing toolchain.
        staged_tools = stage / 'verified'
        staged_tools.mkdir()
        if source == stage:
            package = staged_tools / component['dir']
            package.mkdir()
            for child in list(source.iterdir()):
                if child != staged_tools:
                    shutil.move(str(child), package / child.name)
        else:
            package = staged_tools / component['dir']
            source.rename(package)
        verify(component, staged_tools)
        package.rename(dest)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--component', action='append', help='install/verify only a named component')
    args = parser.parse_args()
    import sys
    if sys.version_info < (3,12):
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
        if args.component and component['name'] not in args.component:
            continue
        try:
            (verify if args.verify else install)(component, ROOT / 'tools')
            print(f"OK {component['name']} {component['expectVersion']} ({component['dir']})", flush=True)
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            failures += 1
            print(f"FAIL {component['name']}: {exc}", flush=True)
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
