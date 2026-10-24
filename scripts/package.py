#!/usr/bin/env python3
"""Package built CLI/library artifacts with their header, license and hashes."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))
from build import target_artifacts  # noqa: E402

TARGETS = ('windows-x64', 'linux-x64', 'android-arm64', 'android-x64')


def read_snapshot(path: Path, label: str) -> bytes:
    try:
        if not path.is_file():
            raise ValueError(f'{path}: missing {label}; rebuild before packaging')
        return path.read_bytes()
    except OSError as exc:
        raise ValueError(f'{path}: unreadable {label} ({exc}); rebuild before packaging') from exc


def package(target: str) -> None:
    directory = ROOT / 'bin' / target
    manifest_path = directory / 'build.json'
    manifest_bytes = read_snapshot(manifest_path, 'build manifest')
    try:
        manifest = json.loads(manifest_bytes.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ValueError('unsupported build manifest; rebuild before packaging') from exc
    if not isinstance(manifest, dict) or manifest.get('schema') != 2 or manifest.get('target') != target:
        raise ValueError('unsupported build manifest; rebuild before packaging')
    required = set(target_artifacts(target))
    artifacts = manifest.get('artifacts')
    if not isinstance(artifacts, dict) or required - set(artifacts):
        raise ValueError('incomplete build manifest; rebuild before packaging')
    files: list[tuple[Path, str, bytes]] = []
    for name, digest in artifacts.items():
        if not isinstance(name, str) or not isinstance(digest, str):
            raise ValueError('incomplete build manifest; rebuild before packaging')
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or '\\' in name or ':' in name or relative.as_posix() != name:
            raise ValueError(f'invalid artifact path: {name}')
        file = directory / name
        if not file.resolve().is_relative_to(directory.resolve()):
            raise ValueError(f'artifact escapes target: {name}')
        data = read_snapshot(file, 'build artifact')
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f'{file}: build digest mismatch; rebuild before packaging')
        files.append((file, name, data))
    files.append((manifest_path, 'build.json', manifest_bytes))
    for doc_name in ('LICENSE', 'docs/building.md', 'docs/library.md'):
        doc_path = ROOT / doc_name
        files.append((doc_path, doc_name, read_snapshot(doc_path, 'package document')))
    dest = ROOT / 'build/packages' / f'rasterfold-{target}-dev.zip'
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for file, name, data in files:
            info = zipfile.ZipInfo(f'rasterfold-{target}/' + name, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o100755 if file.name == 'rasterfold' else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    digest = hashlib.sha256(dest.read_bytes()).hexdigest()
    dest.with_suffix('.zip.sha256').write_text(f'{digest}  {dest.name}\n', encoding='utf-8')
    print(f'{dest.relative_to(ROOT)}  {digest}')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=(*TARGETS, 'all'), default='all')
    a = p.parse_args()
    for t in TARGETS if a.target == 'all' else [a.target]:
        package(t)
