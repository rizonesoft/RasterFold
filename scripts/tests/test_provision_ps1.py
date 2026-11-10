"""tools/provision.ps1: the Basedpyright leg enforces the exact pin.

The completion review for D00 T05 s1 caught a substring version check that
accepted 1.40.20 and 1.40.2rc1 against the 1.40.2 pin, leaving verification
falsely green. Test-BasedpyrightPin parses the reported version and compares
it exactly; these cases run the real function text from provision.ps1.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def extract_function(path: Path, name: str) -> str:
    lines = path.read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith(f"function {name} {{"))
    depth = 0
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if i > start and depth == 0:
            return "\n".join(lines[start:i + 1])
    raise ValueError(f"function {name} never closes")


@unittest.skipUnless(shutil.which("pwsh"), "pwsh not available")
class BasedpyrightPinTests(unittest.TestCase):
    def check(self, output: str, pin: str) -> bool:
        func = extract_function(ROOT / "tools" / "provision.ps1", "Test-BasedpyrightPin")
        script = f"{func}\nif (Test-BasedpyrightPin '{output}' '{pin}') {{ 'yes' }} else {{ 'no' }}\n"
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / "probe.ps1"
            probe.write_text(script, encoding="utf-8")
            proc = subprocess.run(["pwsh", "-NoProfile", "-File", str(probe)],
                                  capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout.strip() == "yes"

    def test_exact_pin_accepted(self):
        self.assertTrue(self.check("basedpyright 1.40.2\nbased on pyright 1.1.414", "1.40.2"))

    def test_prefix_collision_rejected(self):
        self.assertFalse(self.check("basedpyright 1.40.20", "1.40.2"))

    def test_prerelease_suffix_rejected(self):
        self.assertFalse(self.check("basedpyright 1.40.2rc1", "1.40.2"))

    def test_other_version_rejected(self):
        self.assertFalse(self.check("basedpyright 1.40.1", "1.40.2"))


if __name__ == "__main__":
    unittest.main()
