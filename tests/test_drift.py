"""Regression proofs for documentation gates, index isolation and narrow repairs."""
import contextlib
import shutil
import importlib.util
import io
import json
from pathlib import Path
import subprocess
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
        self.write('.gitattributes', '* text=auto\n*.json text eol=lf\n*.md text eol=lf\n*.py text eol=lf\n*.h text eol=lf\n')
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
        self.manifest['reviews'] = {'README.md': {'evidence': 'fixture source review', 'sources': {'src/version.h': drift.digest((self.root / 'src/version.h').read_bytes()), 'README.md': drift.digest((self.root / 'README.md').read_bytes())}}}
        self.save()
        self.write('src/version.h', '#define VERSION 3u\n')
        c = self.check(); c.fix()
        self.assertIn('stale', self.kinds(self.check()))
        self.assertEqual('fixture source review', json.loads((self.root / 'docs/drift.json').read_text(encoding='utf-8'))['reviews']['README.md']['evidence'])

    def test_authored_document_change_invalidates_its_review_receipt(self):
        document = self.root / 'README.md'
        self.manifest['reviews'] = {'README.md': {'evidence': 'author-attested source review', 'sources': {'README.md': drift.digest(document.read_bytes())}}}
        self.save()
        self.write('README.md', document.read_text(encoding='utf-8') + 'New unreviewed claim.\n')
        c = self.check(); c.fix()
        self.assertIn('stale', self.kinds(self.check()))

    def test_explicit_review_requires_evidence(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, drift.main(['--root', str(self.root), '--review', 'README.md']))

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
        self.manifest['reviews'] = {'README.md': {'evidence': 'source audit', 'sources': sources}}; self.save()
        self.assertIn('eol', self.kinds(self.check()))
        attrs = self.root / '.gitattributes'; self.write('.gitattributes', attrs.read_text(encoding='utf-8') + '*.def text eol=lf\n')
        self.assertEqual([], self.check().errors)
        del sources['README.md']; self.save()
        self.assertIn('review', self.kinds(self.check()))

    def test_old_crlf_checkout_cannot_poison_a_renewed_receipt(self):
        sources = {path: drift.digest((self.root / path).read_bytes()) for path in ('README.md', 'src/version.h')}
        self.manifest['reviews'] = {'README.md': {'evidence': 'source audit', 'sources': sources}}; self.save()
        self.assertEqual([], self.check().errors)
        header = self.root / 'src/version.h'; header.write_bytes(header.read_bytes().replace(b'\n', b'\r\n'))
        self.assertTrue(any(e['kind'] == 'eol' and 'working bytes contain CRLF' in e['detail'] for e in self.check().errors))
        before = (self.root / 'docs/drift.json').read_bytes()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, drift.main(['--root', str(self.root), '--review', 'README.md', '--evidence', 'Attempted renewal']))
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
        self.manifest['reviews'] = {name: {'evidence': 'old evidence', 'sources': {'src/version.h': '0' * 64, name: drift.digest((self.root / name).read_bytes())}} for name in ('README.md', 'other.md')}
        self.save()
        before = self.manifest['reviews']['other.md']
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(1, drift.main(['--root', str(self.root), '--review', 'README.md', '--evidence', 'Source read with named test evidence']))
        after = json.loads((self.root / 'docs/drift.json').read_text(encoding='utf-8'))['reviews']
        self.assertEqual(before, after['other.md'])
        self.assertEqual('Source read with named test evidence', after['README.md']['evidence'])
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
        expected_keys = {
            'product_name', 'format_name', 'format_version', 'header_bytes',
            'max_dimension', 'max_pixels', 'max_file_bytes', 'api_major', 'api_minor',
            'exit_ok', 'exit_ok_meaning', 'exit_usage', 'exit_usage_meaning',
            'exit_io', 'exit_io_meaning', 'exit_unsupported', 'exit_unsupported_meaning',
            'exit_malformed', 'exit_malformed_meaning', 'exit_limit', 'exit_limit_meaning',
            'cmd_encode', 'cmd_decode', 'cmd_inspect', 'input_image_ext', 'encoded_ext',
            'measurement_reference_bytes',
        }
        self.assertEqual(expected_keys, set(live_facts))
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
        self.assertEqual(['docs/format.md', 'docs/research-summary.md', 'docs/usage.md'], written)
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
        except OSError:
            pass

        # Repeated staging removes obsolete files from build/site-source and stages only the allowlist.
        self.write('build/site-source/obsolete.md', 'stale file\n')
        staged = build_docs.stage_site(self.root, 'build/site-source')
        self.assertFalse((self.root / 'build/site-source/obsolete.md').exists())
        self.assertEqual(
            [
                '_config.yml',
                '_includes/components/footer.html',
                'assets/brand/cover.svg',
                'assets/brand/icon.svg',
                'benchmarks.md',
                'format.md',
                'getting-started.md',
                'index.md',
                'research-summary.md',
                'usage.md',
            ],
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


if __name__ == '__main__':
    unittest.main()
