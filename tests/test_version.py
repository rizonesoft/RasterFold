"""One product version source: VERSION and everything derived from it."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
VERSION_PY = ROOT / 'scripts' / 'version.py'


def run_version(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(VERSION_PY), '--root', str(root), *args],
        capture_output=True, text=True,
    )


class VersionSource(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for rel in ('VERSION', 'src/product_version.h', 'docs/facts.json'):
            dest = self.root / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / rel, dest)

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def test_check_agrees(self):
        result = run_version(self.root, '--check')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_seeded_header_mismatch_fails(self):
        self.write('src/product_version.h', '#define RF_PRODUCT_VERSION "9.9.9"\n')
        result = run_version(self.root, '--check')
        self.assertEqual(result.returncode, 1)
        self.assertIn('product_version.h', result.stdout)

    def test_write_regenerates_header(self):
        self.write('VERSION', '2.3.4-rc.1\n')
        result = run_version(self.root, '--write')
        self.assertEqual(result.returncode, 0, result.stdout)
        header = (self.root / 'src/product_version.h').read_text(encoding='utf-8')
        self.assertIn('#define RF_PRODUCT_VERSION "2.3.4-rc.1"', header)
        self.assertIn('#define RF_PRODUCT_VERSION_PRERELEASE "rc.1"', header)
        self.assertEqual(run_version(self.root, '--check').returncode, 0)

    def test_malformed_version_fails(self):
        self.write('VERSION', '1.0\n')
        self.assertEqual(run_version(self.root, '--check').returncode, 1)

    def test_tag_matrix(self):
        cases = [
            ('rasterfold-v1.0.0', 0),
            ('rasterfold-v1.0.1', 1),
            ('rasterfold-v1.0.0-rc.1', 1),  # VERSION carries no prerelease
            ('rasterfold-v1.0.0+build', 1),  # build metadata refused
            ('v1.0.0', 1),
            ('rasterfold-v1.0', 1),
            ('rasterfold-v01.0.0', 1),
        ]
        for tag, want in cases:
            with self.subTest(tag=tag):
                self.assertEqual(run_version(self.root, '--tag', tag).returncode, want)
        self.write('VERSION', '1.1.0-rc.1\n')
        self.write('src/product_version.h', (ROOT / 'src/product_version.h').read_text(encoding='utf-8').replace('1.0.0', '1.1.0-rc.1'))
        self.assertEqual(run_version(self.root, '--tag', 'rasterfold-v1.1.0-rc.1').returncode, 0)
        self.assertEqual(run_version(self.root, '--tag', 'rasterfold-v1.1.0').returncode, 1)

    def git(self, *args: str) -> None:
        subprocess.run(['git', '-C', str(self.root), *args], check=True, capture_output=True, text=True)

    def test_release_notes_matrix(self):
        head = '# Changelog\n\nIntro.\n\n'
        unreleased = '## Unreleased\n\n- Pending change.\n\n'
        older = '## 0.9.0\n\n- Older change.\n'
        version = (self.root / 'VERSION').read_text(encoding='utf-8').strip()
        for heading in (f'## {version}', f'## [{version}]', f'## [{version}] - 2026-11-12', f'## {version} - 2026-11-12'):
            with self.subTest(heading=heading):
                self.write('CHANGELOG.md', head + unreleased + f'{heading}\n\n- First change.\n- Second change.\n\n' + older)
                result = run_version(self.root, '--release-notes')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, '- First change.\n- Second change.\n')
                result = run_version(self.root, '--release-notes', '--unreleased')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout, '- Pending change.\n')
        # No section for the version: a release is refused, the dry run still has its notes.
        self.write('CHANGELOG.md', head + unreleased + older)
        result = run_version(self.root, '--release-notes')
        self.assertEqual(result.returncode, 1)
        self.assertIn(f'CHANGELOG.md: lacks a section for version {version}', result.stdout)
        self.assertEqual(run_version(self.root, '--release-notes', '--unreleased').returncode, 0)
        # Another version's section never stands in, however close its name.
        self.write('CHANGELOG.md', head + f'## {version}0\n\n- Wrong.\n\n## {version}-rc.1\n\n- Wrong.\n')
        self.assertEqual(run_version(self.root, '--release-notes').returncode, 1)
        # No Unreleased section, an empty section, a repeated heading, a missing file.
        self.write('CHANGELOG.md', head + f'## {version}\n\n- Only.\n')
        result = run_version(self.root, '--release-notes', '--unreleased')
        self.assertEqual(result.returncode, 1)
        self.assertIn('CHANGELOG.md: lacks an Unreleased section', result.stdout)
        self.write('CHANGELOG.md', head + f'## {version}\n\n' + older)
        result = run_version(self.root, '--release-notes')
        self.assertEqual(result.returncode, 1)
        self.assertIn('is empty', result.stdout)
        self.write('CHANGELOG.md', head + f'## {version}\n\n- A.\n\n## {version}\n\n- B.\n')
        result = run_version(self.root, '--release-notes')
        self.assertEqual(result.returncode, 1)
        self.assertIn('appears twice', result.stdout)
        (self.root / 'CHANGELOG.md').unlink()
        result = run_version(self.root, '--release-notes')
        self.assertEqual(result.returncode, 1)
        self.assertIn('unreadable changelog', result.stdout)
        # --unreleased alone is a usage error.
        self.assertEqual(run_version(self.root, '--unreleased').returncode, 2)

    def test_release_notes_of_this_repository(self):
        """Holds before a release (notes are the Unreleased section) and after one (the version's section, with the
        Unreleased heading possibly empty): at least one of the two prints notes, and neither prints a heading."""
        printed = []
        for extra in ((), ('--unreleased',)):
            result = subprocess.run([sys.executable, str(VERSION_PY), '--release-notes', *extra],
                                    capture_output=True, text=True)
            if result.returncode == 0:
                self.assertTrue(result.stdout.strip())
                self.assertNotIn('\n## ', '\n' + result.stdout)
                printed.append(result.stdout)
            else:
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('CHANGELOG.md: ', result.stdout)
        self.assertTrue(printed, 'the changelog has notes neither for VERSION nor under Unreleased')

    def test_release_notes_after_the_documented_release_preparation(self):
        """docs/building.md: the Unreleased entries move under a `## <version>` heading and the Unreleased heading
        stays. This repository's own changelog, prepared that way, yields the same notes for the version."""
        version = (self.root / 'VERSION').read_text(encoding='utf-8').strip()
        changelog = (ROOT / 'CHANGELOG.md').read_text(encoding='utf-8')
        pending = subprocess.run([sys.executable, str(VERSION_PY), '--release-notes', '--unreleased'],
                                 capture_output=True, text=True)
        if pending.returncode != 0:
            self.skipTest('this changelog has no pending Unreleased entries to move')
        self.assertEqual(changelog.count('\n## Unreleased\n'), 1)
        self.write('CHANGELOG.md', changelog.replace('\n## Unreleased\n', f'\n## Unreleased\n\n## {version}\n'))
        released = run_version(self.root, '--release-notes')
        self.assertEqual(released.returncode, 0, released.stdout)
        self.assertEqual(released.stdout, pending.stdout)
        emptied = run_version(self.root, '--release-notes', '--unreleased')
        self.assertEqual(emptied.returncode, 1)
        self.assertIn('the section "## Unreleased" is empty', emptied.stdout)

    def test_dev_labels(self):
        self.git('init', '-q')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'user.name', 'Fixture')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        clean = run_version(self.root, '--dev')
        self.assertEqual(clean.returncode, 0, clean.stderr)
        revision = subprocess.run(
            ['git', '-C', str(self.root), 'rev-parse', '--short=12', 'HEAD'],
            capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(clean.stdout.strip(), f'1.0.0-dev+{revision}')
        self.write('VERSION', '1.0.0\n\n')  # same content, dirty tree via extra file
        (self.root / 'dirty.txt').write_text('dirty\n', encoding='utf-8')
        dirty = run_version(self.root, '--dev')
        self.assertEqual(dirty.stdout.strip(), f'1.0.0-dev+{revision}.dirty')

    def test_dev_label_unknown_revision(self):
        empty = Path(self.temp.name) / 'elsewhere'
        empty.mkdir()
        (empty / 'VERSION').write_text('1.0.0\n', encoding='utf-8')
        result = run_version(empty, '--dev')
        self.assertEqual(result.stdout.strip(), '1.0.0-dev+unknown')

    def test_build_paths_verify_header_first(self):
        sys.path.insert(0, str(ROOT / 'scripts'))
        import build as build_mod
        self.write('src/product_version.h', '#define RF_PRODUCT_VERSION "9.9.9"\n')
        with mock.patch.object(build_mod, 'ROOT', self.root):
            with self.assertRaises(SystemExit):
                build_mod.build('linux-x64', 1, False)
            with self.assertRaises(SystemExit):
                build_mod.build_pgo_gen(1)

    def test_cli_and_manifest_agree(self):
        target = 'windows-x64' if os.name == 'nt' else 'linux-x64'
        exe = ROOT / f'bin/{target}/bin' / ('rasterfold.exe' if os.name == 'nt' else 'rasterfold')
        manifest = ROOT / f'bin/{target}/build.json'
        if not exe.is_file() or not manifest.is_file():
            self.skipTest(f'{target} build not present')
        want = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
        out = subprocess.run([str(exe), '--version'], capture_output=True, text=True, check=True).stdout
        self.assertIn(f'product version: {want}', out)
        self.assertIn('format version written: 3', out)
        self.assertIn('api version: 1.0', out)
        recorded = json.loads(manifest.read_text(encoding='utf-8'))
        self.assertEqual(recorded['product_version'], want)
        out2 = subprocess.run([str(exe), 'version'], capture_output=True, text=True, check=True).stdout
        self.assertEqual(out, out2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
