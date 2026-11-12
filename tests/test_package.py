"""Development and release packages must contain every required, hash-verified artifact."""
import contextlib
import hashlib
import importlib.util
import io
import json
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


class Packages(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
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
        (self.root / 'CHANGELOG.md').write_text('# Changelog\n', encoding='utf-8')
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
        git('add', '-A')
        git('commit', '-q', '-m', 'fixture')
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

if __name__ == '__main__':
    unittest.main(verbosity=2)
