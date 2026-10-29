"""The OpenCode turn-end adapter uses the shared campaign guard and never continues an interrupted turn."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("campaign_guard_fixture", ROOT / "scripts/campaign_guard.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class OpenCodeCampaignHook(unittest.TestCase):
    def workspace(self, tmp):
        root = Path(guard._workspace(tmp))
        shutil.copyfile(ROOT / "scripts/opencode-campaign-hook.py", root / "scripts/opencode-campaign-hook.py")
        return root

    def run_hook(self, root, payload, cwd=None):
        result = subprocess.run([sys.executable, str(root / "scripts/opencode-campaign-hook.py")], input=payload,
                                capture_output=True, text=True, cwd=cwd or root, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_blocks_only_the_owner_and_never_an_interrupted_turn(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.workspace(tmp)
            guard._guard(str(root), "owner-session")
            cwd = root / "docs"
            for session, interrupted, expected in (("owner-session", False, "block"),
                                                   ("other-session", False, None),
                                                   ("owner-session", True, None)):
                with self.subTest(session=session, interrupted=interrupted):
                    (root / "build/claude-campaign-state.json").unlink(missing_ok=True)
                    payload = json.dumps({"session_id": session, "cwd": str(cwd), "interrupted": interrupted})
                    decision = self.run_hook(root, payload, cwd=cwd)
                    self.assertEqual(decision.get("decision"), expected)
                    if expected:
                        self.assertIn("reason", decision)

    def test_a_missing_guard_lets_the_turn_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.workspace(tmp)
            payload = json.dumps({"session_id": "owner-session", "cwd": str(root), "interrupted": False})
            self.assertEqual(self.run_hook(root, payload), {})

    def test_malformed_and_oversized_payloads_fail_open_and_are_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.workspace(tmp)
            guard._guard(str(root), "owner-session")
            cases = ("not json", "[]", json.dumps({"cwd": str(root)}), json.dumps({"session_id": ""}),
                     json.dumps({"session_id": "owner-session", "pad": "x" * 2_000_001}))
            for payload in cases:
                with self.subTest(payload=payload[:40]):
                    (root / "build/claude-campaign-state.json").unlink(missing_ok=True)
                    self.assertEqual(self.run_hook(root, payload), {})
                    self.assertTrue(guard._state(str(root)).get("hook_error"), "the failure was not recorded")

    def test_a_broken_guard_module_fails_open(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.workspace(tmp)
            (root / "scripts/campaign_guard.py").write_text("raise SystemExit(7)\n", encoding="utf-8")
            payload = json.dumps({"session_id": "owner-session", "cwd": str(root), "interrupted": False})
            self.assertEqual(self.run_hook(root, payload), {})

    def test_records_each_turn_end_and_seen_verifies_the_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.workspace(tmp)
            before = subprocess.run(
                [sys.executable, str(root / "scripts/opencode-campaign-hook.py"),
                 "--seen", "--session", "ses_probe", "--since", "2026-10-29T00:00:00Z"],
                capture_output=True, text=True, cwd=root, timeout=60,
            )
            self.assertEqual(before.returncode, 1)
            self.assertIn("unseen ses_probe", before.stderr)
            payload = json.dumps({"session_id": "ses_probe", "cwd": str(root), "interrupted": False})
            self.assertEqual(self.run_hook(root, payload), {})
            after = subprocess.run(
                [sys.executable, str(root / "scripts/opencode-campaign-hook.py"),
                 "--seen", "--session", "ses_probe", "--since", "2020-01-01T00:00:00Z"],
                capture_output=True, text=True, cwd=root, timeout=60,
            )
            self.assertEqual(after.returncode, 0, after.stderr)
            self.assertRegex(after.stdout.strip(), r"^seen ses_probe at \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
            stale = subprocess.run(
                [sys.executable, str(root / "scripts/opencode-campaign-hook.py"),
                 "--seen", "--session", "ses_probe", "--since", "2099-01-01T00:00:00Z"],
                capture_output=True, text=True, cwd=root, timeout=60,
            )
            self.assertEqual(stale.returncode, 1)
            self.assertIn("stale ses_probe", stale.stderr)
            bad_since = subprocess.run(
                [sys.executable, str(root / "scripts/opencode-campaign-hook.py"),
                 "--seen", "--session", "ses_probe", "--since", "not-a-timestamp"],
                capture_output=True, text=True, cwd=root, timeout=60,
            )
            self.assertEqual(bad_since.returncode, 2)
            self.assertIn("invalid --since", bad_since.stderr)

    def test_seen_record_failure_is_recorded_and_does_not_bypass_the_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.workspace(tmp)
            guard._guard(str(root), "owner-session")
            # Make the atomic replace into build/opencode-hook-seen.json fail by placing a directory there.
            (root / "build/opencode-hook-seen.json").mkdir(parents=True, exist_ok=True)
            payload = json.dumps({"session_id": "owner-session", "cwd": str(root), "interrupted": False})
            decision = self.run_hook(root, payload)
            self.assertEqual(decision.get("decision"), "block")
            self.assertIn("seen-record failed", guard._state(str(root)).get("hook_error", ""))


if __name__ == "__main__":
    unittest.main()
