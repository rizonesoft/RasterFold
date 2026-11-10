---
name: review-todo-section
description: Quality-gate a just-implemented TODO section with self-review, one combined adversarial round, a final completion review, and reconciliations if the completion returns gaps (up to 33 review passes), then write the Verified stamp through the stamp command. Also runs in audit stance on shipped sections. Use after process-todo-section or when asked whether a section is really done.
---

# Review TODO Section

The gate between "code exists" and "the row says `[x]`". Nothing else may flip that row, and the flip happens only through `python3 scripts/todo-graph.py stamp`, which refuses a section with unticked items, an open prerequisite, a waiting Need, or no review evidence.

Review is mandatory and independent: the implementing session self-reviews, and every external pass comes from the reviewer pinned for that writer in `.conclave/reviewers.toml`, run through `scripts/review-dispatch.py`. Codex work goes to Gemini 4 Carbon through the Antigravity CLI; Claude Code, Antigravity (`gemini`), Muse Code and OpenCode work goes to GPT-6 Astra through the Codex CLI. Each route has one reviewer of another family and no fallback. The shared `[schedule]` is the three-pass policy, every pass at `medium`: the plan review before the build (`process-todo-section` runs it), then here one adversarial round whose lenses are `adversarial`, `integration`, `source-defect`, `plan-delivery`, `consistency`, `record`, `quality`, `optimization` and, when `--surface` is passed, `design`, then one completion review of the final candidate. A section whose plan review's candidate has an older pin stays on that pin's schedule and route to its stamp. Every pass and round is recorded in a findings file (`docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md`); the stamp's receipt (`docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.json`) names them. A verdict with no record is an opinion.

## Use this skill when

- A section was just implemented (`process-todo-section` ends by invoking this).
- The user asks "is §N really done?", "re-verify §N", "audit §N": run in **audit stance** (below).
- Before closing a TODO file with `process-todo-file`.

## Step 0 -- resolve the argument

Same front door as `process-todo-section`: `python3 scripts/todo-graph.py resolve "$ARGUMENTS"`. Note whether `surface` is `yes` or `no` (when `surface` is `yes`, pass `--surface` to `scripts/review-dispatch.py` on every round). Exit `3` means the row is already `[x]`: that is the signal for audit stance, not an error.

## The three questions

Every review answers these with code and behavior evidence, never assertion:

1. **Does it make sense?** Does the implementation produce the exact outcome the section asked for, without unnecessary scope?
2. **Is it logical?** Do the encoder and decoder, the C code and the oracle, the spec and the known-answer file, the measurements and their claims, the threading contracts, and the failure paths agree with each other?
3. **Did it break anything else?** Inspect callers, consumers, tests, and adjacent workflows that read what changed.

## Workflow

### 1. Fix the candidate

Name the candidate: the implementation commit, or the commit range (`<BASE_SHA>..<TIP_SHA>`) when the section landed across multiple logical step commits or when fix-loop commits append (commits are never amended once review starts). Record the full SHA(s) in the findings file at `docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md`. For an audit, the candidate is the section's own stamped commit plus every later change to the files it owns, up to today's tip (`<STAMPED_SHA>..HEAD -- <owned paths>`). Read the actual diff (`git show` or `git diff <BASE_SHA>..<TIP_SHA>`), not memory of writing it. Map each ticked micro-step checklist item to the code and test that satisfy it; an item with no corresponding change or a partially met `Done when:` is not done (untick it) or was already true (note where). On a stamped section (audit stance) edit nothing in the section by hand: record the item in the findings file, and when it is a regression, run `unstamp` first.

### 2. Self-review

Work the three questions across the diff and its blast radius. Fix what you find, then re-ask them. Look specifically for the failure modes this codebase invites:

- A decoder that trusts a field: a size, count, or stream length used before it is bounded, a read past a buffer on a truncated stream, an allocation sized by an unchecked product, a loop whose count comes from the file without a limit.
- An encoder change without its oracle, spec, and known-answer change in the same commit, a new method without a new method number, or a changed method without a new format version.
- An oracle change translated line by line from the C instead of written from the spec: it repeats the C's bugs and proves nothing.
- Output that differs between one thread and three threads, a data race on the cross-channel rows or the progress counters, or a thread that can wait forever when another channel fails.
- A write that bypasses `rf_write_atomic`, overwrites an existing destination, or leaves a `.partial` file behind on failure.
- A measurement that cannot be trusted: tuned on `measure/image.bmp` alone, one timing run on a busy machine, a size ratio quoted without the set, total, geometric mean, and worst image, or a thread count left unstated.
- A rule, constant, or branch that looks at the dimensions, hash, file name, or colour statistics of a particular test image.
- A dependency outside the platform contract in `AGENTS.md`: Windows CLI imports stay within kernel32/shell32 and its DLL within kernel32; Linux/Android use their declared libc/pthread runtime. Check all relevant targets with `python3 tests/test_artifacts.py`.
- A frozen behavior that moved, or a test that passes without exercising the behavior.

### 3. The adversarial round (one pass)

After the build and its gates, the pinned reviewer reviews the candidate once through every lens the schedule gives. Write the prompt to a scratch file under `build/` from `docs/review-round-prompt.md` (candidate SHA, section contract, governing documents), then run it with `--writer <this session's key>` (adding `--surface` when `resolve` printed `surface    yes`):

```bash
python3 scripts/review-dispatch.py --candidate <full candidate SHA> --writer <this session's key: claude|codex|gemini|muse|opencode> [--surface] --prompt <scratch-review-prompt.md> --record docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md
```

The dispatcher appends a `## Round 1` section naming the reviewer, its model, effort, and lenses, or `none` with the reason when the reviewer returned no review. Headings that start `## Round`, `## Completion review`, `## Plan review` or `## Reconciliation review` are reserved for what `--record` writes: a writer's own note in a findings file uses another heading (for example `## Writer note: ...`), or the file stops being readable and the next review is refused with the offending line named.

| Lens | Asks |
| ---- | ---- |
| `adversarial` | How does this fail on hostile input, races, aborts, cancellation, automation or links? |
| `integration` | Do callers and consumers still hold, with every contract (threads, ownership, errors) kept? |
| `source-defect` | Is the upstream specification or `spec/FOLD.md` read accurately, with deviations noted? |
| `plan-delivery` | Did the build deliver every checklist item and `Done when:`, with nothing partial, stubbed or narrowed? |
| `consistency` | Does it match the module rules, naming, shared components, settings and sibling code? |
| `record` | Does the evidence match what ran, with owned deferrals and an earned flip of every item? |
| `quality` | Is the code clear, minimal and maintainable, and does every test exercise the behaviour it names? |
| `optimization` | Where does it waste time, memory, allocations or I/O, and what measured target does it miss? |
| `design` | Does the rendered UI match the section's `**Design:**` specification and its tokens? (`--surface` sections) |

There is no second round: the dispatcher refuses one. Verify every finding against the code, fix what is real in new commits appended to the candidate, record why anything else is not real under a `## Writer note:` heading, and file genuine follow-up through `add-todo`. Round verdict for the receipt: `clean` (the round ended `OVERALL: approve` and no fix followed) or `fixed` (it ended `OVERALL: needs-attention` and its findings were dealt with). When the dispatcher exits 1 (the reviewer returned no review), run the round again; an outage is not a review, and a session-only pass is not a substitute.

### 4. Re-run the gates, then the completion review (one pass)

After the last fix, re-run the section's Test checkpoint and its owed gates (`python3 scripts/check-all.py`, `python3 tests/test_correctness.py`, `python3 scripts/todo-graph.py validate`, `python3 scripts/todo-claims.py`, and the Freeze check in frozen files). Quote the outputs. A red gate reopens the candidate: fix it and re-run to green before the completion review.

Then the pinned reviewer reviews that exact final candidate against the contract, the fixes the round led to, and the strongest reference codecs and tools. Write the prompt to a scratch file under `build/` from `docs/completion-review-prompt.md` (the candidate SHA, the section contract, the TODO file's Goal and Outcome, the governing spec and design sections, the full diff, the round's findings and what was done with each), then run it:

```bash
python3 scripts/review-dispatch.py --kind completion --candidate <full candidate SHA> --writer <this session's key: claude|codex|gemini|muse|opencode> --prompt <scratch-completion-prompt.md> --record docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md
```

The review ends `COMPLETION: complete` or `COMPLETION: gaps`, and a findings file takes one returned completion review. The stamp must name this exact commit, so nothing lands on the candidate after it except on the reconciliation route. File each `out-of-scope` item through `add-todo`; those alone do not force gaps. When the dispatcher exits 1, run the pass again.

**On `COMPLETION: gaps`, the stamp is blocked.** The failed review stays on record. Implement every in-scope gap in new commits, without changing what the section promises, add one line per gap under the completion review (`- Implemented: <gap> in <commit>`), re-run the gates, then run a reconciliation pass on the repaired candidate, from `docs/reconciliation-review-prompt.md`:

```bash
python3 scripts/review-dispatch.py --kind reconciliation --candidate <full repaired SHA> --writer <this session's key: claude|codex|gemini|muse|opencode> --prompt <scratch-reconciliation-prompt.md> --record docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md
```

It ends `RECONCILIATION: complete` or `RECONCILIATION: gaps`. On `RECONCILIATION: gaps`, repair those gaps the same way (new commits, `- Implemented:` lines, gates rerun) and run another reconciliation on the new candidate; do not stop to ask the operator. The dispatcher allows reconciliations until one returns complete or the section reaches 33 returned review passes in all (plan, round, completion and reconciliations; `REVIEW_PASS_CAP` in `scripts/review-dispatch.py`), and only a section at the cap stays blocked and is reported. The stamp refuses a reconciliation if the section's contract changed since the failed completion.

### 5. Measurement, bitstream, surface, and frozen checks

- **Format and measurement sections:** Confirm the four places agree (`src/`, `tests/rfi_ref.py`, `spec/FOLD.md`, `KAT_FOLD`), old files decode or are refused cleanly, and every size/speed figure was re-run from its command on `photo`, `tp`, and `synth` with totals, geometric means, worst images, and thread counts stated. A compression or speed section that missed its stop condition stamps as a negative result with its row in `docs/research.md`'s "Already tried" table and nothing ported into `src/`.
- **Surface sections (`surface: yes`):** 100% of every control, menu item, dialog, and state on the Fidelity block is working or deferred to a named, resolving section.
- **Release sections:** Every backlog entry was promoted into a section or carries `reviewed: <tag> <YYYY-MM-DD> deferred by operator: "<words>"`.
- **Frozen files:** Every `**Freeze check:**` ran and passed with the result quoted.

### 6. Stamp

Tick nothing by hand. Confirm every checklist item in the section is `[x]`, then run:

```bash
python3 scripts/todo-graph.py stamp 'DNN TNN §N' --sha <the SHA the last review read> --plan 'docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md' --completion 'docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md:complete' --review '1:docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md:fixed' --check '<checkpoint command> -> <quoted result>'
```

One `--plan` (required, naming the section's own findings file), one `--completion` (required: `:complete`, or `:reconciled` after a passed reconciliation), one `--review` naming the round (`1:<findings>:<clean|fixed>`), and one `--check` per gate actually run. `stamp` replays the `[schedule]` of the pin at the plan review's candidate through `scripts/review-dispatch.py`'s `scheduled()`, refuses any pass whose effort or lenses differ from what the schedule gave it, and under the three-pass policy refuses anything but plan, one round, then completion, on candidates in that order, with the last review on the stamped commit. A section on an older pin stamps under that pin's rules (`:completed`, `:filed`, its round loop, `stopped-at-33`, `stopped-low-severity`). A section carrying `> **Restarted:**` (see `todo/README.md`) stamps only when its line names the recorded plan review, that review was in the findings file at the restart commit, the stamped commit descends from the restart, and the returned round recorded a `Candidate:` (dispatched with `--candidate`) that descends from the restart and is the stamped commit or an ancestor of it. On an older pin, a `stopped-at-33` or `stopped-low-severity` verdict adds `--cap-outcome '<what was done>'`. `stamp` writes the receipt `docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.json`, flips the Implementation Order row to `[x]`, appends the `Verified:` and `Review:` lines, marks the file `done` when its last section stamps, and syncs `todo/implementation-plan.md`. Commit the stamp, receipt, findings file, and regenerated plan together as `todo: stamp <ref>`.

### 7. Audit stance

On an already-`[x]` section: run steps 1, 2, the round in step 3, and step 5 against the section's own candidate and receipt, changing nothing in the section itself until `unstamp` has reopened it. Record the audit's rounds in a findings file of their own, `docs/reviews/<NN-domain>/audit-d<NN>-t<NN>-s<N>.md` (`--record` that path), never in the section's stamped findings file. Confirm the stamp's evidence still holds (re-run the checkpoint) or find the regression. The only permitted row change is `[x]` back to `[ ]`, and only through `python3 scripts/todo-graph.py unstamp '<ref>' --reason '<what regressed; repair -> XREF: <ref>>'`, which removes the stamp lines, unticks the row, reopens the file, keeps the receipt as `docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.revoked-<date>.json`, and adds a dated `Unstamped:` note.

## Guardrails

- Do not stamp without a findings file, its one plan review, its one adversarial round, and its one completion review that passed, directly or through its last reconciliation.
- Do not stamp an unaccounted control on a surface section, a skipped format duty, or a moved frozen behavior.
- Do not hand-write `Verified:` lines, hand-flip rows, or edit a receipt. `stamp` owns the record.
- Do not review the working tree when the candidate is a commit or range. Name the hashes.
- Do not transfer evidence across changed candidates: a new tip re-runs its gates.
