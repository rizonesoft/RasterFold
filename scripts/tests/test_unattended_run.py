"""scripts/unattended-run.sh: the launcher builds its own PATH.

muse, codex and agy install into ~/.local/bin, which only login and
interactive shells add to PATH. A Desktop shortcut launches the script in
neither, so preflight reported all three missing from a sparse PATH even
though every CLI was installed (preflight output pasted 2026-11-10).
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]


class LauncherPathTests(unittest.TestCase):
    def test_sparse_path_still_resolves_user_bin_clis(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / "home"
            user_bin = home / ".local" / "bin"
            user_bin.mkdir(parents=True)
            for cli in ("muse", "codex", "agy"):
                (user_bin / cli).write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                (user_bin / cli).chmod(0o755)
            stub_bin = Path(tmp) / "stub"
            stub_bin.mkdir()
            # Stand in for the preflight: report what PATH resolves, then fail
            # so the launcher stops before it could exec a real campaign.
            (stub_bin / "python3").write_text(
                "#!/bin/sh\nfor c in muse codex agy; do command -v $c; done\nexit 1\n", encoding="utf-8")
            (stub_bin / "python3").chmod(0o755)
            env = {"HOME": str(home), "TERM": "xterm",
                   "PATH": f"{stub_bin}:/usr/bin:/bin"}
            proc = subprocess.run(
                ["bash", str(SCRIPTS / "unattended-run.sh")],
                input="exit\n", capture_output=True, text=True, env=env, timeout=30)
            out = proc.stdout + proc.stderr
            for cli in ("muse", "codex", "agy"):
                self.assertIn(str(user_bin / cli), out)
            self.assertIn("Preflight failed", out)


if __name__ == "__main__":
    unittest.main()
