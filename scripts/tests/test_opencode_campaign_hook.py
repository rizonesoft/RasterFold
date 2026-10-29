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


if __name__ == "__main__":
    unittest.main()
