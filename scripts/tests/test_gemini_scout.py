"""Unit tests for .agents/plugins/rf-scouts/readonly_mcp.py and scripts/gemini-scout.py."""
from __future__ import annotations

import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
MCP_PATH = REPO / ".agents" / "plugins" / "rf-scouts" / "readonly_mcp.py"
SCOUT_PATH = SCRIPTS / "gemini-scout.py"

_mcp_spec = importlib.util.spec_from_file_location("rf_readonly_mcp", MCP_PATH)
mcp = importlib.util.module_from_spec(_mcp_spec)
_mcp_spec.loader.exec_module(mcp)

_scout_spec = importlib.util.spec_from_file_location("rf_gemini_scout", SCOUT_PATH)
gs = importlib.util.module_from_spec(_scout_spec)
_scout_spec.loader.exec_module(gs)


class ReadonlyMcpTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        (self.root / "src").mkdir()
        (self.root / "src" / "rf.h").write_text("#define RF_VERSION 3u\n#define RF_METHOD_PAL3 4u\n", encoding="utf-8")
        (self.root / ".git").mkdir()
        (self.root / ".git" / "config").write_text("secret_git_config\n", encoding="utf-8")
        (self.root / "build" / "logs").mkdir(parents=True)
        (self.root / "build" / "logs" / "gate.log").write_text("PASS one\nFAIL two\n", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_initialize_and_tools_list_mark_tools_always_loaded(self):
        init_resp = mcp.handle_request({"jsonrpc": "2.0", "id": 1, "method": "initialize"}, root=self.root)
        self.assertEqual(init_resp["result"]["serverInfo"]["name"], "rf-readonly")
        list_resp = mcp.handle_request({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}, root=self.root)
        tools = list_resp["result"]["tools"]
        self.assertEqual([t["name"] for t in tools], ["grep_repo", "glob_repo"])
        for tool in tools:
            self.assertEqual(tool.get("_meta"), {"antigravity/alwaysLoad": True})

    def test_grep_repo_searches_source_and_skips_git_and_default_build(self):
        resp = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "tools/call",
                "params": {"name": "grep_repo", "arguments": {"pattern": "RF_METHOD_PAL3"}},
            },
            root=self.root,
        )
        self.assertFalse(resp["result"]["isError"])
        self.assertEqual(resp["result"]["content"][0]["text"], "src/rf.h:2:#define RF_METHOD_PAL3 4u")

        # Default whole-repo grep skips build/ and .git/
        resp_build = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "tools/call",
                "params": {"name": "grep_repo", "arguments": {"pattern": "FAIL two|secret_git_config"}},
            },
            root=self.root,
        )
        self.assertIn("NOT FOUND", resp_build["result"]["content"][0]["text"])

        # Explicit path under build/ is allowed so scouts can inspect build logs
        resp_log = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "tools/call",
                "params": {"name": "grep_repo", "arguments": {"pattern": "FAIL", "path": "build/logs"}},
            },
            root=self.root,
        )
        self.assertFalse(resp_log["result"]["isError"])
        self.assertEqual(resp_log["result"]["content"][0]["text"], "build/logs/gate.log:2:FAIL two")

    def test_glob_repo_and_path_confinement_refusals(self):
        resp = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "tools/call",
                "params": {"name": "glob_repo", "arguments": {"pattern": "*.h"}},
            },
            root=self.root,
        )
        self.assertFalse(resp["result"]["isError"])
        self.assertEqual(resp["result"]["content"][0]["text"], "src/rf.h")

        for bad_path in ("../outside", "/etc/passwd", ".git", ".git/config"):
            err_resp = mcp.handle_request(
                {
                    "jsonrpc": "2.0",
                    "id": 7,
                    "method": "tools/call",
                    "params": {"name": "grep_repo", "arguments": {"pattern": "x", "path": bad_path}},
                },
                root=self.root,
            )
            self.assertTrue(err_resp["result"]["isError"], bad_path)

        for bad_glob in ("../**", "/etc/*", ".git/*"):
            err_glob = mcp.handle_request(
                {
                    "jsonrpc": "2.0",
                    "id": 8,
                    "method": "tools/call",
                    "params": {"name": "glob_repo", "arguments": {"pattern": bad_glob}},
                },
                root=self.root,
            )
            self.assertTrue(err_glob["result"]["isError"], bad_glob)


class GeminiScoutWrapperTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        shutil.copytree(REPO / ".agents" / "plugins" / "rf-scouts", self.root / ".agents" / "plugins" / "rf-scouts")
        (self.root / "src").mkdir()
        rf_h_lines = [f"/* line {i} */" for i in range(1, 21)]
        rf_h_lines[18] = "#define RF_VERSION 3u"
        (self.root / "src" / "rf.h").write_text("\n".join(rf_h_lines) + "\n", encoding="utf-8")
        self.stub_bin = self.root / "stub_bin"
        self.stub_bin.mkdir()
        self.called_marker = self.root / "agy_called"

    def tearDown(self):
        self.tmp.cleanup()

    def write_stub_agy(self, body: str) -> None:
        agy = self.stub_bin / "agy"
        agy.write_text("#!/usr/bin/env python3\n" + body, encoding="utf-8")
        agy.chmod(0o755)

    def run_scout(self, *argv: str, timeout: int = 30) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        old_path = os.environ.get("PATH", "")
        os.environ["PATH"] = f"{self.stub_bin}:{old_path}"
        try:
            with mock.patch("sys.stdout", out), mock.patch("sys.stderr", err):
                code = gs.main(list(argv), root=self.root)
        finally:
            os.environ["PATH"] = old_path
        return code, out.getvalue(), err.getvalue()

    def test_happy_path_runs_on_hydrogen_medium_and_prints_response(self):
        self.write_stub_agy(
            f"import json, pathlib, sys\n"
            f"pathlib.Path({str(self.called_marker)!r}).write_text(' '.join(sys.argv[1:]), encoding='utf-8')\n"
            "print(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n"
            "print(json.dumps({'event': 'step_update', 'step_update': {'step_type': 'tool', 'tool_name': 'mcp_rf-scouts_rf-readonly_grep_repo', 'tool_info': {'parameters': {'pattern': 'RF_VERSION'}}}}))\n"
            "print(json.dumps({'event': 'result', 'result': {'status': 'SUCCESS', 'response': 'src/rf.h:19:#define RF_VERSION 3u\\nCOUNT: 1\\n'}}))\n"
        )
        code, out, err = self.run_scout("--agent", "rf-scout", "--prompt", "Count RF_VERSION")
        self.assertEqual(code, 0, err)
        self.assertEqual(out, "src/rf.h:19:#define RF_VERSION 3u\nCOUNT: 1\n")
        self.assertIn("--model gemini-4-hydrogen-medium --agent rf-scout --output-format stream-json", self.called_marker.read_text(encoding="utf-8"))

    def test_invalid_roster_refuses_before_spawning_agy(self):
        # Plan review P1: attended invocation must validate the scout roster before starting agy.
        self.write_stub_agy(f"import pathlib\npathlib.Path({str(self.called_marker)!r}).write_text('called')\n")
        scout_md = self.root / ".agents" / "plugins" / "rf-scouts" / "agents" / "rf-scout.md"
        orig = scout_md.read_text(encoding="utf-8")
        for bad in (
            orig.replace("  - view_file", "  - view_file\n  - write_to_file"),
            orig.replace("tools:\n  - view_file\n", ""),
            orig.replace("inheritMcp: false", "inheritMcp: true"),
        ):
            scout_md.write_text(bad, encoding="utf-8")
            code, _, err = self.run_scout("--agent", "rf-scout", "--prompt", "Check")
            self.assertEqual(code, 2)
            self.assertIn("scout roster invalid", err)
            self.assertFalse(self.called_marker.exists(), "agy must not start when the roster is invalid")

    def test_incomplete_or_contradictory_streams_fail(self):
        # Plan review P2: missing init, wrong model/agent, disallowed tool, missing result, error event, or nonzero exit despite SUCCESS.
        cases = [
            ("missing init", "import json\nprint(json.dumps({'event': 'result', 'result': {'status': 'SUCCESS', 'response': 'ok'}}))\n", "stream missing init event"),
            ("wrong model", "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-3.8-flash-tiered', 'agent': 'rf-scout'}}))\nprint(json.dumps({'event': 'result', 'result': {'status': 'SUCCESS', 'response': 'ok'}}))\n", "init model is 'gemini-3.8-flash-tiered'"),
            ("wrong agent", "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'other'}}))\nprint(json.dumps({'event': 'result', 'result': {'status': 'SUCCESS', 'response': 'ok'}}))\n", "init agent is 'other'"),
            ("disallowed tool", "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\nprint(json.dumps({'event': 'step_update', 'step_update': {'step_type': 'tool', 'tool_name': 'write_file'}}))\nprint(json.dumps({'event': 'result', 'result': {'status': 'SUCCESS', 'response': 'ok'}}))\n", "disallowed tool call 'write_file'"),
            ("truncated stream", "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n", "stream had 0 result event(s)"),
            ("malformed json", "print('{bad json')\n", "malformed JSON on stream line 1"),
            ("nonzero exit with SUCCESS", "import json, sys\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\nprint(json.dumps({'event': 'result', 'result': {'status': 'SUCCESS', 'response': 'ok'}}))\nsys.exit(1)\n", "process exited 1 despite SUCCESS result event"),
        ]
        for label, script_body, expected_err in cases:
            self.write_stub_agy(script_body)
            code, _, err = self.run_scout("--agent", "rf-scout", "--prompt", "Test")
            self.assertEqual(code, 2, label)
            self.assertIn(expected_err, err, label)

    def test_unquoted_or_hallucinated_response_evidence_fails(self):
        def make_stream(agent: str, response: str) -> str:
            return (
                "import json\n"
                f"print(json.dumps({{'event': 'init', 'init': {{'model': 'gemini-4-hydrogen-medium', 'agent': {agent!r}}}}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {response!r}}}}}))\n"
            )

        bad_cases = [
            ("unquoted prose opinion", "rf-scout", "Yes, the section looks ready to ship.", "unquoted response"),
            ("hallucinated file path", "rf-scout", "src/does_not_exist.c:10:int x;\nCOUNT: 1\n", "hallucinated file path in citation: src/does_not_exist.c:10"),
            ("hallucinated line number", "rf-scout", "src/rf.h:999:#define RF_VERSION 3u\nCOUNT: 1\n", "hallucinated line number in citation: src/rf.h:999"),
            ("miscounted COUNT", "rf-scout", "src/rf.h:19:#define RF_VERSION 3u\nCOUNT: 5\n", "COUNT: 5 does not match 1 cited line(s)"),
            ("missing log-digest blocks", "rf-log-digest", "src/rf.h:19:FAIL\n", "rf-log-digest response missing FAILURES block"),
            ("missing fact-check blocks", "rf-fact-check", "src/rf.h:19:ok\n", "rf-fact-check response missing MISMATCHES block"),
            ("multiline ESCALATE-LEAD", "rf-scout", "ESCALATE-LEAD: needs write\nextra commentary\n", "ESCALATE-LEAD response must be a single line"),
        ]
        for label, agent, resp, expected_err in bad_cases:
            self.write_stub_agy(make_stream(agent, resp))
            code, _, err = self.run_scout("--agent", agent, "--prompt", "Test")
            self.assertEqual(code, 2, label)
            self.assertIn(expected_err, err, label)

        # Single-line ESCALATE-LEAD refusal is valid
        self.write_stub_agy(make_stream("rf-scout", "ESCALATE-LEAD: writing a file is not read-only"))
        code, out, err = self.run_scout("--agent", "rf-scout", "--prompt", "Create a file")
        self.assertEqual(code, 0, err)
        self.assertEqual(out.strip(), "ESCALATE-LEAD: writing a file is not read-only")

    def test_timeout_and_signal_kill_entire_descendant_tree_including_escaped_session(self):
        # Plan review P1: even a grandchild (like an MCP server) that starts its own session is killed and reaped.
        pid_file = self.root / "escaped.pid"
        self.write_stub_agy(
            f"import subprocess, sys, time\n"
            f"child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)\n"
            f"open({str(pid_file)!r}, 'w').write(str(child.pid))\n"
            f"sys.stdout.flush()\n"
            f"time.sleep(60)\n"
        )
        code, _, err = self.run_scout("--agent", "rf-scout", "--prompt", "Timeout probe", "--timeout", "1")
        self.assertEqual(code, 2)
        self.assertIn("timed out after 1s", err)
        escaped_pid = int(pid_file.read_text(encoding="utf-8"))
        self.assertFalse(gs.pid_alive(escaped_pid), "escaped grandchild survived timeout cleanup")

        # Also test signal cancellation while running
        pid_file.unlink()
        env = dict(os.environ, PATH=f"{self.stub_bin}:{os.environ.get('PATH', '')}")
        proc = subprocess.Popen(
            [sys.executable, str(SCOUT_PATH), "--agent", "rf-scout", "--prompt", "Signal probe", "--timeout", "60"],
            cwd=REPO,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            for _ in range(100):
                if pid_file.exists() and pid_file.read_text(encoding="utf-8").strip():
                    break
                time.sleep(0.05)
            escaped_pid = int(pid_file.read_text(encoding="utf-8"))
            proc.send_signal(signal.SIGTERM)
            exit_code = proc.wait(timeout=15)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            for pipe in (proc.stdout, proc.stderr):
                if pipe is not None:
                    pipe.close()
        self.assertEqual(exit_code, 130)
        self.assertFalse(gs.pid_alive(escaped_pid), "escaped grandchild survived SIGTERM cancellation")


if __name__ == "__main__":
    unittest.main()
