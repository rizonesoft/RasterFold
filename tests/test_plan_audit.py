"""Prevent a valid-looking plan from skipping prerequisites or product scope."""
import importlib.util
import argparse
import contextlib
import io
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('plan_audit', ROOT / 'scripts/plan-audit.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
tidy_spec = importlib.util.spec_from_file_location('tidy_gate', ROOT / 'scripts/tidy.py')
tidy_gate = importlib.util.module_from_spec(tidy_spec)
tidy_spec.loader.exec_module(tidy_gate)


class PlanAuditTests(unittest.TestCase):
    def setUp(self):
        self.edges = {'A': set(), 'B': {'A'}, 'C': {'B'}}
        self.order = {'A': (0, 1), 'B': (0, 2), 'C': (1, 3)}
        self.vision = {'release_gate': 'C', 'requirements': {'Preview': ['A']}}

    def errors(self):
        return audit.evaluate(self.edges, self.order, self.vision)

    def test_transitive_requirement_blocks_release(self):
        self.assertEqual([], self.errors())

    def test_later_row_dependency_fails(self):
        self.order['A'] = (0, 5)
        self.assertTrue(any('not earlier' in e for e in self.errors()))

    def test_later_phase_dependency_fails(self):
        self.order['A'] = (2, 1)
        self.assertTrue(any('not earlier' in e for e in self.errors()))

    def test_disconnected_requirement_is_not_coverage(self):
        self.edges['B'].clear()
        self.assertTrue(any('does not block' in e for e in self.errors()))

    def test_deleted_owner_and_empty_requirement_fail(self):
        self.vision['requirements'] = {'Preview': ['missing'], 'Save': []}
        self.assertEqual(2, len(self.errors()))

    def test_duplicate_and_missing_rows_fail(self):
        graph = audit.load_graph(ROOT)
        text = '### Phase 0 -- Example\n| [ ] | `D10 T01 §1` | Contract | 3 |\n' * 2
        _, errors = audit.positions(graph, text)
        self.assertTrue(any('duplicate' in e for e in errors))
        del self.order['A']
        self.assertTrue(any('missing phase row' in e for e in self.errors()))

    def test_cycle_is_finite_and_refused(self):
        self.edges['A'].add('C')
        self.assertTrue(self.errors())

    def test_file_level_dependency_expands_all_sections(self):
        graph = audit.load_graph(ROOT)
        one = graph.Todo(path='a', domain='10-converter', number='01', id='engine', sections={1: graph.Section(1), 2: graph.Section(2)})
        two = graph.Todo(path='b', domain='10-converter', number='02', id='ui', depends_on=['engine'], sections={1: graph.Section(1)})
        edges, errors = audit.dependencies(graph, [one, two])
        self.assertEqual([], errors)
        self.assertEqual({'D10 T01 §1', 'D10 T01 §2'}, edges['D10 T02 §1'])

    def test_adjacency_resolves_reason_bearing_cross_file_owner(self):
        graph = audit.load_graph(ROOT)
        adjacency = graph.adjacency_module()
        a = graph.Todo(path='a', domain='10-converter', number='01', id='a')
        b = graph.Todo(path='b', domain='10-converter', number='02', id='b', sections={5: graph.Section(5, has_body=True, has_row=True)})
        a.xrefs = ['D10 T02 §5 -- safe delivery, not a dependency']
        self.assertEqual((b, 5), adjacency.resolve(a.xrefs[0], a, [a, b]))
        catalog = {'a': {'lines': ['Output file destination'], 'bodies': {}}, 'b': {'bodies': {5: [(1, 'Deliver the output file to its destination.')]}}}
        self.assertEqual('owned', adjacency.stated_steps(a, catalog, [a, b])[0]['status'])
        self.assertIsNone(adjacency.resolve('D10 T02 §5 arbitrary trailing text', a, [a, b]))

    def test_native_context_never_inferred_from_tools_or_environment(self):
        graph = audit.load_graph(ROOT)
        native = {'windows-runtime', 'linux-runtime', 'linux-display', 'android-arm64-runtime', 'android-x64-runtime'}
        self.assertTrue(native <= set(graph.REQUIRES_ALLOWED))
        self.assertFalse(native & graph.detect_context('linux', {'DISPLAY': ':0', 'ANDROID_HOME': '/sdk', 'SESSIONNAME': 'Console'}))
        self.assertEqual({'display-session'}, graph.detect_context('win32', {'SESSIONNAME': 'Console'}))

    def test_real_product_plan_has_complete_release_coverage(self):
        result = audit.inspect(ROOT)
        self.assertEqual([], result['errors'])
        self.assertGreater(result['requirements'], 0)


class PlanProjectionTests(unittest.TestCase):
    def setUp(self):
        self.graph = audit.load_graph(ROOT)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.plan = self.root / 'plan.md'
        self.plan.write_bytes((ROOT / 'todo/implementation-plan.md').read_bytes())
        self.progress = self.root / 'build/todo-progress.json'
        self.operator = self.root / 'build/todo-operator.json'
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(patch.object(self.graph, 'PLAN', self.plan))
        stack.enter_context(patch.object(self.graph, 'PROGRESS_JSON', self.progress))
        stack.enter_context(patch.object(self.graph, 'OPERATOR_JSON', self.operator))

    def run_plan(self, check=True):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = self.graph.cmd_plan(argparse.Namespace(check=check))
        return result, output.getvalue()

    def test_fresh_checkout_checks_plan_without_creating_caches(self):
        before = self.plan.read_bytes()
        code, output = self.run_plan()
        self.assertEqual(0, code, output)
        self.assertEqual(before, self.plan.read_bytes())
        self.assertFalse(self.progress.parent.exists())

    def test_missing_caches_do_not_hide_stale_canonical_plan(self):
        text = self.plan.read_text(encoding='utf-8')
        self.plan.write_text(text.replace('\n| [ ] |', '\n| [x] |', 1), encoding='utf-8')
        before = self.plan.read_bytes()
        code, output = self.run_plan()
        self.assertEqual(1, code, output)
        self.assertIn('stale', output)
        self.assertEqual(before, self.plan.read_bytes())
        self.assertFalse(self.progress.parent.exists())

    def test_sync_materializes_caches_that_check_accepts(self):
        code, output = self.run_plan(check=False)
        self.assertEqual(0, code, output)
        self.assertTrue(self.progress.is_file())
        self.assertTrue(self.operator.is_file())
        code, output = self.run_plan()
        self.assertEqual(0, code, output)

    def test_present_stale_cache_fails_even_when_other_cache_is_absent(self):
        self.progress.parent.mkdir()
        for cache in (self.progress, self.operator):
            with self.subTest(cache=cache.name):
                cache.write_text('{}\n', encoding='utf-8')
                code, output = self.run_plan()
                self.assertEqual(1, code, output)
                self.assertIn(cache.name + ' is stale', output)
                self.assertEqual('{}\n', cache.read_text(encoding='utf-8'))
                cache.unlink()


class HookAndProvisionTests(unittest.TestCase):
    def test_precommit_hook_cancellation_exits_nonzero_and_cleans_up(self):
        if not shutil.which('sh'):
            self.skipTest('POSIX shell is required for Git hook execution')
        real_git = shutil.which('git')
        self.assertIsNotNone(real_git)
        for sig_name in ('TERM', 'INT'):
            with self.subTest(signal=sig_name), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / 'repo'
                root.mkdir()
                subprocess.run([real_git, 'init', '-q'], cwd=root, check=True)
                subprocess.run([real_git, 'config', 'user.email', 'fixture@example.invalid'], cwd=root, check=True)
                subprocess.run([real_git, 'config', 'user.name', 'Fixture'], cwd=root, check=True)
                gate = root / 'scripts/plan-audit.py'
                gate.parent.mkdir(parents=True, exist_ok=True)
                gate.write_text('raise SystemExit(1)\n', encoding='utf-8')
                subprocess.run([real_git, 'add', 'scripts/plan-audit.py'], cwd=root, check=True)

                shim_dir = Path(tmp) / 'bin'
                shim_dir.mkdir()
                git_shim = shim_dir / 'git'
                git_shim.write_text(
                    f'#!/bin/sh\n'
                    f'"{real_git}" "$@"\n'
                    f'rc=$?\n'
                    f'if [ "${{1:-}}" = "checkout-index" ]; then\n'
                    f'  kill -{sig_name} "$PPID"\n'
                    f'  sleep 1\n'
                    f'fi\n'
                    f'exit "$rc"\n',
                    encoding='utf-8',
                )
                git_shim.chmod(git_shim.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
                env = dict(os.environ)
                env['PATH'] = str(shim_dir) + os.pathsep + env.get('PATH', '')
                proc = subprocess.run(
                    ['sh', str(ROOT / 'tools/githooks/pre-commit')],
                    cwd=root,
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertNotEqual(0, proc.returncode, proc.stdout + proc.stderr)
                self.assertEqual([], list((root / '.git').glob('hookgate.*')))

    def test_provision_rejects_python_older_than_3_11(self):
        prov_spec = importlib.util.spec_from_file_location('provision', ROOT / 'tools/provision.py')
        prov = importlib.util.module_from_spec(prov_spec)
        prov_spec.loader.exec_module(prov)
        with self.assertRaisesRegex(ValueError, r'Python 3\.11\+ is required'):
            prov.check_python_version((3, 10, 14))
        prov.check_python_version((3, 11, 0))

        pwsh = shutil.which('pwsh')
        if not pwsh:
            self.skipTest('pwsh is required to test tools/provision.ps1')
        ps1_text = (ROOT / 'tools/provision.ps1').read_text(encoding='utf-8')
        start = ps1_text.index('function Get-Python {')
        end = ps1_text.index('function Test-FfmpegLeg {')
        functions_block = ps1_text[start:end]

        with tempfile.TemporaryDirectory() as tmp:
            bin_310 = Path(tmp) / 'py310'
            bin_310.mkdir()
            for name in ('python3', 'python', 'py'):
                shim = bin_310 / (name + ('.cmd' if os.name == 'nt' else ''))
                if os.name == 'nt':
                    shim.write_text(
                        '@echo off\r\n'
                        'if "%~1"=="--version" (echo Python 3.10.14 & exit /b 0)\r\n'
                        'exit /b 1\r\n',
                        encoding='utf-8',
                    )
                else:
                    shim.write_text(
                        '#!/bin/sh\n'
                        'if [ "${1:-}" = "--version" ]; then echo "Python 3.10.14"; exit 0; fi\n'
                        'exit 1\n',
                        encoding='utf-8',
                    )
                    shim.chmod(shim.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

            env_310 = dict(os.environ)
            env_310['PATH'] = str(bin_310)
            res_310 = subprocess.run(
                [
                    pwsh,
                    '-NoProfile',
                    '-Command',
                    functions_block + '\n$r = Test-PythonLegs; if ($r[0].Ok) { exit 0 } else { Write-Output $r[0].Detail; exit 1 }',
                ],
                env=env_310,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertNotEqual(0, res_310.returncode, res_310.stdout + res_310.stderr)
            self.assertIn('Python 3.11+ not on PATH', res_310.stdout)

            bin_312 = Path(tmp) / 'py312'
            bin_312.mkdir()
            shim_312 = bin_312 / ('python' + ('.cmd' if os.name == 'nt' else ''))
            if os.name == 'nt':
                shim_312.write_text(
                    '@echo off\r\n'
                    'if "%~1"=="--version" (echo Python 3.12.4 & exit /b 0)\r\n'
                    'echo 10.4.0\r\n'
                    'exit /b 0\r\n',
                    encoding='utf-8',
                )
            else:
                shim_312.write_text(
                    '#!/bin/sh\n'
                    'if [ "${1:-}" = "--version" ]; then echo "Python 3.12.4"; exit 0; fi\n'
                    'echo "10.4.0"\n'
                    'exit 0\n',
                    encoding='utf-8',
                )
                shim_312.chmod(shim_312.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

            env_mixed = dict(os.environ)
            env_mixed['PATH'] = str(bin_310) + os.pathsep + str(bin_312)
            res_mixed = subprocess.run(
                [
                    pwsh,
                    '-NoProfile',
                    '-Command',
                    functions_block + '\n$r = Test-PythonLegs; if ($r[0].Ok -and $r[1].Ok) { Write-Output $r[0].Detail; exit 0 } else { exit 1 }',
                ],
                env=env_mixed,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(0, res_mixed.returncode, res_mixed.stdout + res_mixed.stderr)
            self.assertIn('Python 3.12.4', res_mixed.stdout)


class TidyGateTests(unittest.TestCase):
    def test_ratchet_sequence_18_to_15_to_16_and_upward_edit_refusal(self):
        with tempfile.TemporaryDirectory() as tmp:
            baseline = Path(tmp) / '.tidy-baseline'
            baseline.write_text('count: 18\n', encoding='utf-8')
            self.assertEqual('18/18 unique warning(s)', tidy_gate.evaluate_ratchet(18, baseline, ratchet=False, head_baseline=18))
            with self.assertRaisesRegex(ValueError, r'below baseline 18.*--ratchet'):
                tidy_gate.evaluate_ratchet(15, baseline, ratchet=False, head_baseline=18)
            self.assertEqual(
                'ratcheted baseline from 18 to 15',
                tidy_gate.evaluate_ratchet(15, baseline, ratchet=True, head_baseline=18),
            )
            self.assertEqual(15, tidy_gate.parse_baseline(baseline))
            with self.assertRaisesRegex(ValueError, r'exceeding baseline 15'):
                tidy_gate.evaluate_ratchet(16, baseline, ratchet=False, head_baseline=18)
            with self.assertRaisesRegex(ValueError, r'exceeding baseline 15'):
                tidy_gate.evaluate_ratchet(16, baseline, ratchet=True, head_baseline=18)
            self.assertEqual(15, tidy_gate.parse_baseline(baseline))
            baseline.write_text('count: 19\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, r'exceeds committed baseline 18'):
                tidy_gate.evaluate_ratchet(19, baseline, ratchet=False, head_baseline=18)

    def test_committed_baseline_increase_is_refused(self):
        git = shutil.which('git')
        self.assertIsNotNone(git)
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / 'repo'
            (repo / 'todo').mkdir(parents=True)
            subprocess.run([git, 'init', '-q'], cwd=repo, check=True)
            subprocess.run([git, 'config', 'user.email', 'fixture@example.invalid'], cwd=repo, check=True)
            subprocess.run([git, 'config', 'user.name', 'Fixture'], cwd=repo, check=True)
            base_file = repo / 'todo/.tidy-baseline'
            base_file.write_text('count: 20\n', encoding='utf-8')
            subprocess.run([git, 'add', 'todo/.tidy-baseline'], cwd=repo, check=True)
            subprocess.run([git, 'commit', '-qm', 'initial baseline 20'], cwd=repo, check=True)
            self.assertEqual(20, tidy_gate.committed_baseline(repo))

            base_file.write_text('count: 15\n', encoding='utf-8')
            subprocess.run([git, 'add', 'todo/.tidy-baseline'], cwd=repo, check=True)
            subprocess.run([git, 'commit', '-qm', 'shrink baseline to 15'], cwd=repo, check=True)
            self.assertEqual(15, tidy_gate.committed_baseline(repo))

            base_file.write_text('count: 16\n', encoding='utf-8')
            subprocess.run([git, 'add', 'todo/.tidy-baseline'], cwd=repo, check=True)
            subprocess.run([git, 'commit', '-qm', 'illegal baseline increase to 16'], cwd=repo, check=True)
            min_base = tidy_gate.committed_baseline(repo)
            self.assertEqual(15, min_base)
            with self.assertRaisesRegex(ValueError, r'baseline 16 .* exceeds committed baseline 15'):
                tidy_gate.evaluate_ratchet(16, base_file, ratchet=False, head_baseline=min_base)

            shallow_repo = Path(tmp) / 'shallow'
            subprocess.run([git, 'clone', '-q', '--depth', '1', f'file://{repo}', str(shallow_repo)], check=True)
            with self.assertRaisesRegex(ValueError, r'shallow git checkout cannot verify'):
                tidy_gate.committed_baseline(shallow_repo)

            base_file.write_text('count: 15\n', encoding='utf-8')
            subprocess.run([git, 'add', 'todo/.tidy-baseline'], cwd=repo, check=True)
            subprocess.run([git, 'commit', '-qm', 'forward repair restoring baseline to 15'], cwd=repo, check=True)
            min_after_repair = tidy_gate.committed_baseline(repo)
            self.assertEqual(15, min_after_repair)
            self.assertEqual(
                '15/15 unique warning(s)',
                tidy_gate.evaluate_ratchet(15, base_file, ratchet=False, head_baseline=min_after_repair),
            )

    def test_merge_history_and_git_read_failures_fail_closed(self):
        git = shutil.which('git')
        self.assertIsNotNone(git)
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp) / 'repo'
            (repo / 'todo').mkdir(parents=True)
            subprocess.run([git, 'init', '-q', '-b', 'main'], cwd=repo, check=True)
            subprocess.run([git, 'config', 'user.email', 'fixture@example.invalid'], cwd=repo, check=True)
            subprocess.run([git, 'config', 'user.name', 'Fixture'], cwd=repo, check=True)
            base_file = repo / 'todo/.tidy-baseline'
            base_file.write_text('count: 20\n', encoding='utf-8')
            subprocess.run([git, 'add', 'todo/.tidy-baseline'], cwd=repo, check=True)
            subprocess.run([git, 'commit', '-qm', 'initial baseline 20'], cwd=repo, check=True)

            subprocess.run([git, 'checkout', '-qb', 'shrink'], cwd=repo, check=True)
            base_file.write_text('count: 15\n', encoding='utf-8')
            subprocess.run([git, 'add', 'todo/.tidy-baseline'], cwd=repo, check=True)
            subprocess.run([git, 'commit', '-qm', 'shrink to 15 on side branch'], cwd=repo, check=True)

            subprocess.run([git, 'checkout', '-q', 'main'], cwd=repo, check=True)
            subprocess.run([git, 'merge', '--no-ff', '-s', 'ours', '-qm', 'merge keeping 20', 'shrink'], cwd=repo, check=True)
            self.assertEqual('count: 20\n', base_file.read_text(encoding='utf-8'))
            min_merged = tidy_gate.committed_baseline(repo)
            self.assertEqual(15, min_merged)
            with self.assertRaisesRegex(ValueError, r'baseline 20 .* exceeds committed baseline 15'):
                tidy_gate.evaluate_ratchet(20, base_file, ratchet=False, head_baseline=min_merged)

            with patch.object(
                tidy_gate.subprocess,
                'run',
                side_effect=[
                    subprocess.CompletedProcess(['git'], 0, stdout='false\n', stderr=''),
                    subprocess.CompletedProcess(['git'], 128, stdout='', stderr='fatal: bad revision HEAD'),
                ],
            ):
                with self.assertRaisesRegex(ValueError, r'cannot read git history'):
                    tidy_gate.committed_baseline(repo)

            with patch.object(
                tidy_gate.subprocess,
                'run',
                side_effect=[
                    subprocess.CompletedProcess(['git'], 0, stdout='false\n', stderr=''),
                    subprocess.CompletedProcess(['git'], 0, stdout='deadbeef\n', stderr=''),
                    subprocess.CompletedProcess(['git'], 128, stdout='', stderr='fatal: bad object deadbeef:todo/.tidy-baseline'),
                ],
            ):
                with self.assertRaisesRegex(ValueError, r'cannot read historical baseline'):
                    tidy_gate.committed_baseline(repo)

    def test_cross_target_deduplication_and_failure_paths(self):
        out_win = (
            str(ROOT / 'src/cm.c')
            + ":651:49: warning: performing an implicit widening conversion to type 'size_t' (aka 'unsigned long long') [bugprone-implicit-widening-of-multiplication-result]\n"
        )
        out_lin = (
            str(ROOT / 'src/cm.c')
            + ":651:49: warning: performing an implicit widening conversion to type 'size_t' (aka 'unsigned long') [bugprone-implicit-widening-of-multiplication-result]\n"
        )
        f1, _ = tidy_gate.parse_tidy_output(out_win, 0, ROOT, 'windows-x64')
        f2, _ = tidy_gate.parse_tidy_output(out_lin, 0, ROOT, 'linux-x64')
        self.assertEqual(1, len(f1 | f2))
        self.assertEqual({('src/cm.c', 651, 49, 'bugprone-implicit-widening-of-multiplication-result')}, f1 | f2)

        with self.assertRaisesRegex(ValueError, r'exited with code 2'):
            tidy_gate.parse_tidy_output('warning-free output without error marker\n', 2, ROOT, 'linux-x64')
        with self.assertRaisesRegex(ValueError, r'error diagnostic'):
            tidy_gate.parse_tidy_output('src/mem.c:10:5: error: undeclared identifier [clang-diagnostic-error]\n', 0, ROOT, 'linux-x64')

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            with self.assertRaisesRegex(ValueError, r'missing baseline'):
                tidy_gate.parse_baseline(tmp_path / 'missing')
            bad_base = tmp_path / 'bad-baseline'
            for bad_text in ('', 'count: -1\n', 'count: abc\n', 'warnings: 10\n'):
                bad_base.write_text(bad_text, encoding='utf-8')
                with self.assertRaisesRegex(ValueError, r'malformed baseline'):
                    tidy_gate.parse_baseline(bad_base)

            with self.assertRaisesRegex(ValueError, r'missing compilation database'):
                tidy_gate.validate_compdb(tmp_path / 'missing.json', ROOT)
            compdb = tmp_path / 'compile_commands.json'
            compdb.write_text('[]\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, r'empty compilation database'):
                tidy_gate.validate_compdb(compdb, ROOT)
            compdb.write_text('[{"file": "/nonexistent/src/foo.c", "arguments": ["clang"]}]\n', encoding='utf-8')
            with self.assertRaisesRegex(ValueError, r'references missing file'):
                tidy_gate.validate_compdb(compdb, ROOT)


if __name__ == '__main__':
    unittest.main()
