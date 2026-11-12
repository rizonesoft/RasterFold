"""Development and release packages must contain every required, hash-verified artifact."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))
import build as build_mod  # noqa: E402
import package  # noqa: E402
import verify_release  # noqa: E402


class Packages(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()   # a runner's temp path can be a short alias of the real one
        self.patch = patch.object(package, 'ROOT', self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        (self.root / 'VERSION').write_text('1.0.0\n', encoding='utf-8')
        for name in ('LICENSE', 'docs/building.md', 'docs/library.md'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name, encoding='utf-8')

    def fixture(self, target):
        names = list(package.target_artifacts(target))
        directory = self.root / 'bin' / target
        artifacts = {}
        for name in names:
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode('utf-8'))
            artifacts[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest = {'schema': 2, 'target': target, 'artifacts': artifacts,
                    'product_version': '1.0.0', 'revision': 'abc123def456', 'dirty': False}
        self.write_manifest(target, manifest)
        return manifest

    def write_manifest(self, target, manifest):
        (self.root / 'bin' / target / 'build.json').write_text(json.dumps(manifest), encoding='utf-8')

    def test_target_artifacts_match_build_table(self):
        self.assertIs(package.target_artifacts, build_mod.target_artifacts)
        for target in package.TARGETS:
            names = package.target_artifacts(target)
            self.assertIn('include/rasterfold.h', names)
            self.assertIn('lib/librasterfold.a', names)
        with self.assertRaisesRegex(ValueError, 'unknown target'):
            package.target_artifacts('unknown-target')

    def test_complete_packages_for_every_target(self):
        for target in package.TARGETS:
            with self.subTest(target=target):
                manifest = self.fixture(target)
                with contextlib.redirect_stdout(io.StringIO()):
                    package.package(target)
                dest = self.root / 'build/packages' / f'rasterfold-{target}-1.0.0-dev+abc123def456.zip'
                with zipfile.ZipFile(dest) as archive:
                    prefix = f'rasterfold-{target}/'
                    expected = set(manifest['artifacts']) | {'build.json', 'LICENSE', 'docs/building.md', 'docs/library.md'}
                    self.assertEqual(set(archive.namelist()), {prefix + name for name in expected})
                    for name, digest in manifest['artifacts'].items():
                        self.assertEqual(hashlib.sha256(archive.read(prefix + name)).hexdigest(), digest)
                self.assertEqual(
                    dest.with_suffix('.zip.sha256').read_text(encoding='utf-8'),
                    f'{hashlib.sha256(dest.read_bytes()).hexdigest()}  {dest.name}\n',
                )

    def test_missing_required_manifest_entries_refused(self):
        for target in package.TARGETS:
            original = self.fixture(target)
            for name in original['artifacts']:
                with self.subTest(target=target, missing=name):
                    manifest = {**original, 'artifacts': dict(original['artifacts'])}
                    del manifest['artifacts'][name]
                    self.write_manifest(target, manifest)
                    with self.assertRaisesRegex(ValueError, 'incomplete build manifest'):
                        package.package(target)
        self.assertFalse((self.root / 'build/packages').exists())

    def test_missing_or_empty_artifacts_refused(self):
        manifest = self.fixture('linux-x64')
        for artifacts in ({}, None, []):
            manifest['artifacts'] = artifacts
            self.write_manifest('linux-x64', manifest)
            with self.assertRaisesRegex(ValueError, 'incomplete build manifest'):
                package.package('linux-x64')
        del manifest['artifacts']
        self.write_manifest('linux-x64', manifest)
        with self.assertRaisesRegex(ValueError, 'incomplete build manifest'):
            package.package('linux-x64')

    def test_tampered_artifact_refused_before_zip_write(self):
        self.fixture('windows-x64')
        (self.root / 'bin/windows-x64/include/rasterfold.h').write_text('changed', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            package.package('windows-x64')
        self.assertFalse((self.root / 'build/packages').exists())

    def test_missing_or_directory_artifact_refused(self):
        self.fixture('linux-x64')
        header = self.root / 'bin/linux-x64/include/rasterfold.h'
        header.unlink()
        with self.assertRaisesRegex(ValueError, 'missing build artifact; rebuild before packaging'):
            package.package('linux-x64')
        header.mkdir()
        with self.assertRaisesRegex(ValueError, 'missing build artifact; rebuild before packaging'):
            package.package('linux-x64')
        self.assertFalse((self.root / 'build/packages').exists())

    def test_linux_so_symlink_packaged_as_regular_file(self):
        manifest = self.fixture('linux-x64')
        libdir = self.root / 'bin/linux-x64/lib'
        so_real = libdir / 'librasterfold.so.1'
        so_link = libdir / 'librasterfold.so'
        so_real.write_bytes(b'elf-shared-object-v1')
        so_link.unlink()
        try:
            so_link.symlink_to('librasterfold.so.1')
        except OSError:
            self.skipTest('symlinks unavailable in this environment')
        digest = hashlib.sha256(b'elf-shared-object-v1').hexdigest()
        manifest['artifacts']['lib/librasterfold.so.1'] = digest
        manifest['artifacts']['lib/librasterfold.so'] = digest
        self.write_manifest('linux-x64', manifest)
        with contextlib.redirect_stdout(io.StringIO()):
            package.package('linux-x64')
        dest = self.root / 'build/packages/rasterfold-linux-x64-1.0.0-dev+abc123def456.zip'
        with zipfile.ZipFile(dest) as archive:
            self.assertEqual(archive.read('rasterfold-linux-x64/lib/librasterfold.so'), b'elf-shared-object-v1')
            self.assertEqual(archive.read('rasterfold-linux-x64/lib/librasterfold.so.1'), b'elf-shared-object-v1')

    def test_single_read_snapshot_ignores_post_read_mutation(self):
        original_manifest = self.fixture('linux-x64')
        real_read = package.read_snapshot

        def mutating_read(path: Path, label: str) -> bytes:
            data = real_read(path, label)
            if path.name == 'build.json':
                path.write_text('{"schema": 2, "target": "linux-x64", "artifacts": {}}', encoding='utf-8')
            elif path.name == 'rasterfold.h':
                path.write_bytes(b'mutated-after-verification')
            return data

        with patch.object(package, 'read_snapshot', side_effect=mutating_read):
            with contextlib.redirect_stdout(io.StringIO()):
                package.package('linux-x64')
        dest = self.root / 'build/packages/rasterfold-linux-x64-1.0.0-dev+abc123def456.zip'
        with zipfile.ZipFile(dest) as archive:
            archived_manifest = json.loads(archive.read('rasterfold-linux-x64/build.json').decode('utf-8'))
            self.assertEqual(archived_manifest, original_manifest)
            archived_header = archive.read('rasterfold-linux-x64/include/rasterfold.h')
            self.assertEqual(archived_header, b'include/rasterfold.h')
            self.assertEqual(
                hashlib.sha256(archived_header).hexdigest(),
                archived_manifest['artifacts']['include/rasterfold.h'],
            )

    def test_stale_build_manifest_refused(self):
        manifest = self.fixture('linux-x64')
        manifest['product_version'] = '0.9.0'
        self.write_manifest('linux-x64', manifest)
        with self.assertRaisesRegex(ValueError, 'stale build'):
            package.package('linux-x64')
        self.assertFalse((self.root / 'build/packages').exists())

    def test_manifest_revision_escape_refused(self):
        manifest = self.fixture('linux-x64')
        manifest['revision'] = '/../../outside'
        self.write_manifest('linux-x64', manifest)
        with self.assertRaisesRegex(ValueError, 'revision'):
            package.package('linux-x64')
        self.assertFalse((self.root / 'build/packages').exists())

    def test_unsafe_manifest_path_refused(self):
        for name in ('../outside', '/absolute', 'lib/../outside', 'lib\\outside', 'C:/outside'):
            with self.subTest(name=name):
                manifest = self.fixture('linux-x64')
                manifest['artifacts'][name] = '0' * 64
                self.write_manifest('linux-x64', manifest)
                with self.assertRaisesRegex(ValueError, 'invalid artifact path'):
                    package.package('linux-x64')



REVISION = 'abc123def456' + '0' * 28
EPOCH = 1794441600          # 2026-11-12T00:00:00Z


class ReleasePackages(Packages):
    """Release mode: dist/rasterfold-<version>-<target>.zip from a clean committed tree."""

    def setUp(self):
        super().setUp()
        (self.root / 'CHANGELOG.md').write_bytes(b'# Changelog\n')
        (self.root / 'PACKAGE.md').write_bytes((REPO / 'PACKAGE.md').read_bytes())
        self.docs = {'docs/index.md': b'# Index\n\n[Usage](usage.md)\n', 'docs/usage.md': b'# Usage\n\n[Licence](../LICENSE)\n'}
        self.state = patch.object(package, 'release_source_state', lambda: (REVISION, EPOCH))
        self.state.start()
        self.addCleanup(self.state.stop)
        self.offline = patch.object(package, 'offline_docs', lambda revision, source_root: dict(self.docs))
        self.offline.start()
        self.addCleanup(self.offline.stop)

        def fake_export(revision, dest):
            # The fixture root stands for the committed tree.
            for name in ('VERSION', 'LICENSE', 'CHANGELOG.md', 'PACKAGE.md'):
                (dest / name).write_bytes((self.root / name).read_bytes())

        self.export = patch.object(package, 'export_revision', fake_export)
        self.export.start()
        self.addCleanup(self.export.stop)
        for target in package.TARGETS:
            self.fixture(target)

    def dist_snapshot(self):
        dist = self.root / 'dist'
        return {p.name: p.read_bytes() for p in sorted(dist.iterdir())} if dist.is_dir() else {}

    def run_release(self, targets=None):
        with contextlib.redirect_stdout(io.StringIO()):
            package.release(list(targets or package.TARGETS))

    def test_release_exact_contents_modes_checksums_and_timestamps(self):
        self.run_release()
        snapshot = self.dist_snapshot()
        self.assertEqual(sorted(snapshot), ['SHA256SUMS', 'rasterfold-1.0.0-linux-x64.zip', 'rasterfold-1.0.0-windows-x64.zip'])
        self.assertFalse((self.root / 'dist' / package.LOCK_NAME).exists(), 'the lock is released after a run')
        outer = dict(line.split('  ')[::-1] for line in snapshot['SHA256SUMS'].decode().splitlines())
        for target in package.TARGETS:
            name = f'rasterfold-1.0.0-{target}.zip'
            self.assertEqual(outer[name], hashlib.sha256(snapshot[name]).hexdigest())
            top = f'rasterfold-1.0.0-{target}/'
            with zipfile.ZipFile(self.root / 'dist' / name) as z:
                infos = z.infolist()
                names = [i.filename for i in infos]
                expected = sorted(top + n for n in (*package.target_artifacts(target), 'build.json', 'LICENSE', 'CHANGELOG.md',
                                                    'README.md', 'docs/index.md', 'docs/usage.md', 'SHA256SUMS'))
                self.assertEqual(z.read(top + 'README.md'), (REPO / 'PACKAGE.md').read_bytes())
                self.assertEqual(names, expected)
                self.assertEqual(names, sorted(names))
                for info in infos:
                    self.assertEqual(info.date_time, (2026, 11, 12, 0, 0, 0), info.filename)
                    self.assertEqual(info.create_system, 3)
                    mode = (info.external_attr >> 16) & 0o7777
                    self.assertEqual(mode, 0o755 if info.filename == top + 'bin/rasterfold' else 0o644, info.filename)
                sums = z.read(top + 'SHA256SUMS').decode()
                listed = [line.split('  ', 1) for line in sums.splitlines()]
                self.assertEqual(sorted(n for _d, n in listed), sorted(n[len(top):] for n in names if n != top + 'SHA256SUMS'))
                self.assertEqual(len({n for _d, n in listed}), len(listed))
                for digest, member in listed:
                    self.assertEqual(digest, hashlib.sha256(z.read(top + member)).hexdigest(), member)
                self.assertEqual(json.loads(z.read(top + 'build.json'))['revision'], REVISION[:12])

    def test_release_two_runs_are_byte_identical(self):
        self.run_release()
        first = self.dist_snapshot()
        self.run_release()
        self.assertEqual(first, self.dist_snapshot())

    def test_release_refusals_leave_dist_unchanged(self):
        self.run_release()
        before = self.dist_snapshot()

        def refused(pattern, mutate, targets=('linux-x64',)):
            manifest = json.loads((self.root / 'bin/linux-x64/build.json').read_text(encoding='utf-8'))
            original = {p: p.read_bytes() for p in (self.root / 'bin/linux-x64').rglob('*') if p.is_file()}
            mutate(manifest)
            with self.assertRaisesRegex(ValueError, pattern):
                self.run_release(targets)
            self.assertEqual(before, self.dist_snapshot(), pattern)
            for path, data in original.items():
                path.write_bytes(data)

        def write(manifest):
            self.write_manifest('linux-x64', manifest)

        refused('modified tree', lambda m: (m.__setitem__('dirty', True), write(m)))
        refused('modified tree', lambda m: (m.pop('dirty'), write(m)))
        refused("the build is of revision 'ffffffffffff'", lambda m: (m.__setitem__('revision', 'ffffffffffff'), write(m)))
        refused('stale build', lambda m: (m.__setitem__('product_version', '0.9.0'), write(m)))
        refused('incomplete build manifest', lambda m: (m['artifacts'].pop('bin/rasterfold'), write(m)))
        refused('invalid artifact path', lambda m: (m['artifacts'].__setitem__('../escape', '0' * 64), write(m)))
        refused('build digest mismatch', lambda m: (self.root / 'bin/linux-x64/bin/rasterfold').write_bytes(b'tampered'))
        refused('missing build artifact', lambda m: (self.root / 'bin/linux-x64/lib/librasterfold.a').unlink())
        refused('unsupported build manifest', lambda m: (self.root / 'bin/linux-x64/build.json').write_text('{', encoding='utf-8'))
        # Documentation that collides with a package file or escapes the package.
        self.docs['LICENSE'] = b'not the licence'
        refused('collides with a package file', lambda m: None)
        del self.docs['LICENSE']
        self.docs['../outside.md'] = b'x'
        refused('invalid documentation path', lambda m: None)
        del self.docs['../outside.md']
        # A dirty or uncommitted tree is refused before anything is read.
        with patch.object(package, 'release_source_state', side_effect=ValueError('working tree is not clean (1 path(s), first: M x)')):
            with self.assertRaisesRegex(ValueError, 'working tree is not clean'):
                self.run_release()
        self.assertEqual(before, self.dist_snapshot())
        # Something in dist/ that is not a package of this version.
        stranger = self.root / 'dist' / 'rasterfold-0.9.0-linux-x64.zip'
        stranger.write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, 'dist holds something that is not a 1.0.0 package'):
            self.run_release()
        self.assertEqual({**before, stranger.name: b'old'}, self.dist_snapshot())
        stranger.unlink()
        (self.root / 'dist' / 'notes').mkdir()
        with self.assertRaisesRegex(ValueError, 'dist holds something'):
            self.run_release()
        (self.root / 'dist' / 'notes').rmdir()
        self.assertEqual(before, self.dist_snapshot())

    def test_release_sequential_targets_accumulate_checksums(self):
        self.run_release(['windows-x64'])
        only_windows = self.dist_snapshot()
        self.assertEqual(sorted(only_windows), ['SHA256SUMS', 'rasterfold-1.0.0-windows-x64.zip'])
        self.assertEqual(only_windows['SHA256SUMS'].decode().count('\n'), 1)
        self.run_release(['linux-x64'])
        both = self.dist_snapshot()
        self.assertEqual(both['rasterfold-1.0.0-windows-x64.zip'], only_windows['rasterfold-1.0.0-windows-x64.zip'])
        lines = both['SHA256SUMS'].decode().splitlines()
        self.assertEqual([line.split('  ')[1] for line in lines], ['rasterfold-1.0.0-linux-x64.zip', 'rasterfold-1.0.0-windows-x64.zip'])
        for line in lines:
            digest, name = line.split('  ')
            self.assertEqual(digest, hashlib.sha256(both[name]).hexdigest())
        # Running everything again is the same as having run it once.
        self.run_release()
        self.assertEqual(both, self.dist_snapshot())

    def test_release_failure_keeps_earlier_packages_and_matching_checksums(self):
        self.run_release()
        good = self.dist_snapshot()
        # A new commit: Windows rebuilds, Linux does not, so the second target of the run is refused.
        newer = 'feedfacecafe' + '1' * 28
        manifest = json.loads((self.root / 'bin/windows-x64/build.json').read_text(encoding='utf-8'))
        manifest['revision'] = newer[:12]
        self.write_manifest('windows-x64', manifest)
        with patch.object(package, 'release_source_state', lambda: (newer, EPOCH + 60)):
            with self.assertRaisesRegex(ValueError, "linux-x64: the build is of revision 'abc123def456'"):
                self.run_release()
        after = self.dist_snapshot()
        self.assertEqual(sorted(after), sorted(good))
        self.assertNotEqual(after['rasterfold-1.0.0-windows-x64.zip'], good['rasterfold-1.0.0-windows-x64.zip'])
        self.assertEqual(after['rasterfold-1.0.0-linux-x64.zip'], good['rasterfold-1.0.0-linux-x64.zip'])
        for line in after['SHA256SUMS'].decode().splitlines():
            digest, name = line.split('  ')
            self.assertEqual(digest, hashlib.sha256(after[name]).hexdigest(), name)
        self.assertFalse(any(name.endswith('.tmp') for name in after))
        # A failure while writing leaves no temporary and no half-written package.
        with patch.object(zipfile.ZipFile, 'writestr', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                self.run_release(['linux-x64'])
        self.assertEqual(after, self.dist_snapshot())

    def test_package_quickstart_names_only_what_each_package_holds(self):
        """PACKAGE.md ships as the package's README.md: every path its commands use exists in the package of the
        platform the command is written for, and it never points at a checkout-only path."""
        text = (REPO / 'PACKAGE.md').read_text(encoding='utf-8')
        for checkout_only in ('bin/linux-x64', 'bin/windows-x64', 'bin\\windows-x64', 'tools/', 'scripts/', 'build/'):
            self.assertNotIn(checkout_only, text)
        blocks = re.findall(r'^(?P<lead>[^\n]*)\n\n```(?P<lang>bash|powershell)\n(?P<body>.*?)```', text, re.S | re.M)
        self.assertGreaterEqual(len(blocks), 8)
        seen = {target: set() for target in package.TARGETS}
        for lead, lang, body in blocks:
            target = 'windows-x64' if lang == 'powershell' else 'linux-x64'
            self.assertIn('Windows' if target == 'windows-x64' else 'Linux', lead, lead)
            members = set(package.target_artifacts(target)) | {'SHA256SUMS', 'LICENSE', 'CHANGELOG.md', 'build.json'}
            for token in re.findall(r'(?<![\w/\\.-])(?:\.[/\\])?((?:bin|lib|include|symbols)[/\\][A-Za-z0-9_.-]+)', body):
                name = token.replace('\\', '/')
                self.assertIn(name, members, f'{name} in a {target} command')
                seen[target].add(name)
            if 'SHA256SUMS' in body:
                seen[target].add('SHA256SUMS')
        self.assertLessEqual({'bin/rasterfold', 'lib/librasterfold.a', 'lib/librasterfold.so', 'SHA256SUMS'}, seen['linux-x64'])
        self.assertLessEqual({'bin/rasterfold.exe', 'bin/rasterfold.dll', 'lib/librasterfold.dll.a', 'lib/rasterfold.lib', 'SHA256SUMS'}, seen['windows-x64'])
        # Every command the quickstart shows exists, and every table path is a package path.
        build_docs = package._load_tool('rf_build_docs_for_quickstart', 'build-docs.py')
        cli = build_docs.parse_cli_help(REPO, 'src/main.c')
        commands = {command for command, _ in cli['commands']}
        used = set(re.findall(r'rasterfold(?:\.exe)? ([a-z]+)', text))
        self.assertLessEqual(used, commands)
        self.assertLessEqual({'version', 'encode', 'inspect', 'verify', 'decode', 'help'}, used)

    def test_release_runs_are_serialized_and_temporaries_are_exclusive(self):
        self.run_release()
        before = self.dist_snapshot()
        dist = self.root / 'dist'
        # A second run while one holds the lock is refused and touches nothing, the lock included.
        lock = dist / package.LOCK_NAME
        lock.write_text('pid 4242\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, r'another release run holds dist/\.release\.lock \(pid 4242\)'):
            self.run_release()
        self.assertEqual(lock.read_text(encoding='utf-8'), 'pid 4242\n')
        lock.unlink()
        self.assertEqual(before, self.dist_snapshot())
        # Two writers of one package never share a temporary file.
        first = package._exclusive_temporary(dist / 'rasterfold-1.0.0-linux-x64.zip')
        second = package._exclusive_temporary(dist / 'rasterfold-1.0.0-linux-x64.zip')
        self.assertNotEqual(first, second)
        self.assertTrue(first.is_file() and second.is_file())
        self.assertTrue(package._is_stale_temporary(first.name, {'rasterfold-1.0.0-linux-x64.zip'}))
        self.assertFalse(package._is_stale_temporary('rasterfold-0.9.0-linux-x64.zip.x.tmp', {'rasterfold-1.0.0-linux-x64.zip'}))
        # Temporaries a dead run left behind are removed by the next run, which holds the lock.
        first.write_bytes(b'half a package')
        self.run_release()
        self.assertEqual(before, self.dist_snapshot())
        self.assertFalse(first.exists() or second.exists())

    def test_release_takes_source_files_from_the_commit_not_the_working_tree(self):
        self.export.stop()
        self.addCleanup(self.export.start)

        def git(*argv):
            return subprocess.run(['git', '-C', str(self.root), *argv], check=True, capture_output=True, text=True).stdout.strip()

        (self.root / '.gitignore').write_text('bin/\ndist/\n', encoding='utf-8')
        git('init', '-q')
        git('config', 'user.email', 'release-test@example.invalid')
        git('config', 'user.name', 'Release Test')
        git('config', 'core.autocrlf', 'false')          # commit the fixture's bytes as written, on every host
        git('add', '-A')
        git('commit', '-q', '-m', 'fixture')
        # A host that converts line endings on checkout must not change what a package holds.
        git('config', 'core.autocrlf', 'true')
        revision = git('rev-parse', 'HEAD')
        for target in package.TARGETS:
            manifest = json.loads((self.root / 'bin' / target / 'build.json').read_text(encoding='utf-8'))
            manifest['revision'] = revision[:12]
            self.write_manifest(target, manifest)
        committed_licence = (self.root / 'LICENSE').read_bytes()
        # The tree is edited after the clean check passed (the check is patched to the committed state).
        (self.root / 'LICENSE').write_text('edited after the check', encoding='utf-8')
        (self.root / 'CHANGELOG.md').write_text('edited after the check', encoding='utf-8')
        (self.root / 'VERSION').write_text('9.9.9\n', encoding='utf-8')
        with patch.object(package, 'release_source_state', lambda: (revision, EPOCH)):
            self.run_release(['linux-x64'])
        with zipfile.ZipFile(self.root / 'dist' / 'rasterfold-1.0.0-linux-x64.zip') as z:
            self.assertEqual(z.read('rasterfold-1.0.0-linux-x64/LICENSE'), committed_licence)
            self.assertEqual(z.read('rasterfold-1.0.0-linux-x64/CHANGELOG.md'), b'# Changelog\n')
        self.assertFalse((self.root / 'dist' / 'rasterfold-9.9.9-linux-x64.zip').exists())
        # export_revision itself: tracked files only, from the object store.
        out = self.root / 'exported'
        out.mkdir()
        package.export_revision(revision, out)
        self.assertEqual((out / 'LICENSE').read_bytes(), committed_licence)
        self.assertEqual((out / 'VERSION').read_text(encoding='utf-8'), '1.0.0\n')
        self.assertFalse((out / 'bin').exists())

    def test_release_source_state_reads_a_real_git_tree(self):
        self.state.stop()
        self.addCleanup(self.state.start)

        def git(*argv):
            subprocess.run(['git', '-C', str(self.root), *argv], check=True, capture_output=True)

        with self.assertRaisesRegex(ValueError, 'needs a git checkout'):
            package.release_source_state()
        git('init', '-q')
        git('config', 'user.email', 'release-test@example.invalid')
        git('config', 'user.name', 'Release Test')
        git('add', '-A')
        git('commit', '-q', '-m', 'fixture')
        revision, epoch = package.release_source_state()
        self.assertRegex(revision, r'^[0-9a-f]{40}$')
        self.assertGreater(epoch, 1700000000)
        (self.root / 'untracked.txt').write_text('x', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, r'working tree is not clean \(1 path\(s\), first: \?\? untracked.txt\)'):
            package.release_source_state()
        (self.root / 'untracked.txt').unlink()
        (self.root / 'VERSION').write_text('1.0.1\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'working tree is not clean'):
            package.release_source_state()

    def test_release_package_offline_docs_resolve_without_network(self):
        """The real offline documentation, packaged and extracted: every relative link resolves inside the
        extracted tree and every page is reachable from docs/index.md."""
        self.offline.stop()
        self.addCleanup(self.offline.start)
        build_docs = package._load_tool('rf_build_docs_for_test', 'build-docs.py')
        real_docs = build_docs.render_offline_docs(REPO, REVISION)
        with patch.object(package, 'offline_docs', lambda revision, source_root: dict(real_docs)):
            self.run_release(['linux-x64'])
        out = self.root / 'extracted'
        with zipfile.ZipFile(self.root / 'dist' / 'rasterfold-1.0.0-linux-x64.zip') as z:
            z.extractall(out)
        tree = out / 'rasterfold-1.0.0-linux-x64'
        report = build_docs.check_offline_tree(tree)
        self.assertTrue((tree / 'README.md').is_file())
        self.assertEqual(len(report['pages']), 16)
        self.assertGreater(report['links'], 30)
        self.assertEqual((report['unresolved'], report['unreachable'], report['leftovers']), ([], [], []))
        index = (tree / 'docs/index.md').read_text(encoding='utf-8')
        self.assertFalse(index.startswith('---'))
        self.assertIn('(cli.md)', index)
        benchmarks = (tree / 'docs/benchmarks.md').read_text(encoding='utf-8')
        self.assertNotIn('{%', benchmarks)
        self.assertIn('![encode-time chart](charts/encode-time.svg)', benchmarks)
        self.assertTrue((tree / 'docs/charts/encode-time.svg').is_file())
        self.assertIn(f'https://github.com/rizonesoft/RasterFold/blob/{REVISION}/examples/encode.c', (tree / 'docs/examples.md').read_text(encoding='utf-8'))
        self.assertFalse((tree / 'docs/assets').exists(), 'site-only stylesheets and scripts are not shipped')
        # The build guide's link to the repository's PACKAGE.md points at the package's own README.
        self.assertIn('(../README.md)', (tree / 'docs/installation.md').read_text(encoding='utf-8'))
        # The package's README is link-checked too and must lead to the documentation's entry page.
        readme = (tree / 'README.md').read_text(encoding='utf-8')
        (tree / 'README.md').write_text(readme.replace('(docs/index.md)', '(docs/start.md)'), encoding='utf-8')
        no_entry = build_docs.check_offline_tree(tree)
        self.assertIn('README.md: docs/start.md', no_entry['unresolved'])
        self.assertIn('README.md: does not link docs/index.md', no_entry['leftovers'])
        (tree / 'README.md').write_text(readme, encoding='utf-8')
        # The check fails on a tree whose links no longer close.
        (tree / 'docs/cli.md').unlink()
        broken = build_docs.check_offline_tree(tree)
        self.assertTrue(any('cli.md' in item for item in broken['unresolved']))
        (tree / 'docs/usage.md').write_text('# Usage\n\nSee [the site](/RasterFold/usage.html).\n', encoding='utf-8')
        broken = build_docs.check_offline_tree(tree)
        self.assertTrue(any('usage.md' in item for item in broken['unresolved']))
        self.assertTrue(any('hosted path' in item for item in broken['leftovers']))
        with self.assertRaisesRegex(ValueError, 'full commit id'):
            build_docs.render_offline_docs(REPO, 'abc123')
        # A percent-encoded link names the decoded file.
        encoded = build_docs.offline_link_report({
            'docs/index.md': b'[spaced](hello%20world.md) [plain](plain.md)\n',
            'docs/hello world.md': b'# Hello\n\n[back](index.md) [licence](../LICENSE)\n',
            'docs/plain.md': b'# Plain\n',
        })
        self.assertEqual((encoded['unresolved'], encoded['unreachable'], encoded['links']), ([], [], 4))
        missing = build_docs.offline_link_report({'docs/index.md': b'[spaced](hello%20there.md)\n'})
        self.assertEqual(missing['unresolved'], ['docs/index.md: hello%20there.md'])


FAKE_TOOL = """import os, shutil, subprocess, sys, time
mode, version, pidfile = {mode!r}, {version!r}, {pidfile!r}
command = sys.argv[1]
if command == 'version':
    print('RasterFold codec')
    print('product version: ' + version)
elif command == 'encode':
    if mode == 'hang':
        # Bounded on its own: both processes end within a minute if nothing stops them, and neither writes in a loop.
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        with open(pidfile, 'w') as handle:
            handle.write(str(os.getpid()) + ' ' + str(child.pid))
        time.sleep(60)
    if mode == 'fail':
        print('rasterfold: cannot encode', file=sys.stderr)
        sys.exit(3)
    shutil.copyfile(sys.argv[2], sys.argv[3])
elif command == 'verify':
    if mode == 'orphan':
        # Starts a process that holds none of this command's pipes, then exits successfully straight away.
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(pidfile, 'w') as handle:
            handle.write(str(os.getpid()) + ' ' + str(child.pid))
elif command == 'decode':
    data = open(sys.argv[2], 'rb').read()
    if mode == 'lossy':
        data = data[:-1] + bytes([data[-1] ^ 1])
    open(sys.argv[3], 'wb').write(data)
"""


def process_alive(pid: int) -> bool:
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(0x1000, False, pid)       # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        kernel32.CloseHandle(handle)
        return code.value == 259                                 # STILL_ACTIVE
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
    except OSError:
        return False


class ReleaseVerifier(unittest.TestCase):
    """scripts/verify_release.py on fabricated packages whose `rasterfold` is a script run through the interpreter."""

    VERSION = '1.0.0'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()   # a runner's temp path can be a short alias of the real one
        self.dist = self.root / 'dist'
        self.dist.mkdir()
        self.scratch = self.root / 'scratch'
        self.scratch.mkdir()
        self.pidfile = self.root / 'pids.txt'
        self.target = verify_release.host_target()
        self.top = f'rasterfold-{self.VERSION}-{self.target}'
        self.exe = 'bin/rasterfold.exe' if self.target == 'windows-x64' else 'bin/rasterfold'
        patcher = patch.object(verify_release.tempfile, 'tempdir', str(self.scratch))
        patcher.start()
        self.addCleanup(patcher.stop)

    def build(self, mode='good', version=None, tamper=None, extra_member=None, unlisted=None):
        tool = FAKE_TOOL.format(mode=mode, version=version or self.VERSION, pidfile=str(self.pidfile)).encode()
        files = {self.exe: tool, 'LICENSE': b'licence\n', 'docs/index.md': b'# Docs\n'}
        sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in sorted(files.items()))
        if tamper:
            files[tamper] += b'changed after the checksums were written'
        if unlisted:
            files[unlisted] = b'not in the checksum file'
        zip_path = self.dist / f'{self.top}.zip'
        with zipfile.ZipFile(zip_path, 'w') as archive:
            for name, data in files.items():
                info = zipfile.ZipInfo(f'{self.top}/{name}')
                info.external_attr = (0o755 if name == self.exe else 0o644) << 16
                archive.writestr(info, data)
            archive.writestr(f'{self.top}/SHA256SUMS', sums)
            if extra_member:
                archive.writestr(extra_member, b'outside')
        (self.dist / 'SHA256SUMS').write_text(
            f'{hashlib.sha256(zip_path.read_bytes()).hexdigest()}  {zip_path.name}\n', encoding='utf-8')
        return zip_path

    def verify(self, timeout=30.0):
        lines = []
        verify_release.verify(self.dist, self.target, self.VERSION, timeout, launcher=(sys.executable,),
                              report=lines.append)
        return lines

    def refused(self, needle, timeout=30.0):
        with self.assertRaises(verify_release.Refused) as caught:
            self.verify(timeout)
        self.assertIn(needle, str(caught.exception))
        self.assertEqual(list(self.scratch.iterdir()), [], 'the temporary directory is removed after a refusal')

    def test_a_working_package_passes_and_leaves_nothing_behind(self):
        self.build()
        lines = self.verify()
        self.assertEqual(lines[-1], f'verify-release: OK {self.top}.zip')
        self.assertIn('3 file(s) match', lines[1])
        self.assertIn(f'product version: {self.VERSION}', lines[2])
        self.assertIn('round-trip a 61x37 image exactly', lines[3])
        self.assertEqual(list(self.scratch.iterdir()), [])

    def test_a_zip_that_differs_from_its_checksum_is_refused(self):
        zip_path = self.build()
        with open(zip_path, 'ab') as handle:
            handle.write(b'x')
        self.refused(f'dist: {self.top}.zip has SHA-256')

    def test_a_missing_or_unlisted_zip_is_refused(self):
        zip_path = self.build()
        zip_path.unlink()
        self.refused('is listed in SHA256SUMS and missing')
        self.build()
        (self.dist / 'SHA256SUMS').write_text(f'{"0" * 64}  other.zip\n', encoding='utf-8')
        self.refused(f'SHA256SUMS does not list {self.top}.zip')
        (self.dist / 'SHA256SUMS').write_text('not a checksum line\n', encoding='utf-8')
        self.refused('line 1 is not')
        (self.dist / 'SHA256SUMS').unlink()
        self.refused('holds no SHA256SUMS')

    def test_a_package_file_that_differs_from_the_package_checksums_is_refused(self):
        self.build(tamper='LICENSE')
        self.refused(f'{self.top}: LICENSE has SHA-256')

    def test_a_package_file_missing_from_the_package_checksums_is_refused(self):
        self.build(unlisted='bin/extra.bin')
        self.refused('1 file(s) are not in its SHA256SUMS, first bin/extra.bin')

    def test_a_member_outside_the_package_directory_is_refused_before_anything_is_written(self):
        for member in ('../escape.txt', 'other-top/file.txt', f'{self.top}/../../escape.txt'):
            with self.subTest(member=member):
                self.build(extra_member=member)
                self.refused(f'is not under {self.top}/')
                self.assertFalse((self.root / 'escape.txt').exists())
                self.assertFalse((self.scratch.parent / 'escape.txt').exists())

    def test_an_executable_that_reports_another_version_is_refused(self):
        self.build(version='1.0.1')
        self.refused('version reports "product version: 1.0.1", not "product version: 1.0.0"')

    def test_a_failing_command_and_a_lossy_round_trip_are_refused(self):
        self.build(mode='fail')
        self.refused('encode exited 3: rasterfold: cannot encode')
        self.build(mode='lossy')
        self.refused('differs from the image it encoded')

    def test_a_command_past_its_timeout_is_refused_with_its_whole_process_tree_ended(self):
        import time
        self.build(mode='hang')
        started = time.monotonic()
        self.refused(f'{Path(self.exe).name} encode in.ppm did not finish within 3 s; its process tree was ended',
                     timeout=3.0)
        self.assertLess(time.monotonic() - started, 45, 'the refusal does not wait for the command to end by itself')
        parent, child = (int(pid) for pid in self.pidfile.read_text().split())
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and (process_alive(parent) or process_alive(child)):
            time.sleep(0.1)
        self.assertFalse(process_alive(parent), 'the packaged command was ended')
        self.assertFalse(process_alive(child), 'the process it started was ended with it')

    def test_a_process_left_behind_by_a_command_that_exited_in_time_is_ended(self):
        import time
        self.build(mode='orphan')
        lines = self.verify()
        self.assertEqual(lines[-1], f'verify-release: OK {self.top}.zip')
        parent, child = (int(pid) for pid in self.pidfile.read_text().split())
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and process_alive(child):
            time.sleep(0.1)
        self.assertFalse(process_alive(parent))
        self.assertFalse(process_alive(child), 'a process the command started does not outlive the verification')
        self.assertEqual(list(self.scratch.iterdir()), [])

    def test_the_command_line_exits_zero_or_one_with_the_reason(self):
        self.build()
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = verify_release.main(['--dist', str(self.dist), '--version', '9.9.9'])
        self.assertEqual(code, 1)
        self.assertIn('verify-release: REFUSED SHA256SUMS does not list rasterfold-9.9.9-', err.getvalue())
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(verify_release.main(['--dist', str(self.dist), '--timeout', '0']), 1)
        self.assertIn('--timeout must be positive', err.getvalue())

    def test_the_test_image_is_what_read_p6_reads_back(self):
        image, pixels = verify_release.test_image()
        self.assertEqual(verify_release.read_p6(image), (61, 37, pixels))
        self.assertEqual(verify_release.read_p6(b'P6 # note\n2 1\n255\n' + bytes(6)), (2, 1, bytes(6)))
        for bad in (b'P5\n1 1\n255\n\0', b'P6\n1 1\n65535\n' + bytes(6), b'P6\n2 2\n255\n' + bytes(3), b'P6\n1'):
            with self.assertRaises(verify_release.Refused):
                verify_release.read_p6(bad)


WORKFLOWS = REPO / '.github' / 'workflows'
PUBLISH_NEEDS = {'resolve', 'build-gates', 'plan-gates', 'package', 'verify'}


def workflow_jobs(text: str) -> dict[str, str]:
    """A workflow's jobs as {name: block text}, read by indentation (the repository has no YAML dependency)."""
    _, _, body = text.partition('\njobs:\n')
    jobs: dict[str, list[str]] = {}
    current = None
    for line in body.split('\n'):
        named = re.fullmatch(r'  ([A-Za-z0-9_-]+):', line)
        if named:
            current = named.group(1)
            jobs[current] = []
        elif current is not None:
            jobs[current].append(line)
    return {name: '\n'.join(lines) for name, lines in jobs.items()}


def release_workflow_problems(release: str, build: str, plan: str) -> list[str]:
    """What is wrong with the release workflow's contract; empty when it holds."""
    problems = []
    for name, text in (('release.yml', release), ('build.yml', build), ('plan.yml', plan)):
        for line in text.split('\n'):
            used = re.match(r'\s*(?:- )?uses:\s*(\S+)', line)
            if used and not used.group(1).startswith('./') and not re.fullmatch(r'[\w.-]+/[\w./-]+@[0-9a-f]{40}', used.group(1)):
                problems.append(f'{name}: action not pinned by a 40-character commit: {used.group(1)}')
    head, _, _ = release.partition('\njobs:\n')
    if not re.search(r'^on:\n  push:\n    tags:\n      - "rasterfold-v\*"\n  workflow_dispatch:\n', head, re.M):
        problems.append('release.yml: triggers are not exactly a rasterfold-v* tag push and workflow_dispatch')
    if 'branches' in head or 'pull_request' in head or 'schedule' in head:
        problems.append('release.yml: has a trigger other than the tag push and the manual dry run')
    if not re.search(r'^permissions:\n  contents: read\n', head, re.M):
        problems.append('release.yml: the workflow default is not contents: read')
    jobs = workflow_jobs(release)
    for name, block in jobs.items():
        if name != 'publish' and re.search(r':\s*write\b', block):
            problems.append(f'release.yml: job {name} holds a write permission')
        if name != 'publish' and 'gh release' in block:
            problems.append(f'release.yml: job {name} touches releases')
    publish = jobs.get('publish', '')
    needs = re.search(r'^    needs: \[([^\]]*)\]$', publish, re.M)
    listed = {n.strip() for n in needs.group(1).split(',')} if needs else set()
    for missing in sorted(PUBLISH_NEEDS - listed):
        problems.append(f'release.yml: publication does not need {missing}')
    condition = re.search(r'^    if: (.*)$', publish, re.M)
    if not condition or "github.event_name == 'push'" not in condition.group(1) or '||' in condition.group(1) \
            or 'always()' in condition.group(1) or "needs.resolve.outputs.publish == 'true'" not in condition.group(1):
        problems.append('release.yml: publication can run for something other than a pushed release tag')
    if not re.search(r'^    permissions:\n      contents: write\n', publish, re.M) or len(re.findall(r':\s*write\b', publish)) != 1:
        problems.append('release.yml: publication does not hold exactly contents: write')
    if 'gh release create "$TAG"' not in publish or '--verify-tag' not in publish:
        problems.append('release.yml: publication does not create the release for the verified tag')
    for gate, file in (('build-gates', 'build.yml'), ('plan-gates', 'plan.yml')):
        block = jobs.get(gate, '')
        if f'uses: ./.github/workflows/{file}' not in block or 'ref: ${{ needs.resolve.outputs.sha }}' not in block:
            problems.append(f'release.yml: {gate} does not call {file} on the release commit')
    if 'python3 scripts/package.py --release --target all' not in jobs.get('package', ''):
        problems.append('release.yml: the package job does not write the release packages')
    verify = jobs.get('verify', '')
    if 'scripts/verify_release.py' not in verify or not {'linux-x64', 'windows-x64'} <= set(re.findall(r'target: (\S+)', verify)):
        problems.append('release.yml: the packages are not run on both platforms')
    resolve = jobs.get('resolve', '')
    if 'merge-base --is-ancestor "$TARGET_SHA" origin/main' not in resolve or 'scripts/version.py --tag "$TAG"' not in resolve:
        problems.append('release.yml: the tag is not held to VERSION and to main')
    for name, text in (('build.yml', build), ('plan.yml', plan)):
        if not re.search(r'^  workflow_call:\n    inputs:\n      ref:\n', text, re.M):
            problems.append(f'{name}: is not callable with a ref')
    if "group: gates-${{ github.workflow }}-${{ inputs.ref && github.run_id || github.ref }}" not in build:
        problems.append('build.yml: called gates do not get a concurrency group of their own run')
    for suite in ('test_package.py', 'test_version.py'):
        if f'-p {suite}' not in workflow_jobs(build).get('linux', ''):
            problems.append(f'build.yml: the Linux job does not run tests/{suite}')
    return problems


class ReleaseWorkflow(unittest.TestCase):
    """The release workflow's contract, read from the workflow files as text."""

    def setUp(self):
        self.release = (WORKFLOWS / 'release.yml').read_text(encoding='utf-8')
        self.build = (WORKFLOWS / 'build.yml').read_text(encoding='utf-8')
        self.plan = (WORKFLOWS / 'plan.yml').read_text(encoding='utf-8')

    def problems(self, release=None, build=None, plan=None):
        return release_workflow_problems(release or self.release, build or self.build, plan or self.plan)

    def changed(self, old, new, text=None):
        text = self.release if text is None else text
        self.assertEqual(text.count(old), 1, old)
        return text.replace(old, new)

    def test_the_committed_workflows_hold_the_contract(self):
        self.assertEqual(self.problems(), [])
        self.assertEqual(sorted(workflow_jobs(self.release)),
                         ['build-gates', 'package', 'plan-gates', 'publish', 'resolve', 'verify'])

    def test_an_unpinned_action_is_refused(self):
        unpinned = self.changed('actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9 # v6.1.0', 'actions/cache@v6')
        self.assertIn('release.yml: action not pinned by a 40-character commit: actions/cache@v6', self.problems(unpinned))
        build = self.build.replace('actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a', 'actions/upload-artifact@main')
        self.assertTrue(any(p.startswith('build.yml: action not pinned') for p in self.problems(build=build)))

    def test_a_write_permission_outside_publication_is_refused(self):
        widened = self.changed('permissions:\n  contents: read\n\nconcurrency:', 'permissions:\n  contents: write\n\nconcurrency:')
        self.assertIn('release.yml: the workflow default is not contents: read', self.problems(widened))
        job = self.changed('  package:\n    needs: resolve\n', '  package:\n    needs: resolve\n    permissions:\n      contents: write\n')
        self.assertIn('release.yml: job package holds a write permission', self.problems(job))
        more = self.changed('    permissions:\n      contents: write\n', '    permissions:\n      contents: write\n      packages: write\n')
        self.assertIn('release.yml: publication does not hold exactly contents: write', self.problems(more))

    def test_publication_without_a_gate_the_package_or_a_verification_is_refused(self):
        for dropped in sorted(PUBLISH_NEEDS):
            with self.subTest(dropped=dropped):
                kept = ', '.join(n for n in ('resolve', 'build-gates', 'plan-gates', 'package', 'verify') if n != dropped)
                text = self.changed('    needs: [resolve, build-gates, plan-gates, package, verify]', f'    needs: [{kept}]')
                self.assertIn(f'release.yml: publication does not need {dropped}', self.problems(text))
        one_platform = self.changed('          - os: windows-2025\n            target: windows-x64\n            python: python\n', '')
        self.assertIn('release.yml: the packages are not run on both platforms', self.problems(one_platform))
        copied = self.changed('    uses: ./.github/workflows/plan.yml\n', '    runs-on: ubuntu-24.04\n')
        self.assertIn('release.yml: plan-gates does not call plan.yml on the release commit', self.problems(copied))

    def test_publication_on_a_dispatch_is_refused(self):
        wanted = 'release.yml: publication can run for something other than a pushed release tag'
        condition = "    if: github.event_name == 'push' && needs.resolve.outputs.publish == 'true'"
        for replacement in ("    if: needs.resolve.outputs.publish == 'true'",
                            "    if: github.event_name == 'push' || needs.resolve.outputs.publish == 'true'",
                            "    if: always() && github.event_name == 'push' && needs.resolve.outputs.publish == 'true'",
                            ''):
            with self.subTest(replacement=replacement):
                self.assertIn(wanted, self.problems(self.changed(condition, replacement)))
        branch = self.changed('  push:\n    tags:\n      - "rasterfold-v*"\n', '  push:\n    branches: [main]\n    tags:\n      - "rasterfold-v*"\n')
        self.assertTrue(self.problems(branch))
        untied = self.changed('          if ! git merge-base --is-ancestor "$TARGET_SHA" origin/main; then', '          if false; then')
        self.assertIn('release.yml: the tag is not held to VERSION and to main', self.problems(untied))

    def test_the_gate_workflows_stay_callable_and_run_the_new_suites(self):
        self.assertIn('build.yml: is not callable with a ref', self.problems(build=self.build.replace('  workflow_call:\n', '  workflow_run:\n')))
        self.assertIn('plan.yml: is not callable with a ref', self.problems(plan=self.plan.replace('  workflow_call:\n', '  workflow_run:\n')))
        shared = self.build.replace('inputs.ref && github.run_id || github.ref', 'inputs.ref || github.ref')
        self.assertIn('build.yml: called gates do not get a concurrency group of their own run', self.problems(build=shared))
        for suite in ('test_package.py', 'test_version.py'):
            build = self.build.replace(f'          python3 -m unittest discover -s tests -p {suite} -v\n', '')
            self.assertIn(f'build.yml: the Linux job does not run tests/{suite}', self.problems(build=build))


if __name__ == '__main__':
    unittest.main(verbosity=2)
