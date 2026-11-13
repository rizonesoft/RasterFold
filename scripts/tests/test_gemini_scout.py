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
        (self.root / "src" / "rf.h").write_text(
            "#define RF_VERSION 3u\n#define RF_METHOD_PAL3 4u\n#define RF_METHOD_CM 1u\n",
            encoding="utf-8",
        )
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
            self.assertIn("offset", tool["inputSchema"]["properties"])

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

    def test_malformed_params_and_arguments_return_errors_without_crashing(self):
        # Round 1 P2: non-object params or arguments must not raise AttributeError.
        for bad_params in (["malformed"], "bad", 42, True):
            resp = mcp.handle_request({"jsonrpc": "2.0", "id": 9, "method": "tools/call", "params": bad_params}, root=self.root)
            self.assertEqual(resp["error"]["code"], -32602)

        for bad_args in (["bad"], "bad", 123):
            resp = mcp.handle_request(
                {"jsonrpc": "2.0", "id": 10, "method": "tools/call", "params": {"name": "grep_repo", "arguments": bad_args}},
                root=self.root,
            )
            self.assertTrue(resp["result"]["isError"])

        for bad_page in ({"max_results": "many"}, {"max_results": True}, {"max_results": 0}, {"offset": -1}, {"offset": True}):
            args = {"pattern": "RF_", **bad_page}
            resp = mcp.handle_request(
                {"jsonrpc": "2.0", "id": 11, "method": "tools/call", "params": {"name": "grep_repo", "arguments": args}},
                root=self.root,
            )
            self.assertTrue(resp["result"]["isError"], bad_page)

    def test_truncation_marker_and_offset_pagination_in_grep_and_glob(self):
        # Round 1 P2: searches hitting max_results must emit TRUNCATED and support offset pagination.
        (self.root / "src" / "a.c").write_text("int a;\n", encoding="utf-8")
        (self.root / "src" / "b.c").write_text("int b;\n", encoding="utf-8")

        page1 = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 12,
                "method": "tools/call",
                "params": {"name": "grep_repo", "arguments": {"pattern": "^#define", "path": "src/rf.h", "max_results": 2}},
            },
            root=self.root,
        )["result"]["content"][0]["text"]
        self.assertIn("src/rf.h:1:#define RF_VERSION 3u", page1)
        self.assertIn("src/rf.h:2:#define RF_METHOD_PAL3 4u", page1)
        self.assertIn("TRUNCATED: showing 2 match(es) at offset=0 (more matches exist; call grep_repo with offset=2", page1)

        page2 = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 13,
                "method": "tools/call",
                "params": {"name": "grep_repo", "arguments": {"pattern": "^#define", "path": "src/rf.h", "max_results": 2, "offset": 2}},
            },
            root=self.root,
        )["result"]["content"][0]["text"]
        self.assertEqual(page2, "src/rf.h:3:#define RF_METHOD_CM 1u")

        glob1 = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 14,
                "method": "tools/call",
                "params": {"name": "glob_repo", "arguments": {"pattern": "*", "path": "src", "max_results": 2}},
            },
            root=self.root,
        )["result"]["content"][0]["text"]
        self.assertIn("TRUNCATED: showing 2 match(es) at offset=0 (more matches exist; call glob_repo with offset=2", glob1)

        glob2 = mcp.handle_request(
            {
                "jsonrpc": "2.0",
                "id": 15,
                "method": "tools/call",
                "params": {"name": "glob_repo", "arguments": {"pattern": "*", "path": "src", "max_results": 2, "offset": 2}},
            },
            root=self.root,
        )["result"]["content"][0]["text"]
        self.assertEqual(glob2, "src/rf.h")

    def test_grep_repo_checks_bounded_binary_prefix_without_reading_entire_file(self):
        # Round 1 P2: binary files must be rejected after reading at most BINARY_CHECK_BYTES.
        big_bin = self.root / "src" / "blob.bin"
        big_bin.write_bytes(b"\x00" + b"A" * (mcp.BINARY_CHECK_BYTES * 8))
        read_sizes: list[int] = []
        real_open = Path.open

        def tracking_open(path_self, *args, **kwargs):
            fh = real_open(path_self, *args, **kwargs)
            if path_self == big_bin:
                orig_read = fh.read

                def wrapped_read(size=-1):
                    read_sizes.append(size)
                    return orig_read(size)

                fh.read = wrapped_read
            return fh

        with mock.patch.object(Path, "open", tracking_open):
            text, is_err = mcp.run_grep_repo({"pattern": "A", "path": "src/blob.bin"}, root=self.root)
        self.assertFalse(is_err)
        self.assertIn("NOT FOUND", text)
        self.assertEqual(read_sizes, [mcp.BINARY_CHECK_BYTES])

    def test_root_level_glob_and_grep_include_tracked_tools_and_skip_downloaded_toolchains(self):
        # D05 T01 §9 Round 1 P2: default root-level searches must include tracked files under tools/
        # (such as tools/provision.py and tools/githooks/*) while still skipping ignored toolchain dirs.
        (self.root / "tools" / "githooks").mkdir(parents=True)
        (self.root / "tools" / "provision.py").write_text("def check_python_version() -> bool:\n    return True\n", encoding="utf-8")
        (self.root / "tools" / "githooks" / "pre-commit").write_text("#!/bin/sh\n# pre_commit_sentinel\n", encoding="utf-8")
        (self.root / "tools" / "llvm-mingw" / "bin").mkdir(parents=True)
        (self.root / "tools" / "llvm-mingw" / "bin" / "ignored.txt").write_text("check_python_version in toolchain\n", encoding="utf-8")
        (self.root / "src" / "__pycache__").mkdir(parents=True)
        (self.root / "src" / "__pycache__" / "stale.txt").write_text("check_python_version in pycache\n", encoding="utf-8")

        glob_text, glob_err = mcp.run_glob_repo({"pattern": "tools/provision.py"}, root=self.root)
        self.assertFalse(glob_err)
        self.assertEqual(glob_text, "tools/provision.py")

        hook_glob, hook_err = mcp.run_glob_repo({"pattern": "tools/**"}, root=self.root)
        self.assertFalse(hook_err)
        self.assertEqual(hook_glob.splitlines(), ["tools/githooks/pre-commit", "tools/provision.py"])

        grep_text, grep_err = mcp.run_grep_repo({"pattern": "check_python_version|pre_commit_sentinel"}, root=self.root)
        self.assertFalse(grep_err)
        self.assertEqual(
            grep_text.splitlines(),
            [
                "tools/githooks/pre-commit:2:# pre_commit_sentinel",
                "tools/provision.py:1:def check_python_version() -> bool:",
            ],
        )

        # Also verify against the live repository root directly.
        repo_glob, repo_glob_err = mcp.run_glob_repo({"pattern": "tools/provision.py"}, root=REPO)
        self.assertFalse(repo_glob_err)
        self.assertEqual(repo_glob, "tools/provision.py")
        repo_grep, repo_grep_err = mcp.run_grep_repo({"pattern": r"^def check_python_version\b"}, root=REPO)
        self.assertFalse(repo_grep_err)
        self.assertIn("tools/provision.py:13:def check_python_version(", repo_grep)


class GeminiScoutWrapperTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        shutil.copytree(REPO / ".agents" / "plugins" / "rf-scouts", self.root / ".agents" / "plugins" / "rf-scouts")
        (self.root / "src").mkdir()
        rf_h_lines = [f"/* line {i} */" for i in range(1, 21)]
        rf_h_lines[18] = "#define RF_VERSION 3u"
        (self.root / "src" / "rf.h").write_text("\n".join(rf_h_lines) + "\n", encoding="utf-8")
        (self.root / "build" / "logs").mkdir(parents=True)
        (self.root / "build" / "logs" / "blank_tail.log").write_text(
            "Summary: 1 passed, 0 failed\nDone\n\n   \nExit: 0\n",
            encoding="utf-8",
        )
        (self.root / "todo").mkdir()
        (self.root / "todo" / "sample.md").write_text(
            "# Sample\n\n## 1. Test\n\n- Version in `src/rf.h:19` is `#define RF_VERSION 3u`.\n- Missing `src/none.c`.\n"
            '- Quoted semicolon `- `todo/sample.md:6`: "- Missing `src/none.c`."; NOT FOUND: src/none.c`.\n',
            encoding="utf-8",
        )
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
        # Plan review P2 + Round 1 P2: missing init, event before init, event after terminal result, duplicate init,
        # wrong model/agent, disallowed tool, missing result, error event, or nonzero exit despite SUCCESS.
        good_resp = "src/rf.h:19:#define RF_VERSION 3u\nCOUNT: 1\n"
        cases = [
            (
                "missing init",
                f"import json\nprint(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\n",
                "stream missing init event before activity",
            ),
            (
                "result before init",
                f"import json\nprint(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\n"
                "print(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n",
                "stream missing init event before activity",
            ),
            (
                "event after terminal result",
                "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\n"
                "print(json.dumps({'event': 'step_update', 'step_update': {'step_type': 'thinking'}}))\n",
                "stream event on line 3 after terminal result",
            ),
            (
                "duplicate init",
                "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n"
                "print(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\n",
                "duplicate init event on stream line 2",
            ),
            (
                "wrong model",
                "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-3.8-flash-tiered', 'agent': 'rf-scout'}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\n",
                "init model is 'gemini-3.8-flash-tiered'",
            ),
            (
                "wrong agent",
                "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'other'}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\n",
                "init agent is 'other'",
            ),
            (
                "disallowed tool",
                "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n"
                "print(json.dumps({'event': 'step_update', 'step_update': {'step_type': 'tool', 'tool_name': 'write_file'}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\n",
                "disallowed tool call 'write_file'",
            ),
            (
                "truncated stream",
                "import json\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n",
                "stream had 0 result event(s)",
            ),
            ("malformed json", "print('{bad json')\n", "malformed JSON on stream line 1"),
            (
                "nonzero exit with SUCCESS",
                "import json, sys\nprint(json.dumps({'event': 'init', 'init': {'model': 'gemini-4-hydrogen-medium', 'agent': 'rf-scout'}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {good_resp!r}}}}}))\nsys.exit(1)\n",
                "process exited 1 despite SUCCESS result event",
            ),
        ]
        for label, script_body, expected_err in cases:
            self.write_stub_agy(script_body)
            code, _, err = self.run_scout("--agent", "rf-scout", "--prompt", "Test")
            self.assertEqual(code, 2, label)
            self.assertIn(expected_err, err, label)

    def test_unquoted_fabricated_or_structurally_incomplete_responses_fail(self):
        # Round 1 P1: fabricated quotes and partial block structures with NOT FOUND / FAILURES: none found must fail.
        def make_stream(agent: str, response: str) -> str:
            return (
                "import json\n"
                f"print(json.dumps({{'event': 'init', 'init': {{'model': 'gemini-4-hydrogen-medium', 'agent': {agent!r}}}}}))\n"
                f"print(json.dumps({{'event': 'result', 'result': {{'status': 'SUCCESS', 'response': {response!r}}}}}))\n"
            )

        bad_cases = [
            (
                "unquoted prose opinion",
                "rf-scout",
                "Yes, the section looks ready to ship.",
                "unquoted or malformed line in rf-scout response",
            ),
            (
                "fabricated quotation on real line",
                "rf-scout",
                "src/rf.h:1:THIS QUOTE IS FABRICATED\nCOUNT: 1\n",
                "fabricated or mismatched quote at src/rf.h:1",
            ),
            (
                "hallucinated file path",
                "rf-scout",
                "src/does_not_exist.c:10:int x;\nCOUNT: 1\n",
                "hallucinated file path in citation: src/does_not_exist.c:10",
            ),
            (
                "hallucinated line number",
                "rf-scout",
                "src/rf.h:999:#define RF_VERSION 3u\nCOUNT: 1\n",
                "hallucinated line number in citation: src/rf.h:999",
            ),
            (
                "miscounted COUNT",
                "rf-scout",
                "src/rf.h:19:#define RF_VERSION 3u\nCOUNT: 5\n",
                "COUNT: 5 does not match 1 cited line(s)",
            ),
            (
                "unretrieved TRUNCATED marker",
                "rf-scout",
                "src/rf.h:19:#define RF_VERSION 3u\nTRUNCATED: showing 1 match(es)\n",
                "response contains TRUNCATED marker",
            ),
            (
                "log-digest with only FAILURES: none found",
                "rf-log-digest",
                "FAILURES: none found\n",
                "rf-log-digest response missing COUNTS block",
            ),
            (
                "log-digest with fabricated quote in LAST LINES",
                "rf-log-digest",
                "FAILURES: none found\n\nCOUNTS\n- None.\n\nLAST LINES\n- src/rf.h:19:WRONG TEXT\n",
                "fabricated or mismatched quote at src/rf.h:19",
            ),
            (
                "fact-check with NOT FOUND followed by shipping verdict",
                "rf-fact-check",
                "NOT FOUND: example\nOVERALL: approve, ready to ship\n",
                "top-level NOT FOUND response must be a single line",
            ),
            (
                "fact-check missing CHECKED AND HOLDING block despite NOT FOUND in MISMATCHES",
                "rf-fact-check",
                "MISMATCHES\n- `todo/sample.md:6`: `- Missing `src/none.c`.` -> NOT FOUND: src/none.c\n\nNEEDS A COMMAND\n- None.\n",
                "rf-fact-check response missing CHECKED AND HOLDING block",
            ),
            (
                "multiline ESCALATE-LEAD",
                "rf-scout",
                "ESCALATE-LEAD: needs write\nextra commentary\n",
                "ESCALATE-LEAD response must be a single line",
            ),
            (
                "empty quotation on non-blank line",
                "rf-scout",
                "src/rf.h:19:\nCOUNT: 1\n",
                "empty quotation in citation: src/rf.h:19",
            ),
            (
                "blank marker on non-blank line",
                "rf-scout",
                "src/rf.h:19: <blank>\nCOUNT: 1\n",
                "fabricated or mismatched quote at src/rf.h:19: source line is not blank",
            ),
            (
                "non-blank quote on blank line",
                "rf-scout",
                "build/logs/blank_tail.log:3:some text\nCOUNT: 1\n",
                "fabricated or mismatched quote at build/logs/blank_tail.log:3: source line is blank",
            ),
        ]
        for label, agent, resp, expected_err in bad_cases:
            self.write_stub_agy(make_stream(agent, resp))
            code, _, err = self.run_scout("--agent", agent, "--prompt", "Test")
            self.assertEqual(code, 2, label)
            self.assertIn(expected_err, err, label)

        # Valid single-line ESCALATE-LEAD, valid rf-log-digest (including blank and whitespace-only trailing lines),
        # and valid rf-fact-check (both ' -> ' and '; ' separators) all pass
        self.write_stub_agy(make_stream("rf-scout", "ESCALATE-LEAD: writing a file is not read-only"))
        code, out, err = self.run_scout("--agent", "rf-scout", "--prompt", "Create a file")
        self.assertEqual(code, 0, err)
        self.assertEqual(out.strip(), "ESCALATE-LEAD: writing a file is not read-only")

        valid_log = (
            "FAILURES: none found\n\n"
            "COUNTS\n- `build/logs/blank_tail.log:1`: `Summary: 1 passed, 0 failed`\n\n"
            "LAST LINES\n"
            "- `build/logs/blank_tail.log:1`: `Summary: 1 passed, 0 failed`\n"
            "- `build/logs/blank_tail.log:2`: `Done`\n"
            "- `build/logs/blank_tail.log:3`: \"\"\n"
            "- `build/logs/blank_tail.log:4`: `<blank>`\n"
            "- `build/logs/blank_tail.log:5`: `Exit: 0`\n"
        )
        self.write_stub_agy(make_stream("rf-log-digest", valid_log))
        code, _, err = self.run_scout("--agent", "rf-log-digest", "--prompt", "Digest log")
        self.assertEqual(code, 0, err)

        valid_fc = (
            "MISMATCHES\n"
            "- `todo/sample.md:6`: \"- Missing `src/none.c`.\" -> NOT FOUND: src/none.c\n\n"
            "NEEDS A COMMAND\n- None.\n\n"
            "CHECKED AND HOLDING\n"
            "- `todo/sample.md:5`: \"- Version in `src/rf.h:19` is `#define RF_VERSION 3u`.\" -> `src/rf.h:19`: `#define RF_VERSION 3u`\n"
            "HELD: 1\n"
        )
        self.write_stub_agy(make_stream("rf-fact-check", valid_fc))
        code, _, err = self.run_scout("--agent", "rf-fact-check", "--prompt", "Fact check")
        self.assertEqual(code, 0, err)

        valid_fc_semicolon = (
            "MISMATCHES\n"
            "- `todo/sample.md:6`: \"- Missing `src/none.c`.\"; NOT FOUND: src/none.c\n\n"
            "NEEDS A COMMAND\n- None.\n\n"
            "CHECKED AND HOLDING\n"
            "- `todo/sample.md:5`: \"- Version in `src/rf.h:19` is `#define RF_VERSION 3u`.\"; `src/rf.h:19`: `#define RF_VERSION 3u`\n"
            "- `todo/sample.md:7`: '`- `todo/sample.md:6`: \"- Missing `src/none.c`.\"; NOT FOUND: src/none.c`.' -> `src/rf.h:19`: `#define RF_VERSION 3u`\n"
            "HELD: 2\n"
        )
        self.write_stub_agy(make_stream("rf-fact-check", valid_fc_semicolon))
        code, _, err = self.run_scout("--agent", "rf-fact-check", "--prompt", "Fact check semicolon")
        self.assertEqual(code, 0, err)

    def test_timeout_and_signal_kill_entire_descendant_tree_including_escaped_session(self):
        # Plan review P1 + Round 1 P2: even a grandchild that starts its own session is killed and reaped, and the test
        # harness itself tears down the entire descendant tree in finally if the implementation under test ever fails.
        pid_file = self.root / "escaped.pid"
        agy_pid_file = self.root / "agy.pid"

        def cleanup_test_pids() -> None:
            for pf_path in (pid_file, agy_pid_file):
                if pf_path.exists():
                    try:
                        pid = int(pf_path.read_text(encoding="utf-8").strip())
                    except (OSError, ValueError):
                        continue
                    for killer in (
                        lambda p: os.killpg(p, signal.SIGKILL),
                        lambda p: os.kill(p, signal.SIGKILL),
                    ):
                        try:
                            killer(pid)
                        except OSError:
                            pass

        self.write_stub_agy(
            f"import os, subprocess, sys, time\n"
            f"open({str(agy_pid_file)!r}, 'w').write(str(os.getpid()))\n"
            f"child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)\n"
            f"open({str(pid_file)!r}, 'w').write(str(child.pid))\n"
            f"sys.stdout.flush()\n"
            f"time.sleep(60)\n"
        )
        try:
            code, _, err = self.run_scout("--agent", "rf-scout", "--prompt", "Timeout probe", "--timeout", "1")
            self.assertEqual(code, 2)
            self.assertIn("timed out after 1s", err)
            escaped_pid = int(pid_file.read_text(encoding="utf-8"))
            self.assertFalse(gs.pid_alive(escaped_pid), "escaped grandchild survived timeout cleanup")
        finally:
            cleanup_test_pids()

        # Also test signal cancellation while running
        pid_file.unlink(missing_ok=True)
        agy_pid_file.unlink(missing_ok=True)
        env = dict(os.environ, PATH=f"{self.stub_bin}:{os.environ.get('PATH', '')}")
        proc = subprocess.Popen(
            [sys.executable, str(SCOUT_PATH), "--agent", "rf-scout", "--prompt", "Signal probe", "--timeout", "60"],
            cwd=REPO,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            for _ in range(100):
                if pid_file.exists() and pid_file.read_text(encoding="utf-8").strip():
                    break
                time.sleep(0.05)
            escaped_pid = int(pid_file.read_text(encoding="utf-8"))
            proc.send_signal(signal.SIGTERM)
            exit_code = proc.wait(timeout=15)
            self.assertEqual(exit_code, 130)
            self.assertFalse(gs.pid_alive(escaped_pid), "escaped grandchild survived SIGTERM cancellation")
        finally:
            cleanup_test_pids()
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except OSError:
                    proc.kill()
                proc.wait()
            for pipe in (proc.stdout, proc.stderr):
                if pipe is not None:
                    pipe.close()


if __name__ == "__main__":
    unittest.main()
