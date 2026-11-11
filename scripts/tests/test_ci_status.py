"""The CI read-back finds the GitHub CLI under its Linux and its Windows name."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_status", ROOT / "scripts" / "ci_status.py")
ci_status = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ci_status)


class FindGh(unittest.TestCase):
    def test_the_linux_name_wins(self):
        seen = []

        def which(name, path):
            seen.append((name, path))
            return "/usr/bin/gh" if name == "gh" else "/mnt/c/gh.exe"

        self.assertEqual(ci_status.find_gh(which, "/usr/bin"), "/usr/bin/gh")
        self.assertEqual(seen, [("gh", "/usr/bin")])

    def test_the_windows_name_is_found_under_wsl(self):
        def which(name, path):
            return "/mnt/c/Program Files/GitHub CLI/gh.exe" if name == "gh.exe" else None

        self.assertEqual(ci_status.find_gh(which, "/mnt/c/Program Files/GitHub CLI"),
                         "/mnt/c/Program Files/GitHub CLI/gh.exe")

    def test_neither_name_is_unavailable(self):
        self.assertIsNone(ci_status.find_gh(lambda name, path: None, ""))
        with self.assertRaises(ci_status.Unavailable):
            original = ci_status.find_gh
            ci_status.find_gh = lambda: None
            try:
                ci_status.run_gh(["run", "list"])
            finally:
                ci_status.find_gh = original


if __name__ == "__main__":
    unittest.main()
