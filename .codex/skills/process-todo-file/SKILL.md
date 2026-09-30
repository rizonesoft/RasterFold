---
name: process-todo-file
description: Close out a TODO file whose sections are all shipped -- sweep for loose ends, run the file-level Verification block, reconcile deferrals, update the indexes, and set status to done. Use when every Implementation Order row is [x], or when asked to close a whole TODO file.
---

# Process TODO File

The last pass over a finished TODO. Its job is to catch what section-by-section work cannot see: the loose ends between sections, the deferrals nobody picked up, and the gap between "every row is `[x]`" and "this actually works end to end".

## Use this skill when

- Every row in the Implementation Order table is `[x]`.
- A runner reports a file exhausted.
- The user asks to close, finish, or graduate a TODO.
- A phase closeout names the file for a sweep while rows remain open: run steps 1-4 for the shipped rows only, skip step 5, and leave `status` untouched.

Do NOT use it to force a file closed. If sections remain open, they get implemented or explicitly deferred with owners: not swept.

## Workflow

### 1. Confirm the file is actually exhausted

```bash
python3 scripts/todo-graph.py validate
```

Every row `[x]`, every `[x]` covered by a `Verified:` stamp, zero FATALs. If a row is `[x]` without a stamp, the validator says so: go fix that first. In sweep-only mode (phase closeout with open rows remaining), the shipped rows carry stamps instead, and step 5 is skipped: the file keeps its status until exhaustion.

### 2. Run the file-level Verification block

The `## Verification` section is not decoration. Run every item in it and record the real output. This is the only place the file is checked as a whole rather than section by section, and it is where integration gaps surface. In sweep-only mode, a Verification item the shipped rows cannot satisfy is recorded as not-yet-runnable with the owning open section named, never executed speculatively and never trimmed silently.

For a code TODO that means the **full** sweep, not the filtered runs individual sections used (the gate card is `.codex/skills/process-todo-section/gates.md`):

```powershell
python3 scripts/check-all.py             # build, correctness suite, fuzz, TODO gates
python3 tests/test_correctness.py        # the whole suite, once, unfiltered
python3 tests/fuzz_decode.py 5000        # the long fuzz run, with its seed quoted
python3 scripts/windows.py tests/measure_final.py           # the JPEG XL reference comparison (PASS)
```

plus the file's other Verification items (the corpus sizes re-measured with `python3 scripts/windows.py research/ab.py size` on `photo`, `tp`, and `synth`, `python3 scripts/windows.py tests/benchmark.py -n 3` rerun and `benchmarks/results.md` committed where the file moved size or speed, the documented measurement record updated), each executed, none trimmed (in sweep-only mode: executed where the shipped rows allow, otherwise recorded not-yet-runnable with its owner).

### 3. Loose-end sweep

Read the whole file with fresh eyes and check:

- **Stubs and TODOs in the code.** Grep the touched source paths for `TODO`, `FIXME`, `HACK`, `XXX`, `NotImplementedException`, and any commented-out block left during the work. Each is either finished now, or filed through `add-todo`: an owning section and an XREF when it passes the admission test (with its Origin line when a campaign files it), otherwise a backlog entry and the stub removed.
- **Partial items.** Any checklist item ticked when only part shipped. Split it.
- **Orphaned deferrals.** Every `Deferred:` line in every stamp must name a live owner. Confirm the target section still exists and is still open. A deferral pointing at a section that shipped without addressing it is a hole. A deferral never points at the backlog: an entry there has no owner, so work that must ship gets a section through `add-todo` (admission test and Origin line for work a campaign found), and an idea that need not ship becomes a backlog entry with the deferral removed.
- **One-sided XREFs.** The validator warns on these; resolve rather than ignore.
- **Integration reality.** Is the change reachable where a user would use it? The encoder chooses the new method or setting by itself (or through a documented option), `inspect` reports it, `README.md` and `spec/FOLD.md` describe it, and `research/bench.c` and `tests/benchmark.py` measure it. A method the encoder never picks is not done, and neither is an option nothing reads.
- **Distribute separately.** Does each app that consumes this file's work still build, publish, and run on its own, with no runtime dependency on another app?

### 4. Reconcile the frozen contract

If the file is `frozen: true`:

- Every `**Freeze check:**` in it ran and passed, with the result in a stamp.
- No frozen behavior moved. If one did, the operator approval is linked in the relevant stamp.
- Fixtures under `tests/fixtures/` are committed, not left in a scratch directory, and the check ran against a disposable copy rather than the developer's own documents or library.

### 5. Update status and indexes

- Set `status: done` in the frontmatter.
- Move the file's row in the domain `INDEX.md` from the TODOs table to a **Completed** section, with the completion date.
- Update `todo/TODO-00-INDEX.md` if the file appeared under Active TODOs.
- If the work is now better documented elsewhere, link that document rather than leaving a large stale checklist behind.

### 6. Commit

```
<domain>: complete TODO-NN -- <one-line summary of what now works>

<full-suite evidence>
<deferrals carried forward, with their owners>
```

Then sync the plan: `python3 scripts/todo-graph.py plan --sync`.

### 7. Report

Tell the user plainly:

- What now works, in their terms: not a list of section titles.
- What was deferred and who owns it.
- Anything found in the sweep that became a new TODO, section, or backlog entry, how many of the new sections are discovered (Origin line) and under which run id, any backlog entry promoted or merged (with its merge marker), and the budget line (`python3 scripts/todo-graph.py query budget`) after it.
- Any gate that did not run, and why.

## Guardrails

- Do not close a file with open sections. Implement or explicitly defer with an owner. Sweep-only mode never sets `status: done`.
- Do not close a file whose deferrals point nowhere.
- Do not claim the Verification block passed without running it: quote the output.
- Do not delete section detail on closure. The shipped TODO is the record of how it was built.
- Do not set `status: done` on a `frozen: true` file whose freeze checks did not run.
