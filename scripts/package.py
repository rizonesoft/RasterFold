#!/usr/bin/env python3
"""Package built CLI/library artifacts with their header, license and hashes.

Without --release: one development zip per target under build/packages/ (used by CI builds).
With --release: reproducible release packages under dist/ from a clean, committed source revision:
dist/rasterfold-<version>-<target>.zip (binaries, libraries, header, symbols, build manifest, README.md from
PACKAGE.md, LICENSE, CHANGELOG.md, offline documentation, and a SHA256SUMS over every other file) and
dist/SHA256SUMS over the zips.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import tarfile
import tempfile
import time
from pathlib import Path, PurePosixPath
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))
from build import target_artifacts  # noqa: E402

TARGETS = ('windows-x64', 'linux-x64')


def read_snapshot(path: Path, label: str) -> bytes:
    try:
        if not path.is_file():
            raise ValueError(f'{path}: missing {label}; rebuild before packaging')
        return path.read_bytes()
    except OSError as exc:
        raise ValueError(f'{path}: unreadable {label} ({exc}); rebuild before packaging') from exc


def _load_tool(name: str, filename: str):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parent / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def release_source_state() -> tuple[str, int]:
    """(full commit id, committer time) of HEAD, refusing anything but a clean committed tree."""
    def git(*argv: str) -> str:
        try:
            done = subprocess.run(['git', '-C', str(ROOT), *argv], capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError) as exc:
            raise ValueError(f'git {argv[0]} failed ({exc}); a release package needs a git checkout') from exc
        if done.returncode != 0:
            raise ValueError(f'git {argv[0]} failed; a release package needs a git checkout')
        return done.stdout
    dirty = [line for line in git('status', '--porcelain').splitlines() if line.strip()]
    if dirty:
        raise ValueError(f'working tree is not clean ({len(dirty)} path(s), first: {dirty[0].strip()}); commit or remove them before a release package')
    revision = git('rev-parse', 'HEAD').strip()
    if len(revision) != 40 or any(c not in '0123456789abcdef' for c in revision):
        raise ValueError('cannot read the HEAD commit id')
    try:
        epoch = int(git('show', '-s', '--format=%ct', 'HEAD').strip())
    except ValueError as exc:
        raise ValueError('cannot read the HEAD commit time') from exc
    if epoch < 315532800:       # zip timestamps start at 1980-01-01
        raise ValueError('HEAD commit time is before 1980')
    return revision, epoch


def export_revision(revision: str, dest: Path) -> None:
    """Write the tracked files of `revision` into `dest`, from git's object store, never from the working tree.
    Everything a release package takes from the source tree (documentation, LICENSE, CHANGELOG.md, VERSION) is
    read from this export, so an edit made after the clean-tree check cannot reach a package."""
    try:
        # git archive applies the host's checkout line-ending conversion (core.autocrlf, core.eol) to what it
        # exports; a package must hold the committed bytes on every host, so both are pinned off for this call.
        done = subprocess.run(['git', '-C', str(ROOT), '-c', 'core.autocrlf=false', '-c', 'core.eol=lf', 'archive',
                               '--format=tar', revision], capture_output=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as exc:
        raise ValueError(f'git archive failed ({exc})') from exc
    if done.returncode != 0:
        raise ValueError('git archive failed; a release package needs the committed tree')
    import io
    with tarfile.open(fileobj=io.BytesIO(done.stdout), mode='r:') as tar:
        for member in tar.getmembers():
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts:
                raise ValueError(f'unsafe path in the committed tree: {member.name}')
            if member.isdir():
                (dest / member.name).mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target = dest / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                source = tar.extractfile(member)
                target.write_bytes(source.read() if source is not None else b'')
            # Symbolic links and other special entries are not needed by a package and are not written.


def offline_docs(revision: str, source_root: Path) -> dict[str, bytes]:
    """The offline documentation set (package-relative path -> bytes), rendered by the exported tree's own
    scripts/build-docs.py from the exported tree's pages."""
    spec = importlib.util.spec_from_file_location('rf_build_docs_release', source_root / 'scripts' / 'build-docs.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.render_offline_docs(source_root, revision)


def release_members(target: str, version: str, revision: str, docs: dict[str, bytes], source_root: Path) -> list[tuple[str, bytes, int]]:
    """Every file of one release package as (name inside the top directory, bytes, mode), sorted by name."""
    directory = ROOT / 'bin' / target
    manifest_bytes = read_snapshot(directory / 'build.json', 'build manifest')
    try:
        manifest = json.loads(manifest_bytes.decode('utf-8'))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ValueError('unsupported build manifest; rebuild before packaging') from exc
    if not isinstance(manifest, dict) or manifest.get('schema') != 2 or manifest.get('target') != target:
        raise ValueError('unsupported build manifest; rebuild before packaging')
    if manifest.get('product_version') != version:
        raise ValueError(
            f'stale build: manifest product version {manifest.get("product_version")!r} '
            f'!= VERSION {version}; rebuild before packaging')
    if manifest.get('dirty') is not False:
        raise ValueError(f'{target}: the build was made from a modified tree (dirty={manifest.get("dirty")!r}); rebuild from the clean commit')
    if manifest.get('revision') != revision[:12]:
        raise ValueError(
            f'{target}: the build is of revision {manifest.get("revision")!r}, HEAD is {revision[:12]}; rebuild before packaging')
    required = set(target_artifacts(target))
    artifacts = manifest.get('artifacts')
    if not isinstance(artifacts, dict) or required - set(artifacts):
        raise ValueError('incomplete build manifest; rebuild before packaging')
    members: dict[str, tuple[bytes, int]] = {}
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
        members[name] = (data, 0o755 if name == 'bin/rasterfold' else 0o644)
    members['build.json'] = (manifest_bytes, 0o644)
    for doc_name in ('LICENSE', 'CHANGELOG.md'):
        members[doc_name] = (read_snapshot(source_root / doc_name, 'package document'), 0o644)
    # The package's own first page: how to verify and use what was extracted, with package-relative paths.
    members['README.md'] = (read_snapshot(source_root / 'PACKAGE.md', 'package quickstart'), 0o644)
    for name, data in docs.items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts or '\\' in name or relative.as_posix() != name:
            raise ValueError(f'invalid documentation path: {name}')
        if name in members:
            raise ValueError(f'documentation path collides with a package file: {name}')
        members[name] = (data, 0o644)
    if 'SHA256SUMS' in members:
        raise ValueError('a package file is named SHA256SUMS')
    sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, (data, _mode) in sorted(members.items()))
    members['SHA256SUMS'] = (sums.encode('utf-8'), 0o644)
    return [(name, data, mode) for name, (data, mode) in sorted(members.items())]


def _exclusive_temporary(final: Path) -> Path:
    """A new, exclusively created temporary file beside `final`; no other writer can hold the same name."""
    handle, name = tempfile.mkstemp(dir=final.parent, prefix=final.name + '.', suffix='.tmp')
    os.close(handle)
    return Path(name)


def write_release_zip(dest: Path, top: str, members: list[tuple[str, bytes, int]], epoch: int) -> None:
    """Write the package to an exclusively created temporary file beside `dest` and rename it into place only
    when complete."""
    stamp = time.gmtime(epoch)[:6]
    temporary = _exclusive_temporary(dest)
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED) as z:
            for name, data, mode in members:
                info = zipfile.ZipInfo(f'{top}/{name}', date_time=stamp)
                info.create_system = 3                      # the same header bytes on every host
                info.external_attr = (0o100000 | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(info, data, compresslevel=9)
        os.chmod(temporary, 0o644)
        os.replace(temporary, dest)
    finally:
        temporary.unlink(missing_ok=True)


LOCK_NAME = '.release.lock'


def _acquire_release_lock(dist: Path) -> Path:
    """One release run at a time per dist/: the lock file is created exclusively and names its holder."""
    lock = dist / LOCK_NAME
    try:
        handle = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
    except FileExistsError:
        try:
            holder = lock.read_text(encoding='utf-8').strip()
        except OSError:
            holder = 'unknown'
        raise ValueError(
            f'another release run holds dist/{LOCK_NAME} ({holder}); wait for it, or remove the file if that run is gone'
        ) from None
    with os.fdopen(handle, 'w', encoding='utf-8') as fh:
        fh.write(f'pid {os.getpid()}\n')
    return lock


def _is_stale_temporary(name: str, finals: set[str]) -> bool:
    return name.endswith('.tmp') and any(name.startswith(final + '.') for final in finals)


def release(targets: list[str]) -> None:
    revision, epoch = release_source_state()
    dist = (ROOT / 'dist').resolve()
    if dist.parent != ROOT.resolve():
        raise ValueError('dist escapes the repository')
    if dist.is_symlink() or (dist.exists() and not dist.is_dir()):
        raise ValueError('dist is not a directory')
    with tempfile.TemporaryDirectory(prefix='rasterfold-release-') as exported:
        source_root = Path(exported)
        export_revision(revision, source_root)
        version = _load_tool('rf_version_tool', 'version.py').read_version(source_root)
        package_names = {f'rasterfold-{version}-{t}.zip' for t in TARGETS}
        finals = package_names | {'SHA256SUMS'}
        docs = offline_docs(revision, source_root)
        dist.mkdir(exist_ok=True)
        lock = _acquire_release_lock(dist)
        try:
            strangers = sorted(
                entry.name for entry in dist.iterdir()
                if entry.name != LOCK_NAME and not _is_stale_temporary(entry.name, finals)
                and (entry.name not in finals or not entry.is_file() or entry.is_symlink())
            )
            if strangers:
                raise ValueError(f'dist holds something that is not a {version} package or its checksum file: {strangers[0]}')
            # Holding the lock, any temporary still here was left by a run that died; it is ours to remove.
            for entry in dist.iterdir():
                if _is_stale_temporary(entry.name, finals) and entry.is_file() and not entry.is_symlink():
                    entry.unlink()
            try:
                for target in targets:
                    members = release_members(target, version, revision, docs, source_root)
                    dest = dist / f'rasterfold-{version}-{target}.zip'
                    write_release_zip(dest, f'rasterfold-{version}-{target}', members, epoch)
                    print(f'{dest.relative_to(ROOT)}  {hashlib.sha256(dest.read_bytes()).hexdigest()}')
            finally:
                # The checksum file always describes exactly the packages on disk, also after a failed target.
                present = sorted(name for name in package_names if (dist / name).is_file())
                sums = ''.join(f'{hashlib.sha256((dist / name).read_bytes()).hexdigest()}  {name}\n' for name in present)
                sums_path = dist / 'SHA256SUMS'
                if present:
                    temporary = _exclusive_temporary(sums_path)
                    try:
                        temporary.write_text(sums, encoding='utf-8', newline='\n')
                        os.chmod(temporary, 0o644)
                        os.replace(temporary, sums_path)
                    finally:
                        temporary.unlink(missing_ok=True)
                else:
                    sums_path.unlink(missing_ok=True)
        finally:
            lock.unlink(missing_ok=True)


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
    import importlib.util as _ilu
    _spec = _ilu.spec_from_file_location('rf_version_tool', Path(__file__).resolve().parent / 'version.py')
    _vt = _ilu.module_from_spec(_spec); _spec.loader.exec_module(_vt)
    current_version = _vt.read_version(ROOT)
    if manifest.get('product_version') != current_version:
        raise ValueError(
            f'stale build: manifest product version {manifest.get("product_version")!r} '
            f'!= VERSION {current_version}; rebuild before packaging')
    revision = manifest.get('revision')
    if not isinstance(revision, str) or not (
        revision == 'unknown'
        or (len(revision) == 12 and all(c in '0123456789abcdef' for c in revision))
    ):
        raise ValueError('incomplete build manifest (revision); rebuild before packaging')
    dirty = manifest.get('dirty')
    label = _vt.dev_label(current_version, revision, dirty if isinstance(dirty, bool) else None)
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
    packages_dir = (ROOT / 'build/packages').resolve()
    dest = packages_dir / f'rasterfold-{target}-{label}.zip'
    if dest.parent != packages_dir:
        raise ValueError('package destination escapes build/packages')
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
    p.add_argument('--release', action='store_true',
                   help='write reproducible release packages to dist/ from a clean committed tree')
    a = p.parse_args()
    chosen = list(TARGETS) if a.target == 'all' else [a.target]
    if a.release:
        try:
            release(chosen)
        except ValueError as exc:
            print(f'package: {exc}', file=sys.stderr)
            sys.exit(1)
    else:
        for t in chosen:
            package(t)
