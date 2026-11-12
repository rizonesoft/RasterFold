#!/usr/bin/env python3
"""Run a release package before it is published.

`scripts/package.py --release` writes dist/rasterfold-<version>-<target>.zip and dist/SHA256SUMS. This tool takes
that directory and one target and proves the package a user would download works on this host:

1. every file dist/SHA256SUMS lists is present and has that SHA-256, and the target's zip is listed;
2. the zip extracts under one top directory, and the package's own SHA256SUMS holds for every file in it;
3. the packaged `rasterfold version` reports the expected product version;
4. a generated image round-trips through the packaged `encode`, `verify` and `decode` to identical pixels.

It refuses (exit 1, one `verify-release: REFUSED` line naming the reason) on the first failure. Each packaged
command is bounded by a timeout that ends its whole process tree, and everything is extracted into a temporary
directory that is removed on success, refusal and timeout alike. Standard library only; runs on Linux and Windows.

    python3 scripts/verify_release.py                      # dist/ of this checkout, this host's target
    python3 scripts/verify_release.py --dist DIR --target windows-x64 --version 1.0.0
"""
import argparse
import hashlib
import os
import signal
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('windows-x64', 'linux-x64')
SUMS = 'SHA256SUMS'
DEFAULT_TIMEOUT = 120.0


class Refused(Exception):
    """The package is not fit to publish; the message is the reason."""


def host_target() -> str:
    return 'windows-x64' if os.name == 'nt' else 'linux-x64'


def end_tree(process: subprocess.Popen) -> None:
    """End a command and everything it started. On POSIX the command leads its own process group and the group is
    killed; on Windows `taskkill /T /F` ends the tree while its root is still known."""
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(process.pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def run_bounded(argv: list[str], timeout: float, cwd: Path | None = None,
                label: str | None = None) -> subprocess.CompletedProcess:
    """Run one command to completion within `timeout` seconds, capturing its output. A command that outlives the
    bound is refused and its whole process tree is ended first, never only the direct child."""
    command = [str(a) for a in argv]
    label = label or ' '.join([Path(command[0]).name, *command[1:3]])
    pipes = subprocess.PIPE
    try:
        if os.name == 'nt':
            process = subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL, stdout=pipes, stderr=pipes,
                                       creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        else:
            process = subprocess.Popen(command, cwd=cwd, stdin=subprocess.DEVNULL, stdout=pipes, stderr=pipes,
                                       start_new_session=True)
    except OSError as error:
        raise Refused(f'cannot start {label}: {error}') from error
    try:
        out, err = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        end_tree(process)
        try:
            process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        raise Refused(f'{label} did not finish within {timeout:g} s; its process tree was ended') from None
    except BaseException:
        end_tree(process)
        process.wait()
        raise
    if os.name != 'nt':
        end_tree(process)            # nothing the command started outlives it, even when it exited in time
    return subprocess.CompletedProcess(argv, process.returncode, out.decode('utf-8', 'replace'),
                                       err.decode('utf-8', 'replace'))


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def read_sums(path: Path) -> dict[str, str]:
    """A SHA256SUMS file as {relative name: lowercase hex digest}; a malformed line or a repeated name is refused."""
    try:
        text = path.read_text(encoding='utf-8')
    except (OSError, UnicodeDecodeError) as error:
        raise Refused(f'{path.name} cannot be read in {path.parent.name}: {error}') from error
    sums: dict[str, str] = {}
    for number, line in enumerate(text.splitlines(), 1):
        digest, separator, name = line.partition('  ')
        name = name.removeprefix('*')
        if (not separator or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest) or not name
                or name.startswith(('/', '\\')) or '..' in Path(name.replace('\\', '/')).parts or ':' in name):
            raise Refused(f'{path.name} line {number} is not "<sha256>  <relative name>"')
        if name in sums:
            raise Refused(f'{path.name} lists {name} twice')
        sums[name] = digest
    if not sums:
        raise Refused(f'{path.name} lists no file')
    return sums


def check_sums(directory: Path, label: str) -> int:
    """Every file `directory/SHA256SUMS` lists exists with that digest, and no other file is in the directory tree
    (the checksum file aside). Returns the number of files checked."""
    sums = read_sums(directory / SUMS)
    for name, expected in sorted(sums.items()):
        path = directory / name
        if not path.is_file():
            raise Refused(f'{label}: {name} is listed in {SUMS} and missing')
        actual = sha256_of(path)
        if actual != expected:
            raise Refused(f'{label}: {name} has SHA-256 {actual}, and {SUMS} says {expected}')
    return len(sums)


def unlisted_files(directory: Path) -> list[str]:
    sums = read_sums(directory / SUMS)
    present = sorted(p.relative_to(directory).as_posix() for p in directory.rglob('*') if p.is_file())
    return [name for name in present if name != SUMS and name not in sums]


def extract(zip_path: Path, dest: Path, top: str) -> Path:
    """Extract the package under `dest`. Every member must sit under the one top directory, by a relative path with
    no parent step; the mode recorded in the zip is restored so the executable can run."""
    try:
        with zipfile.ZipFile(zip_path) as archive:
            infos = archive.infolist()
            for info in infos:
                parts = Path(info.filename.replace('\\', '/')).parts
                if (not parts or parts[0] != top or '..' in parts or info.filename.startswith(('/', '\\'))
                        or ':' in info.filename):
                    raise Refused(f'{zip_path.name}: member {info.filename!r} is not under {top}/')
            for info in infos:
                target = Path(archive.extract(info, dest))
                mode = (info.external_attr >> 16) & 0o7777
                if mode and not info.is_dir():
                    target.chmod(mode)
    except (zipfile.BadZipFile, OSError) as error:
        raise Refused(f'{zip_path.name} cannot be extracted: {error}') from error
    root = dest / top
    if not root.is_dir():
        raise Refused(f'{zip_path.name} holds no {top}/ directory')
    return root


def test_image(width: int = 61, height: int = 37) -> tuple[bytes, bytes]:
    """A small deterministic P6 image (file bytes, RGB bytes): smooth gradients with a hard-edged block, so both a
    predictor and its edges are exercised. Odd dimensions on purpose."""
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            block = 12 <= x < 30 and 8 <= y < 20
            pixels += bytes(((x * 4 + y) & 255, 200 if block else (x ^ (y * 3)) & 255, (y * 6 + x * x) & 255))
    return b'P6\n%d %d\n255\n' % (width, height) + bytes(pixels), bytes(pixels)


def read_p6(data: bytes) -> tuple[int, int, bytes]:
    """Parse a binary PPM with maxval 255: (width, height, RGB bytes). Comments and any header whitespace are read."""
    fields: list[bytes] = []
    position = 0
    while len(fields) < 4:
        while position < len(data) and data[position:position + 1].isspace():
            position += 1
        if data[position:position + 1] == b'#':
            while position < len(data) and data[position:position + 1] != b'\n':
                position += 1
            continue
        start = position
        while position < len(data) and not data[position:position + 1].isspace():
            position += 1
        if start == position:
            raise Refused('the decoded image is not a complete P6 file')
        fields.append(data[start:position])
    position += 1                                    # the single whitespace byte after maxval
    try:
        width, height, maxval = (int(f) for f in fields[1:])
    except ValueError:
        raise Refused('the decoded image has a malformed P6 header') from None
    if fields[0] != b'P6' or maxval != 255:
        raise Refused('the decoded image is not an 8-bit P6 file')
    pixels = data[position:]
    if len(pixels) != width * height * 3:
        raise Refused(f'the decoded image holds {len(pixels)} pixel bytes for {width}x{height}')
    return width, height, pixels


def verify(dist: Path, target: str, version: str, timeout: float = DEFAULT_TIMEOUT, launcher: tuple[str, ...] = (),
           report=print) -> None:
    """Verify one target's package in `dist`; raises Refused with the reason. `launcher` is an argv prefix for the
    packaged executable (empty for a real package; the tests run a script through the interpreter)."""
    if target not in TARGETS:
        raise Refused(f'unknown target {target!r}; the targets are {", ".join(TARGETS)}')
    dist = Path(dist)
    if not (dist / SUMS).is_file():
        raise Refused(f'{dist} holds no {SUMS}; run python3 scripts/package.py --release first')
    top = f'rasterfold-{version}-{target}'
    zip_name = top + '.zip'
    if zip_name not in read_sums(dist / SUMS):
        raise Refused(f'{SUMS} does not list {zip_name}')
    report(f'verify-release: {check_sums(dist, "dist")} file(s) match dist/{SUMS}')
    with tempfile.TemporaryDirectory(prefix='rf-verify-release-') as scratch:
        package = extract(dist / zip_name, Path(scratch) / 'package', top)
        report(f'verify-release: {check_sums(package, top)} file(s) match {top}/{SUMS}')
        extra = unlisted_files(package)
        if extra:
            raise Refused(f'{top}: {len(extra)} file(s) are not in its {SUMS}, first {extra[0]}')
        executable = package / 'bin' / ('rasterfold.exe' if target == 'windows-x64' else 'rasterfold')
        if not executable.is_file():
            raise Refused(f'{top}: no {executable.relative_to(package).as_posix()}')
        work = Path(scratch) / 'work'
        work.mkdir()

        def run(*args: str) -> subprocess.CompletedProcess:
            result = run_bounded([*launcher, str(executable), *args], timeout, cwd=work,
                                 label=' '.join([executable.name, *args[:2]]))
            if result.returncode != 0:
                detail = (result.stderr or result.stdout).strip().splitlines()
                raise Refused(f'{executable.name} {args[0]} exited {result.returncode}'
                              + (f': {detail[-1][:200]}' if detail else ''))
            return result

        lines = [line.strip() for line in run('version').stdout.splitlines()]
        wanted = f'product version: {version}'
        if wanted not in lines:
            reported = next((line for line in lines if line.startswith('product version:')), 'no product version line')
            raise Refused(f'{executable.name} version reports "{reported}", not "{wanted}"')
        report(f'verify-release: {executable.name} version reports {wanted}')
        image, pixels = test_image()
        (work / 'in.ppm').write_bytes(image)
        run('encode', 'in.ppm', 'out.fold')
        run('verify', 'out.fold')
        run('decode', 'out.fold', 'back.ppm')
        try:
            decoded = (work / 'back.ppm').read_bytes()
        except OSError as error:
            raise Refused(f'decode wrote no image: {error}') from error
        width, height, got = read_p6(decoded)
        if (width, height) != (61, 37) or got != pixels:
            raise Refused('the image decoded by the package differs from the image it encoded')
        size = (work / 'out.fold').stat().st_size
        report(f'verify-release: encode, verify and decode round-trip a {width}x{height} image exactly '
               f'({len(image)} -> {size} bytes)')
    report(f'verify-release: OK {zip_name}')


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dist', default=str(ROOT / 'dist'), help='the directory package.py --release wrote')
    parser.add_argument('--target', default=host_target(), choices=TARGETS, help="default: this host's target")
    parser.add_argument('--version', help='the product version to expect (default: the VERSION file)')
    parser.add_argument('--timeout', type=float, default=DEFAULT_TIMEOUT, help='seconds allowed per packaged command')
    args = parser.parse_args(argv)
    try:
        if args.timeout <= 0:
            raise Refused('--timeout must be positive')
        version = args.version
        if version is None:
            try:
                version = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
            except OSError as error:
                raise Refused(f'VERSION cannot be read: {error}') from error
        verify(Path(args.dist), args.target, version, args.timeout)
    except Refused as refusal:
        print(f'verify-release: REFUSED {refusal}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
