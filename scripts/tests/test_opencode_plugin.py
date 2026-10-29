"""The OpenCode campaign plugin's logic, run by node against a fake context (scripts/tests/opencode_plugin_harness.mjs)."""
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / ".opencode/plugins/rasterfold-campaign/index.js"
HARNESS = ROOT / "scripts/tests/opencode_plugin_harness.mjs"
CASES = (
    "a turn end the guard blocks queues its reason",
    "an interrupted turn is passed on as interrupted and queues nothing",
    "no decision, a malformed decision or a failed adapter queues nothing",
    "a delayed block is dropped when another execution started, was interrupted, or lost the event stream",
    "a heartbeat and a block for the same idle session queue one prompt",
    "a heartbeat fires for an idle session whoever owns the guard and skips a busy or racing one",
    "a session unseen since load is idle only when its idle time covers its last update",
    "a plugin loaded mid-execution queues nothing until the execution ends",
    "a session outside the checkout is never asked about even after moving directories",
    "malformed events and due entries are ignored",
    "after cleanup nothing is queued and a reloaded instance is the only poller",
)


class OpenCodePlugin(unittest.TestCase):
    def test_every_plugin_case_passes_under_node(self):
        # Node.js runs only the plugin's own code (D00 T01 §10 records the dependency); it is required, not skipped.
        node = shutil.which("node")
        self.assertIsNotNone(node, "node (18 or later) is required to test the OpenCode campaign plugin")
        result = subprocess.run([node, str(HARNESS), str(PLUGIN)], capture_output=True, text=True, timeout=120,
                                cwd=ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        passed = [line[3:] for line in result.stdout.splitlines() if line.startswith("ok ")]
        self.assertEqual(passed, list(CASES))


if __name__ == "__main__":
    unittest.main()
