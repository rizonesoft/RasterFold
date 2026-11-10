#!/usr/bin/env python3
"""Fill a review pass's prompt from its template, for one TODO section.

Every section takes a plan review, one adversarial round and a completion
review (and a reconciliation after gaps), and each pass is sent a prompt built
from its template under docs/. Building it is mechanical: the section
reference, the TODO file and its line range, the candidate commit or range,
the files involved and the findings file all come from the graph and from git.
This script does that and nothing else, so a pass is always sent the whole
template: it never shortens the text, and it never limits how much the
reviewer may reply.

    python3 scripts/review-prompt.py --kind plan --ref 'D00 T05 §8' --candidate <SHA>
    python3 scripts/review-prompt.py --kind round --ref 'D00 T05 §8' --base <SHA> --candidate <SHA> --summary '<what it is meant to do>'
    python3 scripts/review-prompt.py --kind completion --ref 'D00 T05 §8' --base <SHA> --candidate <SHA> --summary '<what was built>'
    python3 scripts/review-prompt.py --kind reconciliation --ref 'D00 T05 §8' --failed <SHA> --last <SHA> --candidate <SHA> --gates <file>

It prints the prompt's path, then the review-dispatch.py command that sends
it. The two places a template asks for the writer's own words come from
--summary. Anything it cannot fill is a refusal (exit 2, nothing written),
never a placeholder left for the reviewer to puzzle over.
"""
from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = {
    "plan": "docs/plan-review-prompt.md",
    "round": "docs/review-round-prompt.md",
    "completion": "docs/completion-review-prompt.md",
    "reconciliation": "docs/reconciliation-review-prompt.md",
}
SHA_RE = re.compile(r"[0-9a-f]{40}")
PLACEHOLDER_RE = re.compile(r"<[^<>`\n]*>")
REF_RE = re.compile(r"D(\d{2})\s+T(\d{2})\s+§(\d+)")
PATH_TOKEN_RE = re.compile(r"`([A-Za-z0-9_.\-/]+)`")


class Refused(Exception):
    """A prompt that cannot be filled completely is not written."""


def git(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, timeout=60)


def commit(root: Path, name: str, value: str | None) -> str:
    if not value:
        raise Refused(f"{name} is required for this kind")
    if not SHA_RE.fullmatch(value):
        raise Refused(f"{name} must be a full 40-character commit: {value!r}")
    if git(root, "cat-file", "-e", f"{value}^{{commit}}").returncode != 0:
        raise Refused(f"{name} is not a commit in this repository: {value}")
    return value


def resolve(root: Path, ref: str) -> tuple[str, str]:
    """The section's canonical reference and TODO path, from todo-graph.py resolve (the graph's own front door)."""
    proc = subprocess.run([sys.executable, str(root / "scripts" / "todo-graph.py"), "resolve", ref],
                          cwd=root, capture_output=True, text=True, timeout=120)
    # 3 (already stamped) and 4 (unmet dependencies) still name a real section; 1, 2 and 5 do not.
    if proc.returncode not in (0, 3, 4):
        reason = (proc.stderr or proc.stdout).strip().splitlines()
        raise Refused(f"cannot resolve {ref!r}: {reason[-1] if reason else f'exit {proc.returncode}'}")
    fields = dict(line.split(None, 1) for line in proc.stdout.splitlines() if len(line.split(None, 1)) == 2)
    if "path" not in fields or "ref" not in fields:
        raise Refused(f"cannot resolve {ref!r}: resolve printed no path")
    return fields["ref"].strip(), fields["path"].strip()


def show(root: Path, candidate: str, path: str) -> str:
    """A file as the candidate commit holds it. Every field of the prompt describes the candidate, so the TODO file
    and the template are read from its tree, never from a working tree that may be ahead of it or edited."""
    proc = git(root, "show", f"{candidate}:{path}")
    if proc.returncode != 0:
        raise Refused(f"{path} does not exist at candidate {candidate}")
    return proc.stdout


def section_span(text: str, todo: str, number: int) -> tuple[int, int, list[str]]:
    """First and last line (1-based) of `## <number>. ...` in the TODO file's text, and its lines."""
    lines = text.split("\n")
    heads = [i for i, line in enumerate(lines) if line.startswith("## ")]
    for k, i in enumerate(heads):
        if lines[i].startswith(f"## {number}. "):
            end = heads[k + 1] if k + 1 < len(heads) else len(lines)
            return i + 1, end, lines
    raise Refused(f"{todo} has no section {number}")


def named_paths(tree: frozenset[str], text: str) -> list[str]:
    """Repository paths a block of TODO text names in backticks, in first-seen order: files in the candidate's tree,
    plus paths the section says it will create (a file extension, under a top-level directory the tree has)."""
    tops = {path.split("/", 1)[0] for path in tree if "/" in path}
    seen: list[str] = []
    for token in PATH_TOKEN_RE.findall(text):
        token = token.rstrip(".")
        if token in seen or token.startswith(("-", "/")) or ".." in token:
            continue
        if token in tree or ("/" in token and re.search(r"\.[A-Za-z0-9]{1,6}$", token) and token.split("/", 1)[0] in tops):
            seen.append(token)
    return seen


def inputs_block(lines: list[str]) -> str:
    try:
        start = lines.index("## Inputs")
    except ValueError:
        return ""
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    return "\n".join(lines[start:end])


def fill(kind: str, body: str, values: dict[str, str]) -> str:
    """Substitute every placeholder of the kind's template. Each entry is (exact template text, replacement); a
    template that no longer carries one of them has drifted from this script, which is a refusal, not a guess."""
    v = values
    if kind == "plan":
        table = [
            ("<DNN TNN §N>", v["ref"]),
            ("<todo/.../TODO-NN-....md>", v["todo"]),
            ("<start>-<end>", v["span"]),
            ("<FULL_40_CHAR_HEAD_SHA>", v["candidate"]),
            ("`<paths>`", v["files"]),
            ('`<spec/FOLD.md, docs/research.md, docs/design/... or "none">`', v["governing"]),
        ]
    elif kind == "round":
        table = [
            ("`<FULL_SHA>` or `<BASE_SHA>..<TIP_SHA>`", f"`{v['range']}`"),
            ("`<one or two sentences>`", v["summary"]),
            ("<DNN TNN §N>", v["ref"]),
            ("`<the spec, plan or design file that applies, e.g. spec/FOLD.md, docs/research.md>`", v["governing"]),
        ]
    elif kind == "completion":
        table = [
            ("<DNN TNN §N>", v["ref"]),
            ("`<FULL_SHA>` or `<BASE_SHA>..<TIP_SHA>`", f"`{v['range']}`"),
            ("`<two or three sentences naming the files, symbols, format paths, CLI flags, or UI controls>`", v["summary"]),
            ("`<the design doc or plan file that applies>`", v["governing"]),
            ("<the section's findings file>", v["findings"]),
        ]
    else:
        # The template spells the failed completion's candidate and the repaired candidate identically (`<FULL_SHA>`),
        # so each is substituted together with the words of its own line, never by the bare placeholder.
        table = [
            ("<DNN TNN §N>", v["ref"]),
            ("<docs/reviews/<domain>/<slug>.md>", v["findings"]),
            ("The failed completion's candidate: `<FULL_SHA>`", f"The failed completion's candidate: `{v['failed']}`"),
            ("The repaired candidate: `<FULL_SHA>`", f"The repaired candidate: `{v['candidate']}`"),
            ("<COMPLETION_SHA>..<REPAIRED_SHA>", f"{v['failed']}..{v['candidate']}"),
            ("<LAST_SHA>..<REPAIRED_SHA>", f"{v['last']}..{v['candidate']}"),
            ("<LAST_SHA>", v["last"]),
            ("`<quoted output>`.", v["gates"]),
            ("`<the design doc or plan file that applies>`", v["governing"]),
        ]
    if kind == "round":
        # The template's own instruction: delete the lines that do not apply. A section's round is not a standalone
        # review, and its earlier passes are already in the findings file the dispatcher records into.
        body = "\n".join(line for line in body.split("\n") if not line.startswith("- Findings file, for a standalone review"))
    # Placeholders are a property of the template, so they are counted before any value goes in: a summary or a
    # quoted gate line may legitimately contain angle brackets.
    residue = body
    for needle, _ in table:
        if needle not in body:
            raise Refused(f"{TEMPLATES[kind]} no longer contains {needle!r}: update scripts/review-prompt.py with the template")
        residue = residue.replace(needle, "")
    left = PLACEHOLDER_RE.findall(residue)
    if left:
        raise Refused(f"unfilled placeholder(s) in {TEMPLATES[kind]}: {', '.join(sorted(set(left)))}")
    # One pass over the template, so text a value brings in is never itself substituted.
    pattern = re.compile("|".join(re.escape(needle) for needle, _ in sorted(table, key=lambda pair: -len(pair[0]))))
    lookup = dict(table)
    return pattern.sub(lambda match: lookup[match.group(0)], body)


def build(args: argparse.Namespace, root: Path = ROOT) -> tuple[Path, str]:
    match = REF_RE.search(args.ref)
    if not match:
        raise Refused(f"--ref must look like 'D00 T05 §8': {args.ref!r}")
    ref, todo = resolve(root, args.ref)
    domain, number = todo.split("/")[1], int(match.group(3))
    candidate = commit(root, "--candidate", args.candidate)
    tree = frozenset(git(root, "ls-tree", "-r", "--name-only", candidate).stdout.split("\n"))
    start, end, lines = section_span(show(root, candidate, todo), todo, number)
    slug = f"d{match.group(1)}-t{match.group(2)}-s{number}"
    findings = args.findings or f"docs/reviews/{domain}/D{match.group(1)}-T{match.group(2)}-s{number}.md"
    section_text = "\n".join(lines[start - 1:end])
    governing_paths = [p for p in named_paths(tree, inputs_block(lines)) if p in tree]
    governing = ", ".join(f"`{p}`" for p in [todo, *[p for p in governing_paths if p != todo]])
    values = {"ref": ref, "todo": todo, "span": f"{start}-{end}", "candidate": candidate, "findings": findings,
              "governing": governing}

    if args.kind == "plan":
        files = args.files.split() if args.files else named_paths(tree, section_text)
        values["files"] = ", ".join(f"`{p}`" for p in files) if files else "none named in the section"
    elif args.kind in ("round", "completion"):
        base = commit(root, "--base", args.base)
        if git(root, "merge-base", "--is-ancestor", base, candidate).returncode != 0:
            raise Refused(f"--base {base} is not an ancestor of --candidate {candidate}")
        if not (args.summary or "").strip():
            raise Refused("--summary is required: the template asks what the candidate is meant to do in the writer's words")
        values["range"] = f"{base}..{candidate}"
        touched = git(root, "diff", "--name-only", f"{base}..{candidate}").stdout.split()
        summary = args.summary.strip()
        values["summary"] = (summary + ("" if summary.endswith(".") else ".") + " Files the range touches: "
                             + (", ".join(f"`{p}`" for p in touched) if touched else "none"))
    else:
        values["failed"] = commit(root, "--failed", args.failed)
        values["last"] = commit(root, "--last", args.last or args.failed)
        if not args.gates:
            raise Refused("--gates is required: a file holding the quoted gate output on the repaired candidate")
        try:
            gate_text = Path(args.gates).read_text(encoding="utf-8").strip()
        except (OSError, UnicodeDecodeError):
            gate_text = ""
        if not gate_text:
            raise Refused(f"--gates does not name a readable, non-empty file: {args.gates}")
        values["gates"] = "\n\n```text\n" + gate_text + "\n```\n"

    template = show(root, candidate, TEMPLATES[args.kind])
    if "\n---\n" not in template:
        raise Refused(f"{TEMPLATES[args.kind]} has no `---` line separating the instructions from the prompt body")
    text = fill(args.kind, template.split("\n---\n", 1)[1].lstrip("\n"), values)
    out = Path(args.out) if args.out else root / "build" / "review-prompts" / f"{slug}-{args.kind}.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text, encoding="utf-8")
    shown = out.relative_to(root).as_posix() if out.is_relative_to(root) else str(out)
    kind_flag = "" if args.kind == "round" else f" --kind {args.kind}"
    command = (f"python3 scripts/review-dispatch.py{kind_flag} --candidate {candidate} --writer <claude|codex|gemini|muse|opencode>"
               f" --prompt {shlex.quote(shown)} --record {shlex.quote(findings)}")
    return out, command


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kind", required=True, choices=tuple(TEMPLATES))
    parser.add_argument("--ref", required=True, help="The section, as 'DNN TNN §N'")
    parser.add_argument("--candidate", required=True, help="The full 40-character commit under review")
    parser.add_argument("--base", help="round, completion: the commit before the section's first commit (the range is base..candidate)")
    parser.add_argument("--summary", help="round, completion: what the candidate is meant to do, or what was built, in the writer's words")
    parser.add_argument("--failed", help="reconciliation: the failed completion review's candidate")
    parser.add_argument("--last", help="reconciliation: the last reviewed candidate (default: --failed)")
    parser.add_argument("--gates", help="reconciliation: a file holding the quoted gate output on the repaired candidate")
    parser.add_argument("--files", help="plan: the files the section expects to touch, space-separated (default: the paths its text names)")
    parser.add_argument("--findings", help="The findings file (default: the section's own under docs/reviews/)")
    parser.add_argument("--out", help="Where to write the prompt (default: build/review-prompts/<slug>-<kind>.md)")
    args = parser.parse_args(argv)
    try:
        out, command = build(args, ROOT)
    except Refused as exc:
        print(f"review-prompt: refused: {exc}", file=sys.stderr)
        return 2
    print(out.relative_to(ROOT).as_posix() if out.is_relative_to(ROOT) else out)
    print(f"next: {command}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
