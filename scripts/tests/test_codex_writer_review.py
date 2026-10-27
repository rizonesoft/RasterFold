"""Codex writer routing, Muse's read-only invocation, and review receipts."""
import json
import importlib.util
from pathlib import Path
import subprocess
import sys
import textwrap
import unittest
import unittest.mock

from test_review_dispatch import Harness, SCHEDULED_PIN, write_launcher


PIN = SCHEDULED_PIN.replace(
    "[routes]",
    '[reviewers.spark]\ncli = "muse"\nmodel = "muse-spark-1.3"\neffort = "high"\n\n[routes]',
).replace('agy = ["sol"]', 'agy = ["sol"]\ncodex = ["spark"]')

FAKE_MUSE = textwrap.dedent('''\
    import json, os, sys, time
    args = sys.argv[2:]
    prompt = open(args[args.index("--prompt-file") + 1], encoding="utf-8").read()
    with open(os.environ["FAKE_LOG"], "a", encoding="utf-8") as log:
        log.write(json.dumps({"argv": args, "prompt": prompt}) + "\\n")
    mode = os.environ.get("FAKE_MUSE", "ok")
    if mode == "fail":
        sys.exit(3)
    if mode == "sleep":
        time.sleep(30)
    if mode == "empty":
        sys.exit(0)
    text = os.environ.get("FAKE_SAY", "HIGHEST: none\\nOVERALL: approve")
    if mode == "invalid-json":
        print("not a Muse event")
        sys.exit(0)
    if mode == "nested-json":
        print("[" * 12000 + "0" + "]" * 12000)
        sys.exit(0)
    model = "different-model" if mode == "wrongmodel" else "muse-spark-1.3"
    print(json.dumps({"payload_type": "run.model.configured",
                      "payload": {"model_id": model, "provider_id": "meta"}}))
    print(json.dumps({"payload_type": "tool.result", "payload": {"text": "progress, not a review"}}))
    print(json.dumps({"payload_type": "run.terminal.completed",
                      "payload": {"kind": "run_terminal", "terminal": "completed", "text": text}}))
''')


class MuseRoute(Harness):
    def setUp(self):
        super().setUp()
        (self.repo / ".conclave/reviewers.toml").write_text(PIN, encoding="utf-8")
        fake = self.repo / "fake_muse.py"
        fake.write_text(FAKE_MUSE, encoding="utf-8")
        self.muse = write_launcher(self.bin, "muse", fake)
        self.findings = self.repo / "docs/reviews/codex-review.md"

    def test_codex_routes_to_spark_with_the_shared_schedule(self):
        for n, effort in enumerate(("high", "medium", "low", "low", "low"), 1):
            with self.subTest(round=n):
                result, summary = self.dispatch("--writer", "codex", "--prompt", str(self.prompt),
                                                "--record", str(self.findings))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((summary["reviewer"], summary["model"], summary["cli"], summary["effort"]),
                                 ("spark", "muse-spark-1.3", "muse", effort))
                call = self.calls()[-1]
                self.assertEqual(call["argv"][call["argv"].index("--reasoning-effort") + 1], effort)
                for flag in ("--disable-write", "--disable-shell", "--disable-web-tools", "--disable-reminders"):
                    self.assertIn(flag, call["argv"])
                self.assertIn("--json", call["argv"])
                self.assertEqual(call["argv"][call["argv"].index("--provider") + 1], "meta")
                self.assertEqual(summary["review"], "HIGHEST: none\nOVERALL: approve")
                self.assertIn("## Lenses for this pass", call["prompt"])
                self.assertNotIn(self.prompt.read_text().strip(), call["argv"])
        rounds = self.module().read_rounds(self.findings.read_text(encoding="utf-8"))
        self.assertEqual(len([r for r in rounds if self.module().returned_review(r)]), 5)

    def test_muse_plan_and_completion_are_medium_and_recorded(self):
        for kind, reply in (("plan", "HIGHEST: none\nPLAN: complete\nOVERALL: approve"),
                            ("completion", "COMPLETION: complete")):
            with self.subTest(kind=kind):
                result, summary = self.dispatch("--writer", "codex", "--kind", kind, "--candidate", "a" * 40,
                                                "--prompt", str(self.prompt), "--record", str(self.findings),
                                                muse="say", say=reply)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((summary["effort"], summary["reviewer"]), ("medium", "spark"))
                self.assertEqual(self.calls()[-1]["argv"][self.calls()[-1]["argv"].index("--reasoning-effort") + 1],
                                 "medium")
        findings = self.module().read_findings(self.findings.read_text(encoding="utf-8"))
        self.assertTrue(self.module().returned_one_pass(findings["plans"][0], "plan"))
        self.assertTrue(self.module().returned_one_pass(findings["completions"][0], "completion"))

    def test_muse_failure_never_falls_back(self):
        for mode, reason in (("fail", "exited 3"), ("empty", "returned no review"), ("sleep", "timed out")):
            with self.subTest(mode=mode):
                self.log.unlink(missing_ok=True)
                result, summary = self.dispatch("--writer", "codex", "--prompt", str(self.prompt),
                                                "--timeout", "1", muse=mode)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(summary["skipped"], [{"reviewer": "spark", "reason": reason}])
                self.assertEqual(len(self.calls()), 1)

    def test_malformed_muse_events_and_missing_verdict_are_no_review(self):
        for mode, reply in (("invalid-json", ""), ("nested-json", ""),
                            ("wrongmodel", "HIGHEST: none\nOVERALL: approve"),
                            ("say", "No final verdict")):
            with self.subTest(mode=mode):
                result, summary = self.dispatch("--writer", "codex", "--prompt", str(self.prompt),
                                                muse=mode, say=reply)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIsNone(summary["reviewer"])
                self.assertIsNone(summary["review"])

    def test_memory_error_while_reading_muse_events_is_no_review(self):
        module = self.module()
        with unittest.mock.patch.object(module.Path, "read_text", side_effect=MemoryError):
            answer, problem = module.muse_response(self.prompt, "muse-spark-1.3")
        self.assertIsNone(answer)
        self.assertEqual(problem, "returned malformed Muse events")

    def test_same_family_routes_are_refused(self):
        module = self.module()
        for pin in (PIN.replace('muse = ["sol"]', 'muse = ["spark"]'),
                    PIN.replace('codex = ["spark"]', 'codex = ["sol"]')):
            with self.subTest(pin=pin), self.assertRaisesRegex(module.Refused, "writer's own family"):
                module.load_pin(data=pin.encode())

    def test_stamp_replays_the_committed_codex_route_and_schedule(self):
        subprocess.run(["git", "-C", str(self.repo), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "add", ".conclave/reviewers.toml"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "-c", "user.name=Test", "-c", "user.email=test@example.com",
                        "commit", "-q", "-m", "pin"], check=True)
        head = subprocess.check_output(["git", "-C", str(self.repo), "rev-parse", "HEAD"], text=True).strip()
        for kind, reply in (("plan", "HIGHEST: none\nPLAN: complete\nOVERALL: approve"),
                            ("completion", "COMPLETION: complete")):
            result, _ = self.dispatch("--writer", "codex", "--kind", kind, "--candidate", head,
                                      "--prompt", str(self.prompt), "--record", str(self.findings),
                                      muse="say", say=reply)
            self.assertEqual(result.returncode, 0, result.stderr)
        result, _ = self.dispatch("--writer", "codex", "--prompt", str(self.prompt), "--record", str(self.findings))
        self.assertEqual(result.returncode, 0, result.stderr)
        rel = self.findings.relative_to(self.repo).as_posix()
        pin = self.module().load_pin(self.repo / ".conclave/reviewers.toml")
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
