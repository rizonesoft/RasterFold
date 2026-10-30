"""scripts/review-dispatch.py: the pinned routes, read-only invocation, refusals and records, with a fake Codex CLI."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
import tomllib
import unittest
import unittest.mock

SCRIPTS = Path(__file__).resolve().parents[1]
REPO_PIN = SCRIPTS.parent / ".conclave" / "reviewers.toml"

# A historical three-writer pin stays supported. The Codex-to-Muse route is exercised separately.
FIXTURE_PIN = textwrap.dedent('''    schema_version = 1

    [reviewers.sol]
    cli = "codex"
    model = "gpt-6.1-sol"
    effort = "xhigh"

    [routes]
    claude = ["sol"]
    muse = ["sol"]
    agy = ["sol"]
''')

# A fake Codex CLI. Its launcher is a shell script on Linux and a .cmd file on Windows, so the suite runs natively on both.
FAKE_CLI = textwrap.dedent('''\
    import json, os, subprocess, sys, time
    name, args = sys.argv[1], sys.argv[2:]
    mode = os.environ.get("FAKE_" + name.upper(), "ok")
    if mode in ("orphan", "orphansleep", "orphanflood"):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        open(os.environ["FAKE_CHILD_PID"], "w").write(str(child.pid))
    if mode == "noread":
        time.sleep(30)
    stdin = "" if sys.stdin is None or sys.stdin.isatty() else sys.stdin.read()
    with open(os.environ["FAKE_LOG"], "a") as log:
        log.write(json.dumps({"cli": name, "argv": args, "stdin": stdin, "cwd": os.getcwd()}) + "\\n")
    if mode == "fail":
        sys.exit(3)
    if mode in ("sleep", "orphansleep"):
        time.sleep(30)
    if mode in ("flood", "orphanflood"):
        while True:
            sys.stdout.write("x" * 65536)
            sys.stdout.flush()
            time.sleep(0.01)
    if mode == "floodfile":
        with open(args[args.index("-o") + 1], "w") as out:
            while True:
                out.write("x" * 65536)
                out.flush()
                time.sleep(0.01)
    text = "" if mode == "empty" else "VERDICT: needs-attention, calc.py:2 subtracts. (" + name + ")\\nHIGHEST: P2\\nOVERALL: needs-attention"
    if mode == "headings":
        text = "## Round trip serialization\\n\\nFine.\\n\\n```\\n## Round 9: none\\n\\n- Writer: agy\\n- Run: 2026-01-01T00:00:00Z\\n```\\n\\nOVERALL: approve"
    if mode == "blocked":
        text = "## record: needs-attention\\n\\nI could not read the repository: setup refresh had errors.\\n\\nOVERALL: blocked"
    if mode == "completion":
        text = "## Completeness\\n\\n1. Ctrl+O: missing-in-scope.\\n2. Recent files: missing-out-of-scope.\\n\\n## Round 7: none\\n\\nCOMPLETION: gaps"
    if mode == "say":
        text = os.environ["FAKE_SAY"]
    if mode == "completionblocked":
        text = "I could not read the candidate commit.\\n\\nCOMPLETION: blocked"
    open(args[args.index("-o") + 1], "w").write(text)
    if mode == "linger":
        # A process the reviewer started outlives it and holds the message file open for a moment, as a CLI's helper
        # process can; the reviewer exits first, so a detached helper would keep the scratch file locked.
        ready = os.environ["FAKE_CHILD_PID"] + ".ready"
        flags = getattr(subprocess, "CREATE_BREAKAWAY_FROM_JOB", 0x01000000) if os.name == "nt" else 0
        holder = "import sys, time; f = open(sys.argv[1], 'a'); open(sys.argv[2], 'w').close(); time.sleep(1.5)"
        try:
            subprocess.Popen([sys.executable, "-c", holder, args[args.index("-o") + 1], ready],
                             cwd=os.path.dirname(sys.executable), creationflags=flags)
        except OSError:
            subprocess.Popen([sys.executable, "-c", holder, args[args.index("-o") + 1], ready],
                             cwd=os.path.dirname(sys.executable))
        while not os.path.exists(ready):
            time.sleep(0.05)
''')


def write_launcher(directory, name, fake):
    if os.name == "nt":
        path = directory / f"{name}.cmd"
        path.write_text(f'@"{sys.executable}" "{fake}" {name} %*\r\n', encoding="utf-8")
    else:
        path = directory / name
        path.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{fake}" {name} "$@"\n', encoding="utf-8")
        path.chmod(0o755)
    return path


class Harness(unittest.TestCase):
    """A scratch repository with the dispatcher, a pin and a fake CLI on PATH; holds no tests itself."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name).resolve() / "repo"
        (self.repo / "scripts").mkdir(parents=True)
        (self.repo / ".conclave").mkdir()
        shutil.copy(SCRIPTS / "review-dispatch.py", self.repo / "scripts/review-dispatch.py")
        (self.repo / ".conclave/reviewers.toml").write_text(FIXTURE_PIN, encoding="utf-8")
        self.bin = Path(self._tmp.name) / "bin"
        self.bin.mkdir()
        fake = Path(self._tmp.name) / "fake_cli.py"
        fake.write_text(FAKE_CLI, encoding="utf-8")
        self.codex = write_launcher(self.bin, "codex", fake)
        self.log = Path(self._tmp.name) / "calls.jsonl"
        self.scratch = Path(self._tmp.name) / "scratch"
        self.scratch.mkdir()
        self.prompt = self.repo / "prompt.md"
        self.prompt.write_text("Review calc.py. Do not modify any file.\n", encoding="utf-8")

    def tearDown(self):
        self._tmp.cleanup()

    def dispatch(self, *args, cwd=None, **modes):
        system_root = {"SYSTEMROOT": os.environ["SYSTEMROOT"]} if os.name == "nt" else {}
        env = {"PATH": f"{self.bin}{os.pathsep}{Path(sys.executable).parent}", "FAKE_LOG": str(self.log),
               "FAKE_CHILD_PID": str(Path(self._tmp.name) / "child.pid"), **system_root,
               **{var: str(self.scratch) for var in ("TMP", "TEMP", "TMPDIR")},
               **{f"FAKE_{k.upper()}": v for k, v in modes.items()}}
        result = subprocess.run([sys.executable, "-B", str(self.repo / "scripts/review-dispatch.py"), *args],
                                capture_output=True, text=True, env=env, timeout=60, cwd=cwd)
        summary = json.loads(result.stdout) if result.stdout.strip().startswith("{") else None
        return result, summary

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def module(self):
        spec = importlib.util.spec_from_file_location("dispatch_under_test", self.repo / "scripts/review-dispatch.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


class Dispatch(Harness):
    RESULT = {"writer": "agy", "reviewer": "sol", "model": "gpt-6.1-sol", "cli": "codex", "effort": "xhigh",
              "skipped": []}

    def test_sol_reviews_agy_work_read_only(self):
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((summary["reviewer"], summary["model"], summary["skipped"]), ("sol", "gpt-6.1-sol", []))
        self.assertIn("(codex)", summary["review"])
        (call,) = self.calls()
        self.assertEqual(call["argv"][:3], ["exec", "-m", "gpt-6.1-sol"])
        i = call["argv"].index("--sandbox")
        self.assertEqual(call["argv"][i:i + 2], ["--sandbox", "read-only"])
        for flag in ("--ephemeral", "model_reasoning_effort=xhigh"):
            self.assertIn(flag, call["argv"])
        self.assertEqual(call["argv"][-1], "-", "the prompt goes on stdin, never on the command line")
        self.assertEqual(call["stdin"], self.prompt.read_text())
        self.assertEqual(Path(call["cwd"]).resolve(), self.repo.resolve())

    def test_one_reviewer_and_no_fallback(self):
        # Owner decision 2026-10-24: when the reviewer returns no review, nothing else runs and the round is not an approval.
        for mode, reason in (("fail", "exited 3"), ("empty", "returned no review")):
            with self.subTest(mode):
                self.log.unlink(missing_ok=True)
                result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), codex=mode)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIsNone(summary["reviewer"])
                self.assertIsNone(summary["review"])
                self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": reason}])
                self.assertEqual([c["cli"] for c in self.calls()], ["codex"])

    def test_a_reviewer_that_could_not_reach_the_candidate_returned_no_review(self):
        # A reply ending OVERALL: blocked says the reviewer never saw the work (a sandbox that cannot read the
        # repository): it is not a review.
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), codex="blocked")
        self.assertEqual(result.returncode, 1)
        self.assertIsNone(summary["review"])
        self.assertTrue(summary["skipped"][0]["reason"].startswith("could not review: "), summary["skipped"])
        self.assertIn("setup refresh had errors", summary["skipped"][0]["reason"])

    def test_a_missing_cli_is_no_review(self):
        self.codex.unlink()
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 1)
        self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": "codex is not installed"}])

    def test_a_timeout_is_no_review(self):
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--timeout", "2",
                                        codex="sleep")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": "timed out"}])

    def test_no_review_is_never_an_approval(self):
        record = self.repo / "docs/reviews/round.md"
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record),
                                        codex="fail")
        self.assertEqual(result.returncode, 1)
        self.assertIsNone(summary["reviewer"])
        self.assertIn("not an approval", record.read_text())

    def test_unrouted_and_unknown_writers_are_refused(self):
        # Historical pins do not acquire a route just because the current dispatcher knows another writer.
        for unrouted in ("codex", "opencode", "copilot"):
            result, _ = self.dispatch("--writer", unrouted, "--prompt", str(self.prompt))
            self.assertEqual(result.returncode, 2)
            self.assertIn(f"no route for writer '{unrouted}'", result.stderr)
        result, _ = self.dispatch("--writer", "gemini", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid choice", result.stderr)
        self.assertEqual(self.calls(), [])
        for writer in ("claude", "muse", "agy"):
            with self.subTest(writer):
                result, summary = self.dispatch("--writer", writer, "--prompt", str(self.prompt))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((summary["writer"], summary["reviewer"]), (writer, "sol"))
        self.assertEqual(len(self.calls()), 3)

    def test_an_opencode_writer_dispatches_through_its_own_route(self):
        # Owner decision 2026-10-29: OpenCode (an Anthropic model) writes, and its route is the pin's, never claude's.
        (self.repo / ".conclave/reviewers.toml").write_text(FIXTURE_PIN + 'opencode = ["sol"]\n', encoding="utf-8")
        result, summary = self.dispatch("--writer", "opencode", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((summary["writer"], summary["reviewer"]), ("opencode", "sol"))
        self.assertEqual(len(self.calls()), 1)

    def test_a_copilot_writer_dispatches_through_its_own_route(self):
        # Owner decision 2026-10-30: Copilot CLI (pinned to an Anthropic model) writes, and its route is the pin's.
        (self.repo / ".conclave/reviewers.toml").write_text(FIXTURE_PIN + 'copilot = ["sol"]\n', encoding="utf-8")
        result, summary = self.dispatch("--writer", "copilot", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((summary["writer"], summary["reviewer"]), ("copilot", "sol"))
        self.assertEqual(len(self.calls()), 1)

    def test_rounds_are_numbered_and_appended(self):
        record = self.repo / "docs/reviews/round.md"
        record.parent.mkdir(parents=True)
        record.write_text("# Findings\n", encoding="utf-8")
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record))
        _, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record),
                                   codex="fail")
        text = record.read_text()
        self.assertEqual(summary["round"], 2)
        self.assertTrue(text.startswith("# Findings\n"))
        self.assertIn("## Round 1: sol (`gpt-6.1-sol` via `codex`)", text)
        self.assertIn("## Round 2: none", text)
        self.assertIn("- Skipped: sol (exited 3)", text)

    def check_pin(self, *args):
        result, _ = self.dispatch("--check-pin", *args)
        return result

    def test_check_pin_reports_a_valid_pin(self):
        result = self.check_pin("--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        module = self.module()
        pin = module.load_pin(self.repo / ".conclave/reviewers.toml")
        self.assertEqual((report["ok"], report["schema_version"]), (True, 1))
        self.assertEqual((report["reviewers"], report["routes"]), (pin["reviewers"], pin["routes"]))
        self.assertEqual(report["clis"], {"codex": True, "muse": False})
        self.assertEqual(self.calls(), [], "--check-pin ran a reviewer")

    def test_check_pin_reports_a_refused_pin(self):
        module = self.module()
        pin = self.repo / ".conclave/reviewers.toml"
        good = pin.read_text()
        for bad in bad_pins(good) + (good.replace("schema_version = 1", "schema_version = true"), "not toml ["):
            with self.subTest(bad=bad[-40:]):
                self.assertNotEqual(bad, good, "the replacement must match the fixture, or the case tests nothing")
                pin.write_text(bad, encoding="utf-8")
                with self.assertRaises(module.Refused) as refused:
                    module.load_pin(pin)
                result = self.check_pin("--json")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(json.loads(result.stdout), {"ok": False, "error": str(refused.exception)})
        self.assertEqual(self.calls(), [])

    def test_the_removed_shapes_are_refused_with_their_reason(self):
        # The strict loader names what is wrong: another reviewer CLI, a fallback, a route for a non-writer.
        module, pin, good = self.module(), self.repo / ".conclave/reviewers.toml", FIXTURE_PIN
        mini = '\n[reviewers.mini]\ncli = "codex"\nmodel = "gpt-6-mini"\neffort = "low"\n'
        cases = {good.replace('cli = "codex"', 'cli = "agy"'): "reviewer 'sol' needs cli = one of codex, muse",
                 good.replace('cli = "codex"', 'cli = "claude"'): "reviewer 'sol' needs cli = one of codex, muse",
                 good.replace('agy = ["sol"]', 'agy = ["sol", "mini"]') + mini:
                     "route 'agy' must list exactly one reviewer, with no fallback",
                 good.replace('agy = ["sol"]', 'agy = []'):
                     "route 'agy' must list exactly one reviewer, with no fallback",
                 good + 'codex = ["sol"]\n': "route 'codex' names sol, of the writer's own family; a review comes from a different family",
                 good + 'gemini = ["sol"]\n': "[routes] may only name the writers claude, muse, agy, codex, opencode, copilot",
                 good.replace('effort = "xhigh"', 'effort = "xhigh"\nsandbox = "workspace-write"'):
                     "reviewer 'sol' has unknown keys: sandbox"}
        for text, message in cases.items():
            with self.subTest(message=message, text=text[-30:]):
                pin.write_text(text, encoding="utf-8")
                with self.assertRaises(module.Refused) as refused:
                    module.load_pin(pin)
                self.assertEqual(str(refused.exception), message)

    def test_a_reviewer_of_the_writers_own_family_is_refused(self):
        # Even a historical pin is refused when its reviewer is changed to the writer's family.
        module = self.module()
        with unittest.mock.patch.dict(module.WRITER_FAMILY, {"agy": "openai"}):
            with self.assertRaises(module.Refused) as refused:
                module.load_pin(self.repo / ".conclave/reviewers.toml")
        self.assertEqual(str(refused.exception),
                         "route 'agy' names sol, of the writer's own family; a review comes from a different family")
        module.load_pin(self.repo / ".conclave/reviewers.toml")

    def test_a_pin_that_cannot_be_read_is_refused_not_a_crash(self):
        pin = self.repo / ".conclave/reviewers.toml"
        cases = {"directory": lambda: (pin.unlink(), pin.mkdir()),
                 "not UTF-8": lambda: pin.write_bytes(b"schema_version = 1\n# \xff\n"),
                 "nested too deep": lambda: pin.write_text("x = " + "[" * 5000 + "0" + "]" * 5000 + "\n"),
                 "integer too long": lambda: pin.write_text("schema_version = " + "9" * 5000 + "\n")}
        for name, make in cases.items():
            with self.subTest(name):
                if pin.is_dir():
                    pin.rmdir()
                make()
                result = self.check_pin("--json")
                self.assertEqual(result.returncode, 2, result.stderr)
                report = json.loads(result.stdout)
                self.assertFalse(report["ok"])
                self.assertTrue(report["error"].startswith("unreadable reviewer pin"), report["error"])
                review, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
                self.assertEqual(review.returncode, 2, review.stderr)
                self.assertIn("review-dispatch refused: unreadable reviewer pin", review.stderr)
        self.assertEqual(self.calls(), [])

    def test_check_pin_reports_a_missing_pin(self):
        (self.repo / ".conclave/reviewers.toml").unlink()
        result = self.check_pin("--json")
        self.assertEqual(result.returncode, 2)
        self.assertTrue(json.loads(result.stdout)["error"].startswith("no reviewer pin at"), result.stdout)

    def test_check_pin_without_json_prints_one_line(self):
        result = self.check_pin()
        self.assertEqual((result.returncode, result.stdout), (0, "ok\n"))
        pin = self.repo / ".conclave/reviewers.toml"
        pin.write_text(pin.read_text().replace('effort = "xhigh"', 'effort = "extreme"'), encoding="utf-8")
        result = self.check_pin()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "refused: reviewer 'sol' needs effort = one of low, medium, high, xhigh\n")

    def test_check_pin_needs_no_writer_or_prompt(self):
        self.assertEqual(self.check_pin("--json").returncode, 0)
        result, _ = self.dispatch("--writer", "agy")
        self.assertEqual(result.returncode, 2, "a review without --prompt must still be refused")
        self.assertIn("--writer and --prompt are required", result.stderr)

    def test_list_rounds_reads_back_what_record_wrote(self):
        # Two rounds written by record itself: one reviewed, one where the reviewer returned no review.
        record = self.repo / "docs/reviews/00-design/D00-T01-s1.md"
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record))
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record), codex="fail")
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        (only,) = listing["records"]
        self.assertEqual(only["path"], "docs/reviews/00-design/D00-T01-s1.md")
        reviewed, skipped = only["rounds"]
        self.assertEqual({k: reviewed[k] for k in ("number", "reviewer", "model", "cli", "writer", "effort", "skipped")},
                         {"number": 1, "reviewer": "sol", "model": "gpt-6.1-sol", "cli": "codex", "writer": "agy",
                          "effort": "xhigh", "skipped": []})
        self.assertRegex(reviewed["run"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertEqual({k: skipped[k] for k in ("number", "reviewer", "model", "cli", "effort", "skipped")},
                         {"number": 2, "reviewer": None, "model": None, "cli": None, "effort": None,
                          "skipped": [{"reviewer": "sol", "reason": "exited 3"}]})
        self.assertNotIn("unreadable", listing)

    def test_list_rounds_still_reads_rounds_recorded_under_earlier_reviewers(self):
        # Findings files from before 2026-10-24 hold rounds by other reviewers, a fallback chain and its Skipped lines.
        old = self.repo / "docs/reviews/old.md"
        old.parent.mkdir(parents=True)
        old.write_text("## Round 1: gemini (`gemini-4-argon-medium` via `agy`)\n\n- Writer: claude\n"
                       "- Run: 2026-10-21T00:00:00Z\n- Skipped: astra (exited 3)\n\nReview.\n", encoding="utf-8")
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        (round_,) = listing["records"][0]["rounds"]
        self.assertEqual((round_["reviewer"], round_["cli"], round_["skipped"]),
                         ("gemini", "agy", [{"reviewer": "astra", "reason": "exited 3"}]))

    def test_list_rounds_with_no_records_and_with_an_unreadable_heading(self):
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual((result.returncode, listing), (0, {"records": []}))
        good = self.repo / "docs/reviews/a.md"
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(good))
        bad = self.repo / "docs/reviews/b.md"
        bad.write_text("# Findings\n\n## Round two: sol\n\n- Writer: agy\n", encoding="utf-8")
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual([r["path"] for r in listing["records"]], ["docs/reviews/a.md"])
        self.assertEqual(listing["unreadable"], ["docs/reviews/b.md"])
        self.assertEqual(len(self.calls()), 1, "listing ran a reviewer")
        result, _ = self.dispatch("--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 2, "a review without --writer must still be refused")

    def test_review_text_that_looks_like_a_round_is_neither_counted_nor_listed(self):
        # A review quoting `## Round ...` lines, one a whole fake round in a fence, round-trips through record.
        record = self.repo / "docs/reviews/r.md"
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record), codex="headings")
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record))
        self.assertEqual(summary["round"], 2, "record counted review text as a round")
        text = record.read_text(encoding="utf-8")
        # Stored byte for byte: no escape inside or outside the fence.
        self.assertIn("\n## Round trip serialization\n", text)
        self.assertIn("```\n## Round 9: none\n", text)
        _, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual([r["number"] for r in listing["records"][0]["rounds"]], [1, 2])
        self.assertNotIn("unreadable", listing)

    def test_a_writer_note_under_a_reserved_heading_is_refused_by_name(self):
        # A writer's own `## Round budget` note made a returned review vanish with a bare "cannot be read back"
        # (2026-10-23): the refusal now names the line, and says the prefixes are reserved for --record.
        module = self.module()
        path = self.repo / "docs/reviews/n.md"
        module.record(path, {**self.RESULT, "review": "Plain."}, "2026-10-23T00:00:00Z")
        path.write_text(path.read_text(encoding="utf-8") + "\n## Round budget (writer)\n\nCap 33.\n", encoding="utf-8")
        with self.assertRaises(module.Unrecorded) as caught:
            module.record(path, {**self.RESULT, "review": "Next."}, "2026-10-23T00:00:01Z")
        message = str(caught.exception)
        self.assertIn("'## Round budget (writer)'", message)
        self.assertIn("reserved for --record", message)
        self.assertRegex(message, r"line [0-9]+:")
        with self.assertRaises(module.Unrecorded) as caught:
            module.record_completion(path, {**self.RESULT, "review": "Gaps."}, "2026-10-23T00:00:02Z", "a" * 40)
        self.assertIn("'## Round budget (writer)'", str(caught.exception))
        # A readable file names nothing.
        self.assertEqual(module.unreadable_heading("# Findings\n\n## Writer note\n\nText.\n"), "")

    def test_the_line_count_envelope_holds_against_other_line_breaks_and_hand_edits(self):
        module = self.module()
        path = self.repo / "docs/reviews/u.md"
        # U+2028 and U+0085 split lines for str.splitlines but not for record: a fake round behind them stays text.
        module.record(path, {**self.RESULT, "review": "Quoted ## Round 9: none\n\n- Writer: codex\n- Run: now\u0085x"},
                      "2026-10-20T00:00:00Z")
        module.record(path, {**self.RESULT, "review": "Plain."}, "2026-10-20T00:00:01Z")
        self.assertEqual([r["number"] for r in module.read_rounds(path.read_text(encoding="utf-8"))], [1, 2])
        # A round recorded before the count (no Lines field) still reads, running to the next heading.
        legacy = ("## Round 1: astra (`gpt-6-astra` via `codex`)\n\n- Writer: claude\n- Run: 2026-10-16T00:00:00Z\n\n"
                  "Review.\n\n")
        self.assertEqual([r["number"] for r in module.read_rounds(legacy)], [1])
        # A hand edit that changes the review's line count is refused, not misread.
        text = path.read_text(encoding="utf-8")
        self.assertIsNone(module.read_rounds(text.replace("- Lines: 1", "- Lines: 3")))
        self.assertIsNone(module.read_rounds(text.replace("Plain.", "Plain.\nAdded by hand.")))

    def test_a_lines_value_that_is_not_ascii_digits_is_unreadable_not_a_crash(self):
        # D06 T01 §8 (1): "²" passes str.isdigit and int() raises; so does a count past Python's digit limit.
        good = self.repo / "docs/reviews/good.md"
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(good))
        recorded = good.read_text(encoding="utf-8")
        for name, value in (("superscript.md", "²"), ("huge.md", "9" * 5000)):
            body = re.sub(r"- Lines: \d+", "- Lines: " + value, recorded)
            self.assertNotEqual(body, recorded, name)
            (self.repo / "docs/reviews" / name).write_text(body, encoding="utf-8")
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual([r["path"] for r in listing["records"]], ["docs/reviews/good.md"])
        self.assertEqual(sorted(listing["unreadable"]), ["docs/reviews/huge.md", "docs/reviews/superscript.md"])

    def test_an_edit_inside_a_review_that_keeps_its_line_count_is_refused(self):
        # D06 T01 §8 (2): the count alone cannot see an edit of the same length; the digest can.
        module, path = self.module(), self.repo / "docs/reviews/d.md"
        module.record(path, {**self.RESULT, "review": "Finding one.\nOVERALL: needs-attention"}, "2026-10-20T00:00:00Z")
        text = path.read_bytes().decode("utf-8")
        self.assertRegex(text, r"\n- Digest: [0-9a-f]{16}\n")
        self.assertEqual(len(module.read_rounds(text)), 1)
        self.assertIsNone(module.read_rounds(text.replace("needs-attention", "approve")))
        # A CRLF checkout of the same file still reads, and its digest still matches.
        self.assertEqual(len(module.read_rounds(text.replace("\n", "\r\n"))), 1)

    def test_a_review_is_stored_byte_for_byte_and_an_append_keeps_it_readable(self):
        # D06 T01 §8 (3): a CRLF pair and a lone CR inside a review are stored as they came, and a second round
        # appended after them does not turn the lone CR into a line break that breaks the first round's count.
        module, path = self.module(), self.repo / "docs/reviews/b.md"
        module.record(path, {**self.RESULT, "review": "A\r\nB\rC\nD"}, "2026-10-20T00:00:00Z")
        self.assertIn(b"\nA\r\nB\rC\nD\n", path.read_bytes())
        module.record(path, {**self.RESULT, "review": "Second."}, "2026-10-20T00:00:01Z")
        raw = path.read_bytes()
        self.assertIn(b"\nA\r\nB\rC\nD\n", raw)
        self.assertEqual([r["number"] for r in module.read_rounds(raw.decode("utf-8"))], [1, 2])

    def test_a_model_or_name_ending_in_a_newline_is_refused(self):
        # D06 T01 §8 (4): `$` in MODEL and NAME accepted a trailing newline, which then broke the round heading.
        pin = self.repo / ".conclave/reviewers.toml"
        good = pin.read_text(encoding="utf-8")
        cases = {'model = "gpt-6.1-sol\\n"': "reviewer 'sol' has a malformed model",
                 '[reviewers."sol\\n"]': "invalid reviewer 'sol\\n'"}
        for edit, message in cases.items():
            with self.subTest(edit):
                old = 'model = "gpt-6.1-sol"' if edit.startswith("model") else "[reviewers.sol]"
                pin.write_text(good.replace(old, edit), encoding="utf-8")
                result = self.check_pin("--json")
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertEqual(json.loads(result.stdout)["error"], message)
                review, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
                self.assertEqual(review.returncode, 2, review.stderr)
        self.assertEqual(self.calls(), [])

    def test_record_refuses_to_append_to_a_findings_file_it_cannot_read(self):
        # D06 T01 §8 (5): numbering an unreadable file by counting headings counted quoted ones; now nothing is
        # appended, the review still reaches stdout, and the exit is 2.
        bad = self.repo / "docs/reviews/bad.md"
        bad.parent.mkdir(parents=True)
        body = "# Findings\n\n## Round two: sol\n\n- Writer: agy\n"
        bad.write_bytes(body.encode("utf-8"))
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(bad))
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(bad.read_bytes(), body.encode("utf-8"), "record appended to a file it cannot read")
        self.assertIn("calc.py:2 subtracts", summary["review"])
        self.assertIsNone(summary["round"])
        self.assertIn("cannot be read back", result.stderr)

    def test_a_cli_in_the_working_directory_is_not_on_path(self):
        # D01 T01 §5: Windows shutil.which looks in the working directory first unless NoDefaultCurrentDirectoryInExePath
        # is set, so a codex committed to the reviewed repository was reported as installed and run as the reviewer.
        # The dispatch() environment never sets that variable, so this runs as a plain Windows shell would.
        self.codex.unlink()
        planted = self.repo / ("codex.cmd" if os.name == "nt" else "codex")
        marker = self.repo / "planted-ran.txt"
        if os.name == "nt":
            planted.write_text(f'@echo planted> "{marker}"\r\n', encoding="utf-8")
        else:
            planted.write_text(f'#!/bin/sh\necho planted > "{marker}"\n', encoding="utf-8")
            planted.chmod(0o755)
        result, _ = self.dispatch("--check-pin", "--json", cwd=self.repo)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(json.loads(result.stdout)["clis"]["codex"], "a codex in the working directory counted as on PATH")
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), cwd=self.repo)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(marker.exists(), "the planted codex in the working directory ran as the reviewer")
        self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": "codex is not installed"}])

    @unittest.skipUnless(os.name == "nt", "drive-relative paths, PATHEXT and extensionless lookups are Windows rules")
    def test_find_cli_on_windows_paths(self):
        module = self.module()
        folder = Path(self._tmp.name) / "clis"
        folder.mkdir()
        (folder / "codex.cmd").write_text("@exit /b 0\r\n", encoding="utf-8")
        drive_relative = str(folder)[2:]  # "R:\x\clis" -> "\x\clis", resolved against the current drive
        with unittest.mock.patch.dict(os.environ, {"PATH": drive_relative, "PATHEXT": ".CMD"}):
            self.assertIsNone(module.find_cli("codex"), "a drive-relative PATH entry was searched")
        with unittest.mock.patch.dict(os.environ, {"PATH": str(folder), "PATHEXT": ""}):
            self.assertEqual(module.find_cli("codex"), str(folder / "codex.CMD"), "an empty PATHEXT found nothing")
        outside = Path(self._tmp.name) / "reviewed"
        outside.mkdir()
        (outside / "codex.cmd").write_text("@exit /b 0\r\n", encoding="utf-8")
        (folder / "codex").mkdir()
        with unittest.mock.patch.dict(os.environ, {"PATH": str(folder), "PATHEXT": "\\..\\..\\reviewed\\codex.cmd"}):
            self.assertEqual(module.find_cli("codex"), str(folder / "codex.CMD"), "a PATHEXT entry led outside PATH")
        (folder / "codex").rmdir()
        (folder / "codex").write_text("#!/bin/sh\n", encoding="utf-8")
        with unittest.mock.patch.dict(os.environ, {"PATH": str(folder), "PATHEXT": ".EXE;.CMD"}):
            self.assertEqual(module.find_cli("codex"), str(folder / "codex.CMD"))
            (folder / "codex.cmd").unlink()
            self.assertEqual(module.find_cli("codex"), str(folder / "codex"), "an extensionless executable was lost")

    def test_a_round_number_past_the_digit_limit_is_unreadable_and_keeps_the_review(self):
        bad = self.repo / "docs/reviews/huge-number.md"
        bad.parent.mkdir(parents=True)
        body = "## Round " + "9" * 5000 + ": none\n\n- Writer: agy\n- Run: 2026-10-20T00:00:00Z\n"
        bad.write_bytes(body.encode("utf-8"))
        self.assertIsNone(self.module().read_rounds(body))
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(bad))
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("calc.py:2 subtracts", summary["review"])
        self.assertEqual(bad.read_bytes(), body.encode("utf-8"))
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual((result.returncode, listing["unreadable"]), (1, ["docs/reviews/huge-number.md"]))

    def test_the_largest_review_record_writes_is_one_its_reader_reads(self):
        module, path = self.module(), self.repo / "docs/reviews/large.md"
        module.record(path, {**self.RESULT, "review": "\n" * module.MAX_OUTPUT_BYTES}, "2026-10-20T00:00:00Z")
        module.record(path, {**self.RESULT, "review": "Second."}, "2026-10-20T00:00:01Z")
        self.assertEqual([r["number"] for r in module.read_rounds(path.read_bytes().decode("utf-8"))], [1, 2])

    @unittest.skipIf(os.name == "nt", "creating a symlink needs a privilege Windows does not grant by default")
    def test_a_symlink_loop_is_reported_without_aborting_the_listing(self):
        good = self.repo / "docs/reviews/good.md"
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(good))
        os.symlink("loop.md", self.repo / "docs/reviews/loop.md")
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual([r["path"] for r in listing["records"]], ["docs/reviews/good.md"])
        self.assertEqual(listing["unreadable"], ["docs/reviews/loop.md"])

    def test_a_malformed_or_unreadable_record_is_named_and_the_rest_still_listed(self):
        good = self.repo / "docs/reviews/good.md"
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(good))
        recorded = good.read_text(encoding="utf-8")
        cases = {
            "empty-effort.md": recorded.replace("- Effort: xhigh", "- Effort:\n- Skipped: x (y)"),
            "two-writers.md": recorded.replace("- Writer: agy", "- Writer: agy\n- Writer: codex"),
            "no-blank.md": recorded.replace("via `codex`)\n\n", "via `codex`)\n"),
        }
        for name, body in cases.items():
            self.assertNotEqual(body, recorded, name)
            (self.repo / "docs/reviews" / name).write_text(body, encoding="utf-8")
        (self.repo / "docs/reviews/not-utf8.md").write_bytes(recorded.replace("agy", "ag\udcff", 1)
                                                              .encode("utf-8", "surrogateescape"))
        (self.repo / "docs/reviews/folder.md").mkdir()  # a path that cannot be read as a file
        result, listing = self.dispatch("--list-rounds", "--json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual([r["path"] for r in listing["records"]], ["docs/reviews/good.md"])
        self.assertEqual(sorted(listing["unreadable"]), sorted(
            f"docs/reviews/{n}" for n in (*cases, "not-utf8.md", "folder.md")))

    def test_an_unsearchable_findings_subdirectory_is_reported_unreadable(self):
        module = self.module()
        good = self.repo / "docs/reviews/00-workspace/good.md"
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(good))
        blocked = self.repo / "docs/reviews/01-compression"
        blocked.mkdir(parents=True)
        real_scandir = os.scandir

        def guarded_scandir(path):
            if Path(path).resolve() == blocked.resolve():
                raise PermissionError(13, "Permission denied", str(path))
            return real_scandir(path)

        with unittest.mock.patch("os.scandir", side_effect=guarded_scandir):
            listing = module.list_rounds(self.repo / "docs/reviews")
            code = module.main(["--list-rounds", "--json", "--records", str(self.repo / "docs/reviews")])
        self.assertEqual(code, 1)
        self.assertEqual([r["path"] for r in listing["records"]], ["docs/reviews/00-workspace/good.md"])
        self.assertEqual(listing["unreadable"], ["docs/reviews/01-compression"])

    def test_a_runaway_reviewer_is_stopped_while_it_runs(self):
        started = time.monotonic()
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--timeout", "40",
                                        codex="flood")
        self.assertLess(time.monotonic() - started, 30, "the cap was only applied at the timeout")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": "returned more than 2000000 bytes"}])

    def test_codex_flooding_its_message_file_is_stopped_while_it_runs(self):
        started = time.monotonic()
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--timeout", "40",
                                        codex="floodfile")
        self.assertLess(time.monotonic() - started, 30, "the cap was only applied at the timeout")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": "returned more than 2000000 bytes"}])

    def test_a_file_still_held_after_the_reviewer_exits_does_not_cost_the_round(self):
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), codex="linger")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(summary["reviewer"], "sol")
        self.assertIn("(codex)", summary["review"])
        self.assertNotIn("left", result.stderr)
        self.assertEqual(list(self.scratch.iterdir()), [], "the reviewer's scratch directory was not removed")

    def test_a_reviewer_that_never_reads_a_large_prompt_still_times_out(self):
        # Larger than any pipe buffer: a blocking write here would hang past the timeout.
        self.prompt.write_text("Review this.\n" + "x" * 1_000_000, encoding="utf-8")
        started = time.monotonic()
        _, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--timeout", "3", codex="noread")
        self.assertLess(time.monotonic() - started, 20)
        self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": "timed out"}])

    def test_a_prompt_with_a_nul_byte_is_refused_before_any_reviewer_runs(self):
        self.prompt.write_text("Review this.\x00", encoding="utf-8")
        result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 2)
        self.assertIn("NUL byte", result.stderr)
        self.assertEqual(self.calls(), [])

    def assert_pid_gone(self, pid):
        for _ in range(50):
            if os.name == "nt":
                out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                                     capture_output=True, text=True).stdout
                if not re.search(rf"\b{pid}\b", out):
                    return
            else:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return
            time.sleep(0.1)
        try:
            os.kill(pid, 9)
        except OSError:
            pass
        self.fail("the reviewer's child process was still running")

    def test_a_reviewers_children_do_not_outlive_it(self):
        _, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), codex="orphan")
        self.assertEqual(summary["reviewer"], "sol")
        pid = int((Path(self._tmp.name) / "child.pid").read_text())
        self.assert_pid_gone(pid)

    def test_a_reviewers_children_do_not_outlive_timeout_or_output_flood(self):
        for mode, extra, expected in (("orphansleep", ("--timeout", "2"), "timed out"),
                                      ("orphanflood", ("--timeout", "40"), "returned more than 2000000 bytes")):
            with self.subTest(mode):
                child_pid = Path(self._tmp.name) / "child.pid"
                child_pid.unlink(missing_ok=True)
                result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), *extra, codex=mode)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": expected}])
                self.assert_pid_gone(int(child_pid.read_text()))
                self.assertEqual(list(self.scratch.iterdir()), [])

    def test_windows_job_containment_operates_and_refuses_when_containment_fails(self):
        module = self.module()
        with tempfile.TemporaryDirectory() as tmp:
            stdout_path = Path(tmp) / "stdout.txt"
            with unittest.mock.patch.object(module.os, "name", "nt"):
                fake_k32 = unittest.mock.MagicMock()
                fake_ntdll = unittest.mock.MagicMock()
                with unittest.mock.patch.object(module, "_win32_apis", return_value=(fake_k32, fake_ntdll)), \
                     unittest.mock.patch.object(module.ctypes, "get_last_error", return_value=5, create=True), \
                     unittest.mock.patch.object(module.ctypes, "WinError",
                                                side_effect=lambda err: OSError(err, f"WinError {err}"), create=True):
                    # 1. CreateJobObjectW fails (returns NULL) before Popen runs.
                    fake_k32.CreateJobObjectW.return_value = 0
                    with unittest.mock.patch.object(module.subprocess, "Popen") as popen_mock:
                        code, reason = module.run(["codex"], "hi", 5, [stdout_path])
                    self.assertIsNone(code)
                    self.assertEqual(reason, "could not start: WinError 5")
                    popen_mock.assert_not_called()

                    # 2. SetInformationJobObject fails: the job handle is closed and Popen never runs.
                    fake_k32.reset_mock()
                    fake_k32.CreateJobObjectW.return_value = 4141
                    fake_k32.SetInformationJobObject.return_value = 0
                    with unittest.mock.patch.object(module.subprocess, "Popen") as popen_mock:
                        code, reason = module.run(["codex"], "hi", 5, [stdout_path])
                    self.assertIsNone(code)
                    self.assertEqual(reason, "could not start: WinError 5")
                    fake_k32.CloseHandle.assert_called_once_with(4141)
                    popen_mock.assert_not_called()

                    # 3. Popen fails after job creation: the job handle is terminated and closed, and the launch error preserved.
                    fake_k32.reset_mock()
                    fake_k32.CreateJobObjectW.return_value = 4242
                    fake_k32.SetInformationJobObject.return_value = 1
                    with unittest.mock.patch.object(module.subprocess, "Popen",
                                                    side_effect=OSError(2, "No such executable")):
                        code, reason = module.run(["codex"], "hi", 5, [stdout_path])
                    self.assertIsNone(code)
                    self.assertEqual(reason, "could not start: No such executable")
                    fake_k32.TerminateJobObject.assert_called_once_with(4242, 1)
                    fake_k32.CloseHandle.assert_called_once_with(4242)

                    # 4. AssignProcessToJobObject fails after suspended Popen: NtResumeProcess is never called,
                    # and the suspended process and job are terminated and cleaned up.
                    fake_k32.reset_mock()
                    fake_ntdll.reset_mock()
                    fake_k32.CreateJobObjectW.return_value = 7777
                    fake_k32.SetInformationJobObject.return_value = 1
                    fake_k32.AssignProcessToJobObject.return_value = 0
                    fake_proc = unittest.mock.MagicMock()
                    fake_proc._handle = 999
                    fake_proc.stdin = unittest.mock.MagicMock()
                    with unittest.mock.patch.object(module.subprocess, "Popen", return_value=fake_proc) as popen_mock:
                        code, reason = module.run(["codex"], "hi", 5, [stdout_path])
                    self.assertIsNone(code)
                    self.assertEqual(reason, "could not start: WinError 5")
                    self.assertEqual(
                        popen_mock.call_args.kwargs.get("creationflags"),
                        module.CREATE_NEW_PROCESS_GROUP | module.CREATE_SUSPENDED,
                    )
                    fake_ntdll.NtResumeProcess.assert_not_called()
                    fake_k32.TerminateJobObject.assert_called_once_with(7777, 1)
                    fake_k32.CloseHandle.assert_called_once_with(7777)
                    fake_proc.stdin.close.assert_called_once()
                    fake_proc.kill.assert_called_once()
                    fake_proc.wait.assert_called_once()

                    # 5. NtResumeProcess fails after AssignProcessToJobObject: process and job are terminated and cleaned up.
                    fake_k32.reset_mock()
                    fake_ntdll.reset_mock()
                    fake_k32.CreateJobObjectW.return_value = 8888
                    fake_k32.SetInformationJobObject.return_value = 1
                    fake_k32.AssignProcessToJobObject.return_value = 1
                    fake_ntdll.NtResumeProcess.return_value = -1073741823
                    fake_proc_resume = unittest.mock.MagicMock()
                    fake_proc_resume._handle = 1000
                    fake_proc_resume.stdin = unittest.mock.MagicMock()
                    with unittest.mock.patch.object(module.subprocess, "Popen", return_value=fake_proc_resume):
                        code, reason = module.run(["codex"], "hi", 5, [stdout_path])
                    self.assertIsNone(code)
                    self.assertIn("NtResumeProcess failed", reason)
                    fake_k32.TerminateJobObject.assert_called_once_with(8888, 1)
                    fake_k32.CloseHandle.assert_called_once_with(8888)
                    fake_proc_resume.stdin.close.assert_called_once()
                    fake_proc_resume.kill.assert_called_once()
                    fake_proc_resume.wait.assert_called_once()

                    # 6. An unexpected exception inside the watch loop still cleans up the job and process in `finally`.
                    fake_k32.reset_mock()
                    fake_ntdll.reset_mock()
                    fake_k32.CreateJobObjectW.return_value = 9999
                    fake_k32.SetInformationJobObject.return_value = 1
                    fake_k32.AssignProcessToJobObject.return_value = 1
                    fake_ntdll.NtResumeProcess.return_value = 0
                    fake_proc2 = unittest.mock.MagicMock()
                    fake_proc2._handle = 1001
                    fake_proc2.stdin = unittest.mock.MagicMock()
                    fake_proc2.poll.side_effect = RuntimeError("poll exploded")
                    with unittest.mock.patch.object(module.subprocess, "Popen", return_value=fake_proc2):
                        with self.assertRaisesRegex(RuntimeError, "poll exploded"):
                            module.run(["codex"], "hi", 5, [stdout_path])
                    fake_k32.TerminateJobObject.assert_called_once_with(9999, 1)
                    fake_k32.CloseHandle.assert_called_once_with(9999)
                    fake_proc2.kill.assert_called_once()
                    fake_proc2.wait.assert_called_once()

                    # 7. BaseException (e.g. KeyboardInterrupt) before job assignment still terminates the suspended
                    # process and closes the job handle while propagating the cancellation, and kills/waits before
                    # closing stdin so a blocked feeder thread cannot deadlock cleanup.
                    fake_k32.reset_mock()
                    fake_ntdll.reset_mock()
                    fake_k32.CreateJobObjectW.return_value = 6060
                    fake_k32.SetInformationJobObject.return_value = 1
                    fake_k32.AssignProcessToJobObject.side_effect = KeyboardInterrupt("cancelled during assignment")
                    order = []
                    fake_proc3 = unittest.mock.MagicMock()
                    fake_proc3._handle = 1002
                    fake_proc3.kill.side_effect = lambda: order.append("kill")
                    fake_proc3.wait.side_effect = lambda: order.append("wait")
                    fake_proc3.stdin = unittest.mock.MagicMock()
                    fake_proc3.stdin.close.side_effect = lambda: order.append("stdin.close")
                    with unittest.mock.patch.object(module.subprocess, "Popen", return_value=fake_proc3):
                        with self.assertRaises(KeyboardInterrupt):
                            module.run(["codex"], "hi", 5, [stdout_path])
                    fake_ntdll.NtResumeProcess.assert_not_called()
                    fake_k32.TerminateJobObject.assert_called_once_with(6060, 1)
                    fake_k32.CloseHandle.assert_called_once_with(6060)
                    self.assertEqual(order, ["kill", "wait", "stdin.close"])

    def test_the_prompt_never_reaches_the_command_line(self):
        # A Windows .cmd shim re-parses its arguments through cmd.exe, so prose there could become commands.
        argv = self.module().command({"cli": "codex", "model": "gpt-6.1-sol", "effort": "xhigh"}, "C:/tools/codex.cmd",
                                     Path("o"))
        self.assertEqual(argv[-1], "-")
        self.assertEqual(argv[argv.index("-o") + 1], "o")

    def test_a_reviewer_that_cannot_start_is_no_review(self):
        # Installed, but not a program the system can start, so the launch itself fails.
        self.codex.unlink()
        if os.name == "nt":
            target = self.bin / "codex.exe"
            shutil.copyfile(Path(os.environ["SystemRoot"]) / "System32" / "where.exe", target)
            subprocess.run(["icacls", str(target), "/deny", "*S-1-1-0:(X)"], capture_output=True, check=True)
        else:
            write_launcher(self.bin, "codex", "unused").write_text("#!/nonexistent/interpreter\n", encoding="utf-8")
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 1)
        self.assertTrue(summary["skipped"][0]["reason"].startswith("could not start"), summary["skipped"])

    def test_a_non_integer_schema_version_is_refused(self):
        # TOML true is Python True and 1.0 == 1, so both passed a `!= 1` check and loaded.
        pin = self.repo / ".conclave/reviewers.toml"
        good = pin.read_text()
        for bad in ("schema_version = true", "schema_version = 1.0"):
            with self.subTest(bad=bad):
                pin.write_text(good.replace("schema_version = 1", bad), encoding="utf-8")
                result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("needs exactly schema_version = 1, [reviewers.*] and [routes]", result.stderr)
                self.assertEqual(self.calls(), [])

    def test_a_bad_pin_refuses_before_any_reviewer_runs(self):
        pin = self.repo / ".conclave/reviewers.toml"
        good = pin.read_text()
        for bad in bad_pins(good):
            with self.subTest(bad=bad[-60:]):
                self.assertNotEqual(bad, good, "the replacement must match the fixture, or the case tests nothing")
                pin.write_text(bad, encoding="utf-8")
                result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
                self.assertEqual(result.returncode, 2)
                self.assertEqual(self.calls(), [])
        pin.unlink()
        result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 2)
        self.assertIn("no reviewer pin", result.stderr)

    CANDIDATE = "0123456789abcdef0123456789abcdef01234567"

    def complete(self, record, *extra, **modes):
        return self.dispatch("--kind", "completion", "--candidate", self.CANDIDATE, "--writer", "agy", "--prompt",
                             str(self.prompt), "--record", str(record), *extra, **modes)

    def test_a_completion_review_is_recorded_as_its_own_section_and_not_counted_as_a_round(self):
        record = self.repo / "docs/reviews/00-workspace/D00-T01-s1.md"
        result, summary = self.complete(record, codex="completion")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((summary["kind"], summary["candidate"], summary["recorded"]),
                         ("completion", self.CANDIDATE, True))
        text = record.read_text(encoding="utf-8")
        self.assertIn("## Completion review: sol (`gpt-6.1-sol` via `codex`)\n", text)
        self.assertIn(f"\n- Candidate: {self.CANDIDATE}\n", text)
        self.assertNotRegex(text, r"(?m)^## Round 1:")
        # The first regular round after it is round 1, and the round quoted inside the completion review is not one.
        _, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record))
        self.assertEqual(summary["round"], 1)
        module = self.module()
        found = module.read_findings(record.read_bytes().decode("utf-8"))
        self.assertEqual([r["number"] for r in found["rounds"]], [1])
        (completion,) = found["completions"]
        self.assertEqual((completion["reviewer"], completion["model"], completion["candidate"], completion["writer"]),
                         ("sol", "gpt-6.1-sol", self.CANDIDATE, "agy"))
        self.assertTrue(completion["review"].endswith("COMPLETION: gaps"))
        _, listing = self.dispatch("--list-rounds", "--json")
        (entry,) = listing["records"]
        self.assertEqual(([r["number"] for r in entry["rounds"]], len(entry["completions"])), ([1], 1))

    def test_the_completion_review_is_one_pass(self):
        record = self.repo / "docs/reviews/c.md"
        # A pass that returned no review (or lacked a COMPLETION: footer) records `none` and may be retried.
        result, _ = self.complete(record, codex="fail")
        self.assertEqual(result.returncode, 1)
        self.assertIn("## Completion review: none\n", record.read_text(encoding="utf-8"))
        result, _ = self.complete(record, codex="completionblocked")
        self.assertEqual(result.returncode, 1, "COMPLETION: blocked is not a review")
        result, summary = self.complete(record, codex="say", say="1. Missing footer line without COMPLETION tag.")
        self.assertEqual(result.returncode, 1, "a completion reply without COMPLETION: is not a review")
        self.assertEqual(summary["skipped"], [{"reviewer": "sol", "reason": "returned no completion verdict"}])
        result, _ = self.complete(record, codex="completion")
        self.assertEqual(result.returncode, 0, result.stderr)
        before = record.read_bytes()
        result, summary = self.complete(record, codex="completion")
        self.assertEqual(result.returncode, 2)
        self.assertIn("already holds a completion review", result.stderr)
        self.assertFalse(summary["recorded"])
        self.assertEqual(record.read_bytes(), before, "a second completion review was appended")

    def test_a_completion_review_needs_a_full_candidate_and_a_round_takes_none(self):
        for candidate in (None, "abc1234", self.CANDIDATE.upper(), self.CANDIDATE + "0"):
            with self.subTest(candidate=candidate):
                args = ["--kind", "completion", "--writer", "agy", "--prompt", str(self.prompt)]
                result, _ = self.dispatch(*args, *(["--candidate", candidate] if candidate else []))
                self.assertEqual(result.returncode, 2)
                self.assertIn("--kind completion needs --candidate", result.stderr)
        result, _ = self.dispatch("--candidate", self.CANDIDATE, "--writer", "agy", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.calls(), [])

    def test_a_completion_review_edited_by_hand_is_unreadable(self):
        record = self.repo / "docs/reviews/c.md"
        self.complete(record, codex="completion")
        text = record.read_text(encoding="utf-8")
        module = self.module()
        self.assertIsNotNone(module.read_findings(text))
        for edited in (text.replace("COMPLETION: gaps", "COMPLETION: done"),  # same line count, digest differs
                       text.replace(f"- Candidate: {self.CANDIDATE}\n", ""),
                       text.replace(f"- Candidate: {self.CANDIDATE}", "- Candidate: abc1234"),
                       re.sub(r"- Digest: [0-9a-f]{16}\n", "", text)):
            with self.subTest(edited=edited[-60:]):
                self.assertNotEqual(edited, text)
                self.assertIsNone(module.read_findings(edited))
                self.assertIsNone(module.read_rounds(edited), "a broken completion review must not hide behind rounds")

    def test_the_writers_notes_after_a_completion_review_are_read_back(self):
        record = self.repo / "docs/reviews/c.md"
        self.complete(record, codex="completion")
        with open(record, "a", encoding="utf-8", newline="") as handle:
            handle.write("\n- Implemented: gap 1 in abc1234.\n- Filed: D02 T01 §9 (gap 2)\n")
        self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(record))
        found = self.module().read_findings(record.read_bytes().decode("utf-8"))
        self.assertEqual(found["completions"][0]["notes"], ["- Implemented: gap 1 in abc1234.",
                                                            "- Filed: D02 T01 §9 (gap 2)"])
        self.assertEqual([r["number"] for r in found["rounds"]], [1])


def bad_pins(good):
    """Every rule a parsable pin can break, each as one edit of the fixture, shared by the review and check tests."""
    mini = '\n[reviewers.mini]\ncli = "codex"\nmodel = "gpt-6-mini"\neffort = "low"\n'
    return (good.replace('effort = "xhigh"', 'effort = "extreme"'),
            good.replace('effort = "xhigh"', 'effort = "max"'),
            good.replace('effort = "xhigh"\n', ''),
            good.replace('cli = "codex"', 'cli = "bash"'),
            good.replace('cli = "codex"', 'cli = "agy"'),
            good.replace('cli = "codex"', 'cli = "claude"'),
            good.replace('agy = ["sol"]', 'agy = ["opus"]'),
            good.replace('model = "gpt-6.1-sol"', 'model = "gpt-6.1-sol; rm -rf ~"'),
            good + '\n[extra]\nkey = 1\n',
            good.replace('agy = ["sol"]', 'agy = [["sol"]]'),
            good.replace('effort = "xhigh"', 'effort = "xhigh"\nsandbox = "workspace-write"'),
            good.replace('agy = ["sol"]', 'agy = ["sol", "mini"]') + mini,
            good.replace('agy = ["sol"]', 'agy = []'),
            good + 'claude = ["sol"]\n',
            good + 'codex = ["sol"]\n',
            good + 'gemini = ["sol"]\n')


SCHEDULED_PIN = FIXTURE_PIN + textwrap.dedent('''
    [schedule]
    severity_stop_after = 0

    [schedule.plan]
    effort = "medium"
    lenses = ["plan"]

    [schedule.completion]
    effort = "medium"

    [[schedule.rounds]]
    effort = "high"
    lenses = ["adversarial", "integration"]

    [[schedule.rounds]]
    effort = "medium"
    lenses = ["adversarial"]

    [[schedule.rounds]]
    effort = "low"
    lenses = ["adversarial"]

    [schedule.rotation]
    effort = "low"
    lenses = ["consistency", "design", "record"]

    [schedule.escalation]
    effort = "medium"
    lenses = ["adversarial"]
''')


class Schedule(Harness):
    """The pin's [schedule]: the dispatcher picks each pass's effort and lenses from the rounds already recorded."""

    def setUp(self):
        super().setUp()
        (self.repo / ".conclave/reviewers.toml").write_text(SCHEDULED_PIN, encoding="utf-8")
        self.findings = self.repo / "findings.md"

    def round(self, say="OVERALL: approve", *extra):
        self.log.unlink(missing_ok=True)
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record",
                                        str(self.findings), *extra, codex="say", say=say)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        return summary, self.calls()

    def effort_of(self, call):
        return next(a.split("=", 1)[1] for a in call["argv"] if a.startswith("model_reasoning_effort="))

    def test_rounds_follow_the_fixed_list_then_rotate_one_lens_each(self):
        seen = []
        for _ in range(7):
            summary, (call,) = self.round()
            seen.append((summary["position"], self.effort_of(call), summary["lenses"]))
            self.assertIn("## Lenses for this pass", call["stdin"])
            for name in summary["lenses"]:
                self.assertIn(f"- `{name}`:", call["stdin"])
        self.assertEqual(seen, [(1, "high", ["adversarial", "integration"]), (2, "medium", ["adversarial"]),
                                (3, "low", ["adversarial"]), (4, "low", ["consistency"]), (5, "low", ["record"]),
                                (6, "low", ["consistency"]), (7, "low", ["record"])],
                         "without --surface the rotation skips the design lens")
        rounds = self.module().read_rounds(self.findings.read_text(encoding="utf-8"))
        self.assertEqual([(r["effort"], r["lenses"]) for r in rounds][:2],
                         [("high", ["adversarial", "integration"]), ("medium", ["adversarial"])])

    def test_surface_puts_design_in_the_rotation(self):
        lenses = [self.round("OVERALL: approve", "--surface")[0]["lenses"] for _ in range(6)]
        self.assertEqual(lenses[3:], [["consistency"], ["design"], ["record"]])

    def test_an_outage_round_does_not_move_the_position(self):
        self.round()
        self.log.unlink(missing_ok=True)
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record",
                                        str(self.findings), codex="fail")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(summary["position"], 2)
        summary, (call,) = self.round()
        self.assertEqual((summary["position"], summary["round"], self.effort_of(call)), (2, 3, "medium"))

    def test_a_self_review_or_a_reply_without_a_verdict_does_not_move_the_position(self):
        # scripts/todo-graph.py stamp replays `scheduled`, so neither may count as a review, like an outage.
        module = self.module()
        schedule = module.load_pin(str(self.repo / ".conclave/reviewers.toml"))["schedule"]
        returned = {"reviewer": "astra", "cli": "codex", "writer": "agy", "effort": "high",
                    "review": "OVERALL: approve"}
        self.assertEqual(module.scheduled(schedule, "round", [returned, returned], False)["position"], 3)
        for own in ({"reviewer": "gemini", "cli": "agy"}, {"reviewer": "x", "cli": "unknown"},
                    {"review": "1. The reply stopped before its verdict."}):
            with self.subTest(own=own):
                self.assertEqual(module.scheduled(schedule, "round", [returned, {**returned, **own}], False)
                                 ["position"], 2)

    def test_a_low_effort_p1_escalates_the_next_round(self):
        for _ in range(3):
            self.round()
        summary, _ = self.round("1. P1 calc.py:2 subtracts.\n\nHIGHEST: P1\nOVERALL: needs-attention")
        self.assertEqual((summary["position"], summary["escalated"]), (4, False))
        summary, (call,) = self.round()
        self.assertEqual((summary["position"], summary["escalated"], self.effort_of(call), summary["lenses"]),
                         (5, True, "medium", ["adversarial"]))
        self.assertIn("escalated after a low-effort round found a P1", call["stdin"])
        summary, (call,) = self.round()
        self.assertEqual((summary["escalated"], self.effort_of(call)), (False, "low"),
                         "a medium escalation that finds a P1 does not escalate again")

    def test_severity_stop_on_first_p3_only_round_and_never_on_p1_or_p2(self):
        p3 = "1. P3 calc.py:2 a name.\n\nHIGHEST: P3\nOVERALL: approve"
        stops = [self.round(p3)[0]["severity_stop"] for _ in range(4)]
        self.assertEqual(stops, [True, True, True, True])
        self.assertFalse(self.round("1. P3 calc.py:2.\n\nHIGHEST: P3")[0]["severity_stop"],
                         "a round missing OVERALL: is not a returned review and never stops the loop")
        self.assertFalse(self.round("1. P1 calc.py:2.\n\nHIGHEST: P1\nOVERALL: needs-attention")[0]["severity_stop"])
        self.assertFalse(self.round("1. P2 calc.py:2.\n\nHIGHEST: P2\nOVERALL: needs-attention")[0]["severity_stop"])
        self.assertFalse(self.round("1. calc.py:2 untagged.\n\nOVERALL: needs-attention")[0]["severity_stop"],
                         "a round with no HIGHEST line never stops the loop")
        # Historical pins with severity_stop_after = 3 remain supported for already-stamped receipts.
        (self.repo / ".conclave/reviewers.toml").write_text(
            SCHEDULED_PIN.replace("severity_stop_after = 0", "severity_stop_after = 3"), encoding="utf-8")
        self.findings.unlink(missing_ok=True)
        legacy_stops = [self.round(p3)[0]["severity_stop"] for _ in range(4)]
        self.assertEqual(legacy_stops, [False, False, False, True])

    def test_an_unreadable_findings_file_is_refused_before_a_review_runs(self):
        self.findings.write_text("## Round one\n\nnot a record\n", encoding="utf-8")
        result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--record", str(self.findings))
        self.assertEqual(result.returncode, 2)
        self.assertIn("so no round was run", result.stderr)
        self.assertEqual(self.calls(), [], "no review is spent that could not be recorded")

    def test_the_plan_review_is_one_pass_recorded_with_its_lens(self):
        candidate = "a" * 40
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--kind", "plan",
                                        "--candidate", candidate, "--record", str(self.findings), codex="say",
                                        say="1. Ctrl+K reach: missing.\n\nPLAN: gaps\nOVERALL: needs-attention")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((summary["kind"], summary["effort"], summary["lenses"]), ("plan", "medium", ["plan"]))
        self.assertNotIn("position", summary)
        (call,) = self.calls()
        self.assertEqual(self.effort_of(call), "medium")
        i = call["argv"].index("--sandbox")
        self.assertEqual(call["argv"][i:i + 2], ["--sandbox", "read-only"])
        self.assertIn("- `plan`:", call["stdin"])
        text = self.findings.read_text(encoding="utf-8")
        self.assertIn("## Plan review: sol (`gpt-6.1-sol` via `codex`)", text)
        (plan,) = self.module().read_plans(text)
        self.assertEqual((plan["candidate"], plan["lenses"], plan["effort"]), (candidate, ["plan"], "medium"))
        self.assertEqual(self.module().read_rounds(text), [], "a plan review is not a round")
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--kind", "plan",
                                        "--candidate", candidate, "--record", str(self.findings))
        self.assertEqual(result.returncode, 2)
        self.assertIn("already holds a plan review", result.stderr)
        summary, _ = self.round()
        self.assertEqual(summary["position"], 1, "the plan review does not move the round position")

    def test_a_plan_review_needs_a_full_candidate(self):
        for extra in ((), ("--candidate", "abc123")):
            with self.subTest(extra=extra):
                result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--kind", "plan", *extra)
                self.assertEqual(result.returncode, 2)
                self.assertIn("--kind plan needs --candidate", result.stderr)

    def test_the_completion_review_takes_its_scheduled_effort_and_no_lenses(self):
        result, summary = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), "--kind", "completion",
                                        "--candidate", "b" * 40, codex="say", say="COMPLETION: complete")
        self.assertEqual(result.returncode, 0, result.stderr)
        (call,) = self.calls()
        self.assertEqual(self.effort_of(call), "medium")
        i = call["argv"].index("--sandbox")
        self.assertEqual(call["argv"][i:i + 2], ["--sandbox", "read-only"])
        self.assertNotIn("## Lenses for this pass", call["stdin"])
        self.assertEqual(call["stdin"], self.prompt.read_text())

    def test_all_three_review_stages_pass_adjacent_read_only_sandbox_argv(self):
        candidate = "c" * 40
        for kind, extra, say in (
            ("plan", ("--kind", "plan", "--candidate", candidate), "HIGHEST: none\nPLAN: complete\nOVERALL: approve"),
            ("completion", ("--kind", "completion", "--candidate", candidate), "COMPLETION: complete"),
            ("round", ("--kind", "round"), "HIGHEST: none\nOVERALL: approve"),
        ):
            with self.subTest(kind=kind):
                self.log.unlink(missing_ok=True)
                result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt), *extra, codex="say", say=say)
                self.assertEqual(result.returncode, 0, result.stderr)
                (call,) = self.calls()
                idx = call["argv"].index("--sandbox")
                self.assertEqual(call["argv"][idx:idx + 2], ["--sandbox", "read-only"])

    def test_a_pin_without_a_schedule_keeps_the_reviewers_effort_and_prompt(self):
        (self.repo / ".conclave/reviewers.toml").write_text(FIXTURE_PIN, encoding="utf-8")
        summary, (call,) = self.round()
        self.assertEqual(self.effort_of(call), "xhigh")
        self.assertEqual(call["stdin"], self.prompt.read_text())
        self.assertNotIn("position", summary)

    def test_check_pin_reports_the_schedule(self):
        result, summary = self.dispatch("--check-pin", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(summary["schedule"]["severity_stop_after"], 0)

    def test_a_malformed_schedule_refuses_every_dispatch(self):
        module = self.module()
        good = tomllib.loads(SCHEDULED_PIN)["schedule"]
        bad = {
            "unknown lens": {**good, "plan": {"effort": "medium", "lenses": ["vibes"]}},
            "bad effort": {**good, "completion": {"effort": "extreme"}},
            "completion lenses": {**good, "completion": {"effort": "medium", "lenses": ["plan"]}},
            "no rounds": {**good, "rounds": []},
            "duplicate lens": {**good, "rotation": {"effort": "low", "lenses": ["record", "record"]}},
            "missing key": {k: v for k, v in good.items() if k != "escalation"},
            "unknown key": {**good, "extra": 1},
            "stop negative": {**good, "severity_stop_after": -1},
            "stop bool": {**good, "severity_stop_after": True},
        }
        module.check_schedule(good, ["codex"])
        for name, schedule in bad.items():
            with self.subTest(name), self.assertRaises(module.Refused):
                module.check_schedule(schedule, ["codex"])
        broken = SCHEDULED_PIN.replace('lenses = ["plan"]', 'lenses = ["vibes"]')
        (self.repo / ".conclave/reviewers.toml").write_text(broken, encoding="utf-8")
        result, _ = self.dispatch("--writer", "agy", "--prompt", str(self.prompt))
        self.assertEqual(result.returncode, 2)
        self.assertIn("[schedule] plan", result.stderr)
        self.assertEqual(self.calls(), [])


class RepositoryPin(unittest.TestCase):
    def test_the_repository_pin_loads_under_the_dispatchers_rules(self):
        spec = importlib.util.spec_from_file_location("dispatch_pin_check", SCRIPTS / "review-dispatch.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        pin = module.load_pin(REPO_PIN)
        self.assertTrue(pin["routes"], "the repository pin routes no writer")

    def test_the_repository_pin_routes_codex_to_spark_and_other_writers_to_astra(self):
        with open(REPO_PIN, "rb") as handle:
            pin = tomllib.load(handle)
        self.assertEqual(pin["reviewers"], {"astra": {"cli": "codex", "model": "gpt-6-astra", "effort": "medium"},
                                           "spark": {"cli": "muse", "model": "muse-spark-1.3", "effort": "high"}})
        self.assertEqual(pin["routes"], {"claude": ["astra"], "muse": ["astra"], "agy": ["astra"],
                                         "codex": ["spark"], "opencode": ["astra"], "copilot": ["astra"]})
        schedule = pin["schedule"]
        self.assertEqual(schedule["plan"], {"effort": "medium", "lenses": ["plan"]})
        self.assertEqual(schedule["completion"], {"effort": "medium"})
        self.assertEqual([(r["effort"], r["lenses"][0]) for r in schedule["rounds"]],
                         [("high", "adversarial"), ("medium", "adversarial"), ("low", "adversarial")])
        self.assertEqual(schedule["rotation"]["effort"], "low")
        self.assertEqual(schedule["escalation"], {"effort": "medium", "lenses": ["adversarial"]})

    def test_the_pins_severity_stop_is_the_stamps_cutoff(self):
        # The dispatcher reports severity_stop from the pin and `stamp` re-checks it against SEVERITY_CUTOFF_ROUND: the
        # two numbers must agree, or the dispatcher would stop a loop the stamp then refuses.
        spec = importlib.util.spec_from_file_location("todo_graph_cutoff", SCRIPTS / "todo-graph.py")
        todo_graph = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = todo_graph
        try:
            spec.loader.exec_module(todo_graph)
        finally:
            del sys.modules[spec.name]
        with open(REPO_PIN, "rb") as handle:
            pin = tomllib.load(handle)
        self.assertEqual(pin["schedule"]["severity_stop_after"], todo_graph.SEVERITY_CUTOFF_ROUND)

    def test_stamp_schedule_rejects_unpinned_reviewer_and_allows_retry_after_invalid_one_pass(self):
        spec = importlib.util.spec_from_file_location("todo_graph_route_check", SCRIPTS / "todo-graph.py")
        todo_graph = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = todo_graph
        try:
            spec.loader.exec_module(todo_graph)
        finally:
            del sys.modules[spec.name]
        dispatch = todo_graph.review_dispatch()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "todo/00-workspace").mkdir(parents=True)
            (root / ".conclave").mkdir(parents=True)
            (root / ".conclave/reviewers.toml").write_bytes(REPO_PIN.read_bytes())
            subprocess.run(["git", "-C", str(root), "init", "-q"], check=True)
            subprocess.run(["git", "-C", str(root), "add", ".conclave/reviewers.toml"], check=True)
            subprocess.run(
                ["git", "-C", str(root), "-c", "user.name=Test", "-c", "user.email=test@example.com",
                 "commit", "-q", "-m", "pin"],
                check=True,
            )
            report_rel = "docs/reviews/00-workspace/D00-T01-s6.md"
            report = root / report_rel
            head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                                  capture_output=True, text=True, check=True).stdout.strip()
            with open(REPO_PIN, "rb") as handle:
                pin = tomllib.load(handle)
            # Pre-seed an invalid completion entry (missing COMPLETION:) with valid digest, then verify
            # record_completion allows a valid retry rather than permanently consuming the slot.
            bad_lines = ["1. Incomplete reply without footer."]
            report.parent.mkdir(parents=True, exist_ok=True)
            report.write_text("\n".join([
                "## Completion review: astra (`gpt-6-astra` via `codex`)",
                "",
                "- Writer: agy",
                "- Run: 2026-10-24T00:00:00Z",
                f"- Candidate: {head}",
                "- Effort: medium",
                f"- Lines: {len(bad_lines)}",
                f"- Digest: {dispatch.digest(bad_lines)}",
                "",
                *bad_lines,
                "",
            ]), encoding="utf-8")
            dispatch.record_completion(
                report,
                {"writer": "agy", "reviewer": "astra", "cli": "codex", "model": "gpt-6-astra",
                 "effort": "medium", "skipped": [], "review": "COMPLETION: complete"},
                "2026-10-24T00:01:00Z",
                head,
                "completion",
            )
            # Now record a plan review and round 1 from an unpinned reviewer (`sol` / `gpt-6.1-sol`) and verify
            # schedule_ok refuses it against the committed `astra` / `gpt-6-astra` pin.
            dispatch.record_completion(
                report,
                {"writer": "agy", "reviewer": "astra", "cli": "codex", "model": "gpt-6-astra",
                 "effort": "medium", "lenses": ["plan"], "skipped": [],
                 "review": "HIGHEST: none\nPLAN: complete\nOVERALL: approve"},
                "2026-10-24T00:02:00Z",
                head,
                "plan",
            )
            dispatch.record(
                report,
                {"writer": "agy", "reviewer": "sol", "cli": "codex", "model": "gpt-6.1-sol",
                 "effort": "high", "lenses": ["adversarial", "integration"], "skipped": [],
                 "review": "HIGHEST: none\nOVERALL: approve"},
                "2026-10-24T00:03:00Z",
            )
            data = {
                "implementation_sha": head,
                "schedule": pin["schedule"],
                "plan": {"report": report_rel, "candidate": head, "reviewer": "astra",
                         "model": "gpt-6-astra", "effort": "medium", "lenses": ["plan"]},
                "completion": {"report": report_rel, "verdict": "complete", "candidate": head,
                               "reviewer": "astra", "model": "gpt-6-astra", "filed": []},
                "reviews": [{"round": 1, "report": report_rel, "verdict": "clean"}],
            }
            with self.assertRaisesRegex(todo_graph.Refused, r"not the reviewer astra"):
                todo_graph.schedule_ok(root, data, "D00 T01 §6", False)


RESTART_TODO = """---
schema_version: 1
id: restart-fixture
domain: 00-workspace
status: active
title: "TODO-01 -- Restart fixture"
track: Z1
---

# TODO-01 -- Restart fixture

> **Goal:** Fixture for the Restarted marker.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | The restarted thing | - |  [ ]   |

---

## 1. The Restarted Thing

{prose}

> **Started:** 2026-10-30T01:00:00Z{restart}

- [x] Did the thing
- [ ] Commit: `"fixture: the thing"`

**Test checkpoint:** `true` proves the fixture.
"""


class RestartedMarkerTests(unittest.TestCase):
    """`stamp` with a plan review that followed `Started:` (D00 T01 §11): refused without `> **Restarted:**`, accepted
    with a well-formed one recorded after the review, and every refusal leaves the TODO file and receipt untouched."""

    REF = "D00 T01 §1"
    TODO = "todo/00-workspace/TODO-01-restart-fixture.md"
    REPORT = "docs/reviews/00-workspace/D00-T01-s1.md"

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("todo_graph_restart", SCRIPTS / "todo-graph.py")
        cls.tg = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.tg
        try:
            spec.loader.exec_module(cls.tg)
        finally:
            del sys.modules[spec.name]
        cls.dispatch = cls.tg.review_dispatch()

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / ".conclave").mkdir()
        (self.root / ".conclave/reviewers.toml").write_bytes(REPO_PIN.read_bytes())
        self.schedule = self.dispatch.load_pin(str(self.root / ".conclave/reviewers.toml"))["schedule"]
        (self.root / "todo/00-workspace").mkdir(parents=True)
        self.saved = (self.tg.REPO, self.tg.TODO_DIR, self.tg.PLAN)
        self.tg.REPO = self.root
        self.tg.TODO_DIR = self.root / "todo"
        self.tg.PLAN = self.root / "todo/implementation-plan.md"
        self.git("init", "-q")

    def tearDown(self):
        self.tg.REPO, self.tg.TODO_DIR, self.tg.PLAN = self.saved
        self._tmp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), "-c", "user.name=Test", "-c", "user.email=t@example.com",
                               *args], capture_output=True, text=True, check=True).stdout.strip()

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def write(self, prose="Fixture prose.", restart=""):
        (self.root / self.TODO).write_text(RESTART_TODO.format(prose=prose, restart=restart), encoding="utf-8")

    def reviewed(self, kind, candidate=None, highest="none"):
        report = self.root / self.REPORT
        report.parent.mkdir(parents=True, exist_ok=True)
        base = {"writer": "copilot", "reviewer": "astra", "cli": "codex", "model": "gpt-6-astra", "skipped": []}
        if kind == "round":
            rounds = self.dispatch.read_rounds(report.read_text(encoding="utf-8"))
            step = self.dispatch.scheduled(self.schedule, "round", rounds, False)
            self.dispatch.record(report, {**base, "effort": step["effort"], "lenses": step["lenses"],
                                          "review": f"HIGHEST: {highest}\nOVERALL: approve"}, "2026-10-30T03:00:00Z")
            return
        step = self.dispatch.scheduled(self.schedule, kind, [], False)
        footer = "HIGHEST: none\nPLAN: complete\nOVERALL: approve" if kind == "plan" else "COMPLETION: complete"
        self.dispatch.record_completion(report, {**base, "effort": step["effort"], "lenses": step["lenses"],
                                                 "review": footer}, "2026-10-30T02:00:00Z", candidate, kind)

    def late_review(self, restart=None, restart_before_review=False):
        """Started, then a correction, then the plan review of the correction; returns the corrected commit."""
        self.write()
        self.commit("start")
        self.write(prose="Corrected fixture prose.")
        fixed = self.commit("correct the section after Started")
        if restart_before_review:
            self.write(prose="Corrected fixture prose.", restart="\n" + restart(fixed))
            self.commit("restart")
        self.reviewed("plan", fixed)
        self.commit("record the plan review")
        if restart and not restart_before_review:
            self.write(prose="Corrected fixture prose.", restart="\n" + restart(fixed))
            self.commit("restart")
        return fixed

    def finish(self, round_after=True):
        head = self.git("rev-parse", "HEAD")
        self.reviewed("completion", head)
        if round_after:
            self.reviewed("round")
        return self.commit("build and review")

    def stamp(self, sha):
        return self.tg.stamp(self.root, self.REF, sha, [{"round": 1, "report": self.REPORT, "verdict": "clean"}],
                             ["true -> ok"], "2026-10-30", sync_plan=False,
                             completion={"report": self.REPORT, "verdict": "complete"}, plan=self.REPORT)

    def refused(self, sha, needle):
        before = (self.root / self.TODO).read_bytes()
        with self.assertRaises(self.tg.Refused) as caught:
            self.stamp(sha)
        self.assertIn(needle, str(caught.exception))
        self.assertEqual((self.root / self.TODO).read_bytes(), before, "a refusal leaves the TODO file untouched")
        self.assertFalse((self.root / "docs/reviews/d00-t01-s1.json").exists(), "a refusal writes no receipt")

    @staticmethod
    def line(sha, reason="the plan review read the correction"):
        return f"> **Restarted:** 2026-10-30T02:30:00Z | plan review {sha} | {reason}"

    def test_without_restart_a_late_plan_review_is_refused(self):
        self.late_review()
        self.refused(self.finish(), "is not the commit the build began at")

    def test_restart_after_the_review_stamps_and_reads_back(self):
        self.late_review(restart=lambda fixed: self.line(fixed[:12]))
        sha = self.finish()
        receipt = self.stamp(sha)
        data = json.loads((self.root / receipt).read_text(encoding="utf-8"))
        self.tg.receipt_data_ok(data, self.REF, sha, root=self.root)
        text = (self.root / self.TODO).read_text(encoding="utf-8")
        self.assertIn("> **Started:** 2026-10-30T01:00:00Z\n> **Restarted:** ", text, "Started keeps its instant")
        self.assertIn(f"> **Verified:** 2026-10-30 | §1 | {sha}", text)

    def test_restart_naming_another_sha_is_refused(self):
        self.late_review(restart=lambda fixed: self.line("abcdef1234567"))
        self.refused(self.finish(), "names plan review abcdef1234567")

    def test_restart_committed_before_the_review_was_recorded_is_refused(self):
        self.late_review(restart=lambda fixed: self.line(fixed[:12]), restart_before_review=True)
        self.refused(self.finish(), "does not yet hold the returned plan review")

    def test_stamping_a_commit_older_than_the_restart_is_refused(self):
        fixed = self.late_review(restart=lambda fixed: self.line(fixed[:12]))
        self.finish()
        self.refused(fixed, "is not the stamped commit")

    def test_restart_without_a_later_round_is_refused(self):
        self.late_review()
        head = self.git("rev-parse", "HEAD")
        self.reviewed("completion", head)
        self.reviewed("round")
        fixed = self.git("log", "--format=%H", "-n1", "--grep=correct the section")
        self.write(prose="Corrected fixture prose.", restart="\n" + self.line(fixed[:12]))
        self.refused(self.commit("restart after the only round"), "no returned review round")

    def test_malformed_repeated_and_misplaced_lines_are_refused(self):
        fixed = self.late_review()
        sha = self.finish()
        good = self.line(fixed[:12])
        cases = [
            ("\n" + good.replace("2026-10-30T02:30:00Z", "2026-10-30 02:30"), "malformed"),
            ("\n" + good.replace(fixed[:12], fixed[:6]), "malformed"),
            ("\n" + good.replace(fixed[:12], fixed[:12].upper()), "malformed"),
            ("\n" + self.line(fixed[:12], reason=""), "malformed"),
            ("\n" + good + "\n" + good, "at most one"),
            ("\n\n" + good, "directly under"),
        ]
        for restart, needle in cases:
            with self.subTest(needle=needle, restart=restart):
                self.write(prose="Corrected fixture prose.", restart=restart)
                self.refused(sha, needle)
        text = RESTART_TODO.format(prose="Corrected fixture prose.", restart="")
        (self.root / self.TODO).write_text(
            text.replace("> **Started:** 2026-10-30T01:00:00Z", good), encoding="utf-8")
        self.refused(sha, "needs the section's '> **Started:**' line")


if __name__ == "__main__":
    unittest.main()
