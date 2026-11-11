"""scripts/unattended-run.sh: the launcher builds its own PATH and starts Claude Code with the exact unattended flags.

claude and codex install into ~/.local/bin, which only login and interactive shells add to PATH. A Desktop shortcut
launches the script in neither, so preflight reported the CLIs missing from a sparse PATH even though each was
installed (preflight output pasted 2026-11-10). The stubs below stand in for the preflight and for Claude Code, so
no campaign can start from a test.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

SCRIPTS = Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
LAUNCHER = SCRIPTS / "unattended-run.sh"


def campaign_prompt_in_script() -> str:
    lines = [line for line in LAUNCHER.read_text(encoding="utf-8").split("\n") if line.startswith("PROMPT='")]
    assert len(lines) == 1 and lines[0].endswith("'"), "one single-quoted PROMPT line"
    return lines[0][len("PROMPT='"):-1]


class Launch:
    """One launcher run in a throwaway HOME with stub `python3` (the preflight) and `claude` on PATH."""

    def __init__(self, tmp: str, preflight_exit: int = 0, env: dict | None = None, path_prefix: str = ""):
        home = Path(tmp) / "home"
        self.user_bin = home / ".local" / "bin"
        self.user_bin.mkdir(parents=True, exist_ok=True)
        stub_bin = Path(tmp) / "stub"
        stub_bin.mkdir(exist_ok=True)
        self.argv_file = Path(tmp) / "claude-argv"
        self.preflight_file = Path(tmp) / "preflight-argv"
        for path in (self.argv_file, self.preflight_file):
            path.unlink(missing_ok=True)
        # The stub claude writes its arguments NUL-separated: the prompt holds spaces and punctuation.
        (self.user_bin / "claude").write_text(
            f'#!/bin/sh\nprintf "%s\\0" "$@" > "{self.argv_file}"\necho $$ > "{self.argv_file}.pid"\n'
            f'flock -n build/unattended-launch.lock true && echo free > "{self.argv_file}.lock"\nexit 0\n', encoding="utf-8")
        (self.user_bin / "codex").write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        (stub_bin / "python3").write_text(
            f'#!/bin/sh\nprintf "%s\\n" "$*" > "{self.preflight_file}"\nfor c in claude codex; do command -v $c; done\nexit {preflight_exit}\n',
            encoding="utf-8")
        for stub in (self.user_bin / "claude", self.user_bin / "codex", stub_bin / "python3"):
            stub.chmod(0o755)
        run_env = {"HOME": str(home), "TERM": "xterm", "PATH": f"{path_prefix + ':' if path_prefix else ''}{stub_bin}:/usr/bin:/bin", **(env or {})}
        proc = subprocess.run(["/bin/bash", str(LAUNCHER)], input="exit\n", capture_output=True, text=True, env=run_env, timeout=30)
        self.out = proc.stdout + proc.stderr
        self.argv = self.argv_file.read_bytes().decode("utf-8").split("\0")[:-1] if self.argv_file.exists() else None
        self.preflight = self.preflight_file.read_text(encoding="utf-8").strip() if self.preflight_file.exists() else None
        pid_file, lock_file = Path(f"{self.argv_file}.pid"), Path(f"{self.argv_file}.lock")
        self.claude_pid = pid_file.read_text(encoding="utf-8").strip() if pid_file.exists() else None
        self.lock_free_at_start = lock_file.exists()


class LauncherPathTests(unittest.TestCase):
    def test_sparse_path_still_resolves_user_bin_clis(self):
        with tempfile.TemporaryDirectory() as tmp:
            launch = Launch(tmp, preflight_exit=1)
            for cli in ("claude", "codex"):
                self.assertIn(str(launch.user_bin / cli), launch.out)
            self.assertIn("Preflight failed", launch.out)


class LauncherClaudeTests(unittest.TestCase):
    def test_starts_claude_code_with_the_exact_unattended_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            launch = Launch(tmp)
        argv = launch.argv
        self.assertIsNotNone(argv, launch.out)
        session = argv[argv.index("--session-id") + 1]
        # The claim names the session id Claude Code is then started with, and the pid `exec claude` keeps.
        self.assertEqual(launch.preflight, f"scripts/unattended-preflight.py --repair --claim {session} --claim-pid {launch.claude_pid}")
        self.assertTrue(launch.lock_free_at_start, "the launch lock must be released before Claude Code starts")
        self.assertEqual(str(uuid.UUID(session)), session)
        self.assertEqual(argv, ["--dangerously-skip-permissions", "--model", "opus", "--effort", "medium",
                                "--session-id", session,
                                "--disallowedTools", "AskUserQuestion,EnterPlanMode,ExitPlanMode",
                                "--plugin-dir", ".claude/lsp/rasterfold-lsp", campaign_prompt_in_script()])

    def test_the_model_is_the_opus_alias_never_a_dated_id(self):
        text = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("--model opus --effort medium", text)
        self.assertNotRegex(text, r"--model\s+claude-")
        self.assertNotIn("muse", text.lower())

    def test_each_launch_gets_its_own_session_id(self):
        ids = []
        for _ in range(2):
            with tempfile.TemporaryDirectory() as tmp:
                argv = Launch(tmp).argv
                ids.append(argv[argv.index("--session-id") + 1])
        self.assertNotEqual(ids[0], ids[1])

    def test_a_failed_preflight_starts_no_claude(self):
        with tempfile.TemporaryDirectory() as tmp:
            launch = Launch(tmp, preflight_exit=1)
        self.assertIsNone(launch.argv)
        self.assertIn("the unattended run was not started", launch.out)

    def test_the_failure_shell_does_not_keep_the_launch_lock(self):
        # `exec bash` keeps the window open after a failed preflight; if it kept descriptor 9 too, every later
        # launch would wait on the lock forever. The stub shell below is that `exec bash`.
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "lock-probe"
            shell_bin = Path(tmp) / "shell"
            shell_bin.mkdir()
            (shell_bin / "bash").write_text(
                f'#!/bin/sh\nif [ "$#" -eq 0 ]; then flock -n build/unattended-launch.lock true && echo free > "{probe}"; exit 0; fi\nexec /bin/bash "$@"\n',
                encoding="utf-8")
            (shell_bin / "bash").chmod(0o755)
            Launch(tmp, preflight_exit=1, path_prefix=str(shell_bin))
            self.assertTrue(probe.exists(), "the failure shell still held build/unattended-launch.lock")

    def test_prompt_override_replaces_the_prompt_and_nothing_else(self):
        with tempfile.TemporaryDirectory() as tmp:
            override = Path(tmp) / "dry.txt"
            override.write_text("Report your session id, then end the turn.", encoding="utf-8")
            launch = Launch(tmp, env={"UNATTENDED_PROMPT_OVERRIDE": str(override)})
            self.assertEqual(launch.argv[-1], "Report your session id, then end the turn.")
            self.assertEqual(launch.argv[:5], ["--dangerously-skip-permissions", "--model", "opus", "--effort", "medium"])
            missing = Launch(tmp, env={"UNATTENDED_PROMPT_OVERRIDE": str(Path(tmp) / "absent.txt")})
            self.assertIsNone(missing.argv)
            self.assertIn("does not name a readable file", missing.out)

    def test_the_document_carries_the_seed_prompt_byte_for_byte(self):
        doc = (REPO / "docs" / "unattended-run.md").read_text(encoding="utf-8").split("\n")
        start = doc.index("## The seed prompt (verbatim in unattended-run.sh)")
        quoted = [line for line in doc[start + 1:start + 4] if line.startswith("> ")]
        self.assertEqual(len(quoted), 1)
        self.assertEqual(quoted[0][2:], campaign_prompt_in_script())

    def test_the_seed_prompt_carries_the_goal_the_autonomy_rule_and_the_scout_rule(self):
        prompt = campaign_prompt_in_script()
        for phrase in ("Finish RasterFold 100% complete and excellent", "Act with full autonomy", "Never ask the operator a question",
                       "CronCreate, 3-59/5 * * * *", "$CLAUDE_CODE_SESSION_ID", "rf-scout, rf-log-digest, rf-fact-check",
                       "never delegate", "scripts/review-prompt.py", "pushed straight on main"):
            self.assertIn(phrase, prompt)


if __name__ == "__main__":
    unittest.main()
