"""scripts/review-prompt.py: a review pass is sent its whole template, with every field filled or nothing written.

The hand-written prompts this script replaces had drifted into short paraphrases that capped the reviewer's reply
(build/plan-d99s8.md, 2026-11-10). So the central assertion here is equality: the output is the template body, in
order, after exactly the permitted substitutions, with the expected values computed in the test.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest

SCRIPTS = Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
_spec = importlib.util.spec_from_file_location("rf_review_prompt", SCRIPTS / "review-prompt.py")
rp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rp)

TODO = "todo/00-workspace/TODO-09-fixture.md"
REF = "D00 T09 §2"
FINDINGS = "docs/reviews/00-workspace/D00-T09-s2.md"
TODO_TEXT = """# TODO-09

## Inputs

- [`spec/rules.md`](../../spec/rules.md) -- the governing rules
- `missing/never.md` is named here and does not exist

## 1. First

- [ ] Nothing.

## 2. Second

- [ ] Edit `src/thing.c` and create `scripts/new_tool.py`. Done when: `python3 scripts/new_tool.py` exits 0.
- [ ] Commit: `"x"`

## Verification
"""
# A stand-in for todo-graph.py resolve: the fixture root has no real graph, and the script's contract with it is
# only the `path`/`ref` lines and the exit code.
RESOLVE_STUB = f"""import sys
ref = " ".join(sys.argv[2:])
if "T09" not in ref:
    print("no TODO for that reference", file=sys.stderr); sys.exit(1)
print("path       {TODO}")
print("ref        {REF}")
sys.exit(int(open(__file__ + ".exit").read()) if __import__("os").path.exists(__file__ + ".exit") else 0)
"""


def args(**kw):
    base = dict(kind="plan", ref=REF, candidate=None, base=None, summary=None, failed=None, last=None, gates=None,
                files=None, findings=None, out=None)
    base.update(kw)
    return types.SimpleNamespace(**base)


class ReviewPromptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = cls.root = Path(cls.tmp.name)
        for rel in rp.TEMPLATES.values():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / rel, root / rel)
        for rel, text in ((TODO, TODO_TEXT), ("spec/rules.md", "rules\n"), ("src/thing.c", "int x;\n"),
                          ("scripts/todo-graph.py", RESOLVE_STUB)):
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text, encoding="utf-8")
        env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid", GIT_COMMITTER_NAME="t",
                   GIT_COMMITTER_EMAIL="t@example.invalid")

        def git(*a):
            return subprocess.run(["git", *a], cwd=root, env=env, capture_output=True, text=True, check=True).stdout.strip()
        git("init", "-q")
        git("add", "-A")
        git("commit", "-q", "-m", "one")
        cls.c1 = git("rev-parse", "HEAD")
        (root / "src/thing.c").write_text("int y;\n", encoding="utf-8")
        git("commit", "-q", "-am", "two")
        cls.c2 = git("rev-parse", "HEAD")
        (root / "scripts/new_tool.py").write_text("pass\n", encoding="utf-8")
        git("add", "-A")
        git("commit", "-q", "-m", "three")
        cls.c3 = git("rev-parse", "HEAD")
        cls.governing = f"`{TODO}`, `spec/rules.md`"

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def body(self, kind):
        return (REPO / rp.TEMPLATES[kind]).read_text(encoding="utf-8").split("\n---\n", 1)[1].lstrip("\n")

    def run_kind(self, **kw):
        out = self.root / "build" / "out.md"
        if out.exists():
            out.unlink()
        path, command = rp.build(args(out=str(out), **kw), self.root)
        return path.read_text(encoding="utf-8"), command

    def expect(self, kind, pairs, drop=None):
        text = self.body(kind)
        if drop:
            text = "\n".join(line for line in text.split("\n") if not line.startswith(drop))
        for old, new in pairs:
            self.assertEqual(text.count(old), 1 if not old.startswith("<DNN") else text.count(old), old)
            text = text.replace(old, new)
        return text

    def test_plan_is_the_whole_template_with_each_field_from_its_source(self):
        got, command = self.run_kind(kind="plan", candidate=self.c3)
        want = self.expect("plan", [
            ("<DNN TNN §N>", REF), ("<todo/.../TODO-NN-....md>", TODO), ("<start>-<end>", "12-16"),
            ("<FULL_40_CHAR_HEAD_SHA>", self.c3), ("`<paths>`", "`src/thing.c`, `scripts/new_tool.py`"),
            ('`<spec/FOLD.md, docs/research.md, docs/design/... or "none">`', self.governing)])
        self.assertEqual(got, want)
        self.assertIn(f"--kind plan --candidate {self.c3}", command)
        self.assertIn(f"--record {FINDINGS}", command)

    def test_plan_files_override_and_the_none_case(self):
        got, _ = self.run_kind(kind="plan", candidate=self.c3, files="a/b.c c.py")
        self.assertIn("Files the section expects to touch: `a/b.c`, `c.py`.", got)
        todo = self.root / TODO
        original = todo.read_text(encoding="utf-8")
        try:
            todo.write_text(original.replace("Edit `src/thing.c` and create `scripts/new_tool.py`. Done when: `python3 scripts/new_tool.py` exits 0.", "Think."), encoding="utf-8")
            got, _ = self.run_kind(kind="plan", candidate=self.c3)
        finally:
            todo.write_text(original, encoding="utf-8")
        self.assertIn("Files the section expects to touch: none named in the section.", got)

    def test_round_is_the_whole_template_minus_the_standalone_line(self):
        got, command = self.run_kind(kind="round", base=self.c1, candidate=self.c3, summary="Makes the thing work")
        want = self.expect("round", [
            ("`<FULL_SHA>` or `<BASE_SHA>..<TIP_SHA>`", f"`{self.c1}..{self.c3}`"),
            ("`<one or two sentences>`", "Makes the thing work. Files the range touches: `scripts/new_tool.py`, `src/thing.c`"),
            ("<DNN TNN §N>", REF),
            ("`<the spec, plan or design file that applies, e.g. spec/FOLD.md, docs/research.md>`", self.governing)],
            drop="- Findings file, for a standalone review")
        self.assertEqual(got, want)
        self.assertNotIn("--kind", command)

    def test_completion_is_the_whole_template(self):
        got, command = self.run_kind(kind="completion", base=self.c2, candidate=self.c3, summary="Built the tool.")
        want = self.expect("completion", [
            ("<DNN TNN §N>", REF), ("`<FULL_SHA>` or `<BASE_SHA>..<TIP_SHA>`", f"`{self.c2}..{self.c3}`"),
            ("`<two or three sentences naming the files, symbols, format paths, CLI flags, or UI controls>`",
             "Built the tool. Files the range touches: `scripts/new_tool.py`"),
            ("`<the design doc or plan file that applies>`", self.governing),
            ("<the section's findings file>", FINDINGS)])
        self.assertEqual(got, want)
        self.assertIn("--kind completion", command)

    def test_reconciliation_puts_three_different_commits_on_their_own_fields(self):
        gates = self.root / "build" / "gates.txt"
        gates.parent.mkdir(exist_ok=True)
        gates.write_text("42 passed, 0 failed\n", encoding="utf-8")
        got, _ = self.run_kind(kind="reconciliation", failed=self.c1, last=self.c2, candidate=self.c3, gates=str(gates))
        text = self.body("reconciliation")
        self.assertEqual(text.count("`<FULL_SHA>`"), 2)
        want = text.replace("<DNN TNN §N>", REF).replace("<docs/reviews/<domain>/<slug>.md>", FINDINGS)
        want = want.replace("The failed completion's candidate: `<FULL_SHA>`", f"The failed completion's candidate: `{self.c1}`")
        want = want.replace("The repaired candidate: `<FULL_SHA>`", f"The repaired candidate: `{self.c3}`")
        want = want.replace("<COMPLETION_SHA>..<REPAIRED_SHA>", f"{self.c1}..{self.c3}")
        want = want.replace("<LAST_SHA>..<REPAIRED_SHA>", f"{self.c2}..{self.c3}").replace("<LAST_SHA>", self.c2)
        want = want.replace("`<quoted output>`.", "\n\n```text\n42 passed, 0 failed\n```\n")
        want = want.replace("`<the design doc or plan file that applies>`", self.governing)
        self.assertEqual(got, want)
        self.assertIn(f"The last reviewed candidate (the failed completion's, or the last reconciliation's): `{self.c2}`", got)

    def test_no_output_carries_a_reply_cap_or_a_placeholder(self):
        gates = self.root / "build" / "gates.txt"
        gates.parent.mkdir(exist_ok=True)
        gates.write_text("ok\n", encoding="utf-8")
        for kw in (dict(kind="plan", candidate=self.c3),
                   dict(kind="round", base=self.c1, candidate=self.c3, summary="s"),
                   dict(kind="completion", base=self.c1, candidate=self.c3, summary="s"),
                   dict(kind="reconciliation", failed=self.c1, candidate=self.c3, gates=str(gates))):
            got, _ = self.run_kind(**kw)
            self.assertIsNone(rp.PLACEHOLDER_RE.search(got), kw["kind"])
            self.assertGreaterEqual(len(got), len(self.body(kw["kind"])) - 200, kw["kind"])
            for cap in ("under 250 words", "at most 3 gaps", "at most 3 findings"):
                self.assertNotIn(cap, got)

    def refused(self, reason, **kw):
        out = self.root / "build" / "refused.md"
        if out.exists():
            out.unlink()
        with self.assertRaises(rp.Refused) as caught:
            rp.build(args(out=str(out), **kw), self.root)
        self.assertIn(reason, str(caught.exception))
        self.assertFalse(out.exists(), "a refusal writes nothing")

    def test_refuses_an_unknown_ref_and_a_malformed_one(self):
        self.refused("cannot resolve", kind="plan", ref="D00 T77 §1", candidate=self.c3)
        self.refused("must look like", kind="plan", ref="section two", candidate=self.c3)
        self.refused("has no section 9", kind="plan", ref="D00 T09 §9", candidate=self.c3)

    def test_refuses_a_candidate_that_is_not_a_full_commit_here(self):
        self.refused("full 40-character", kind="plan", candidate=self.c3[:12])
        self.refused("not a commit in this repository", kind="plan", candidate="0" * 40)
        self.refused("--candidate is required", kind="plan", candidate=None)

    def test_refuses_a_range_without_its_base_or_summary(self):
        for kind in ("round", "completion"):
            self.refused("--base is required", kind=kind, candidate=self.c3, summary="s")
            self.refused("not a commit in this repository", kind=kind, base="1" * 40, candidate=self.c3, summary="s")
            self.refused("is not an ancestor", kind=kind, base=self.c3, candidate=self.c1, summary="s")
            self.refused("--summary is required", kind=kind, base=self.c1, candidate=self.c3)
            self.refused("--summary is required", kind=kind, base=self.c1, candidate=self.c3, summary="  ")

    def test_refuses_a_reconciliation_without_its_commits_or_gates(self):
        gates = self.root / "build" / "g2.txt"
        gates.parent.mkdir(exist_ok=True)
        gates.write_text("ok\n", encoding="utf-8")
        self.refused("--failed is required", kind="reconciliation", candidate=self.c3, gates=str(gates))
        self.refused("full 40-character", kind="reconciliation", failed="abc", candidate=self.c3, gates=str(gates))
        self.refused("not a commit", kind="reconciliation", failed=self.c1, last="2" * 40, candidate=self.c3, gates=str(gates))
        self.refused("--gates is required", kind="reconciliation", failed=self.c1, candidate=self.c3)
        self.refused("readable, non-empty", kind="reconciliation", failed=self.c1, candidate=self.c3,
                     gates=str(self.root / "build" / "absent.txt"))
        empty = self.root / "build" / "empty.txt"
        empty.write_text("  \n", encoding="utf-8")
        self.refused("readable, non-empty", kind="reconciliation", failed=self.c1, candidate=self.c3, gates=str(empty))
        binary = self.root / "build" / "binary.txt"
        binary.write_bytes(b"\xff\xfe\x00bad")
        self.refused("readable, non-empty", kind="reconciliation", failed=self.c1, candidate=self.c3, gates=str(binary))

    def test_refuses_a_template_that_drifted_from_the_table(self):
        template = self.root / rp.TEMPLATES["plan"]
        original = template.read_text(encoding="utf-8")
        try:
            template.write_text(original.replace("<FULL_40_CHAR_HEAD_SHA>", "<HEAD_SHA>"), encoding="utf-8")
            self.refused("no longer contains", kind="plan", candidate=self.c3)
            template.write_text(original.replace("## What to check", "- Extra: `<a new field>`.\n\n## What to check"), encoding="utf-8")
            self.refused("unfilled placeholder", kind="plan", candidate=self.c3)
            template.write_text(original.replace("\n---\n", "\n"), encoding="utf-8")
            self.refused("no `---` line", kind="plan", candidate=self.c3)
        finally:
            template.write_text(original, encoding="utf-8")

    def test_command_line_on_the_real_tree(self):
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "plan.md"
            proc = subprocess.run([sys.executable, str(SCRIPTS / "review-prompt.py"), "--kind", "plan", "--ref", "D00 T05 §8",
                                   "--candidate", head, "--out", str(out)], cwd=REPO, capture_output=True, text=True, timeout=120)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            text = out.read_text(encoding="utf-8")
            self.assertIn("5. **Prerequisite order:**", text)
            self.assertIn("PLAN: complete|gaps", text)
            self.assertIn("`scripts/review-prompt.py`", text)
            self.assertIsNone(rp.PLACEHOLDER_RE.search(text))
            bad = subprocess.run([sys.executable, str(SCRIPTS / "review-prompt.py"), "--kind", "round", "--ref", "D00 T05 §8",
                                  "--candidate", head, "--out", str(Path(tmp) / "r.md")], cwd=REPO, capture_output=True, text=True, timeout=120)
            self.assertEqual(bad.returncode, 2)
            self.assertIn("refused: --base is required", bad.stderr)
            self.assertFalse((Path(tmp) / "r.md").exists())


if __name__ == "__main__":
    unittest.main()
