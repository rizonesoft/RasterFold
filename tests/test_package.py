"""Development packages must contain every required, hash-verified artifact."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
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
        manifest = {'schema': 2, 'target': target, 'artifacts': artifacts}
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
                dest = self.root / 'build/packages' / f'rasterfold-{target}-dev.zip'
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
        dest = self.root / 'build/packages/rasterfold-linux-x64-dev.zip'
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
        dest = self.root / 'build/packages/rasterfold-linux-x64-dev.zip'
        with zipfile.ZipFile(dest) as archive:
            archived_manifest = json.loads(archive.read('rasterfold-linux-x64/build.json').decode('utf-8'))
            self.assertEqual(archived_manifest, original_manifest)
            archived_header = archive.read('rasterfold-linux-x64/include/rasterfold.h')
            self.assertEqual(archived_header, b'include/rasterfold.h')
            self.assertEqual(
                hashlib.sha256(archived_header).hexdigest(),
                archived_manifest['artifacts']['include/rasterfold.h'],
            )

    def test_unsafe_manifest_path_refused(self):
        for name in ('../outside', '/absolute', 'lib/../outside', 'lib\\outside', 'C:/outside'):
            with self.subTest(name=name):
                manifest = self.fixture('linux-x64')
                manifest['artifacts'][name] = '0' * 64
                self.write_manifest('linux-x64', manifest)
                with self.assertRaisesRegex(ValueError, 'invalid artifact path'):
                    package.package('linux-x64')


if __name__ == '__main__':
    unittest.main(verbosity=2)
