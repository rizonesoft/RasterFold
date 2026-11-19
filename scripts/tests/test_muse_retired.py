"""Muse Code's retirement: the current pin refuses it, nothing runs it, and historical Codex-to-Muse receipts replay."""
import json
import importlib.util
from pathlib import Path
import subprocess
import sys
import textwrap
import unittest

from test_review_dispatch import Harness, SCHEDULED_PIN, write_launcher


PIN = SCHEDULED_PIN.replace(
    "[routes]",
    '[reviewers.spark]\ncli = "muse"\nmodel = "muse-spark-1.3"\neffort = "high"\n\n[routes]',
).replace('agy = ["sol"]', 'agy = ["sol"]\ncodex = ["spark"]')

# A stand-in `muse` on PATH that logs any call: Muse Code is retired (owner decision 2026-11-19), so no test may see one.
FAKE_MUSE = textwrap.dedent('''\
    import json, os, sys
    with open(os.environ["FAKE_LOG"], "a", encoding="utf-8") as log:
        log.write(json.dumps({"cli": "muse", "argv": sys.argv[2:]}) + "\\n")
    print("HIGHEST: none\\nOVERALL: approve")
''')

SPARK = {"writer": "codex", "reviewer": "spark", "cli": "muse", "model": "muse-spark-1.3", "skipped": []}


class MuseRetired(Harness):
    def setUp(self):
        super().setUp()
        (self.repo / ".conclave/reviewers.toml").write_text(PIN, encoding="utf-8")
        fake = self.repo / "fake_muse.py"
        fake.write_text(FAKE_MUSE, encoding="utf-8")
        self.muse = write_launcher(self.bin, "muse", fake)
        self.findings = self.repo / "docs/reviews/codex-review.md"

    def test_the_current_pin_may_not_name_muse(self):
        module = self.module()
        message = "reviewer 'spark' names the retired muse CLI; only a historical pin may name it"
        with self.assertRaises(module.Refused) as refused:
            module.load_pin(self.repo / ".conclave/reviewers.toml")
        self.assertEqual(str(refused.exception), message)
        result, _ = self.dispatch("--check-pin", "--json")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"ok": False, "error": message})
        for writer in ("codex", "claude"):
            with self.subTest(writer=writer):
                result, _ = self.dispatch("--writer", writer, "--prompt", str(self.prompt))
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
        self.assertEqual(self.calls(), [])

    def test_check_pin_lists_only_the_clis_it_runs(self):
        (self.repo / ".conclave/reviewers.toml").write_text(SCHEDULED_PIN, encoding="utf-8")
        result, _ = self.dispatch("--check-pin", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(json.loads(result.stdout)["clis"]), ["agy", "codex"])

    def commit_pin(self, text, message):
        (self.repo / ".conclave/reviewers.toml").write_text(text, encoding="utf-8")
        if not (self.repo / ".git").exists():
            subprocess.run(["git", "-C", str(self.repo), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "add", ".conclave/reviewers.toml"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "-c", "user.name=Test", "-c", "user.email=test@example.com",
                        "commit", "-q", "-m", message], check=True)
        return subprocess.check_output(["git", "-C", str(self.repo), "rev-parse", "HEAD"], text=True).strip()

    def test_a_section_held_to_a_historical_muse_route_records_an_outage_and_never_runs_muse(self):
        # The section's plan review ran under the committed pin that routed Codex to Muse Spark; the pin today is
        # valid and names no Muse reviewer. The dispatcher follows the section's pin and records a retirement outage.
        start = self.commit_pin(PIN, "the section's plan review ran under the Muse route")
        self.findings.parent.mkdir(parents=True, exist_ok=True)
        self.module().record_completion(self.findings, {**SPARK, "effort": "medium", "lenses": ["plan"],
                                                        "review": "HIGHEST: none\nPLAN: complete\nOVERALL: approve"},
                                        "2026-10-28T00:00:00Z", start, "plan")
        self.commit_pin(SCHEDULED_PIN, "the pin today names no Muse reviewer")
        result, summary = self.dispatch("--writer", "codex", "--prompt", str(self.prompt),
                                        "--record", str(self.findings))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIsNone(summary["reviewer"])
        self.assertEqual(summary["skipped"], [{"reviewer": "spark", "reason": "muse is retired and never runs"}])
        recorded = self.findings.read_text(encoding="utf-8")
        self.assertIn("## Round 1: none", recorded)
        self.assertIn("- Skipped: spark (muse is retired and never runs)", recorded)
        self.assertEqual(self.calls(), [], "a retired Muse reviewer was run")

    def test_the_refusal_names_the_clis_each_mode_accepts(self):
        module = self.module()
        bad = PIN.replace('cli = "muse"', 'cli = "claude"')
        for historical, accepted in ((False, "codex, agy"), (True, "codex, agy, muse")):
            with self.subTest(historical=historical), self.assertRaises(module.Refused) as refused:
                module.load_pin(data=bad.encode(), historical=historical)
            self.assertEqual(str(refused.exception), f"reviewer 'spark' needs cli = one of {accepted}")

    def test_same_family_routes_are_refused(self):
        module = self.module()
        for pin in (PIN.replace('muse = ["sol"]', 'muse = ["spark"]'),
                    PIN.replace('codex = ["spark"]', 'codex = ["sol"]')):
            with self.subTest(pin=pin), self.assertRaisesRegex(module.Refused, "writer's own family"):
                module.load_pin(data=pin.encode(), historical=True)

    def test_stamp_replays_recorded_muse_reviews_under_the_committed_pin(self):
        head = self.commit_pin(PIN, "pin")
        module = self.module()
        pin = module.load_pin(data=PIN.encode(), historical=True)
        self.findings.parent.mkdir(parents=True, exist_ok=True)
        module.record_completion(self.findings, {**SPARK, "effort": "medium", "lenses": ["plan"],
                                                 "review": "HIGHEST: none\nPLAN: complete\nOVERALL: approve"},
                                 "2026-10-28T00:00:00Z", head, "plan")
        module.record_completion(self.findings, {**SPARK, "effort": "medium", "lenses": [],
                                                 "review": "COMPLETION: complete"},
                                 "2026-10-28T00:01:00Z", head, "completion")
        module.record(self.findings, {**SPARK, "effort": "high",
                                      "lenses": pin["schedule"]["rounds"][0]["lenses"],
                                      "review": "HIGHEST: none\nOVERALL: approve"}, "2026-10-28T00:02:00Z", head)
        rel = self.findings.relative_to(self.repo).as_posix()
        data = {"implementation_sha": head, "schedule": pin["schedule"],
                "plan": {"report": rel, "candidate": head},
                "completion": {"report": rel, "candidate": head},
                "reviews": [{"round": 1, "report": rel, "verdict": "clean"}]}
        graph_spec = importlib.util.spec_from_file_location("codex_writer_graph",
                                                           Path(__file__).resolve().parents[1] / "todo-graph.py")
        graph = importlib.util.module_from_spec(graph_spec)
        sys.modules[graph_spec.name] = graph
        try:
            graph_spec.loader.exec_module(graph)
        finally:
            del sys.modules[graph_spec.name]
        graph.schedule_ok(self.repo, data, "D00 T01 §6", False)
        data["schedule"] = {**data["schedule"], "completion": {"effort": "high"}}
        with self.assertRaisesRegex(graph.Refused, "schedule expects high"):
            graph.schedule_ok(self.repo, data, "D00 T01 §6", False)
        self.assertEqual(self.calls(), [])

    def test_legacy_panel_provenance_recognizes_terra_without_retired_scripts(self):
        graph_spec = importlib.util.spec_from_file_location("codex_legacy_graph",
                                                           Path(__file__).resolve().parents[1] / "todo-graph.py")
        graph = importlib.util.module_from_spec(graph_spec)
        sys.modules[graph_spec.name] = graph
        try:
            graph_spec.loader.exec_module(graph)
        finally:
            del sys.modules[graph_spec.name]
        self.assertTrue(graph.codex_implementer("Codex (gpt-6.1-terra)"))
        self.assertFalse(graph.codex_implementer("Codex (claude-opus-5-5)"))
        self.assertEqual(graph.panel_fallback("## GPT panel\n", "2026-10-27",
                                              "Codex (gpt-6.1-terra)"), (True, False))


if __name__ == "__main__":
    unittest.main()
