"""scripts/unattended-preflight.py: the Claude Code launch preconditions, the owner check, and the bounded repair.

Two failures these cases exist for. A second double-click during a live Claude Code campaign must refuse, never hand
the guard to a second writer or rebuild under the first: so a live owner is found through the session registry, an
unreadable registry entry for a live process is unknown rather than dead, and the repair runs only after ownership
is settled. And "read-only Sonnet scouts" must be a property the roster check can refuse, not a sentence.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
_spec = importlib.util.spec_from_file_location("rf_unattended_preflight", SCRIPTS / "unattended-preflight.py")
pf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pf)

SESSION = "11111111-2222-4333-8444-555555555555"
GOOD_SCOUT = "---\nname: {name}\ndescription: Read-only scout.\ntools: Read, Grep, Glob\nmodel: sonnet\neffort: low\n---\n\nBody.\n"


def dead_pid() -> int:
    pid = 4_000_000
    while pf.pid_alive(pid):
        pid += 1
    return pid


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sessions = Path(self.tmp.name) / "sessions"
        self.sessions.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def entry(self, pid, text):
        (self.sessions / f"{pid}.json").write_text(text, encoding="utf-8")

    def test_a_live_pid_naming_the_session_is_running(self):
        self.entry(os.getpid(), json.dumps({"sessionId": SESSION, "pid": os.getpid()}))
        self.assertIs(pf.claude_registry_running(SESSION, self.sessions), True)

    def test_a_dead_pid_naming_the_session_is_not_running(self):
        self.entry(dead_pid(), json.dumps({"sessionId": SESSION}))
        self.assertIs(pf.claude_registry_running(SESSION, self.sessions), False)

    def test_another_live_session_is_not_this_one(self):
        self.entry(os.getpid(), json.dumps({"sessionId": "someone-else"}))
        self.assertIs(pf.claude_registry_running(SESSION, self.sessions), False)

    def test_an_unreadable_entry_for_a_live_pid_is_unknown_never_dead(self):
        self.entry(os.getpid(), "{ not json")
        self.assertIsNone(pf.claude_registry_running(SESSION, self.sessions))
        with mock.patch.object(Path, "read_text", side_effect=PermissionError("denied")):
            self.assertIsNone(pf.claude_registry_running(SESSION, self.sessions))

    def test_an_unreadable_entry_for_a_dead_pid_is_ignored(self):
        self.entry(dead_pid(), "{ not json")
        self.assertIs(pf.claude_registry_running(SESSION, self.sessions), False)

    def test_a_directory_that_cannot_be_listed_is_unknown(self):
        with mock.patch.object(Path, "glob", side_effect=PermissionError("denied")):
            self.assertIsNone(pf.claude_registry_running(SESSION, self.sessions))

    def test_no_registry_and_key_files_are_not_owners(self):
        self.assertIs(pf.claude_registry_running(SESSION, self.sessions / "absent"), False)
        (self.sessions / f"{os.getpid()}.abcdef.key").write_text("x", encoding="utf-8")
        self.assertIs(pf.claude_registry_running(SESSION, self.sessions), False)

    def test_running_wins_then_unknown_then_dead(self):
        for table, registry, want in ((False, True, True), (True, None, True), (False, None, None), (None, False, None),
                                      (False, False, False)):
            with mock.patch.object(pf, "process_table_running", return_value=table), \
                    mock.patch.object(pf, "claude_registry_running", return_value=registry):
                self.assertIs(pf.session_running(SESSION), want, (table, registry))
        self.assertIs(pf.session_running(""), False)

    def test_a_launcher_started_session_is_found_by_its_command_line(self):
        sleeper = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)", "--session-id", SESSION])
        try:
            time.sleep(0.2)
            self.assertIs(pf.process_table_running(SESSION), True)
        finally:
            sleeper.kill()
            sleeper.wait()


class PreconditionTests(unittest.TestCase):
    def test_the_repository_wires_the_campaign_stop_hook(self):
        self.assertTrue(pf.stop_hook_wired(pf.read_json(REPO / ".claude" / "settings.json")))

    def test_a_missing_or_foreign_stop_hook_fails(self):
        self.assertFalse(pf.stop_hook_wired({}))
        self.assertFalse(pf.stop_hook_wired({"hooks": {"Stop": [{"hooks": [{"type": "command", "command": "echo done"}]}]}}))
        self.assertFalse(pf.stop_hook_wired({"hooks": {"PreToolUse": [{"hooks": [{"command": "campaign_guard.py stop-hook"}]}]}}))

    def test_dialog_flags_must_be_true_not_merely_present(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "checkout"
            root.mkdir()
            self.assertTrue(pf.workspace_trusted({"projects": {str(root): {"hasTrustDialogAccepted": True}}}, root))
            self.assertTrue(pf.workspace_trusted({"projects": {tmp: {"hasTrustDialogAccepted": True}}}, root), "a trusted parent")
            self.assertFalse(pf.workspace_trusted({"projects": {str(root): {"hasTrustDialogAccepted": False}}}, root))
            self.assertFalse(pf.workspace_trusted({"projects": {str(root): {"hasTrustDialogAccepted": "true"}}}, root))
            self.assertFalse(pf.workspace_trusted({}, root))
            bad = Path(tmp) / "settings.json"
            bad.write_text("[1, 2]", encoding="utf-8")
            self.assertEqual(pf.read_json(bad), {})
            self.assertEqual(pf.read_json(Path(tmp) / "absent.json"), {})

    def test_cron_disabled_fails(self):
        self.assertTrue(pf.cron_enabled({}))
        self.assertTrue(pf.cron_enabled({"CLAUDE_CODE_DISABLE_CRON": "0"}))
        for value in ("1", "true", "yes"):
            self.assertFalse(pf.cron_enabled({"CLAUDE_CODE_DISABLE_CRON": value}), value)


class RosterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.agents = Path(self.tmp.name) / "agents"
        self.agents.mkdir()
        for name in pf.SCOUTS:
            (self.agents / f"{name}.md").write_text(GOOD_SCOUT.format(name=name), encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def edit(self, old, new, name="rf-scout"):
        path = self.agents / f"{name}.md"
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new), encoding="utf-8")

    def problems(self):
        return "; ".join(pf.roster_problems(self.agents))

    def test_the_repository_roster_is_the_three_read_only_sonnet_scouts(self):
        self.assertEqual(pf.roster_problems(REPO / ".claude" / "agents"), [])
        self.assertEqual(pf.roster_problems(self.agents), [])

    def test_another_model_is_refused(self):
        self.edit("model: sonnet", "model: opus")
        self.assertIn("rf-scout: model is 'opus', not sonnet", self.problems())
        self.edit("model: opus\n", "")
        self.assertIn("rf-scout: model is None, not sonnet", self.problems())

    def test_a_tool_that_writes_or_runs_is_refused(self):
        for tools in ("Read, Grep, Glob, Bash", "Read, Edit", "Read, Write", "Read, Agent", "*"):
            self.edit("tools: Read, Grep, Glob", f"tools: {tools}")
            self.assertIn("rf-scout: tools beyond Read, Grep, Glob", self.problems(), tools)
            self.edit(f"tools: {tools}", "tools: Read, Grep, Glob")

    def test_a_missing_tools_line_is_refused_because_it_inherits_everything(self):
        self.edit("tools: Read, Grep, Glob\n", "")
        self.assertIn("rf-scout: no tools line", self.problems())

    def test_keys_that_could_add_a_tool_or_a_mode_are_refused(self):
        for key in ("mcpServers: something", "permissionMode: bypassPermissions", "skills: x", "hooks: y", "disallowedTools: Bash"):
            self.edit("effort: low", f"effort: low\n{key}")
            self.assertIn("frontmatter keys not allowed on a scout: " + key.split(":")[0], self.problems(), key)
            self.edit(f"effort: low\n{key}", "effort: low")

    def test_an_extra_missing_or_misnamed_agent_is_refused(self):
        (self.agents / "rf-hands.md").write_text(GOOD_SCOUT.format(name="rf-hands"), encoding="utf-8")
        self.assertIn("roster is rf-fact-check, rf-hands, rf-log-digest, rf-scout", self.problems())
        (self.agents / "rf-hands.md").unlink()
        (self.agents / "rf-scout.md").unlink()
        self.assertIn("expected rf-fact-check, rf-log-digest, rf-scout", self.problems())
        (self.agents / "rf-scout.md").write_text(GOOD_SCOUT.format(name="rf-other"), encoding="utf-8")
        self.assertIn("rf-scout: name is 'rf-other'", self.problems())

    def test_frontmatter_that_is_missing_or_nested_is_refused(self):
        (self.agents / "rf-scout.md").write_text("No frontmatter.\n", encoding="utf-8")
        self.assertIn("rf-scout: frontmatter is missing", self.problems())
        (self.agents / "rf-scout.md").write_text("---\nname: rf-scout\ntools:\n  - Read\n  - Bash\nmodel: sonnet\n---\n", encoding="utf-8")
        self.assertIn("rf-scout: frontmatter is missing or not flat", self.problems())
        self.assertIn("(empty)", "; ".join(pf.roster_problems(self.agents / "absent")))


class RepairFlowTests(unittest.TestCase):
    """main() against a throwaway root, with every subprocess faked: what matters is the order of ownership and repair."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "build").mkdir()
        self.calls = []
        self.database_current = False
        self.repair_exit = 0

    def tearDown(self):
        self.tmp.cleanup()

    def fake_run(self, args, timeout=300):
        words = [str(a) for a in args]
        self.calls.append(" ".join(words[1:]) if words[0] != "git" else " ".join(words))
        out, code = "ok", 0
        if words[:2] == ["git", "branch"]:
            out = "main\n"
        elif words[:2] == ["git", "status"]:
            out = ""
        elif words[:2] == ["git", "config"]:
            out = "tools/githooks\n"
        elif "--rebuild-stale" in words:
            code = self.repair_exit
            self.database_current = code == 0
            out = "rebuilt" if code == 0 else "gen-compdb: could not bring current: linux-x64 (build exit 2)"
        elif words[-2:] == ["scripts/gen-compdb.py", "--check"]:
            code = 0 if self.database_current else 1
            out = "current" if code == 0 else "gen-compdb: per-target compile_commands.json for linux-x64 does not match build.py"
        return subprocess.CompletedProcess(args, code, out, "")

    def preflight(self, *argv, running=False, guard=False):
        if guard:
            (self.root / "build" / "claude-campaign-guard.json").write_text(json.dumps({"session_id": SESSION}), encoding="utf-8")
        out = io.StringIO()
        with mock.patch.object(pf, "ROOT", self.root), mock.patch.object(pf, "run", self.fake_run), \
                mock.patch.object(pf, "session_running", return_value=running), contextlib.redirect_stdout(out):
            code = pf.main(list(argv))
        return code, out.getvalue()

    def rebuilt(self):
        return any("--rebuild-stale" in call for call in self.calls)

    def test_repair_rebuilds_a_stale_database_when_nobody_owns_the_tree(self):
        _, text = self.preflight("--repair")
        self.assertTrue(self.rebuilt())
        self.assertIn("REPAIRED  compile_commands.json", text)
        self.assertIn("PASS  compile_commands.json current", text)

    def test_without_repair_the_preflight_writes_nothing(self):
        code, text = self.preflight()
        self.assertFalse(self.rebuilt())
        self.assertEqual(code, 1)
        self.assertIn("FAIL  compile_commands.json current", text)

    def test_a_live_owner_refuses_and_nothing_is_rebuilt(self):
        code, text = self.preflight("--repair", running=True, guard=True)
        self.assertEqual(code, 1)
        self.assertFalse(self.rebuilt(), "no build may run under a live writer")
        self.assertIn(f"FAIL  no live campaign guard  -- session {SESSION} is running", text)

    def test_an_unknown_owner_refuses_and_nothing_is_rebuilt(self):
        code, text = self.preflight("--repair", running=None, guard=True)
        self.assertEqual(code, 1)
        self.assertFalse(self.rebuilt())
        self.assertIn("FAIL  no live campaign guard  -- cannot tell whether session", text)

    def test_a_dead_owner_is_stale_and_the_repair_may_run(self):
        _, text = self.preflight("--repair", running=False, guard=True)
        self.assertIn(f"STALE  stale campaign guard: {SESSION} (handover on start)", text)
        self.assertNotIn("FAIL  no live campaign guard", text)
        self.assertTrue(self.rebuilt())

    def test_a_failed_repair_is_a_fail(self):
        self.repair_exit = 1
        code, text = self.preflight("--repair")
        self.assertEqual(code, 1)
        self.assertIn("FAIL  compile_commands.json current  -- gen-compdb: could not bring current", text)
        self.assertNotIn("REPAIRED", text)

    def test_a_dirty_tree_blocks_the_repair(self):
        clean = self.fake_run

        def dirty(args, timeout=300):
            result = clean(args, timeout)
            if [str(a) for a in args][:2] == ["git", "status"]:
                result.stdout = " M src/cm.c\n"
            return result
        self.fake_run = dirty
        code, text = self.preflight("--repair")
        self.assertEqual(code, 1)
        self.assertIn("FAIL  tree clean", text)
        self.assertFalse(self.rebuilt())

    def test_the_writer_checks_are_claude_code_not_muse(self):
        _, text = self.preflight()
        for line in ("claude on PATH", "codex on PATH", "campaign Stop hook wired", "bypass-permissions prompt pre-accepted",
                     "workspace trusted", "session cron tools enabled", "read-only Sonnet scout roster",
                     "Claude Code language-server plugin"):
            self.assertIn(line, text)
        self.assertNotIn("muse on PATH", text)
        self.assertNotIn("agy on PATH", text)


if __name__ == "__main__":
    unittest.main()
