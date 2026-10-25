"""Regression proofs for documentation gates, index isolation and narrow repairs."""
import contextlib
import shutil
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('drift', REPO / 'scripts/drift-check.py')
drift = importlib.util.module_from_spec(spec)
spec.loader.exec_module(drift)

build_docs_spec = importlib.util.spec_from_file_location('build_docs', REPO / 'scripts/build-docs.py')
build_docs = importlib.util.module_from_spec(build_docs_spec)
build_docs_spec.loader.exec_module(build_docs)


class DriftTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.git('init', '-q')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'user.name', 'Fixture')
        self.write('.gitattributes', '* text=auto\n*.json text eol=lf\n*.md text eol=lf\n*.py text eol=lf\n*.h text eol=lf\n*.csv text eol=lf\n')
        self.write('scripts/todo-claims.py', (REPO / 'scripts/todo-claims.py').read_text(encoding='utf-8'))
        self.write('src/version.h', '#define VERSION 2u\n')
        self.write('README.md', '# Guide\n\nAuthored words stay here.\n\n<!-- drift:begin version -->\nVersion 2.\n<!-- drift:end version -->\n')
        self.manifest = {
            'schema': 1, 'coverage': [{'pattern': '*', 'role': 'current', 'reason': 'fixture'}],
            'facts': {'version': {'kind': 'regex', 'source': 'src/version.h', 'pattern': r'^#define VERSION (\d+)u$', 'type': 'int'}},
            'blocks': {'version': {'path': 'README.md', 'template': 'Version {version}.'}},
            'reviews': {}, 'assertions': [], 'generators': [], 'historical_links': {},
            'site_sources': ['README.md', 'src/*'], 'forbidden_site_paths': ['.codex', 'todo', 'agents.md']}
        self.save()
        self.write('docs/facts.json', '{\n  "version": 2\n}\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'Initial fixture')

    def git(self, *args):
        return subprocess.check_output(['git', *args], cwd=self.root, stderr=subprocess.PIPE).decode().strip()

    def write(self, path, text):
        p = self.root / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding='utf-8', newline='\n')

    def save(self):
        self.write('docs/drift.json', json.dumps(self.manifest, indent=2) + '\n')

    def check(self):
        c = drift.Checker(self.root)
        c.load(); c.derive(); c.documents()
        return c

    def kinds(self, c):
        return {e['kind'] for e in c.errors}

    def test_clean_tree_and_facts(self):
        self.assertEqual([], self.check().errors)

    def test_changed_source_detects_fact_and_block_drift(self):
        self.write('src/version.h', '#define VERSION 3u\n')
        c = self.check()
        self.assertEqual({'README.md', 'docs/facts.json'}, set(c.repairs))
        self.assertIn('generated', self.kinds(c))

    def test_repair_preserves_prose_and_is_idempotent(self):
        self.write('src/version.h', '#define VERSION 3u\n')
        c = self.check(); c.fix()
        self.assertIn('Authored words stay here.', (self.root / 'README.md').read_text(encoding='utf-8'))
        self.assertIn('Version 3.', (self.root / 'README.md').read_text(encoding='utf-8'))
        self.assertEqual([], self.check().errors)
        self.assertEqual({}, self.check().repairs)

    def test_compare_before_write_refuses_new_edit(self):
        self.write('src/version.h', '#define VERSION 3u\n')
        c = self.check()
        self.write('README.md', 'Concurrent edit\n')
        with self.assertRaisesRegex(ValueError, 'changed during repair'):
            c.fix()
        self.assertEqual('Concurrent edit\n', (self.root / 'README.md').read_text(encoding='utf-8'))

    def test_duplicate_or_missing_blocks_fail(self):
        text = (self.root / 'README.md').read_text(encoding='utf-8')
        self.write('README.md', text + text)
        self.assertIn('block', self.kinds(self.check()))
        self.write('README.md', '# Without a required block\n')
        self.assertIn('block', self.kinds(self.check()))

    def test_unclosed_blocks_and_facts_fail(self):
        self.write('note.txt', '<!-- drift:fact version = 2\n<!-- drift:commit nope -->\n')
        c = self.check()
        self.assertIn('fact', self.kinds(c)); self.assertIn('commit', self.kinds(c))

    def test_claims_beyond_todo_and_fenced_examples(self):
        self.write('note.md', '<!-- claim: exists missing.txt -->\n')
        self.assertIn('claim', self.kinds(self.check()))
        self.write('note.md', '```md\n<!-- claim: exists missing.txt -->\n```\n')
        self.assertNotIn('claim', self.kinds(self.check()))

    def test_same_day_staleness_is_not_auto_accepted(self):
        self.manifest['reviews'] = {'README.md': {'evidence': '2026-10-25 fixture source review', 'sources': {'src/version.h': drift.digest((self.root / 'src/version.h').read_bytes()), 'README.md': drift.digest((self.root / 'README.md').read_bytes())}}}
        self.save()
        self.write('src/version.h', '#define VERSION 3u\n')
        c = self.check(); c.fix()
        self.assertIn('stale', self.kinds(self.check()))
        self.assertEqual('2026-10-25 fixture source review', json.loads((self.root / 'docs/drift.json').read_text(encoding='utf-8'))['reviews']['README.md']['evidence'])

    def test_authored_document_change_invalidates_its_review_receipt(self):
        document = self.root / 'README.md'
        self.manifest['reviews'] = {'README.md': {'evidence': '2026-10-25 author-attested source review', 'sources': {'README.md': drift.digest(document.read_bytes())}}}
        self.save()
        self.write('README.md', document.read_text(encoding='utf-8') + 'New unreviewed claim.\n')
        c = self.check(); c.fix()
        self.assertIn('stale', self.kinds(self.check()))

    def test_explicit_review_requires_evidence(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, drift.main(['--root', str(self.root), '--review', 'README.md']))
            self.assertEqual(2, drift.main(['--root', str(self.root), '--review', 'README.md', '--evidence', 'Undated evidence string']))

    def test_new_supported_files_are_checked(self):
        self.write('new/info.txt', '<!-- drift:fact version = 9 -->\n')
        c = self.check()
        self.assertIn('new/info.txt', [i['path'] for i in c.inventory])
        self.assertIn('fact', self.kinds(c))

    def test_coverage_rule_missing_fails(self):
        self.manifest['coverage'] = []
        self.save()
        self.assertIn('coverage', self.kinds(self.check()))

    def test_structured_information_syntax(self):
        for path, text in [('wrong.json', '{bad'), ('wrong.toml', 'a = ['), ('wrong.svg', '<svg>')]:
            self.write(path, text)
        c = self.check()
        self.assertEqual(3, sum(e['kind'] == 'syntax' for e in c.errors))

    def test_legacy_notice_encoding_preserved(self):
        path = self.root / 'NOTICE.txt'; data = 'Original © notice'.encode('cp1252'); path.write_bytes(data)
        self.manifest['encodings'] = {'NOTICE.txt': 'cp1252'}; self.save()
        c = self.check(); c.fix()
        self.assertEqual([], c.errors); self.assertEqual(data, path.read_bytes())

    def test_links_anchors_html_and_duplicate_headings(self):
        self.write('target.md', '# One\n# One\n')
        self.write('links.md', '[Valid](target.md#one-1)\n<a href="target.md#missing">Bad</a>\n[Missing](gone.txt)\n')
        kinds = self.kinds(self.check())
        self.assertIn('anchor', kinds); self.assertIn('link', kinds)

    def test_commit_reference_and_path(self):
        head = self.git('rev-parse', 'HEAD')
        self.write('history.md', f'<!-- drift:commit {head} src/version.h -->\n')
        self.assertEqual([], self.check().errors)
        self.write('history.md', f'<!-- drift:commit {head} missing.h -->\n')
        self.assertIn('commit', self.kinds(self.check()))

    def test_historical_measurement_not_rewritten(self):
        self.manifest['coverage'].insert(0, {'pattern': 'history.md', 'role': 'historical', 'reason': 'dated evidence'})
        self.manifest['assertions'] = [{'pattern': '*.md', 'regex': r'Version (\d+)', 'fact': 'version'}]
        self.save(); self.write('history.md', 'Version 1 measured in 2020.\n')
        c = self.check(); c.fix()
        self.assertEqual('Version 1 measured in 2020.\n', (self.root / 'history.md').read_text(encoding='utf-8'))
        self.assertEqual([], c.errors)

    def test_unique_csv_binding_and_changed_value(self):
        self.write('data.csv', 'image,codec,bytes\na,rf,123\n')
        self.manifest['facts']['bytes'] = {'kind': 'csv', 'source': 'data.csv', 'where': {'image': 'a', 'codec': 'rf'}, 'column': 'bytes', 'type': 'int'}
        self.save(); c = self.check(); self.assertEqual(123, c.facts['bytes'])
        self.write('data.csv', 'image,codec,bytes\na,rf,123\na,rf,123\n')
        with self.assertRaisesRegex(ValueError, 'expected one CSV row'): self.check()

    def test_paths_cannot_escape_or_follow_symlinks(self):
        c = self.check()
        for path in ('../outside', '/tmp/outside', 'docs/../../outside'):
            with self.assertRaises(ValueError): c.path(path)
        try: (self.root / 'alias').symlink_to(self.root / 'docs', target_is_directory=True)
        except OSError: self.skipTest('host cannot create symlinks')
        with self.assertRaises(ValueError): c.path('alias/facts.json')

    def site(self):
        self.write('.gitignore', '_site/\n')
        self.write('_site/index.html', '<html><a href="/RasterFold/page.html#ok">Page</a></html>')
        self.write('_site/page.html', '<h1 id="ok">Title</h1>')
        c = self.check()
        self.write('_site/drift-build.json', json.dumps(c.site_receipt('_site')))
        return c

    def test_built_site_and_source_receipt(self):
        c = self.site(); c.site('_site', '/RasterFold/')
        self.assertEqual([], c.errors)
        self.write('src/version.h', '#define VERSION 3u\n')
        c.site('_site', '/RasterFold/')
        self.assertIn('site', self.kinds(c))

    def test_built_output_tamper_detected(self):
        c = self.site(); self.write('_site/page.html', '<h1 id="ok">Wrong text</h1>')
        c.site('_site', '/RasterFold/')
        self.assertIn('site', self.kinds(c))

    def test_built_site_links_and_private_paths(self):
        c = self.site()
        self.write('_site/index.html', '<a href="/wrong/">Wrong root</a><a href="page.html#missing">Missing</a>')
        self.write('_site/.codex/private.html', 'private')
        c.site('_site', '/RasterFold/')
        self.assertTrue({'site-link', 'anchor', 'site-private'} <= self.kinds(c))

    def test_missing_site_is_not_a_pass(self):
        c = self.check(); c.site('_site', '/RasterFold/')
        self.assertIn('site', self.kinds(c))

    def test_commit_message_uses_index_not_unstaged_facts(self):
        self.write('message.txt', 'Commit\n\nDrift-Fact: version=2\n')
        self.write('docs/facts.json', '{"version": 99}\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--commit-message', str(self.root / 'message.txt')]))
        self.write('message.txt', 'Commit\n\nDrift-Fact: version=99\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--commit-message', str(self.root / 'message.txt')]))

    def test_exported_index_checks_only_staged_bytes(self):
        self.write('README.md', (self.root / 'README.md').read_text(encoding='utf-8').replace('Version 2.', 'Version 8.'))
        self.git('add', 'README.md')
        self.write('README.md', (self.root / 'README.md').read_text(encoding='utf-8').replace('Version 8.', 'Version 2.'))
        self.assertEqual([], self.check().errors)
        exported = self.root / '.git' / 'fixture-index'; exported.mkdir()
        self.git('checkout-index', '-a', '--prefix=' + str(exported) + '/')
        c = drift.Checker(exported, self.root / '.git'); c.load(); c.derive(); c.documents()
        self.assertIn('generated', self.kinds(c))

    def test_stray_backtick_cannot_hide_next_paragraph_contract(self):
        self.write('note.md', 'Press the ` key.\n\n<!-- drift:fact version = 99 -->\n\nUse `help` now.\n')
        self.assertIn('fact', self.kinds(self.check()))

    def test_receipts_require_self_binding_and_portable_byte_policy(self):
        self.write('src/exports.def', 'EXPORTS\n')
        sources = {path: drift.digest((self.root / path).read_bytes()) for path in ('README.md', 'src/exports.def')}
        self.manifest['reviews'] = {'README.md': {'evidence': '2026-10-25 source audit', 'sources': sources}}; self.save()
        self.assertIn('eol', self.kinds(self.check()))
        attrs = self.root / '.gitattributes'; self.write('.gitattributes', attrs.read_text(encoding='utf-8') + '*.def text eol=lf\n')
        self.assertEqual([], self.check().errors)
        del sources['README.md']; self.save()
        self.assertIn('review', self.kinds(self.check()))

    def test_old_crlf_checkout_cannot_poison_a_renewed_receipt(self):
        sources = {path: drift.digest((self.root / path).read_bytes()) for path in ('README.md', 'src/version.h')}
        self.manifest['reviews'] = {'README.md': {'evidence': '2026-10-25 source audit', 'sources': sources}}; self.save()
        self.assertEqual([], self.check().errors)
        header = self.root / 'src/version.h'; header.write_bytes(header.read_bytes().replace(b'\n', b'\r\n'))
        self.assertTrue(any(e['kind'] == 'eol' and 'working bytes contain CRLF' in e['detail'] for e in self.check().errors))
        before = (self.root / 'docs/drift.json').read_bytes()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, drift.main(['--root', str(self.root), '--review', 'README.md', '--evidence', '2026-10-25 Attempted renewal']))
        self.assertEqual(before, (self.root / 'docs/drift.json').read_bytes())

    def test_git_partial_commit_uses_temporary_index(self):
        if not shutil.which('sh'): self.skipTest('POSIX shell is required for Git hooks')
        self.write('scripts/drift-check.py', (REPO / 'scripts/drift-check.py').read_text(encoding='utf-8'))
        self.git('add', 'scripts/drift-check.py'); self.git('commit', '-qm', 'Install fixture checker')
        self.git('config', 'core.hooksPath', str(REPO / 'tools/githooks'))
        self.write('bad-unrelated.md', '<!-- drift:fact version = 99 -->\n'); self.git('add', 'bad-unrelated.md')
        original = (self.root / 'README.md').read_text(encoding='utf-8')
        self.write('README.md', original.replace('Version 2.', 'Version 99.'))
        result = subprocess.run(['git', 'commit', '--only', '-qm', 'Bad selected path', '--', 'README.md'], cwd=self.root, capture_output=True, timeout=60)
        self.assertNotEqual(0, result.returncode)
        self.write('README.md', original + 'A reviewed fixture change.\n')
        result = subprocess.run(['git', 'commit', '--only', '-qm', 'Good selected path', '--', 'README.md'], cwd=self.root, capture_output=True, timeout=60)
        self.assertEqual(0, result.returncode, (result.stdout + result.stderr).decode(errors='replace'))
        self.assertIn('bad-unrelated.md', self.git('diff', '--cached', '--name-only'))
        self.assertNotIn('bad-unrelated.md', self.git('ls-tree', '--name-only', 'HEAD'))

    def test_inline_example_does_not_hide_real_fact_and_fenced_blocks_are_examples(self):
        self.write('note.md', '`<!-- drift:fact example = 9 -->` <!-- drift:fact version = 9 -->\n```md\n<!-- drift:begin example -->\nExample\n<!-- drift:end example -->\n```\n`[Example](missing.md)`\n')
        c = self.check()
        self.assertEqual({'fact'}, self.kinds(c))
        self.assertEqual(1, len(c.errors))

    def test_crlf_legacy_block_repair_preserves_authored_bytes(self):
        self.manifest['blocks']['legacy'] = {'path': 'notice.txt', 'template': 'Version {version}.'}
        self.manifest['encodings'] = {'notice.txt': 'cp1252'}; self.save()
        old = 'Original © text.\r\n<!-- drift:begin legacy -->\r\nVersion 1.\r\n<!-- drift:end legacy -->\r\n'.encode('cp1252')
        (self.root / 'notice.txt').write_bytes(old)
        c = self.check(); c.fix()
        self.assertEqual(old.replace(b'Version 1.', b'Version 2.'), (self.root / 'notice.txt').read_bytes())
        self.assertEqual([], self.check().errors)

    def test_review_changes_only_requested_receipt_and_records_evidence(self):
        self.write('other.md', 'Other document.\n')
        self.manifest['reviews'] = {name: {'evidence': '2026-10-20 old evidence', 'sources': {'src/version.h': '0' * 64, name: drift.digest((self.root / name).read_bytes())}} for name in ('README.md', 'other.md')}
        self.save()
        before = self.manifest['reviews']['other.md']
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--review', 'README.md', '--evidence', '2026-10-25 Source read with named test evidence']))
        after = json.loads((self.root / 'docs/drift.json').read_text(encoding='utf-8'))['reviews']
        self.assertEqual(before, after['other.md'])
        self.assertEqual('2026-10-25 Source read with named test evidence', after['README.md']['evidence'])
        self.assertEqual(drift.digest((self.root / 'src/version.h').read_bytes()), after['README.md']['sources']['src/version.h'])

    def test_missing_indexed_document_is_a_reported_error(self):
        (self.root / 'README.md').unlink()
        self.assertIn('missing', self.kinds(self.check()))

    def test_fix_keeps_manual_errors_failing(self):
        self.write('src/version.h', '#define VERSION 3u\n')
        self.write('note.txt', '<!-- drift:fact version = 99 -->\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--fix']))
        self.assertIn('Version 3.', (self.root / 'README.md').read_text(encoding='utf-8'))
        self.assertIn('99', (self.root / 'note.txt').read_text(encoding='utf-8'))

    def test_repair_generator_failure_is_not_success(self):
        self.manifest['generators'] = [{'script': 'scripts/fail.py', 'check': [], 'fix': []}]
        self.write('scripts/fail.py', 'raise SystemExit(1)\n'); self.save()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, drift.main(['--root', str(self.root), '--fix']))

    def test_site_receipt_refuses_private_artifact_before_writing(self):
        self.site(); (self.root / '_site/drift-build.json').unlink()
        self.write('_site/.codex/private.html', 'private')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--site-dir', '_site', '--write-site-receipt']))
        self.assertFalse((self.root / '_site/drift-build.json').exists())

    def test_clean_site_receipt_and_readonly_recheck(self):
        self.site(); (self.root / '_site/drift-build.json').unlink()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', '_site', '--write-site-receipt']))
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', '_site']))

    def test_commit_range_uses_own_source_and_detects_false_history(self):
        base = self.git('rev-parse', 'HEAD')
        self.git('commit', '--allow-empty', '-qm', 'Old source\n\nDrift-Fact: version=2')
        good = self.git('rev-parse', 'HEAD')
        self.write('src/version.h', '#define VERSION 3u\n'); self.check().fix()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--commit-range', base + '..' + good]))
        self.git('add', '.'); self.git('commit', '-qm', 'Bad declaration\n\nDrift-Fact: version=2')
        bad = self.git('rev-parse', 'HEAD')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--fix', '--commit-range', base + '..' + bad]))

    def test_commit_declaration_cannot_trust_forged_facts_file(self):
        self.write('docs/facts.json', '{"version": 99}'); self.git('add', 'docs/facts.json')
        self.write('message.txt', 'Commit\n\nDrift-Fact: version=99\n')
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, drift.main(['--root', str(self.root), '--commit-message', str(self.root / 'message.txt')]))

    def hook(self, name, *args):
        if not shutil.which('sh'):
            self.skipTest('POSIX shell is required for Git hook execution')
        self.write('scripts/drift-check.py', (REPO / 'scripts/drift-check.py').read_text(encoding='utf-8'))
        self.git('add', 'scripts/drift-check.py')
        return subprocess.run(['sh', str(REPO / 'tools/githooks' / name), *args], cwd=self.root, capture_output=True, text=True, timeout=60)

    def test_staged_fact_read_ignores_unstaged_symlink(self):
        header = self.root / 'src/version.h'
        header.unlink()
        try: header.symlink_to(self.root / 'README.md')
        except OSError: self.skipTest('host cannot create symlinks')
        self.write('message.txt', 'Commit\n\nDrift-Fact: version=2\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--commit-message', str(self.root / 'message.txt')]))

    def test_real_precommit_hook_refuses_bad_index_with_clean_working_copy(self):
        original = (self.root / 'README.md').read_text(encoding='utf-8')
        self.write('README.md', original.replace('Version 2.', 'Version 8.')); self.git('add', 'README.md')
        self.write('README.md', original)
        result = self.hook('pre-commit')
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn('staged documentation drift', result.stderr)
        self.git('add', 'README.md')
        result = self.hook('pre-commit')
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_real_commit_message_hook_checks_facts_and_keeps_human_author(self):
        message = 'Change\n\nCo-Authored-By: Codex <ai@example.invalid>\nCo-Authored-By: Human <human@example.invalid>\nDrift-Fact: version=99\n'
        self.write('message.txt', message)
        result = self.hook('commit-msg', str(self.root / 'message.txt'))
        self.assertNotEqual(0, result.returncode)
        self.assertNotIn('Codex', (self.root / 'message.txt').read_text(encoding='utf-8'))
        self.assertIn('Human', (self.root / 'message.txt').read_text(encoding='utf-8'))
        self.write('message.txt', 'Change\n\nDrift-Fact: version=2\n')
        self.assertEqual(0, self.hook('commit-msg', str(self.root / 'message.txt')).returncode)

    def test_live_repository_facts_and_secondary_source_matches(self):
        live_manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
        live_facts = drift.derive_facts(live_manifest, lambda rel: (REPO / rel).read_text(encoding='utf-8'))
        committed_facts = json.loads((REPO / 'docs/facts.json').read_text(encoding='utf-8'))
        self.assertEqual(committed_facts, live_facts)
        core_keys = {
            'product_name', 'format_name', 'format_version', 'header_bytes',
            'max_dimension', 'max_pixels', 'max_file_bytes', 'api_major', 'api_minor',
            'exit_ok', 'exit_ok_meaning', 'exit_usage', 'exit_usage_meaning',
            'exit_io', 'exit_io_meaning', 'exit_unsupported', 'exit_unsupported_meaning',
            'exit_malformed', 'exit_malformed_meaning', 'exit_limit', 'exit_limit_meaning',
            'cmd_encode', 'cmd_decode', 'cmd_inspect', 'input_image_ext', 'encoded_ext',
            'measurement_reference_bytes',
        }
        self.assertTrue(core_keys <= set(live_facts))
        self.assertEqual(set(live_manifest['facts']), set(live_facts))
        for mutated, target, old, new in [
            ('format_version', 'src/rf.h', '#define RF_VERSION 2u', '#define RF_VERSION 3u'),
            ('exit_limit', 'include/rasterfold.h', '#define RF_ERR_LIMIT 5', '#define RF_ERR_LIMIT 6'),
            ('cmd_inspect', 'src/main.c', 'wide_eq(pos[0], "inspect")', 'wide_eq(pos[0], "check")'),
        ]:
            def read_mutated(rel, target=target, old=old, new=new):
                text = (REPO / rel).read_text(encoding='utf-8')
                return text.replace(old, new, 1) if rel == target else text
            with self.assertRaisesRegex(ValueError, mutated):
                drift.derive_facts(live_manifest, read_mutated)

    def test_live_assertions_and_readme_claims_and_bindings(self):
        live_manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
        self.manifest['assertions'] = live_manifest['assertions']
        self.manifest['facts'].update({
            'format_version': {'kind': 'regex', 'source': 'src/version.h', 'pattern': r'^#define VERSION (\d+)u$', 'type': 'int'},
            'api_major': {'kind': 'regex', 'source': 'src/api.h', 'pattern': r'^#define API_MAJOR (\d+)u$', 'type': 'int'},
            'api_minor': {'kind': 'regex', 'source': 'src/api.h', 'pattern': r'^#define API_MINOR (\d+)u$', 'type': 'int'},
            'max_dimension': {'kind': 'regex', 'source': 'src/api.h', 'pattern': r'^#define MAX_DIM (\d+)u$', 'type': 'int'},
            'max_pixels': {'kind': 'regex', 'source': 'src/api.h', 'pattern': r'^#define MAX_PIX (\d+)u$', 'type': 'int'},
            'max_file_bytes': {'kind': 'regex', 'source': 'src/api.h', 'pattern': r'^#define MAX_FILE (\d+)u$', 'type': 'int'},
        })
        self.write('src/api.h', '#define API_MAJOR 1u\n#define API_MINOR 0u\n#define MAX_DIM 32768u\n#define MAX_PIX 33554432u\n#define MAX_FILE 268435456u\n')
        self.manifest['coverage'].insert(0, {'pattern': 'docs/history.md', 'role': 'historical', 'reason': 'historical fixture'})
        self.manifest['coverage'].insert(0, {'pattern': 'todo/plan.md', 'role': 'plan', 'reason': 'plan fixture'})
        self.save()
        self.check().fix()
        base_readme = (self.root / 'README.md').read_text(encoding='utf-8')
        for sample in (
            'Current codec: format 3.',
            'Current codec: format 2.5.',
            'C API 2.0 is current.',
            'C API 1.9 is current.',
            'C API 1.0.5 is current.',
            'Maximum dimension: 16384.',
            'Maximum dimension: 32768.5.',
            'Maximum image: 1000000 pixels.',
            'Maximum file size: 999999.',
            'Maximum file size: 268435456 KiB.',
            'Maximum file size: 268435456.0 KiB.',
            'Maximum file size: 256 MB.',
            'Maximum file size: 268435456.5 bytes.',
            'Maximum file size: 12,34 bytes.',
            'Maximum file size: 268435456 widgets.',
        ):
            self.write('README.md', base_readme + '\n' + sample + '\n')
            c = self.check()
            self.assertIn('prose-fact', self.kinds(c), sample)
        for valid_file_size in (
            'Maximum file size: 268435456.',
            'Maximum file size: 268,435,456 bytes.',
            'Maximum file size: 268435456 bytes.',
            'Maximum file size: 256 MiB.',
            'Maximum file size: 256.0 MiB.',
            'Maximum file size: 0.25 GiB.',
            'Maximum dimension: 32,768.',
        ):
            self.write('README.md', base_readme + '\n' + valid_file_size + '\n')
            self.assertEqual([], self.check().errors, valid_file_size)
        self.write('README.md', base_readme + '\n```md\nCurrent codec: format 9, C API 9.9. Maximum dimension: 1.\n```\n')
        self.write('docs/history.md', 'Format version 1 and API 0.9 with a 1024-pixel dimension limit.\n')
        self.write('todo/plan.md', 'Planned format version 3 and API 2.0.\n')
        self.assertEqual([], self.check().errors)
        self.write('README.md', base_readme + '\n<!-- drift:fact format_version = 2\n')
        self.assertIn('fact', self.kinds(self.check()))
        self.write('README.md', base_readme + '\n<!-- claim: exists no_such_file.c -->\n')
        self.assertIn('claim', self.kinds(self.check()))
        self.write('README.md', base_readme + '\n<!-- claim: exists src/version.h\n')
        self.assertIn('claim', self.kinds(self.check()))

    def test_build_docs_generator_is_mandatory_in_live_manifest(self):
        live_manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
        gen = next(g for g in live_manifest['generators'] if g['script'] == 'scripts/build-docs.py')
        self.assertNotIn('planned_owner', gen)
        self.assertEqual(['--check'], gen['check'])
        self.assertEqual(['--write'], gen['fix'])
        self.manifest['generators'] = [gen]
        self.save()
        c = self.check()
        c.generators()
        self.assertIn('generator', self.kinds(c))

    def test_build_docs_mirror_stage_allowlist_and_destination_confinement(self):
        for rel in (
            'docs/site-content.json',
            'docs/_config.yml',
            'docs/_includes/components/footer.html',
            'docs/Gemfile',
            'docs/index.md',
            'docs/getting-started.md',
            'docs/benchmarks.md',
            'README.md',
            'spec/FOLD.md',
            'docs/research.md',
            'assets/brand/icon.svg',
            'assets/brand/cover.svg',
            'docs/measurements.md',
            'docs/building.md',
            'docs/library.md',
            'docs/vision.md',
            'include/rasterfold.h',
            'research/README.md',
            'benchmarks/corpus.csv',
            'benchmarks/results.csv',
        ):
            self.write(rel, (REPO / rel).read_text(encoding='utf-8'))
        (self.root / 'examples').mkdir(parents=True, exist_ok=True)
        self.write('examples/encode_ppm.c', '/* fixture */\n')
        self.write('todo/secret.md', '# Internal plan\n')
        self.write('AGENTS.md', '# Internal agent instructions\n')
        self.write('docs/reviews/r1.md', '# Internal review\n')
        self.write('docs/phase-runs/run.md', '# Internal run\n')
        self.write('.intelligence/lessons.md', '# Internal intelligence\n')

        written = build_docs.write_generated_pages(self.root)
        expected_written = sorted(
            [
                'docs/format.md',
                'docs/research-summary.md',
                'docs/usage.md',
                *(f'docs/_includes/generated/charts/{n}.svg' for n in build_docs.CHART_NAMES),
                *(f'docs/_includes/generated/tables/{n}.md' for n in build_docs.CHART_NAMES),
            ]
        )
        self.assertEqual(expected_written, written)
        self.assertEqual([], build_docs.check_generated_pages(self.root))

        # Editing spec/FOLD.md without rerunning makes --check fail (exit 1).
        orig_spec = (self.root / 'spec/FOLD.md').read_text(encoding='utf-8')
        self.write('spec/FOLD.md', orig_spec + '\nEdited spec line.\n')
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--check']))
        self.write('spec/FOLD.md', orig_spec)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, build_docs.main(['--root', str(self.root), '--check']))

        # Staging refuses unsafe destinations without modifying existing files.
        sentinel = (self.root / 'docs/index.md').read_bytes()
        (self.root / 'build').mkdir(parents=True, exist_ok=True)
        self.write('build/not-a-dir', 'keep me')
        for bad_dest in ('.', 'docs', 'build', 'build/../docs', '../outside', '/tmp/outside-stage', 'build/not-a-dir'):
            with self.assertRaises(ValueError):
                build_docs.stage_site(self.root, bad_dest)
        self.assertEqual(sentinel, (self.root / 'docs/index.md').read_bytes())
        self.assertEqual('keep me', (self.root / 'build/not-a-dir').read_text(encoding='utf-8'))
        try:
            (self.root / 'build/symlink-dest').symlink_to(self.root / 'docs', target_is_directory=True)
            with self.assertRaises(ValueError):
                build_docs.stage_site(self.root, 'build/symlink-dest')
            self.assertEqual(sentinel, (self.root / 'docs/index.md').read_bytes())
            self.write('build/victim/site/keep.txt', 'victim data')
            (self.root / 'build/alias').symlink_to(self.root / 'build/victim', target_is_directory=True)
            with self.assertRaises(ValueError):
                build_docs.stage_site(self.root, 'build/alias/site')
            self.assertEqual('victim data', (self.root / 'build/victim/site/keep.txt').read_text(encoding='utf-8'))
        except OSError:
            pass

        # Reference-style Markdown links, HTML attributes with whitespace around =, and query strings are rebased cleanly across both authored and mirrored pages.
        orig_readme = (self.root / 'README.md').read_text(encoding='utf-8')
        self.write(
            'README.md',
            orig_readme.replace(
                '[The build guide](docs/building.md)',
                '[The build guide](docs/building.md?plain=1#L2) and [Build Ref][build] and <a href = "docs/building.md?html=1#L4">HTML Build</a>\n\n[build]: docs/building.md?raw=1#L3',
            ),
        )
        build_docs.write_generated_pages(self.root)
        mirrored_usage = (self.root / 'docs/usage.md').read_text(encoding='utf-8')
        self.assertIn('[The build guide](building.md?plain=1#L2)', mirrored_usage)
        self.assertIn('[build]: building.md?raw=1#L3', mirrored_usage)
        self.assertIn('<a href = "building.md?html=1#L4">', mirrored_usage)
        self.write(
            'docs/index.md',
            sentinel.decode('utf-8')
            + '\nSee [Build Ref][build], <a href = "building.md?plain=1#L2">HTML Build</a>, and [Header](../include/rasterfold.h?plain=1#L1).\n\n[build]: building.md?raw=1#L3\n',
        )
        build_docs.stage_site(self.root, 'build/site-source')
        rebased_probe = (self.root / 'build/site-source/index.md').read_text(encoding='utf-8')
        self.assertIn('[build]: https://github.com/rizonesoft/RasterFold/blob/main/docs/building.md?raw=1#L3', rebased_probe)
        self.assertIn('<a href = "https://github.com/rizonesoft/RasterFold/blob/main/docs/building.md?plain=1#L2">', rebased_probe)
        self.assertIn('(https://github.com/rizonesoft/RasterFold/blob/main/include/rasterfold.h?plain=1#L1)', rebased_probe)
        rebased_usage = (self.root / 'build/site-source/usage.md').read_text(encoding='utf-8')
        self.assertIn('[The build guide](https://github.com/rizonesoft/RasterFold/blob/main/docs/building.md?plain=1#L2)', rebased_usage)
        self.assertIn('[build]: https://github.com/rizonesoft/RasterFold/blob/main/docs/building.md?raw=1#L3', rebased_usage)
        self.assertIn('<a href = "https://github.com/rizonesoft/RasterFold/blob/main/docs/building.md?html=1#L4">', rebased_usage)
        self.write('README.md', orig_readme)
        build_docs.write_generated_pages(self.root)
        self.write('docs/index.md', sentinel.decode('utf-8'))

        # Repeated staging removes obsolete files from build/site-source and stages only the allowlist.
        self.write('build/site-source/obsolete.md', 'stale file\n')
        staged = build_docs.stage_site(self.root, 'build/site-source')
        self.assertFalse((self.root / 'build/site-source/obsolete.md').exists())
        self.assertEqual(
            sorted(
                [
                    '_config.yml',
                    '_includes/components/footer.html',
                    *(f'_includes/generated/charts/{n}.svg' for n in build_docs.CHART_NAMES),
                    *(f'_includes/generated/tables/{n}.md' for n in build_docs.CHART_NAMES),
                    'assets/brand/cover.svg',
                    'assets/brand/icon.svg',
                    'benchmarks.md',
                    'format.md',
                    'getting-started.md',
                    'index.md',
                    'research-summary.md',
                    'usage.md',
                ]
            ),
            staged,
        )
        staged_index = (self.root / 'build/site-source/index.md').read_text(encoding='utf-8')
        self.assertIn('/RasterFold/getting-started.html', staged_index)
        self.assertIn('/RasterFold/assets/brand/cover.svg', staged_index)

        # Allowlist refuses private/internal paths in site-content.json or page links without clobbering existing stage.
        orig_sc = json.loads((self.root / 'docs/site-content.json').read_text(encoding='utf-8'))
        for forbidden_src in ('todo/secret.md', 'AGENTS.md', 'docs/reviews/r1.md', 'docs/phase-runs/run.md', '.intelligence/lessons.md'):
            mutated = json.loads(json.dumps(orig_sc))
            mutated['pages'].append({'id': 'bad', 'source': forbidden_src, 'staged': 'bad.md', 'title': 'Bad', 'nav_order': 99, 'kind': 'authored'})
            self.write('docs/site-content.json', json.dumps(mutated, indent=2) + '\n')
            with self.assertRaises(ValueError):
                build_docs.stage_site(self.root, 'build/site-source')
        self.write('docs/site-content.json', json.dumps(orig_sc, indent=2) + '\n')
        self.write('docs/index.md', sentinel.decode('utf-8') + '\n[Leaked](../todo/secret.md)\n')
        with self.assertRaises(ValueError):
            build_docs.stage_site(self.root, 'build/site-source')
        self.assertEqual(staged_index, (self.root / 'build/site-source/index.md').read_text(encoding='utf-8'))

    def test_all_three_generators_reject_stale_outputs_and_leave_files_unchanged(self):
        for d in ('todo', 'research', 'spec', 'assets', 'benchmarks'):
            shutil.copytree(REPO / d, self.root / d, dirs_exist_ok=True)
        for s in ('todo-graph.py', 'build-docs.py', 'todo-claims.py'):
            self.write(f'scripts/{s}', (REPO / 'scripts' / s).read_text(encoding='utf-8'))
        for p in (REPO / 'docs').iterdir():
            if p.name in ('reviews', 'phase-runs', 'design', 'drift.json', 'facts.json'):
                continue
            if p.is_dir():
                shutil.copytree(p, self.root / 'docs' / p.name, dirs_exist_ok=True)
            else:
                self.write(f'docs/{p.name}', p.read_text(encoding='utf-8'))
        self.write('include/rasterfold.h', (REPO / 'include/rasterfold.h').read_text(encoding='utf-8'))
        self.write('tests/test_correctness.py', (REPO / 'tests/test_correctness.py').read_text(encoding='utf-8'))
        (self.root / 'examples').mkdir(parents=True, exist_ok=True)
        self.write('examples/encode_ppm.c', '/* fixture */\n')
        # Keep README.md matching the live repository for build-docs.py while keeping the fixture block for drift.json.
        live_readme = (REPO / 'README.md').read_text(encoding='utf-8')
        self.write('README.md', live_readme + '\n<!-- drift:begin version -->\nVersion 2.\n<!-- drift:end version -->\n')
        build_docs.write_generated_pages(self.root)

        live_manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
        self.manifest['coverage'] = [
            {'pattern': 'todo/*', 'role': 'template', 'reason': 'fixture'},
            {'pattern': 'docs/*', 'role': 'template', 'reason': 'fixture'},
            {'pattern': 'README.md', 'role': 'template', 'reason': 'fixture'},
            {'pattern': 'spec/*', 'role': 'template', 'reason': 'fixture'},
            {'pattern': 'research/*', 'role': 'template', 'reason': 'fixture'},
            {'pattern': 'benchmarks/*', 'role': 'data', 'reason': 'fixture'},
            {'pattern': '*', 'role': 'current', 'reason': 'fixture'},
        ]
        self.manifest['blocks'] = {}
        self.manifest['generators'] = live_manifest['generators']
        self.save()

        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(0, drift.main(['--root', str(self.root)]), out.getvalue())

        for script_rel, target_rel, expected_substr, mutator in (
            (
                'scripts/build-docs.py',
                'docs/format.md',
                'docs/format.md: generated file differs from source',
                lambda t: t + '\n<!-- seeded stale output -->\n',
            ),
            (
                'research/milestones.py',
                'todo/05-milestones/TODO-01-beat-jxl-and-webp.md',
                'todo/05-milestones/TODO-01-beat-jxl-and-webp.md is stale',
                lambda t: t + '\n<!-- seeded stale output -->\n',
            ),
            (
                'scripts/todo-graph.py',
                'todo/implementation-plan.md',
                'todo/implementation-plan.md is stale',
                lambda t: t.replace('| [x] | `D08 T01 §1` |', '| [ ] | `D08 T01 §1` |', 1),
            ),
        ):
            target_path = self.root / target_rel
            orig_text = target_path.read_text(encoding='utf-8')
            stale_text = mutator(orig_text)
            self.assertNotEqual(orig_text, stale_text, script_rel)
            self.write(target_rel, stale_text)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = drift.main(['--root', str(self.root)])
            self.assertEqual(1, rc, script_rel)
            output = buf.getvalue()
            self.assertIn(f'generator: {script_rel}:', output)
            self.assertIn(expected_substr, output)
            # --check must never mutate or silently regenerate the stale output on disk.
            self.assertEqual(stale_text, target_path.read_text(encoding='utf-8'))
            self.write(target_rel, orig_text)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, drift.main(['--root', str(self.root)]))

        # Planned outputs (docs/cli.md, docs/api.md) are rejected if created before promotion to mandatory outputs.
        self.write('docs/cli.md', '# Unregistered CLI reference\n')
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(1, drift.main(['--root', str(self.root)]))
        self.assertIn('planned output docs/cli.md (D08 T03 §1) exists before being promoted to outputs', buf.getvalue())
        (self.root / 'docs/cli.md').unlink()

        # Read-only guard catches a generator --check that creates a planned output (docs/api.md) or mutates a prior generator's output (docs/research.md) while exiting 0.
        orig_build_docs = (self.root / 'scripts/build-docs.py').read_text(encoding='utf-8')
        orig_research = (self.root / 'docs/research.md').read_text(encoding='utf-8')
        try:
            self.write(
                'scripts/build-docs.py',
                'import pathlib, sys\n'
                'if "--check" in sys.argv:\n'
                '    pathlib.Path("docs/api.md").write_text("# Sneaky API\\n", encoding="utf-8")\n'
                'sys.exit(0)\n',
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(1, drift.main(['--root', str(self.root)]))
            self.assertIn('planned output docs/api.md (D08 T03 §2) exists before being promoted to outputs', buf.getvalue())
            self.assertIn('--check modified docs/api.md; check mode must be read-only', buf.getvalue())
            if (self.root / 'docs/api.md').exists():
                (self.root / 'docs/api.md').unlink()

            self.write(
                'scripts/build-docs.py',
                'import pathlib, sys\n'
                'if "--check" in sys.argv:\n'
                '    pathlib.Path("docs/api.md").mkdir(parents=True, exist_ok=True)\n'
                'sys.exit(0)\n',
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(1, drift.main(['--root', str(self.root)]))
            self.assertIn('planned output docs/api.md (D08 T03 §2) exists before being promoted to outputs', buf.getvalue())
            self.assertIn('--check modified docs/api.md; check mode must be read-only', buf.getvalue())
            if (self.root / 'docs/api.md').is_dir():
                shutil.rmtree(self.root / 'docs/api.md')

            self.write(
                'scripts/build-docs.py',
                'import pathlib, sys\n'
                'if "--check" in sys.argv:\n'
                '    p = pathlib.Path("docs/research.md")\n'
                '    p.write_text(p.read_text(encoding="utf-8") + "\\n<!-- cross-generator write -->\\n", encoding="utf-8")\n'
                'sys.exit(0)\n',
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(1, drift.main(['--root', str(self.root)]))
            self.assertIn('generator: scripts/build-docs.py: --check modified docs/research.md; check mode must be read-only', buf.getvalue())
            self.write('docs/research.md', orig_research)

            orig_mode = (self.root / 'docs/format.md').stat().st_mode
            self.write(
                'scripts/build-docs.py',
                'import os, sys\n'
                'if "--check" in sys.argv:\n'
                '    os.chmod("docs/format.md", 0o755)\n'
                'sys.exit(0)\n',
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(1, drift.main(['--root', str(self.root)]))
            self.assertIn('generator: scripts/build-docs.py: --check modified docs/format.md; check mode must be read-only', buf.getvalue())
            (self.root / 'docs/format.md').chmod(orig_mode)

            self.write(
                'scripts/build-docs.py',
                'import os, sys\n'
                'if "--check" in sys.argv:\n'
                '    st = os.stat("docs")\n'
                '    os.utime("docs", ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000_000))\n'
                'sys.exit(0)\n',
            )
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(1, drift.main(['--root', str(self.root)]))
            self.assertIn('generator: scripts/build-docs.py: --check modified docs; check mode must be read-only', buf.getvalue())
        finally:
            self.write('scripts/build-docs.py', orig_build_docs)
            self.write('docs/research.md', orig_research)

        # --fix repairs all three stale generator outputs deterministically.
        originals = {
            rel: (self.root / rel).read_text(encoding='utf-8')
            for rel in ('docs/format.md', 'todo/05-milestones/TODO-01-beat-jxl-and-webp.md', 'todo/implementation-plan.md')
        }
        self.write('docs/format.md', originals['docs/format.md'] + '\n<!-- stale for fix -->\n')
        self.write('todo/05-milestones/TODO-01-beat-jxl-and-webp.md', originals['todo/05-milestones/TODO-01-beat-jxl-and-webp.md'] + '\n<!-- stale for fix -->\n')
        self.write('todo/implementation-plan.md', originals['todo/implementation-plan.md'].replace('| [x] | `D08 T01 §1` |', '| [ ] | `D08 T01 §1` |', 1))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--fix']))
        for rel, orig_text in originals.items():
            self.assertEqual(orig_text, (self.root / rel).read_text(encoding='utf-8'))

    def test_comparison_charts_tables_and_self_test_mutations(self):
        for rel in (
            'docs/site-content.json',
            'docs/_config.yml',
            'docs/_includes/components/footer.html',
            'docs/index.md',
            'docs/getting-started.md',
            'docs/benchmarks.md',
            'README.md',
            'spec/FOLD.md',
            'docs/research.md',
            'assets/brand/icon.svg',
            'assets/brand/cover.svg',
            'benchmarks/corpus.csv',
            'benchmarks/results.csv',
        ):
            self.write(rel, (REPO / rel).read_text(encoding='utf-8'))

        build_docs.write_generated_pages(self.root)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(0, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn('7 chart(s) and 7 table(s) match CSV data', out.getvalue())

        # Verify tp (10 overlapping images) vs tp-only (5 exclusive CSV rows) and N/A on retained jxl-e9 sets.
        corpus_table = (self.root / 'docs/_includes/generated/tables/corpus-size-ratio.md').read_text(encoding='utf-8')
        self.assertIn('| tp | 10 |', corpus_table)
        self.assertIn('| tp-only | 5 |', corpus_table)
        bench_table = (self.root / 'docs/_includes/generated/tables/benchmark-size-ratio.md').read_text(encoding='utf-8')
        self.assertIn('N/A (not measured in retained 2026-09-29 set)', bench_table)

        # Independently compute corpus and benchmark aggregates directly from raw CSV rows and verify tables & SVG attributes.
        import csv as csv_mod
        import math as math_mod
        import xml.etree.ElementTree as ET
        from unittest import mock

        c_lines = [ln for ln in (self.root / 'benchmarks/corpus.csv').read_text(encoding='utf-8').splitlines() if ln.strip() and not ln.startswith('#')]
        c_rows = list(csv_mod.DictReader(c_lines))
        self.assertEqual(55, len(c_rows))
        photo_rows = [r for r in c_rows if r['set'] == 'photo']
        exp_photo_rf = sum(int(r['rasterfold']) for r in photo_rows)
        exp_photo_e7 = sum(int(r['jxl_e7']) for r in photo_rows)
        exp_photo_rat = f'{exp_photo_rf / exp_photo_e7:.4f}'
        exp_photo_gm = f'{math_mod.prod(int(r["rasterfold"]) / int(r["jxl_e7"]) for r in photo_rows) ** (1.0 / len(photo_rows)):.4f}'
        self.assertIn(f'| photo | 39 | {exp_photo_rf} | {exp_photo_e7} |', corpus_table)
        self.assertIn(f'| {exp_photo_rat} | {exp_photo_gm} |', corpus_table)

        r_lines = [ln for ln in (self.root / 'benchmarks/results.csv').read_text(encoding='utf-8').splitlines() if ln.strip() and not ln.startswith('#')]
        r_rows = list(csv_mod.DictReader(r_lines))
        photos_rf = {r['image']: r for r in r_rows if r['set'] == 'photos' and r['codec'] == 'rasterfold'}
        photos_jxl = {r['image']: r for r in r_rows if r['set'] == 'photos' and r['codec'] == 'jxl'}
        exp_photos_size_gm = f'{math_mod.prod(int(photos_rf[k]["bytes"]) / int(photos_jxl[k]["bytes"]) for k in photos_rf) ** (1.0 / len(photos_rf)):.4f}'
        exp_photos_dec_gm = f'{math_mod.prod(float(photos_rf[k]["decode_us"]) / float(photos_jxl[k]["decode_us"]) for k in photos_rf) ** (1.0 / len(photos_rf)):.4f}'
        exp_photos_enc_pk = str(max(int(r['encode_peak']) for r in photos_rf.values()))
        self.assertIn(f'| photos | rasterfold | 10 | {sum(int(r["bytes"]) for r in photos_rf.values())} | {exp_photos_size_gm} |', bench_table)
        svd_svg = ET.fromstring((self.root / 'docs/_includes/generated/charts/size-vs-decode.svg').read_text(encoding='utf-8'))
        svd_pt = next(el for el in svd_svg.iter() if el.attrib.get('data-kind') == 'point' and el.attrib.get('data-group') == 'photos' and el.attrib.get('data-series') == 'rasterfold')
        self.assertEqual((exp_photos_dec_gm, exp_photos_size_gm), (svd_pt.attrib.get('data-x'), svd_pt.attrib.get('data-y')))
        # Verify scatter-sets N/A elements have distinct coordinates and point-label + legend + ticks exist.
        na_els = [el for el in svd_svg.iter() if el.attrib.get('data-kind') == 'na']
        self.assertEqual(2, len(na_els))
        self.assertEqual(2, len({(el.attrib.get('data-x'), el.attrib.get('data-y')) for el in na_els}))
        self.assertTrue(any(el.attrib.get('data-kind') == 'point-label' and (el.text or '') == 'P:rasterfold' for el in svd_svg.iter()))
        pm_svg = ET.fromstring((self.root / 'docs/_includes/generated/charts/peak-memory.svg').read_text(encoding='utf-8'))
        pm_bar = next(el for el in pm_svg.iter() if el.attrib.get('data-kind') == 'bar' and el.attrib.get('data-group') == 'photos-encode' and el.attrib.get('data-series') == 'rasterfold')
        self.assertEqual(exp_photos_enc_pk, pm_bar.attrib.get('data-value'))

        # Verify that an aggregation bug in compute_chart_datasets followed by full regeneration is caught by --self-test.
        real_compute = build_docs.compute_chart_datasets

        def buggy_compute(r_path):
            ds = real_compute(r_path)
            ds['corpus-size-ratio']['bars'][0]['value'] = '0.8888'
            ds['corpus-size-ratio']['bars'][0]['num'] = 0.8888
            return ds

        with mock.patch.object(build_docs, 'compute_chart_datasets', side_effect=buggy_compute):
            build_docs.write_generated_pages(self.root)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn("('photo', 'rasterfold') data-value='0.8888' != '0.9712'", err.getvalue())
        build_docs.write_generated_pages(self.root)

        # 1. Hand-edited bar data-value fails --self-test.
        csr_rel = 'docs/_includes/generated/charts/corpus-size-ratio.svg'
        csr_orig = (self.root / csr_rel).read_text(encoding='utf-8')
        self.write(csr_rel, csr_orig.replace('data-value="0.9712"', 'data-value="0.1234"', 1))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn("('photo', 'rasterfold') data-value='0.1234' != '0.9712'", err.getvalue())

        # 2. Hand-edited bar SVG geometry height fails --self-test even when data-value is untouched.
        self.write(csr_rel, csr_orig.replace('height="209.77"', 'height="100.00"', 1))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn('bar geometry height=100.00', err.getvalue())
        self.write(csr_rel, csr_orig)

        # 3. Hand-edited scatter point value and geometry fail --self-test.
        sc_rel = 'docs/_includes/generated/charts/scatter-e9.svg'
        sc_orig = (self.root / sc_rel).read_text(encoding='utf-8')
        self.write(sc_rel, sc_orig.replace('data-y="0.9781"', 'data-y="0.5000"', 1))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn("('photo', 'measure') point data-x/data-y=", err.getvalue())
        self.write(sc_rel, sc_orig.replace('cy="311.35"', 'cy="200.00"', 1))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn('point geometry cx=', err.getvalue())
        self.write(sc_rel, sc_orig)

        # 4. Deleted series element (e.g. removing an N/A or bar element) fails --self-test.
        bsr_rel = 'docs/_includes/generated/charts/benchmark-size-ratio.svg'
        bsr_orig = (self.root / bsr_rel).read_text(encoding='utf-8')
        dropped = '\n'.join(ln for ln in bsr_orig.splitlines() if 'data-group="photos" data-series="jxl-e9"' not in ln) + '\n'
        self.write(bsr_rel, dropped)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn("missing series element for ('photos', 'jxl-e9')", err.getvalue())
        self.write(bsr_rel, bsr_orig)

        # 5. Hand-edited Markdown table cell fails --self-test.
        tbl_rel = 'docs/_includes/generated/tables/corpus-size-ratio.md'
        tbl_orig = (self.root / tbl_rel).read_text(encoding='utf-8')
        self.write(tbl_rel, tbl_orig.replace('30865211', '99999999', 1))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn('table rows do not match CSV calculation', err.getvalue())
        self.write(tbl_rel, tbl_orig)

        # 6. Stripping the provenance comment from benchmarks/results.csv fails --self-test.
        res_orig = (self.root / 'benchmarks/results.csv').read_text(encoding='utf-8')
        self.write('benchmarks/results.csv', '\n'.join(res_orig.splitlines()[1:]) + '\n')
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn('benchmarks/results.csv: missing or invalid header codec_revision=', err.getvalue())

        # 7. pixels_match=False or duplicate rows in benchmarks/results.csv fail --self-test.
        self.write('benchmarks/results.csv', res_orig.replace(',True\n', ',False\n', 1))
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn('pixels_match is not True', err.getvalue())

        res_lines = res_orig.splitlines()
        self.write('benchmarks/results.csv', '\n'.join([*res_lines, res_lines[2]]) + '\n')
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(1, build_docs.main(['--root', str(self.root), '--self-test']))
        self.assertIn('invalid or duplicate row', err.getvalue())

        # 8. Fractional microsecond medians (from even repeat counts) round-trip through regeneration and --self-test.
        self.write('benchmarks/results.csv', res_orig.replace(',401129,384760,', ',401129.5,384760.5,', 1))
        build_docs.write_generated_pages(self.root)
        self.assertIn('401129.5', (self.root / 'docs/_includes/generated/tables/encode-time.md').read_text(encoding='utf-8'))
        self.assertIn('384760.5', (self.root / 'docs/_includes/generated/tables/decode-time.md').read_text(encoding='utf-8'))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, build_docs.main(['--root', str(self.root), '--self-test']))

        self.write('benchmarks/results.csv', res_orig)
        build_docs.write_generated_pages(self.root)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, build_docs.main(['--root', str(self.root), '--self-test']))

    def test_measured_figures_bound_to_data_and_aggregates(self):
        import csv as csv_mod
        from decimal import Decimal
        import math as math_mod
        import re as re_mod

        live_manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
        live_facts = drift.derive_facts(live_manifest, lambda rel: (REPO / rel).read_text(encoding='utf-8'))

        # 1. Independently verify corpus group memberships against research/ab.py SETS (photo 39, tp 10 overlapping, tp-only 5 exclusive, synth 11, all 55) and aggregate values.
        ab_spec = importlib.util.spec_from_file_location('ab_mod', REPO / 'research/ab.py')
        ab_mod = importlib.util.module_from_spec(ab_spec)
        ab_spec.loader.exec_module(ab_mod)
        self.assertEqual(set(ab_mod.SETS['photo']), {img for _s, img in drift.CORPUS_GROUP_MEMBERS['photo']})
        self.assertEqual(set(ab_mod.SETS['tp']), {img for _s, img in drift.CORPUS_GROUP_MEMBERS['tp']})
        self.assertEqual(set(ab_mod.SETS['synth']), {img for _s, img in drift.CORPUS_GROUP_MEMBERS['synth']})
        self.assertEqual(set(ab_mod.SETS['all']), {img for _s, img in drift.CORPUS_GROUP_MEMBERS['all']})

        corpus_raw = (REPO / 'benchmarks/corpus.csv').read_text(encoding='utf-8')
        corpus_rows = list(csv_mod.DictReader([ln for ln in corpus_raw.splitlines() if ln.strip() and not ln.startswith('#')]))
        tp_overlap = {'kodim01', 'kodim03', 'kodim05', 'kodim13', 'kodim23'}
        expected_groups = {
            'photo': (39, [r for r in corpus_rows if r['set'] == 'photo']),
            'tp': (10, [r for r in corpus_rows if r['set'] == 'tp' or r['image'] in tp_overlap]),
            'tp_only': (5, [r for r in corpus_rows if r['set'] == 'tp']),
            'synth': (11, [r for r in corpus_rows if r['set'] == 'synth']),
            'all': (55, corpus_rows),
        }
        self.assertEqual(6651583, live_facts['corpus_tp_rf_bytes'])
        self.assertEqual('0.9743', live_facts['corpus_tp_jxl_e7_ratio'])
        self.assertEqual('0.9722', live_facts['corpus_tp_jxl_e7_geomean'])
        for key_prefix, (exp_count, grp_rows) in expected_groups.items():
            self.assertEqual(exp_count, len(grp_rows), key_prefix)
            self.assertEqual(exp_count, live_facts[f'corpus_{key_prefix}_count'])
            exp_rf = sum(int(r['rasterfold']) for r in grp_rows)
            self.assertEqual(exp_rf, live_facts[f'corpus_{key_prefix}_rf_bytes'])
            for ref_col in ('jxl_e7', 'jxl_e9', 'webp'):
                exp_ref = sum(int(r[ref_col]) for r in grp_rows)
                exp_ratio = f'{float(Decimal(exp_rf) / Decimal(exp_ref)):.4f}'
                exp_gm = f'{math_mod.exp(sum(math_mod.log(float(Decimal(r["rasterfold"]) / Decimal(r[ref_col]))) for r in grp_rows) / len(grp_rows)):.4f}'
                self.assertEqual(exp_ref, live_facts[f'corpus_{key_prefix}_{ref_col}_bytes'])
                self.assertEqual(exp_ratio, live_facts[f'corpus_{key_prefix}_{ref_col}_ratio'])
                self.assertEqual(exp_gm, live_facts[f'corpus_{key_prefix}_{ref_col}_geomean'])

        # Independently verify results.csv reference and retained set aggregates.
        results_raw = (REPO / 'benchmarks/results.csv').read_text(encoding='utf-8')
        results_rows = list(csv_mod.DictReader([ln for ln in results_raw.splitlines() if ln.strip() and not ln.startswith('#')]))
        self.assertEqual('measured', live_facts['results_measure_status'])
        self.assertEqual('2026-10-25', live_facts['results_measure_date'])
        self.assertEqual('retained', live_facts['results_photos_status'])
        self.assertEqual('2026-09-29', live_facts['results_photos_date'])
        self.assertEqual('retained', live_facts['results_synthetic_status'])
        self.assertEqual('2026-09-29', live_facts['results_synthetic_date'])
        self.assertEqual('2026-10-25', live_facts['corpus_date'])
        self.assertEqual('0.958', live_facts['measurement_reference_ratio_jxl_e7'])
        self.assertEqual('0.978', live_facts['measurement_reference_ratio_jxl_e9'])
        for bset, exp_cnt in (('photos', 10), ('synthetic', 11)):
            rf_set = {r['image']: r for r in results_rows if r['set'] == bset and r['codec'] == 'rasterfold'}
            jxl_set = {r['image']: r for r in results_rows if r['set'] == bset and r['codec'] == 'jxl'}
            self.assertEqual(exp_cnt, live_facts[f'results_{bset}_count'])
            self.assertEqual(sum(int(r['bytes']) for r in rf_set.values()), live_facts[f'results_{bset}_rf_bytes'])
            self.assertEqual(sum(int(r['bytes']) for r in jxl_set.values()), live_facts[f'results_{bset}_jxl_bytes'])
            self.assertEqual(max(int(r['encode_peak']) for r in rf_set.values()), live_facts[f'results_{bset}_rf_encode_peak_max'])
            self.assertEqual(max(int(r['decode_peak']) for r in rf_set.values()), live_facts[f'results_{bset}_rf_decode_peak_max'])

        # Verify every measured figure in docs/benchmarks.md has an active drift:fact binding.
        bm_text = (REPO / 'docs/benchmarks.md').read_text(encoding='utf-8')
        self.assertIn('<!-- drift:begin benchmarks-measurements -->', bm_text)
        self.assertIn('<!-- drift:end benchmarks-measurements -->', bm_text)
        fact_matches = list(drift.FACT.finditer(bm_text))
        self.assertGreaterEqual(len(fact_matches), 110)

        # Set up a full fixture with docs/benchmarks.md, CSVs, and historical research files.
        for rel in (
            'src/main.c', 'src/rf.h', 'include/rasterfold.h',
            'tests/test_correctness.py',
            'benchmarks/results.csv', 'benchmarks/corpus.csv',
            'docs/benchmarks.md',
        ):
            self.write(rel, (REPO / rel).read_text(encoding='utf-8'))
        for hist_doc in ('docs/measurements.md', 'docs/research.md', 'docs/research-summary.md'):
            self.write(hist_doc, f'# Historical record ({hist_doc})\n\nFormat version 1 measured in 2020: 9999999 bytes.\n')
        for chart_name in build_docs.CHART_NAMES:
            for kind, ext in (('charts', 'svg'), ('tables', 'md')):
                rel = f'docs/_includes/generated/{kind}/{chart_name}.{ext}'
                self.write(rel, (REPO / rel).read_text(encoding='utf-8'))
        self.manifest['coverage'] = live_manifest['coverage']
        self.manifest['facts'].update(live_manifest['facts'])
        self.manifest['blocks']['benchmarks-measurements'] = live_manifest['blocks']['benchmarks-measurements']
        self.save()
        c = self.check()
        c.fix()
        self.assertEqual([], self.check().errors)

        # 2. Mutating a CSV cell in benchmarks/results.csv or benchmarks/corpus.csv fails with fact + generated errors.
        orig_results = (self.root / 'benchmarks/results.csv').read_text(encoding='utf-8')
        self.write('benchmarks/results.csv', orig_results.replace(',1409028,', ',1409999,', 1))
        c = self.check()
        self.assertIn('fact', self.kinds(c))
        self.assertIn('generated', self.kinds(c))
        self.assertTrue(any(e['path'] == 'docs/benchmarks.md' and 'measurement_reference_bytes' in e['detail'] for e in c.errors))
        self.write('benchmarks/results.csv', orig_results)
        self.assertEqual([], self.check().errors)

        orig_corpus = (self.root / 'benchmarks/corpus.csv').read_text(encoding='utf-8')
        self.write('benchmarks/corpus.csv', orig_corpus.replace('photo,measure,1672,941,1409028,', 'photo,measure,1672,941,1409999,', 1))
        c = self.check()
        self.assertIn('fact', self.kinds(c))
        self.assertTrue(any(e['path'] == 'docs/benchmarks.md' and 'corpus_photo_rf_bytes' in e['detail'] for e in c.errors))
        self.write('benchmarks/corpus.csv', orig_corpus)
        self.assertEqual([], self.check().errors)

        # 3. Changing a visible figure while keeping its <!-- drift:fact ... --> annotation unchanged fails with a fact error.
        orig_bm = (self.root / 'docs/benchmarks.md').read_text(encoding='utf-8')
        self.write(
            'docs/benchmarks.md',
            orig_bm.replace(
                '`1,409,028` bytes <!-- drift:fact measurement_reference_bytes = 1409028 -->',
                '`1,409,999` bytes <!-- drift:fact measurement_reference_bytes = 1409028 -->',
                1,
            ),
        )
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'fact'
                and e['path'] == 'docs/benchmarks.md'
                and 'visible figure before measurement_reference_bytes does not match 1409028' in e['detail']
                for e in c.errors
            ),
            c.errors,
        )

        # Also test non-numeric visible figure mismatch (changing date or extension in prose while keeping annotation).
        self.write(
            'docs/benchmarks.md',
            orig_bm.replace(
                'on `2026-10-25` <!-- drift:fact results_measure_date = 2026-10-25 -->',
                'on `2026-10-24` <!-- drift:fact results_measure_date = 2026-10-25 -->',
                1,
            ),
        )
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'fact'
                and e['path'] == 'docs/benchmarks.md'
                and "visible figure before results_measure_date does not match '2026-10-25'" in e['detail']
                for e in c.errors
            ),
            c.errors,
        )

        # HTML comments cannot smuggle a matching number after a wrong visible figure, and substring prefixes (.folded vs .fold) fail.
        self.write(
            'docs/benchmarks.md',
            orig_bm.replace(
                '`1,409,028` bytes <!-- drift:fact measurement_reference_bytes = 1409028 -->',
                '`999` bytes <!-- previous: 1409028 --> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
                1,
            ),
        )
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'fact'
                and e['path'] == 'docs/benchmarks.md'
                and 'visible figure before measurement_reference_bytes does not match 1409028' in e['detail']
                for e in c.errors
            ),
            c.errors,
        )
        self.write(
            'docs/benchmarks.md',
            orig_bm + '\nEncode to `.folded` <!-- drift:fact encoded_ext = .fold -->.\n',
        )
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'fact'
                and e['path'] == 'docs/benchmarks.md'
                and "visible figure before encoded_ext does not match '.fold'" in e['detail']
                for e in c.errors
            ),
            c.errors,
        )
        for smuggled in (
            'Size: <span data-bytes="1409028">999</span> bytes <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span title="x > 1409028">wrong</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <script>1409028</script> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span hidden>1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span hidden>1409028 <!-- drift:fact measurement_reference_bytes = 1409028 --></span>',
            'Size: 999 bytes <span\nhidden>1409028 <!-- drift:fact measurement_reference_bytes = 1409028 --></span>',
            'Size: 999 bytes <span hidden />1409028 <!-- drift:fact measurement_reference_bytes = 1409028 --></span>',
            '<!--\n<span>1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="display&#58;none">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="opacity:0.0">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'For `<x>`, size: 1409028 9999 <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            '1409028 <span style="--opacity:0">999</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            '1409028 <span style="opacity:0;opacity:1">999</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="display:none;display:bogus">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="visibility:hidden;visibility:bogus">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="opacity:0;opacity:bogus">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="opacity:0;opacity:1.">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="font-size:0;font-size:16.px">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span style="opacity:0e0">1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <div style="content-visibility:hidden">1409028</div> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <div style="content-visibility:hidden;content-visibility:bogus">1409028</div> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            '1409028 <span aria-hidden="true">999</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes <span hidden><span>1409028</span>1409028</span> <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            '<div hidden>\nSize: 1,409,028 bytes <!-- drift:fact measurement_reference_bytes = 1409028 -->\n</div>',
            'Size: [999 bytes](README.md#1409028) <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: [999 bytes](https://example.invalid/a(b(c))#1409028) <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            "Size: [999 bytes](https://example.invalid/it's#1409028) <!-- drift:fact measurement_reference_bytes = 1409028 -->",
            'Size: 999 bytes ![1409028](_includes/generated/charts/corpus-size-ratio.svg) <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Wrong size: 999 bytes. ![1,409,028 <!-- drift:fact measurement_reference_bytes = 1409028 -->](_includes/generated/charts/corpus-size-ratio.svg)',
            'Wrong size: 999 bytes. ![\n1,409,028 <!-- drift:fact measurement_reference_bytes = 1409028 -->\n](_includes/generated/charts/corpus-size-ratio.svg)',
            r'Size: 999 bytes \\![1409028](_includes/generated/charts/corpus-size-ratio.svg) <!-- drift:fact measurement_reference_bytes = 1409028 -->',
            'Size: 999 bytes [source][1409028] <!-- drift:fact measurement_reference_bytes = 1409028 -->\n\n[1409028]: README.md',
        ):
            self.write('docs/benchmarks.md', orig_bm + '\n' + smuggled + '\n')
            c = self.check()
            self.assertTrue(
                any(
                    e['kind'] == 'fact'
                    and e['path'] == 'docs/benchmarks.md'
                    and 'visible figure before measurement_reference_bytes does not match 1409028' in e['detail']
                    for e in c.errors
                ),
                (smuggled, c.errors),
            )
        self.write(
            'docs/benchmarks.md',
            orig_bm
            + '\nSize: [1,409,028 bytes](https://example.invalid/source) <!-- drift:fact measurement_reference_bytes = 1409028 -->\n'
            + 'Size: <span aria-hidden="true">1,409,028</span> bytes <!-- drift:fact measurement_reference_bytes = 1409028 -->\n',
        )
        self.assertEqual([], self.check().errors)

        # Fractional microsecond medians (from even repeat counts in tests/benchmark.py) derive and render cleanly.
        self.write('docs/benchmarks.md', orig_bm)
        self.write('benchmarks/results.csv', orig_results.replace(',401129,384760,', ',401129.5,384760.5,', 1))
        c = self.check()
        c.fix()
        self.assertEqual([], self.check().errors)
        self.assertIn('`401,129.5` us <!-- drift:fact measurement_reference_rf_encode_us = 401129.5 -->', (self.root / 'docs/benchmarks.md').read_text(encoding='utf-8'))
        self.write('benchmarks/results.csv', orig_results)
        self.write('docs/benchmarks.md', orig_bm)
        self.check().fix()

        # Partial benchmark rerun (e.g. photos-only on 2026-10-26 via tests/benchmark.py resolve_set_provenance) updates per-set status/date/revision without falsifying retained measure provenance.
        bench_spec = importlib.util.spec_from_file_location('bench_mod', REPO / 'tests/benchmark.py')
        bench_mod = importlib.util.module_from_spec(bench_spec)
        bench_spec.loader.exec_module(bench_mod)
        prev_meta = drift._csv_meta(orig_results)
        new_rev = '9' * 40
        prov = bench_mod.resolve_set_provenance(prev_meta, ['photos'], ['measure', 'synthetic'], new_rev, '2026-10-26')
        new_header = (
            f'# codec_revision={new_rev} kat_fold_sha256={prev_meta["kat_fold_sha256"]} date=2026-10-26 '
            f'measured_sets=photos retained_sets=measure,synthetic '
            f'retained_revision={prov["retained_revision"]} retained_date={prov["retained_date"]} '
            f'retained_kat_fold_sha256={prov["retained_kat_fold_sha256"]} '
            f'set_revisions={prov["set_revisions"]} set_dates={prov["set_dates"]} '
            f'set_kat_hashes={prov["set_kat_hashes"]}'
        )
        rerun_results = '\n'.join([new_header, *orig_results.splitlines()[1:]]) + '\n'
        self.write('benchmarks/results.csv', rerun_results)
        c = self.check()
        c.fix()
        self.assertEqual([], self.check().errors)
        rerun_bm = (self.root / 'docs/benchmarks.md').read_text(encoding='utf-8')
        self.assertIn('(`measure` set `retained` <!-- drift:fact results_measure_status = retained --> on `2026-10-25` <!-- drift:fact results_measure_date = 2026-10-25 -->', rerun_bm)
        self.assertIn('**`photos` benchmark set (`measured` <!-- drift:fact results_photos_status = measured --> on `2026-10-26` <!-- drift:fact results_photos_date = 2026-10-26 -->', rerun_bm)
        self.write('benchmarks/results.csv', orig_results)
        self.write('docs/benchmarks.md', orig_bm)
        self.check().fix()

        # Removing the visible figure before an annotation (standalone drift:fact) also fails.
        self.write('docs/benchmarks.md', orig_bm + '\n<!-- drift:fact format_version = 2 -->\n')
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'fact'
                and e['path'] == 'docs/benchmarks.md'
                and 'visible figure before format_version does not match 2' in e['detail']
                for e in c.errors
            ),
            c.errors,
        )

        # 4. Editing non-fact text inside the generated benchmarks-measurements block fails with generated.
        self.write('docs/benchmarks.md', orig_bm.replace('RGB8,', 'RGB24,', 1))
        c = self.check()
        self.assertIn('generated', self.kinds(c))
        self.assertIn('docs/benchmarks.md', c.repairs)

        # 5. Removing required block markers fails with block.
        stripped_markers = re_mod.sub(r'<!-- drift:(?:begin|end) benchmarks-measurements -->\n?', '', orig_bm)
        self.write('docs/benchmarks.md', stripped_markers)
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'block'
                and e['path'] == 'docs/benchmarks.md'
                and 'required block benchmarks-measurements is missing' in e['detail']
                for e in c.errors
            ),
            c.errors,
        )
        self.write('docs/benchmarks.md', orig_bm)
        self.assertEqual([], self.check().errors)

        # 6. Missing or ambiguous CSV selections and malformed aggregate rules fail derive_facts.
        for bad_rule, expected_msg in (
            ({'kind': 'csv', 'source': 'benchmarks/results.csv', 'where': {'set': 'no_such_set'}, 'column': 'bytes'}, 'expected one CSV row, got 0'),
            ({'kind': 'csv', 'source': 'benchmarks/results.csv', 'where': {'set': 'photos'}, 'column': 'bytes'}, 'expected one CSV row, got 60'),
            ({'kind': 'csv-meta', 'source': 'benchmarks/results.csv', 'key': 'missing_meta_key'}, "missing CSV metadata key 'missing_meta_key'"),
            ({'kind': 'csv-aggregate', 'source': 'benchmarks/corpus.csv', 'op': 'count', 'group': 'bad_group'}, "unknown corpus group 'bad_group'"),
            ({'kind': 'csv-aggregate', 'source': 'benchmarks/corpus.csv', 'op': 'count', 'where': {'set': 'missing'}}, 'CSV selector matched 0 rows'),
            (
                {
                    'kind': 'csv-aggregate',
                    'source': 'benchmarks/results.csv',
                    'op': 'geomean',
                    'where': {'set': 'photos', 'codec': 'rasterfold'},
                    'column': 'bytes',
                    'denominator_where': {'set': 'synthetic', 'codec': 'jxl'},
                },
                'geomean denominator rows do not match numerator keys',
            ),
        ):
            bad_manifest = {'facts': {'probe': bad_rule}}
            with self.assertRaisesRegex(ValueError, expected_msg):
                drift.derive_facts(bad_manifest, lambda rel: (self.root / rel).read_text(encoding='utf-8'))

        # Wrong corpus group row count, duplicate image, or unexpected image member fails derive_facts.
        truncated_corpus = '\n'.join(orig_corpus.splitlines()[:-1]) + '\n'
        with self.assertRaisesRegex(ValueError, "corpus group 'synth' expected 11 authoritative members, got 10"):
            drift.derive_facts(
                {'facts': {'probe': {'kind': 'csv-aggregate', 'source': 'benchmarks/corpus.csv', 'op': 'count', 'group': 'synth'}}},
                lambda _rel: truncated_corpus,
            )
        dup_kodim_corpus = orig_corpus.replace('photo,kodim02,', 'photo,kodim01,', 1)
        with self.assertRaisesRegex(ValueError, 'invalid or duplicate corpus CSV row'):
            drift.derive_facts(
                {'facts': {'probe': {'kind': 'csv-aggregate', 'source': 'benchmarks/corpus.csv', 'op': 'count', 'group': 'photo'}}},
                lambda _rel: dup_kodim_corpus,
            )
        unknown_member_corpus = orig_corpus.replace('photo,kodim02,', 'photo,kodim99,', 1)
        with self.assertRaisesRegex(ValueError, "corpus group 'photo' expected 39 authoritative members"):
            drift.derive_facts(
                {'facts': {'probe': {'kind': 'csv-aggregate', 'source': 'benchmarks/corpus.csv', 'op': 'count', 'group': 'photo'}}},
                lambda _rel: unknown_member_corpus,
            )

        # Duplicate numerator row or unexpected benchmark set image in results.csv fails derive_facts.
        res_lines = orig_results.splitlines()
        deer_rf_line = next(ln for ln in res_lines if ln.startswith('photos,deer_crop,') and ',rasterfold,' in ln)
        dup_deer_results = '\n'.join([*res_lines, deer_rf_line]) + '\n'
        with self.assertRaisesRegex(ValueError, 'invalid or duplicate benchmark CSV row'):
            drift.derive_facts(
                {
                    'facts': {
                        'probe': {
                            'kind': 'csv-aggregate',
                            'source': 'benchmarks/results.csv',
                            'op': 'geomean',
                            'where': {'set': 'photos', 'codec': 'rasterfold'},
                            'column': 'bytes',
                            'denominator_where': {'set': 'photos', 'codec': 'jxl'},
                        }
                    }
                },
                lambda _rel: dup_deer_results,
            )
        unknown_bench_results = orig_results.replace('photos,deer_crop,', 'photos,unknown_crop,', 1)
        with self.assertRaisesRegex(ValueError, "benchmark set 'photos' expected 10 authoritative images"):
            drift.derive_facts(
                {
                    'facts': {
                        'probe': {
                            'kind': 'csv-aggregate',
                            'source': 'benchmarks/results.csv',
                            'op': 'count',
                            'where': {'set': 'photos', 'codec': 'rasterfold'},
                        }
                    }
                },
                lambda _rel: unknown_bench_results,
            )

        # 7. Historical research documents remain historical and untouched by --fix.
        hist_roles = {i['path']: i['role'] for i in self.check().inventory}
        for hist_doc in ('docs/measurements.md', 'docs/research.md', 'docs/research-summary.md'):
            self.assertEqual('historical', hist_roles[hist_doc])
            before_bytes = (self.root / hist_doc).read_bytes()
            c = self.check()
            c.fix()
            self.assertEqual(before_bytes, (self.root / hist_doc).read_bytes())

    def test_staleness_verified_dates_and_data_revisions(self):
        live_manifest = json.loads((REPO / 'docs/drift.json').read_text(encoding='utf-8'))
        expected_docs = {
            'README.md', 'AGENTS.md', 'GEMINI.md', 'CONTRIBUTING.md',
            'docs/building.md', 'docs/library.md', 'docs/index.md',
            'docs/getting-started.md', 'docs/benchmarks.md',
            'docs/document-drift.md', 'docs/README.md',
            'docs/agent-skills-and-rules-structure.md',
            'spec/FOLD.md', 'research/README.md', 'testdata/photos/README.md',
        }
        self.assertTrue(expected_docs.issubset(set(live_manifest['reviews'])))
        for doc in expected_docs:
            rev = live_manifest['reviews'][doc]
            self.assertRegex(rev['evidence'], r'\b\d{4}-\d{2}-\d{2}\b', doc)
            self.assertIn(doc, rev['sources'], doc)
            for src, exp_digest in rev['sources'].items():
                self.assertEqual(exp_digest, drift.digest((REPO / src).read_bytes()), (doc, src))

        # Populate fixture with reviewed prose documents, KAT_FOLD source, and benchmark CSVs.
        bench_spec = importlib.util.spec_from_file_location('bench_mod', REPO / 'tests/benchmark.py')
        bench_mod = importlib.util.module_from_spec(bench_spec)
        bench_spec.loader.exec_module(bench_mod)

        for rel in (
            'src/main.c', 'src/rf.h', 'include/rasterfold.h',
            'tests/test_correctness.py',
            'benchmarks/results.csv', 'benchmarks/corpus.csv',
            'docs/benchmarks.md', 'CONTRIBUTING.md',
            'scripts/build.py', 'scripts/package.py', 'scripts/check-all.py',
            'toolchain.json', 'measure/verdict.json',
        ):
            self.write(rel, (REPO / rel).read_text(encoding='utf-8'))
        for hist_doc in ('docs/measurements.md', 'docs/research.md', 'docs/research-summary.md'):
            self.write(hist_doc, f'# Historical record ({hist_doc})\n')
        for link_target in ('AGENTS.md', 'docs/building.md', 'todo/README.md', 'docs/document-drift.md', 'SECURITY.md'):
            self.write(link_target, f'# {link_target}\n')
        for chart_name in build_docs.CHART_NAMES:
            for kind, ext in (('charts', 'svg'), ('tables', 'md')):
                rel = f'docs/_includes/generated/{kind}/{chart_name}.{ext}'
                self.write(rel, (REPO / rel).read_text(encoding='utf-8'))
        self.manifest['coverage'] = live_manifest['coverage']
        self.manifest['facts'].update(live_manifest['facts'])
        self.manifest['blocks']['benchmarks-measurements'] = live_manifest['blocks']['benchmarks-measurements']
        self.manifest['reviews'] = {
            'CONTRIBUTING.md': live_manifest['reviews']['CONTRIBUTING.md'],
            'docs/benchmarks.md': live_manifest['reviews']['docs/benchmarks.md'],
        }
        self.save()
        c = self.check()
        c.fix()
        self.assertEqual([], self.check().errors)

        # 1. Same-day source change or date-only prose edit invalidates the review receipt; --fix cannot renew it.
        orig_build_py = (self.root / 'scripts/build.py').read_text(encoding='utf-8')
        self.write('scripts/build.py', orig_build_py + '\n# same-day source change\n')
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'stale'
                and e['path'] == 'CONTRIBUTING.md'
                and 'scripts/build.py changed since review' in e['detail']
                for e in c.errors
            ),
            c.errors,
        )
        c.fix()
        c_after_fix = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'stale'
                and e['path'] == 'CONTRIBUTING.md'
                and 'scripts/build.py changed since review' in e['detail']
                for e in c_after_fix.errors
            )
        )
        # Explicit --review renews the receipt.
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                0,
                drift.main(
                    [
                        '--root', str(self.root),
                        '--review', 'CONTRIBUTING.md',
                        '--evidence', 'Author-attested: 2026-10-25 re-read CONTRIBUTING.md after build.py edit',
                    ]
                ),
            )
        self.assertEqual([], self.check().errors)
        # Restore scripts/build.py and original receipt.
        self.write('scripts/build.py', orig_build_py)
        self.manifest = json.loads((self.root / 'docs/drift.json').read_text(encoding='utf-8'))
        self.manifest['reviews']['CONTRIBUTING.md'] = live_manifest['reviews']['CONTRIBUTING.md']
        self.save()
        self.check().fix()
        self.assertEqual([], self.check().errors)

        # 2. Regenerating KAT_FOLD in tests/test_correctness.py without rerunning benchmarks fails both CSVs; --fix cannot silence it.
        orig_tc = (self.root / 'tests/test_correctness.py').read_text(encoding='utf-8')
        orig_results = (self.root / 'benchmarks/results.csv').read_text(encoding='utf-8')
        orig_corpus = (self.root / 'benchmarks/corpus.csv').read_text(encoding='utf-8')
        mutated_tc = orig_tc.replace('"52464902', '"52464903', 1)
        self.assertNotEqual(orig_tc, mutated_tc)
        self.write('tests/test_correctness.py', mutated_tc)
        c = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'stale'
                and e['path'] == 'benchmarks/corpus.csv'
                and 'KAT_FOLD changed in tests/test_correctness.py' in e['detail']
                and 'regenerate benchmarks/corpus.csv' in e['detail']
                for e in c.errors
            ),
            c.errors,
        )
        self.assertTrue(
            any(
                e['kind'] == 'stale'
                and e['path'] == 'benchmarks/results.csv'
                and 'stale sets: measure, photos, synthetic' in e['detail']
                and 'regenerate benchmarks/results.csv' in e['detail']
                for e in c.errors
            ),
            c.errors,
        )
        c.fix()
        self.assertEqual(orig_results, (self.root / 'benchmarks/results.csv').read_text(encoding='utf-8'))
        self.assertEqual(orig_corpus, (self.root / 'benchmarks/corpus.csv').read_text(encoding='utf-8'))
        self.assertIn('stale', self.kinds(self.check()))

        # 3. Partial rerun of only 'measure' after KAT_FOLD change updates measure's KAT hash but leaves retained photos and synthetic failing.
        old_meta, rows = bench_mod.read_results_csv(self.root / 'benchmarks/results.csv', require_provenance=True)
        new_kat = bench_mod.kat_fold_sha256(self.root)
        partial_meta = bench_mod.resolve_set_provenance(
            old_meta,
            ['measure'],
            ['photos', 'synthetic'],
            '3' * 40,
            '2026-10-26',
            new_kat,
        )
        bench_mod.write_csv_rows(self.root / 'benchmarks/results.csv', rows, meta=partial_meta)
        c_partial = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'stale'
                and e['path'] == 'benchmarks/results.csv'
                and 'stale sets: photos, synthetic' in e['detail']
                for e in c_partial.errors
            ),
            c_partial.errors,
        )

        # 4. Missing or date-only provenance headers fail (date-only freshness inference is never accepted).
        with self.assertRaisesRegex(ValueError, "missing CSV metadata key 'date'"):
            self.write('benchmarks/corpus.csv', '\n'.join(orig_corpus.splitlines()[1:]) + '\n')
            self.check()
        self.write('benchmarks/corpus.csv', '\n'.join([f'# codec_revision={"4" * 40} date=2026-10-26', *orig_corpus.splitlines()[1:]]) + '\n')
        c_missing = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'stale'
                and e['path'] == 'benchmarks/corpus.csv'
                and 'missing or malformed provenance header' in e['detail']
                for e in c_missing.errors
            ),
            c_missing.errors,
        )

        # 5. Legitimate full regeneration of all sets + corpus CSV (plus renewing docs/benchmarks.md receipt) restores exit 0.
        full_meta = bench_mod.resolve_set_provenance(
            partial_meta,
            ['measure', 'photos', 'synthetic'],
            [],
            '4' * 40,
            '2026-10-26',
            new_kat,
        )
        bench_mod.write_csv_rows(self.root / 'benchmarks/results.csv', rows, meta=full_meta)
        regen_corpus = '\n'.join(
            [
                f'# codec_revision={"4" * 40} kat_fold_sha256={new_kat} date=2026-10-26',
                *orig_corpus.splitlines()[1:],
            ]
        ) + '\n'
        self.write('benchmarks/corpus.csv', regen_corpus)
        c_regen = self.check()
        c_regen.fix()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(
                0,
                drift.main(
                    [
                        '--root', str(self.root),
                        '--review', 'docs/benchmarks.md',
                        '--evidence', 'Author-attested: 2026-10-26 re-read docs/benchmarks.md after full benchmark and corpus regeneration',
                    ]
                ),
            )
        self.assertEqual([], self.check().errors)

        # 6. Whitespace-containing KAT chunks, duplicate set keys, and undated review evidence are rejected.
        ws_tc = mutated_tc.replace('efce5b"\n)', 'efce5b"\n    "00 00"\n)', 1)
        self.assertNotEqual(mutated_tc, ws_tc)
        self.write('tests/test_correctness.py', ws_tc)
        c_ws = self.check()
        self.assertTrue(any(e['kind'] == 'stale' and e['path'] == 'benchmarks/corpus.csv' for e in c_ws.errors))
        self.assertTrue(any(e['kind'] == 'stale' and e['path'] == 'benchmarks/results.csv' for e in c_ws.errors))
        self.write('tests/test_correctness.py', mutated_tc)

        dup_results = (self.root / 'benchmarks/results.csv').read_text(encoding='utf-8').replace(
            f'set_kat_hashes=measure:{new_kat},photos:{new_kat},synthetic:{new_kat}',
            f'set_kat_hashes=measure:{new_kat},photos:{new_kat},synthetic:{new_kat},photos:{"0" * 64}',
        )
        self.write('benchmarks/results.csv', dup_results)
        c_dup = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'stale'
                and e['path'] == 'benchmarks/results.csv'
                and 'missing or malformed provenance header' in e['detail']
                for e in c_dup.errors
            )
        )
        with self.assertRaisesRegex(ValueError, 'duplicate set key'):
            bench_mod.read_results_csv(self.root / 'benchmarks/results.csv', require_provenance=True)
        bench_mod.write_csv_rows(self.root / 'benchmarks/results.csv', rows, meta=full_meta)

        self.manifest = json.loads((self.root / 'docs/drift.json').read_text(encoding='utf-8'))
        self.manifest['reviews']['CONTRIBUTING.md']['evidence'] = 'Undated review evidence'
        self.save()
        c_undated = self.check()
        self.assertTrue(
            any(
                e['kind'] == 'review'
                and e['path'] == 'CONTRIBUTING.md'
                and 'dated review evidence' in e['detail']
                for e in c_undated.errors
            )
        )

    def test_setext_nested_links_and_gate_entrypoints(self):
        self.write(
            'target.md',
            '---\n'
            'layout: default\n'
            'title: Target Frontmatter\n'
            '---\n'
            'Setext H1\n'
            '=========\n\n'
            'Setext H2\n'
            '---------\n\n'
            'Multi Line\n'
            'Setext\n'
            '------\n\n'
            'Setext Dup\n'
            '==========\n\n'
            'Setext Dup\n'
            '----------\n\n'
            '   ## Indented ATX\n\n'
            '## [Linked Heading](README.md)\n\n'
            '## [Guide][guide]\n\n'
            '## [Collapsed][]\n\n'
            '[Linked Setext [Inner]](README.md)\n'
            '----------------------------------\n\n'
            'Setext [Ref Heading][guide]\n'
            '===========================\n\n'
            '<em>Inline HTML Heading</em>\n'
            '============================\n\n'
            '#hashtag Setext\n'
            '---------------\n\n'
            '[guide]:\n'
            '  README.md\n'
            '[collapsed]:\n'
            '  <dir/file (2).md>\n\n'
            'Paragraph before thematic break.\n\n'
            '---\n\n'
            '\t# Tab Indented Code\n\n'
            'Tab Paragraph\n'
            '\t---\n\n'
            '<div>Block HTML</div>\n'
            '---\n\n'
            '- List item\n'
            '---\n\n'
            '> Blockquote line\n'
            '---\n\n'
            '| Table Col |\n'
            '| --- |\n',
        )
        self.write('dir/file(1).md', '   ### Paren Target\n\n[back](../valid_links.md)\n')
        self.write('dir/file (2).md', 'Angle Target\n============\n')
        self.write('dir/badge.svg', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"></svg>\n')
        self.write(
            'valid_links.md',
            '[a [nested] label](target.md#setext-h1)\n'
            '[h2](target.md#setext-h2)\n'
            '[multi](target.md#multi-line-setext)\n'
            '[dup0](target.md#setext-dup)\n'
            '[dup1](target.md#setext-dup-1)\n'
            '[indented](target.md#indented-atx)\n'
            '[linked](target.md#linked-heading)\n'
            '[ref-atx](target.md#guide)\n'
            '[ref-collapsed](target.md#collapsed)\n'
            '[linked-setext](target.md#linked-setext-inner)\n'
            '[ref-setext](target.md#setext-ref-heading)\n'
            '[inline-html-setext](target.md#inline-html-heading)\n'
            '[hashtag-setext](target.md#hashtag-setext)\n'
            '[paren-file](dir/file(1).md#paren-target "title")\n'
            '[angle-file](<dir/file (2).md#angle-target>)\n'
            '[multiline-dest](\n  target.md#setext-h1\n)\n'
            '[multiline-title](\n  dir/file(1).md#paren-target\n  "title"\n)\n'
            '[array[i][j]](README.md)\n'
            '[guide](README.md "see [foo][bar]")\n'
            '[![badge](dir/badge.svg)](target.md#setext-h1)\n'
            '[ref-use][ref_ok]\n'
            '[ref-nextline][ref_next]\n'
            '[ref-titled][ref_titled]\n'
            '[ref-titled2][ref_titled2]\n\n'
            '    [literal][example]\n'
            '\t[literal][example2]\n\n'
            '> [bq-guide][bq_ref]\n'
            '>\n'
            '> [bq_ref]:\n'
            '>   README.md "see [bq_foo][bq_bar]"\n\n'
            '[ref_ok]: <dir/file (2).md#angle-target>\n'
            '[ref_next]:\n'
            '  dir/file(1).md#paren-target\n'
            '[ref_titled]: README.md "see [foo][bar]"\n'
            '[ref_titled2]: README.md\n'
            '  \'see [baz][qux]\'\n',
        )
        self.assertEqual([], self.check().errors)

        # Direct Checker.links probe on indented code and unclosed blockquote fences followed by adjacent live links/references.
        probe = drift.Checker(self.root)
        probe.links('valid_links.md', '    [literal][example]\n\n[live][missing_after_code]\n', self.root)
        self.assertEqual(
            [{'kind': 'link', 'path': 'valid_links.md', 'detail': 'unresolved reference link: [missing_after_code]'}],
            probe.errors,
        )
        probe_bq = drift.Checker(self.root)
        probe_bq.links('valid_links.md', '> ```\n> [literal](ignored.md)\n\n[live](missing-review-target.md)\n', self.root)
        self.assertEqual(
            [{'kind': 'link', 'path': 'valid_links.md', 'detail': 'missing target: missing-review-target.md'}],
            probe_bq.errors,
        )
        probe_lazy = drift.Checker(self.root)
        probe_lazy.links('valid_links.md', '> paragraph\n    [live](missing-lazy-target.md)\n', self.root)
        self.assertEqual(
            [{'kind': 'link', 'path': 'valid_links.md', 'detail': 'missing target: missing-lazy-target.md'}],
            probe_lazy.errors,
        )

        # Non-Setext constructs (frontmatter, tab-indented code blocks, block HTML, list item before ---, blockquote before ---, table separator) must not create anchors.
        for bad_frag in ('#title-target-frontmatter', '#tab-indented-code', '#tab-paragraph', '#block-html', '#list-item', '#blockquote-line', '#table-col'):
            self.write('bad_anchor.md', f'[Bad](target.md{bad_frag})\n')
            c_bad = self.check()
            self.assertTrue(
                any(e['kind'] == 'anchor' and e['path'] == 'bad_anchor.md' and bad_frag in e['detail'] for e in c_bad.errors),
                (bad_frag, c_bad.errors),
            )
        (self.root / 'bad_anchor.md').unlink()

        # Broken nested-label, parenthesized, angle-bracketed, multiline, nested-image, next-line reference, blockquote reference, and unresolved reference links must each report a link error.
        for bad_md, expected_target in (
            ('[a [nested] label](gone.md)\n', 'gone.md'),
            ('[paren](dir/missing(1).md)\n', 'dir/missing(1).md'),
            ('[angle](<dir/missing (2).md>)\n', 'dir/missing (2).md'),
            ('[multiline](\n  missing-multiline.md\n)\n', 'missing-multiline.md'),
            ('[![badge](dir/missing.svg)](target.md)\n', 'dir/missing.svg'),
            ('[ref][r]\n\n[r]: <dir/missing (3).md>\n', 'dir/missing (3).md'),
            ('[guide][g]\n\n[g]:\n  dir/missing_nextline.md\n', 'dir/missing_nextline.md'),
            ('> [bq][bq_bad]\n>\n> [bq_bad]: missing-bq.md\n', 'missing-bq.md'),
            ('[Guide][undefined_ref]\n', 'unresolved reference link: [undefined_ref]'),
        ):
            self.write('bad_link.md', bad_md)
            c_link = self.check()
            self.assertTrue(
                any(e['kind'] == 'link' and e['path'] == 'bad_link.md' and expected_target in e['detail'] for e in c_link.errors),
                (bad_md, c_link.errors),
            )
        (self.root / 'bad_link.md').unlink()
        self.assertEqual([], self.check().errors)

        # Built Jekyll artifact in build/site-artifact/ validates relative links, /RasterFold/ base-path links, and HTML IDs.
        self.write('.gitignore', 'build/\n')
        self.write('build/site-artifact/index.html', '<html><body><a href="/RasterFold/format.html#format-id">Format</a></body></html>\n')
        self.write('build/site-artifact/format.html', '<html><body><h2 id="format-id">Format</h2><a href="index.html">Home</a></body></html>\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact', '--write-site-receipt']))
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))

        # Seed broken relative link, out-of-base-path link, and missing HTML fragment in build/site-artifact/.
        self.write(
            'build/site-artifact/index.html',
            '<html><body>'
            '<a href="missing-page.html">Dead</a>'
            '<a href="/outside/page.html">Outside</a>'
            '<a href="/RasterFold/format.html#missing-html-id">BadFrag</a>'
            '</body></html>\n',
        )
        c_site = self.check()
        self.write('build/site-artifact/drift-build.json', json.dumps(c_site.site_receipt('build/site-artifact')))
        out_buf = io.StringIO()
        with contextlib.redirect_stdout(out_buf):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))
        site_out = out_buf.getvalue()
        self.assertIn('link: index.html: missing target: missing-page.html', site_out)
        self.assertIn('site-link: index.html: URL outside Pages base path: /outside/page.html', site_out)
        self.assertIn('anchor: index.html: missing heading or id: /RasterFold/format.html#missing-html-id', site_out)

        # Restore valid built site and verify clean exit 0.
        self.write('build/site-artifact/index.html', '<html><body><a href="/RasterFold/format.html#format-id">Format</a></body></html>\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact', '--write-site-receipt']))
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))

        # Verify and execute gate entrypoints (scripts/check-all.ps1, scripts/check-all.py, .github/workflows/plan.yml, tools/githooks/pre-commit).
        ps1_text = (REPO / 'scripts/check-all.ps1').read_text(encoding='utf-8')
        self.assertIn('scripts/check-all.py', ps1_text)
        self.assertIn('exit $LASTEXITCODE', ps1_text)
        check_all_text = (REPO / 'scripts/check-all.py').read_text(encoding='utf-8')
        self.assertIn("('document drift','scripts/drift-check.py',[])", check_all_text)
        plan_yml_text = (REPO / '.github/workflows/plan.yml').read_text(encoding='utf-8')
        self.assertIn('python3 scripts/drift-check.py\n', plan_yml_text)
        self.assertIn('python3 scripts/drift-check.py --site-dir build/site-artifact --write-site-receipt', plan_yml_text)
        self.assertIn('python3 scripts/drift-check.py --site-dir build/site-artifact\n', plan_yml_text)

        self.write('scripts/drift-check.py', (REPO / 'scripts/drift-check.py').read_text(encoding='utf-8'))
        self.write('scripts/check-all.py', (REPO / 'scripts/check-all.py').read_text(encoding='utf-8'))
        self.write('scripts/check-all.ps1', (REPO / 'scripts/check-all.ps1').read_text(encoding='utf-8'))
        self.git('add', '-A')
        self.git('commit', '-qm', 'Install updated drift-check and check-all for entrypoint tests')

        orig_readme = (self.root / 'README.md').read_text(encoding='utf-8')
        # Seed a dead link in README.md and verify failure across local runner (check-all.py / check-all.ps1) and CI drift-check command.
        self.write('README.md', orig_readme + '\nSee [broken [nested] link](nonexistent.md).\n')
        local_fail = subprocess.run([sys.executable, 'scripts/check-all.py', '--gate', 'document drift'], cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(1, local_fail.returncode)
        self.assertIn('FAIL document drift', local_fail.stdout)
        self.assertIn('missing target: nonexistent.md', local_fail.stdout)
        if shutil.which('pwsh'):
            ps_fail = subprocess.run(['pwsh', 'scripts/check-all.ps1', '-Gate', 'document drift'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertEqual(1, ps_fail.returncode)
            self.assertIn('FAIL document drift', ps_fail.stdout)
        ci_fail = subprocess.run([sys.executable, 'scripts/drift-check.py'], cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(1, ci_fail.returncode)
        self.assertIn('missing target: nonexistent.md', ci_fail.stdout)

        # Restore README.md and verify clean exit 0 across local runner and CI drift-check command.
        self.write('README.md', orig_readme)
        local_ok = subprocess.run([sys.executable, 'scripts/check-all.py', '--gate', 'document drift'], cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(0, local_ok.returncode, local_ok.stdout + local_ok.stderr)
        self.assertIn('PASS document drift', local_ok.stdout)
        if shutil.which('pwsh'):
            ps_ok = subprocess.run(['pwsh', 'scripts/check-all.ps1', '-Gate', 'document drift'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertEqual(0, ps_ok.returncode, ps_ok.stdout + ps_ok.stderr)
            self.assertIn('PASS document drift', ps_ok.stdout)
        ci_ok = subprocess.run([sys.executable, 'scripts/drift-check.py'], cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(0, ci_ok.returncode)
        ci_site_ok = subprocess.run([sys.executable, 'scripts/drift-check.py', '--site-dir', 'build/site-artifact'], cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(0, ci_site_ok.returncode)

        if shutil.which('sh'):
            self.git('config', 'core.hooksPath', str(REPO / 'tools/githooks'))
            head_before = self.git('rev-parse', 'HEAD')
            self.write('README.md', orig_readme + '\nSee [broken [nested] link](nonexistent.md).\n')
            self.git('add', 'README.md')
            proc = subprocess.run(['git', 'commit', '-m', 'Should fail on broken link'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(0, proc.returncode)
            self.assertIn('missing target: nonexistent.md', (proc.stdout or '') + (proc.stderr or ''))
            self.assertEqual(head_before, self.git('rev-parse', 'HEAD'))
            # Restore valid link and verify hook recovery (commit succeeds and HEAD advances).
            self.write('README.md', orig_readme + '\nSee [valid [nested] link](target.md#guide).\n')
            self.git('add', 'README.md')
            proc_ok = subprocess.run(['git', 'commit', '-m', 'Recovered commit after fixing broken link'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertEqual(0, proc_ok.returncode, (proc_ok.stdout or '') + (proc_ok.stderr or ''))
            self.assertNotEqual(head_before, self.git('rev-parse', 'HEAD'))

    def test_deterministic_repair_and_complete_file_coverage(self):
        import os
        from unittest import mock

        # 1. Live repository coverage inventory: every supported file has an explicit role, all 7 roles are represented,
        # docs/facts.json is generated, and .gitattributes/.gitignore are configuration.
        live = drift.Checker(REPO)
        live.load()
        self.assertEqual([], [e for e in live.errors if e['kind'] == 'coverage'])
        role_by_path = {entry['path']: entry['role'] for entry in live.inventory}
        self.assertEqual('generated', role_by_path.get('docs/facts.json'))
        self.assertEqual('configuration', role_by_path.get('.gitattributes'))
        self.assertEqual('configuration', role_by_path.get('.gitignore'))
        self.assertEqual(drift.ROLES, {entry['role'] for entry in live.inventory})

        # Set up a generator in the fixture manifest so both direct and generator repairs (including cache_outputs) are exercised.
        orig_gen = (
            'import json, os, pathlib, sys\n'
            'p = pathlib.Path("todo/implementation-plan.md")\n'
            'prog = pathlib.Path("build/todo-progress.json")\n'
            'want = "# Plan\\n"\n'
            'ts = os.environ.get("TEST_PROGRESS_TS", "2026-10-25T08:00:01Z")\n'
            'want_prog = json.dumps({"generated_at": ts, "ok": True}, indent=2, sort_keys=True) + "\\n"\n'
            'if "--check" in sys.argv:\n'
            '    ok = p.is_file() and p.read_text(encoding="utf-8") == want\n'
            '    if prog.exists():\n'
            '        data = json.loads(prog.read_text(encoding="utf-8"))\n'
            '        ok = ok and isinstance(data, dict) and data.get("ok") is True and bool(data.get("generated_at"))\n'
            '    sys.exit(0 if ok else 1)\n'
            'if "--sync" in sys.argv:\n'
            '    p.parent.mkdir(parents=True, exist_ok=True)\n'
            '    p.write_text(want, encoding="utf-8")\n'
            '    prog.parent.mkdir(parents=True, exist_ok=True)\n'
            '    prog.write_text(want_prog, encoding="utf-8")\n'
            '    sys.exit(0)\n'
        )
        self.write('scripts/todo-graph.py', orig_gen)
        self.write('todo/implementation-plan.md', '# Plan\n')
        self.manifest['encodings'] = {}
        self.manifest['generators'] = [
            {
                'script': 'scripts/todo-graph.py',
                'check': ['plan', '--check'],
                'fix': ['plan', '--sync'],
                'outputs': ['todo/implementation-plan.md'],
                'cache_outputs': ['build/todo-progress.json'],
                'planned_outputs': {'docs/api.md': 'D08 T01 §4'},
            }
        ]
        self.save()
        self.git('add', '.')
        self.git('commit', '-qm', 'Add generator fixture for deterministic repair tests')

        # 2. Manifest contract validation catches invalid rules even if they do not match a file in the working tree.
        base_manifest = json.loads((self.root / 'docs/drift.json').read_text(encoding='utf-8'))

        def assert_manifest_rejected(mutator, msg_fragment):
            m = json.loads(json.dumps(base_manifest))
            mutator(m)
            self.write('docs/drift.json', json.dumps(m, indent=2) + '\n')
            c = drift.Checker(self.root)
            with self.assertRaises(ValueError) as ctx:
                c.load()
            self.assertIn(msg_fragment, str(ctx.exception))

        try:
            assert_manifest_rejected(
                lambda m: m['coverage'].append({'pattern': 'never-matches-*.md', 'role': 'not-a-role', 'reason': 'bad'}),
                'invalid coverage rule',
            )
            assert_manifest_rejected(
                lambda m: m['coverage'].append({'pattern': 'never-matches-*.md', 'role': 'current', 'reason': '   '}),
                'invalid coverage rule',
            )
            for bad_kind in ('c-define', 'file-sha256', 'line-count'):
                assert_manifest_rejected(
                    lambda m, k=bad_kind: m['facts'].update({'bad_fact': {'kind': k, 'source': 'src/version.h'}}),
                    f"unknown provider kind '{bad_kind}'",
                )
            assert_manifest_rejected(
                lambda m: m['blocks']['version'].update({'template': 'Version {unknown_fact}\n'}),
                "unknown fact placeholder 'unknown_fact'",
            )
            assert_manifest_rejected(
                lambda m: m['blocks']['version'].update({'path': '../escape.md'}),
                'path escapes repository',
            )
            assert_manifest_rejected(
                lambda m: m['assertions'].append({'pattern': 'README.md', 'regex': r'v(\d+)', 'fact': 'no_such_fact'}),
                'invalid assertion rule',
            )
            assert_manifest_rejected(
                lambda m: m['encodings'].update({'README.md': 'not-a-real-codec-name'}),
                'unknown encoding',
            )
            assert_manifest_rejected(
                lambda m: m['generators'][0]['outputs'].append('../escape.md'),
                'path escapes repository',
            )
        finally:
            self.write('docs/drift.json', json.dumps(base_manifest, indent=2) + '\n')

        # Invalid bytes under a declared encoding report an encoding error cleanly.
        orig_readme_text = (self.root / 'README.md').read_text(encoding='utf-8')
        (self.root / 'README.md').write_bytes(b'\xff\xfeinvalid utf-8\n')
        c_enc = self.check()
        self.assertIn('encoding', self.kinds(c_enc))
        self.write('README.md', orig_readme_text)

        # 3. Registered repair target & block-boundary enforcement in Checker.fix().
        c_unreg = self.check()
        self.write('docs/authored.md', '# Authored\n')
        self.git('add', 'docs/authored.md')
        c_unreg.repairs['docs/authored.md'] = (b'# Authored\n', b'# Tampered\n')
        with self.assertRaises(ValueError) as ctx:
            c_unreg.fix()
        self.assertIn('refusing to repair unregistered path: docs/authored.md', str(ctx.exception))
        self.assertEqual('# Authored\n', (self.root / 'docs/authored.md').read_text(encoding='utf-8'))

        readme_before = (self.root / 'README.md').read_bytes()
        c_outside = self.check()
        c_outside.repairs['README.md'] = (
            readme_before,
            readme_before.replace(b'# Guide\n', b'# Altered Heading\n'),
        )
        with self.assertRaises(ValueError) as ctx:
            c_outside.fix()
        self.assertIn('README.md: refusing to modify content outside registered generated blocks', str(ctx.exception))
        self.assertEqual(readme_before, (self.root / 'README.md').read_bytes())

        # 4. Staged generator --fix safety: undeclared writes, planned_outputs writes, and concurrent edits.
        plan_before = (self.root / 'todo/implementation-plan.md').read_bytes()

        # 4a. Generator --fix modifying an undeclared file (README.md or nested src/version.h) is refused in staging and prevents publication.
        self.write(
            'scripts/todo-graph.py',
            'import pathlib, sys\n'
            'if "--sync" in sys.argv:\n'
            '    pathlib.Path("README.md").write_text("tampered\\n", encoding="utf-8")\n'
            '    pathlib.Path("todo/implementation-plan.md").write_text("# Plan\\n", encoding="utf-8")\n'
            'sys.exit(0)\n',
        )
        c_undecl = drift.Checker(self.root)
        c_undecl.load()
        with self.assertRaises(ValueError) as ctx:
            c_undecl.generators(fix=True)
        self.assertIn('modified undeclared path README.md', str(ctx.exception))
        self.assertEqual(readme_before, (self.root / 'README.md').read_bytes())

        version_before = (self.root / 'src/version.h').read_bytes()
        self.write('todo/implementation-plan.md', 'stale plan\n')
        self.write(
            'scripts/todo-graph.py',
            'import pathlib, sys\n'
            'if "--sync" in sys.argv:\n'
            '    pathlib.Path("src/version.h").write_text("#define VERSION 99u\\n", encoding="utf-8")\n'
            '    pathlib.Path("todo/implementation-plan.md").write_text("# Plan\\n", encoding="utf-8")\n'
            'sys.exit(0)\n',
        )
        c_undecl_src = drift.Checker(self.root)
        c_undecl_src.load()
        with self.assertRaises(ValueError) as ctx:
            c_undecl_src.generators(fix=True)
        self.assertIn('modified undeclared path src/version.h', str(ctx.exception))
        self.assertEqual(version_before, (self.root / 'src/version.h').read_bytes())
        self.assertEqual(b'stale plan\n', (self.root / 'todo/implementation-plan.md').read_bytes())
        (self.root / 'todo/implementation-plan.md').write_bytes(plan_before)

        # 4b. Generator --fix creating a planned_outputs path (docs/api.md) is refused in staging.
        self.write(
            'scripts/todo-graph.py',
            'import pathlib, sys\n'
            'if "--sync" in sys.argv:\n'
            '    pathlib.Path("docs/api.md").write_text("planned\\n", encoding="utf-8")\n'
            'sys.exit(0)\n',
        )
        c_planned = drift.Checker(self.root)
        c_planned.load()
        with self.assertRaises(ValueError) as ctx:
            c_planned.generators(fix=True)
        self.assertIn('planned output docs/api.md', str(ctx.exception))
        self.assertFalse((self.root / 'docs/api.md').exists())

        # 4c. Concurrent edit to a generator output while staging or during os.fsync is refused by compare-before-write publication.
        self.write('scripts/todo-graph.py', orig_gen)
        self.write('todo/implementation-plan.md', 'stale plan\n')
        c_race = drift.Checker(self.root)
        c_race.load()

        def concurrent_plan_edit(script):
            if script == 'scripts/todo-graph.py':
                (self.root / 'todo/implementation-plan.md').write_text('concurrent user edit\n', encoding='utf-8')

        c_race._before_generator_publish_hook = concurrent_plan_edit
        with self.assertRaises(ValueError) as ctx:
            c_race.generators(fix=True)
        self.assertIn('todo/implementation-plan.md changed during repair; refusing to overwrite', str(ctx.exception))
        self.assertEqual('concurrent user edit\n', (self.root / 'todo/implementation-plan.md').read_text(encoding='utf-8'))
        (self.root / 'todo/implementation-plan.md').write_bytes(plan_before)

        # Concurrent edit during os.fsync in Checker.fix() is refused before os.replace.
        self.write('README.md', '# Guide\n\nAuthored words stay here.\n\n<!-- drift:begin version -->\nstale block\n<!-- drift:end version -->\n')
        c_fsync_race = self.check()
        real_fsync = os.fsync

        def fsync_mutate_readme(fd):
            real_fsync(fd)
            (self.root / 'README.md').write_text('concurrent edit during fsync\n', encoding='utf-8')

        with mock.patch('os.fsync', side_effect=fsync_mutate_readme):
            with self.assertRaises(ValueError) as ctx:
                c_fsync_race.fix()
        self.assertIn('README.md changed during repair; refusing to overwrite', str(ctx.exception))
        self.assertEqual('concurrent edit during fsync\n', (self.root / 'README.md').read_text(encoding='utf-8'))
        self.assertEqual([], list(self.root.rglob('.drift-*')))
        (self.root / 'README.md').write_bytes(readme_before)

        # Unreferenced symlink in the checkout pointing outside self.root is refused before copying or running generators.
        with tempfile.TemporaryDirectory() as ext_dir:
            ext_file = Path(ext_dir) / 'outside.c'
            ext_file.write_text('int outside = 1;\n', encoding='utf-8')
            ext_link = self.root / 'src/ext_link.c'
            try:
                ext_link.symlink_to(ext_file)
            except OSError:
                ext_link = None
            if ext_link is not None:
                try:
                    c_sym = drift.Checker(self.root)
                    c_sym.load()
                    with self.assertRaises(ValueError) as ctx:
                        c_sym.generators(fix=True)
                    self.assertIn('src/ext_link.c', str(ctx.exception))
                    self.assertEqual('int outside = 1;\n', ext_file.read_text(encoding='utf-8'))
                finally:
                    ext_link.unlink(missing_ok=True)

        # 4d. Direct test of research/milestones.py atomic_write_text safety (compare-before-write including during fsync, symlink refusal, failure cleanup).
        ms_spec = importlib.util.spec_from_file_location('milestones_mod', REPO / 'research/milestones.py')
        ms_mod = importlib.util.module_from_spec(ms_spec)
        ms_spec.loader.exec_module(ms_mod)
        ms_mod.REPO = self.root
        ms_target = self.root / 'todo/05-milestones/TODO-01-milestone-gates.md'
        ms_target.parent.mkdir(parents=True, exist_ok=True)
        ms_target.write_bytes(b'original milestone bytes\n')
        os.chmod(ms_target, 0o640)
        with self.assertRaises(ValueError) as ctx:
            ms_mod.atomic_write_text(ms_target, 'new bytes\n', b'wrong before bytes\n')
        self.assertIn('changed during write; refusing to overwrite', str(ctx.exception))
        self.assertEqual(b'original milestone bytes\n', ms_target.read_bytes())

        def fsync_mutate_milestone(fd):
            real_fsync(fd)
            ms_target.write_bytes(b'concurrent milestone edit during fsync\n')

        with mock.patch('os.fsync', side_effect=fsync_mutate_milestone):
            with self.assertRaises(ValueError) as ctx:
                ms_mod.atomic_write_text(ms_target, 'new bytes\n', b'original milestone bytes\n')
        self.assertIn('changed during write; refusing to overwrite', str(ctx.exception))
        self.assertEqual(b'concurrent milestone edit during fsync\n', ms_target.read_bytes())
        self.assertEqual([], list(ms_target.parent.glob('.milestones-*')))
        ms_target.write_bytes(b'original milestone bytes\n')
        os.chmod(ms_target, 0o640)

        with mock.patch('os.replace', side_effect=OSError('simulated milestone replace failure')):
            with self.assertRaises(OSError):
                ms_mod.atomic_write_text(ms_target, 'new bytes\n', b'original milestone bytes\n')
        self.assertEqual(b'original milestone bytes\n', ms_target.read_bytes())
        self.assertEqual(0o640, ms_target.stat().st_mode & 0o777)
        self.assertEqual([], list(ms_target.parent.glob('.milestones-*')))
        ms_mod.atomic_write_text(ms_target, 'updated milestone bytes\n', b'original milestone bytes\n')
        self.assertEqual(b'updated milestone bytes\n', ms_target.read_bytes())
        self.assertEqual(0o640, ms_target.stat().st_mode & 0o777)
        ms_target.unlink()

        # 5. Injected write/replacement failure preservation, temp cleanup, permission preservation, stale cache repair, and idempotent recovery.
        self.write('README.md', '# Guide\n\nAuthored words stay here.\n\n<!-- drift:begin version -->\nstale block\n<!-- drift:end version -->\n')
        self.write('docs/facts.json', '{\n  "version": 999\n}\n')
        self.write('todo/implementation-plan.md', 'stale plan\n')
        self.write('build/todo-progress.json', '{"ok": false}\n')
        for rel in ('README.md', 'docs/facts.json', 'todo/implementation-plan.md', 'build/todo-progress.json'):
            os.chmod(self.root / rel, 0o640)
        drifted_readme = (self.root / 'README.md').read_bytes()
        drifted_facts = (self.root / 'docs/facts.json').read_bytes()
        drifted_plan = (self.root / 'todo/implementation-plan.md').read_bytes()
        drifted_prog = (self.root / 'build/todo-progress.json').read_bytes()

        with mock.patch('os.fsync', side_effect=OSError('simulated fsync failure')):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(2, drift.main(['--root', str(self.root), '--fix']))
        self.assertEqual(drifted_readme, (self.root / 'README.md').read_bytes())
        self.assertEqual(drifted_facts, (self.root / 'docs/facts.json').read_bytes())
        self.assertEqual(drifted_plan, (self.root / 'todo/implementation-plan.md').read_bytes())
        self.assertEqual(drifted_prog, (self.root / 'build/todo-progress.json').read_bytes())
        self.assertEqual(0o640, (self.root / 'README.md').stat().st_mode & 0o777)
        self.assertEqual([], list(self.root.rglob('.drift-*')))

        with mock.patch('os.replace', side_effect=OSError('simulated replace failure')):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(2, drift.main(['--root', str(self.root), '--fix']))
        self.assertEqual(drifted_readme, (self.root / 'README.md').read_bytes())
        self.assertEqual(drifted_facts, (self.root / 'docs/facts.json').read_bytes())
        self.assertEqual(drifted_plan, (self.root / 'todo/implementation-plan.md').read_bytes())
        self.assertEqual(drifted_prog, (self.root / 'build/todo-progress.json').read_bytes())
        self.assertEqual(0o640, (self.root / 'README.md').stat().st_mode & 0o777)
        self.assertEqual([], list(self.root.rglob('.drift-*')))

        # Recovery and idempotence: unpatched --fix succeeds, repairs build/todo-progress.json, preserves 0o640 mode,
        # and a second --fix in a later second (different generated_at) preserves build/todo-progress.json byte-for-byte.
        out_fix1 = io.StringIO()
        with mock.patch.dict(os.environ, {'TEST_PROGRESS_TS': '2026-10-25T08:00:01Z'}):
            with contextlib.redirect_stdout(out_fix1):
                self.assertEqual(0, drift.main(['--root', str(self.root), '--fix']))
        self.assertIn('0 error(s); 0 deterministic repair(s)', out_fix1.getvalue())
        self.assertEqual(
            json.dumps({'generated_at': '2026-10-25T08:00:01Z', 'ok': True}, indent=2, sort_keys=True) + '\n',
            (self.root / 'build/todo-progress.json').read_text(encoding='utf-8'),
        )
        for rel in ('README.md', 'docs/facts.json', 'todo/implementation-plan.md', 'build/todo-progress.json'):
            self.assertEqual(0o640, (self.root / rel).stat().st_mode & 0o777)
        snap_after_fix1 = {
            rel: drift.digest((self.root / rel).read_bytes())
            for rel in ('README.md', 'docs/facts.json', 'todo/implementation-plan.md', 'build/todo-progress.json')
        }
        out_fix2 = io.StringIO()
        with mock.patch.dict(os.environ, {'TEST_PROGRESS_TS': '2026-10-25T08:00:09Z'}):
            with contextlib.redirect_stdout(out_fix2):
                self.assertEqual(0, drift.main(['--root', str(self.root), '--fix']))
        self.assertIn('0 error(s); 0 deterministic repair(s)', out_fix2.getvalue())
        snap_after_fix2 = {
            rel: drift.digest((self.root / rel).read_bytes())
            for rel in ('README.md', 'docs/facts.json', 'todo/implementation-plan.md', 'build/todo-progress.json')
        }
        self.assertEqual(snap_after_fix1, snap_after_fix2)

        # Also verify against real scripts/todo-graph.py generated_progress_text across distinct UTC seconds.
        tg_spec = importlib.util.spec_from_file_location('todo_graph_mod', REPO / 'scripts/todo-graph.py')
        tg_mod = importlib.util.module_from_spec(tg_spec)
        sys.modules['todo_graph_mod'] = tg_mod
        tg_spec.loader.exec_module(tg_mod)
        real_todos = tg_mod.load_todos()
        payload_t1 = tg_mod.generated_progress_text(real_todos, generated_at='2026-10-25T08:00:01Z').encode('utf-8')
        payload_t2 = tg_mod.generated_progress_text(real_todos, generated_at='2026-10-25T08:00:02Z').encode('utf-8')
        self.assertNotEqual(payload_t1, payload_t2)
        self.assertTrue(drift.Checker._cache_output_semantically_equal(payload_t1, payload_t2))
        payload_future = tg_mod.generated_progress_text(real_todos, generated_at='2099-01-01T00:00:00Z').encode('utf-8')
        self.assertFalse(drift.Checker._cache_output_semantically_equal(payload_future, payload_t2))
        mutated_obj = json.loads(payload_t2.decode('utf-8'))
        mutated_obj['sentinel'] = -1
        self.assertFalse(
            drift.Checker._cache_output_semantically_equal(
                payload_t1,
                (json.dumps(mutated_obj, indent=2, sort_keys=True) + '\n').encode('utf-8'),
            )
        )
        # Numeric and boolean type substitutions (0 vs 0.0, 0 vs False, 1 vs True) must not be treated as equal.
        float_obj = json.loads(payload_t1.decode('utf-8'))
        float_obj['current_phase_id'] = 0.0
        self.assertFalse(
            drift.Checker._cache_output_semantically_equal(
                (json.dumps(float_obj, indent=2, sort_keys=True) + '\n').encode('utf-8'),
                payload_t2,
            )
        )
        bool_obj = json.loads(payload_t1.decode('utf-8'))
        bool_obj['current_phase_id'] = False
        self.assertFalse(
            drift.Checker._cache_output_semantically_equal(
                (json.dumps(bool_obj, indent=2, sort_keys=True) + '\n').encode('utf-8'),
                payload_t2,
            )
        )
        schema_bool_obj = json.loads(payload_t1.decode('utf-8'))
        schema_bool_obj['schema'] = True
        self.assertFalse(
            drift.Checker._cache_output_semantically_equal(
                (json.dumps(schema_bool_obj, indent=2, sort_keys=True) + '\n').encode('utf-8'),
                payload_t2,
            )
        )

    def test_commit_and_built_site_provenance(self):
        # 1. Explicit commit/path references: commit object required, normalized relative path required.
        head0 = self.git('rev-parse', 'HEAD')
        tree0 = self.git('rev-parse', 'HEAD^{tree}')
        blob0 = self.git('rev-parse', 'HEAD:src/version.h')
        self.write('history.md', f'Valid ref <!-- drift:commit {head0} src/version.h --> and commit <!-- drift:commit {head0} -->.\n')
        self.assertEqual([], self.check().errors)

        for bad_ref in (
            '<!-- drift:commit deadbeefdeadbeef -->',
            f'<!-- drift:commit {tree0} src/version.h -->',
            f'<!-- drift:commit {blob0} -->',
            f'<!-- drift:commit {head0} src/missing.h -->',
            f'<!-- drift:commit {head0} /src/version.h -->',
            f'<!-- drift:commit {head0} src/version.h/ -->',
            f'<!-- drift:commit {head0} ../src/version.h -->',
            f'<!-- drift:commit {head0} ./src/version.h -->',
            f'<!-- drift:commit {head0} src//version.h -->',
            f'<!-- drift:commit {head0} src\\version.h -->',
        ):
            self.write('history.md', bad_ref + '\n')
            c_ref = self.check()
            self.assertIn('commit', self.kinds(c_ref), bad_ref)
        (self.root / 'history.md').unlink()

        # 2. Historical Drift-Fact commit declarations evaluated against their own tree, plus malformed trailer rejection.
        self.git('commit', '--allow-empty', '-qm', 'Declare v2\n\nDrift-Fact: version=2')
        commit_v2 = self.git('rev-parse', 'HEAD')
        self.write('src/version.h', '#define VERSION 3u\n')
        c_up = self.check()
        c_up.fix()
        self.git('add', '.')
        self.git('commit', '-qm', 'Advance to v3\n\nDrift-Fact: version=3')
        commit_v3 = self.git('rev-parse', 'HEAD')

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--commit-range', f'{head0}..{commit_v3}']))
            self.assertEqual(0, drift.main(['--root', str(self.root), '--commit-range', commit_v2]))
            self.assertEqual(0, drift.main(['--root', str(self.root), '--commit-range', commit_v3]))

        for bad_msg in (
            'Title\n\ndrift-fact: version=3\n',
            'Title\n\n  Drift-Fact: version=3\n',
            'Title\n\nDrift-Fact: version=\n',
            'Title\n\nDrift-Fact version=3\n',
            'Title\n\nDrift-Fact: version=2\n',
        ):
            self.write('msg.txt', bad_msg)
            out_msg = io.StringIO()
            with contextlib.redirect_stdout(out_msg):
                self.assertEqual(1, drift.main(['--root', str(self.root), '--commit-message', str(self.root / 'msg.txt')]), bad_msg)
            self.assertIn('commit-fact:', out_msg.getvalue())
        (self.root / 'msg.txt').unlink()

        # 3. Built-site provenance bound to docs/site-content.json allowlist and site_sources.
        self.write('.gitignore', 'build/\n')
        self.manifest['site_sources'] = [
            'README.md',
            'docs/*.md',
            'docs/facts.json',
            'docs/site-content.json',
            'docs/_*',
            'src/*',
            'assets/*',
            'docs/drift.json',
        ]
        self.manifest['forbidden_site_paths'] = ['.agents', '.claude', '.codex', 'todo', 'agents.md', 'gemini.md']
        self.write('docs/_config.yml', 'title: RasterFold\n')
        self.write('docs/index.md', '# Overview\n')
        self.write('docs/usage.md', '# Usage\n')
        self.write('assets/brand/icon.svg', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"></svg>\n')
        site_content = {
            'schema': 1,
            'repository': 'rizonesoft/RasterFold',
            'visibility': 'public',
            'base_path': '/RasterFold/',
            'config': [{'source': 'docs/_config.yml', 'staged': '_config.yml'}],
            'pages': [
                {'id': 'overview', 'source': 'docs/index.md', 'staged': 'index.md', 'title': 'Overview', 'nav_order': 1, 'kind': 'authored'},
                {'id': 'usage', 'source': 'README.md', 'output': 'docs/usage.md', 'staged': 'usage.md', 'title': 'Usage', 'nav_order': 2, 'kind': 'mirrored'},
            ],
            'assets': [{'source': 'assets/brand/icon.svg', 'staged': 'assets/brand/icon.svg'}],
        }
        self.write('docs/site-content.json', json.dumps(site_content, indent=2) + '\n')
        self.save()
        c_sync = self.check()
        c_sync.fix()

        self.write('build/site-artifact/index.html', '<html><body><h1 id="top">Overview</h1><a href="/RasterFold/usage.html#cli">Usage</a></body></html>\n')
        self.write('build/site-artifact/usage.html', '<html><body><h1 id="cli">CLI</h1><a href="index.html#top">Home</a></body></html>\n')
        self.write('build/site-artifact/assets/brand/icon.svg', '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"></svg>\n')

        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact', '--write-site-receipt']))
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))

        receipt_data = json.loads((self.root / 'build/site-artifact/drift-build.json').read_text(encoding='utf-8'))
        for expected_src in ('docs/site-content.json', 'docs/_config.yml', 'docs/index.md', 'README.md', 'docs/usage.md', 'assets/brand/icon.svg'):
            self.assertIn(expected_src, receipt_data['sources'])

        # Changing docs/site-content.json mapping invalidates the built-site receipt.
        mutated_sc = dict(site_content, pages=[dict(site_content['pages'][0], title='Changed Title'), site_content['pages'][1]])
        self.write('docs/site-content.json', json.dumps(mutated_sc, indent=2) + '\n')
        out_sc = io.StringIO()
        with contextlib.redirect_stdout(out_sc):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))
        self.assertIn('site source receipt differs from current inputs', out_sc.getvalue())
        self.write('docs/site-content.json', json.dumps(site_content, indent=2) + '\n')

        # Omitting docs/site-content.json from site_sources fails the allowlist coverage contract.
        orig_sources = list(self.manifest['site_sources'])
        self.manifest['site_sources'] = [p for p in orig_sources if p != 'docs/site-content.json']
        self.save()
        out_uncovered = io.StringIO()
        with contextlib.redirect_stdout(out_uncovered):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))
        self.assertIn('allowlisted site source not covered by site_sources: docs/site-content.json', out_uncovered.getvalue())
        self.manifest['site_sources'] = orig_sources
        self.save()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact', '--write-site-receipt']))

        # Missing allowlisted built output, private path (.agents / gemini.md), or broken HTML fragment fails.
        (self.root / 'build/site-artifact/usage.html').unlink()
        out_missing_page = io.StringIO()
        with contextlib.redirect_stdout(out_missing_page):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))
        self.assertIn('missing allowlisted built output: usage.html', out_missing_page.getvalue())

        self.write('build/site-artifact/usage.html', '<html><body><h1 id="wrong-id">CLI</h1></body></html>\n')
        self.write('build/site-artifact/.agents/private.html', 'secret\n')
        self.write('build/site-artifact/GEMINI.md', 'secret\n')
        c_re = self.check()
        self.write('build/site-artifact/drift-build.json', json.dumps(c_re.site_receipt('build/site-artifact')))
        out_broken = io.StringIO()
        with contextlib.redirect_stdout(out_broken):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--site-dir', 'build/site-artifact']))
        self.assertIn('anchor: index.html: missing heading or id: /RasterFold/usage.html#cli', out_broken.getvalue())
        self.assertIn('site-private: build/site-artifact: private path in artifact: .agents', out_broken.getvalue())
        self.assertIn('site-private: build/site-artifact: private path in artifact: GEMINI.md', out_broken.getvalue())
        shutil.rmtree(self.root / 'build/site-artifact')

        # 4. Staged hooks (pre-commit and commit-msg) refuse unstaged cover-ups and staged gate-script removal.
        if shutil.which('sh'):
            self.write('scripts/drift-check.py', (REPO / 'scripts/drift-check.py').read_text(encoding='utf-8'))
            self.git('add', '.')
            self.git('commit', '-qm', 'Sync fixture with live drift-check.py')
            self.git('config', 'core.hooksPath', str(REPO / 'tools/githooks'))
            head_before = self.git('rev-parse', 'HEAD')

            # (a) Bad staged document concealed by clean unstaged working copy -> refused by pre-commit.
            good_readme = (self.root / 'README.md').read_text(encoding='utf-8')
            self.write('README.md', good_readme.replace('Version 3.', 'Version 99.'))
            self.git('add', 'README.md')
            self.write('README.md', good_readme)
            res_doc = subprocess.run(['git', 'commit', '-qm', 'Concealed bad staged doc'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(0, res_doc.returncode)
            self.assertIn('staged documentation drift', res_doc.stderr)
            self.assertEqual(head_before, self.git('rev-parse', 'HEAD'))
            self.git('add', 'README.md')

            # (b) False staged Drift-Fact concealed by unstaged source modification -> refused by commit-msg.
            self.write('src/version.h', '#define VERSION 4u\n')
            c_v4 = self.check()
            c_v4.fix()
            self.git('add', 'src/version.h', 'README.md', 'docs/facts.json')
            # Mutate unstaged working tree back to VERSION 2 so an unstaged reader would think version=2.
            self.write('src/version.h', '#define VERSION 2u\n')
            self.write('docs/facts.json', '{\n  "version": 2\n}\n')
            res_fact = subprocess.run(['git', 'commit', '-qm', 'Concealed false fact\n\nDrift-Fact: version=2'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(0, res_fact.returncode)
            self.assertIn('commit-fact: Drift-Fact: version=2: differs from that commit\'s facts', res_fact.stdout + res_fact.stderr)
            self.assertEqual(head_before, self.git('rev-parse', 'HEAD'))

            # Restore unstaged files to match the valid v4 index and commit with Drift-Fact: version=4 -> succeeds.
            self.git('checkout', '--', 'src/version.h', 'docs/facts.json')
            res_ok = subprocess.run(['git', 'commit', '-qm', 'Valid v4 fact\n\nDrift-Fact: version=4'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertEqual(0, res_ok.returncode, res_ok.stdout + res_ok.stderr)
            self.assertNotEqual(head_before, self.git('rev-parse', 'HEAD'))

            # (c) Staged removal of scripts/drift-check.py -> refused by both pre-commit and commit-msg.
            head_v4 = self.git('rev-parse', 'HEAD')
            self.git('rm', '-q', '--cached', 'scripts/drift-check.py')
            res_rm = subprocess.run(['git', 'commit', '-qm', 'Attempt to drop gate script'], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(0, res_rm.returncode)
            self.assertIn('required gate script scripts/drift-check.py removed from index', res_rm.stderr)
            self.write('msg-rm.txt', 'Attempt\n')
            res_cm_rm = subprocess.run(['sh', str(REPO / 'tools/githooks/commit-msg'), str(self.root / 'msg-rm.txt')], cwd=self.root, capture_output=True, text=True, timeout=60)
            self.assertNotEqual(0, res_cm_rm.returncode)
            self.assertIn('required gate script scripts/drift-check.py removed from index', res_cm_rm.stderr)
            (self.root / 'msg-rm.txt').unlink()
            self.git('reset', '-q', 'HEAD', 'scripts/drift-check.py')
            self.assertEqual(head_v4, self.git('rev-parse', 'HEAD'))

        # 5. Verify exact-candidate CI workflow configuration in .github/workflows/plan.yml.
        plan_yml = (REPO / '.github/workflows/plan.yml').read_text(encoding='utf-8')
        self.assertGreaterEqual(plan_yml.count('ref: ${{ github.event.pull_request.head.sha || github.sha }}'), 2)
        self.assertIn('test "$ACTUAL_SHA" = "$HEAD_SHA"', plan_yml)
        self.assertIn('python3 scripts/drift-check.py --commit-range "$BASE_SHA..$HEAD_SHA"', plan_yml)
        self.assertIn('python3 scripts/drift-check.py --commit-range "$(git rev-parse "$HEAD_SHA^")..$HEAD_SHA"', plan_yml)
        self.assertIn('python3 scripts/drift-check.py --commit-range "$HEAD_SHA"', plan_yml)


if __name__ == '__main__':
    unittest.main()
