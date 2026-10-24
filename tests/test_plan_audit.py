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


if __name__ == '__main__':
    unittest.main()
