"""The Codex Stop hook uses the shared campaign guard and its real workspace root."""
import importlib.util
import json
import os
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


class CodexCampaignHook(unittest.TestCase):
    def test_configured_hook_blocks_only_the_owner_from_a_subdirectory(self):
        config = json.loads((ROOT / ".codex/hooks.json").read_text(encoding="utf-8"))
        hook = config["hooks"]["Stop"][0]["hooks"][0]
        command = hook["commandWindows"] if os.name == "nt" else hook["command"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(guard._workspace(tmp))
            (root / ".codex").mkdir()
            shutil.copyfile(ROOT / ".codex/hooks.json", root / ".codex/hooks.json")
            shutil.copyfile(ROOT / "scripts/codex-campaign-hook.py", root / "scripts/codex-campaign-hook.py")
            guard._guard(str(root), "owner-session")
            cwd = root / "docs"
            for session, expected, extra in (("owner-session", "block", {}), ("other-session", None, {}),
                                             ("owner-session", "block", {"conversationId": None}),
                                             ("owner-session", "block", {"conversationId": "foreign-session"}),
                                             ("owner-session", "block", {"conversationId": "foreign-session",
                                                                          "hook_event_name": None})):
                (root / "build/claude-campaign-state.json").unlink(missing_ok=True)
                payload = json.dumps({"session_id": session, "cwd": str(cwd), "hook_event_name": "Stop", **extra})
                result = subprocess.run(command, shell=True, input=payload, capture_output=True, text=True,
                                        cwd=cwd, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout).get("decision"), expected)

    def test_hook_fails_open_when_git_is_broken(self):
        config = json.loads((ROOT / ".codex/hooks.json").read_text(encoding="utf-8"))
        hook = config["hooks"]["Stop"][0]["hooks"][0]
        command = hook["commandWindows"] if os.name == "nt" else hook["command"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(guard._workspace(tmp))
            (root / ".codex").mkdir()
            shutil.copyfile(ROOT / ".codex/hooks.json", root / ".codex/hooks.json")
            shutil.copyfile(ROOT / "scripts/codex-campaign-hook.py", root / "scripts/codex-campaign-hook.py")
            bindir = root / "fake-bin"
            bindir.mkdir()
            fake_git = bindir / ("git.cmd" if os.name == "nt" else "git")
            fake_git.write_text("@exit /b 1\n" if os.name == "nt" else "#!/bin/sh\nexit 1\n", encoding="utf-8")
            fake_git.chmod(0o755)
            env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ.get("PATH", ""))
            payload = json.dumps({"session_id": "any", "cwd": str(root), "hook_event_name": "Stop"})
            result = subprocess.run(command, shell=True, input=payload, capture_output=True, text=True,
                                    cwd=root, timeout=60, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), {})

    def test_nested_git_repository_fails_open_without_executing_a_parent_guard(self):
        config = json.loads((ROOT / ".codex/hooks.json").read_text(encoding="utf-8"))
        hook = config["hooks"]["Stop"][0]["hooks"][0]
        command = hook["commandWindows"] if os.name == "nt" else hook["command"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(guard._workspace(tmp))
            (root / ".codex").mkdir()
            shutil.copyfile(ROOT / ".codex/hooks.json", root / ".codex/hooks.json")
            shutil.copyfile(ROOT / "scripts/codex-campaign-hook.py", root / "scripts/codex-campaign-hook.py")
            guard._guard(str(root), "owner-session")
            nested = root / "docs/nested"
            nested.mkdir()
            subprocess.run(["git", "init", "-q", str(nested)], check=True)
            payload = json.dumps({"session_id": "owner-session", "cwd": str(nested), "hook_event_name": "Stop"})
            result = subprocess.run(command, shell=True, input=payload, capture_output=True, text=True,
                                    cwd=nested, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), {})

    def test_parent_markers_cannot_replace_the_project_guard(self):
        config = json.loads((ROOT / ".codex/hooks.json").read_text(encoding="utf-8"))
        hook = config["hooks"]["Stop"][0]["hooks"][0]
        command = hook["commandWindows"] if os.name == "nt" else hook["command"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(guard._workspace(tmp))
            (root / ".codex").mkdir()
            shutil.copyfile(ROOT / ".codex/hooks.json", root / ".codex/hooks.json")
            shutil.copyfile(ROOT / "scripts/codex-campaign-hook.py", root / "scripts/codex-campaign-hook.py")
            guard._guard(str(root), "owner-session")
            parent = root.parent
            (parent / ".codex").mkdir()
            (parent / "scripts").mkdir()
            (parent / ".codex/hooks.json").write_text("{}", encoding="utf-8")
            (parent / "scripts/codex-campaign-hook.py").write_text("print('{}')\n", encoding="utf-8")
            payload = json.dumps({"session_id": "owner-session", "cwd": str(root), "hook_event_name": "Stop"})
            result = subprocess.run(command, shell=True, input=payload, capture_output=True, text=True,
                                    cwd=root, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout).get("decision"), "block")

    def test_nested_markers_cannot_replace_the_project_guard(self):
        config = json.loads((ROOT / ".codex/hooks.json").read_text(encoding="utf-8"))
        hook = config["hooks"]["Stop"][0]["hooks"][0]
        command = hook["commandWindows"] if os.name == "nt" else hook["command"]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(guard._workspace(tmp))
            (root / ".codex").mkdir()
            shutil.copyfile(ROOT / ".codex/hooks.json", root / ".codex/hooks.json")
            shutil.copyfile(ROOT / "scripts/codex-campaign-hook.py", root / "scripts/codex-campaign-hook.py")
            guard._guard(str(root), "owner-session")
            nested = root / "docs/nested"
            (nested / ".codex").mkdir(parents=True)
            (nested / "scripts").mkdir()
            (nested / ".codex/hooks.json").write_text("{}", encoding="utf-8")
            (nested / "scripts/codex-campaign-hook.py").write_text("print('{}')\n", encoding="utf-8")
            payload = json.dumps({"session_id": "owner-session", "cwd": str(nested), "hook_event_name": "Stop"})
            result = subprocess.run(command, shell=True, input=payload, capture_output=True, text=True,
                                    cwd=nested, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout).get("decision"), "block")

    def test_adapter_fails_open_for_oversized_input_and_import_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(guard._workspace(tmp))
            script = root / "scripts/codex-campaign-hook.py"
            shutil.copyfile(ROOT / "scripts/codex-campaign-hook.py", script)
            guard._guard(str(root), "owner-session")
            payload = json.dumps({"session_id": "owner-session", "cwd": str(root),
                                  "hook_event_name": "Stop", "last_assistant_message": "x" * 2_000_001})
            result = subprocess.run([sys.executable, str(script)], input=payload, capture_output=True, text=True,
                                    cwd=root, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), {})
            (root / "scripts/campaign_guard.py").write_text("raise SystemExit(7)\n", encoding="utf-8")
            result = subprocess.run([sys.executable, str(script)], input="{}", capture_output=True, text=True,
                                    cwd=root, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), {})


if __name__ == "__main__":
    unittest.main()
