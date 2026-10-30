---
name: review-todo-section
description: Quality-gate a just-implemented TODO section with self-review, completion review, adaptive independent review rounds, and a bounded fix loop, then write the Verified stamp through the stamp command. Also runs in audit stance on shipped sections. Use after process-todo-section or when asked whether a section is really done.
---

# Review TODO Section

The gate between "code exists" and "the row says `[x]`". Nothing else may flip that row, and the flip happens only through `python3 scripts/todo-graph.py stamp`, which refuses a section with unticked items, an open prerequisite, a waiting Need, or no review evidence.

Review is mandatory and independent: the implementing session self-reviews, and every external pass comes from the reviewer pinned for that writer in `.conclave/reviewers.toml`, run through `scripts/review-dispatch.py`. Codex work goes to Muse Spark 1.3; Claude Code, Muse Code, Antigravity, OpenCode and Copilot CLI work goes to GPT-6 Astra. Each route has one reviewer of another family and no fallback. The shared `[schedule]` owns each pass's reasoning effort and lenses: plan and completion run at `medium`, round 1 at `high` (`adversarial`, `integration`), round 2 at `medium` (`adversarial`), round 3 at `low` (`adversarial`), and every later round at `low` with one lens in rotation (`consistency`, `design` when `--surface` is passed, `record`). A `low` round that declares `HIGHEST: P1` makes the next round the `medium` adversarial escalation. Every pass and round is recorded in a findings file (`docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md`); the stamp's receipt (`docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.json`) names them. A verdict with no record is an opinion.

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

### 3. Completion and excellence review (one pass)

Before the regular rounds, once per section, the pinned reviewer reviews the built feature or codec change against the contract and the strongest reference codecs / tools. Write the prompt to a scratch file under `build/` from `docs/completion-review-prompt.md` (the candidate SHA, the section contract, the TODO file's Goal and Outcome, the governing spec/design sections, the delivered diff), then run it:

```bash
python3 scripts/review-dispatch.py --kind completion --candidate <full candidate SHA> --writer <this session's key: claude|muse|agy|codex|opencode|copilot> --prompt <scratch-completion-prompt.md> --record docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md
```

The dispatcher appends a `## Completion review` section (reviewer, model, candidate SHA), not a round: it is not counted against the cap, and a findings file takes one returned completion review. The review ends `COMPLETION: complete` or `COMPLETION: gaps`.

Then, before any regular round, implement every `missing-in-scope` gap and every `in-scope` excellence item in this section (new commits appended to the candidate, the section's checklist and gates covering them), and file every `missing-out-of-scope` gap and `out-of-scope` item through `add-todo` as a new section (or backlog entry per the admission test). Under the completion review in the findings file, add one line per gap: `- Implemented: <gap> in <commit>`, or `- Filed: DNN TNN §N (<gap>)` for each section filed (the stamp reads these lines and refuses a ref that does not resolve), and add a `> **Deferred:** <what> -> XREF: DNN TNN §N` line to the section when its surface shows the filed item. The stamp takes `--completion 'docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md:complete'` (no gaps), `:completed` (every gap implemented in the section) or `:filed` (some gaps filed as new sections, each on a `- Filed:` line). When the dispatcher exits 1 (the reviewer returned no completion review), run the pass again; the regular rounds wait for it.

### 4. Independent review rounds (the pinned reviewer)

The dispatcher picks each round's lenses and effort from the pin's schedule, at least one round by the pinned independent reviewer. Write the prompt to a scratch file under `build/` from `docs/review-round-prompt.md` (candidate SHA, section contract, governing documents), then run it with `--writer <this session's key>` (adding `--surface` when `resolve` printed `surface    yes`):

```bash
python3 scripts/review-dispatch.py --writer <claude|muse|agy|codex|opencode|copilot> [--surface] --prompt <scratch-review-prompt.md> --record docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md
```

The dispatcher appends a `## Round N` section naming the reviewer, its model, effort, and lenses, or `none` with the reason when the reviewer returned no review. Headings that start `## Round`, `## Completion review` or `## Plan review` are reserved for what `--record` writes: a writer's own note in a findings file uses another heading (for example `## Writer note: ...`), or the file stops being readable and the next review is refused with the offending line named.

| Lens | Asks |
| ---- | ---- |
| `plan` | Will the section as written produce a 100% complete feature? (plan review only) |
| `adversarial` | How does this fail on hostile input, races, aborts, cancellation, automation or links? |
| `integration` | Do callers and consumers still hold, with every contract (threads, ownership, errors) kept? |
| `consistency` | Does it match the module rules, naming, shared components, settings and sibling code? |
| `design` | Does the rendered UI match the section's `**Design:**` specification and its tokens? (`--surface` sections) |
| `record` | Does the evidence match what ran, with owned deferrals and an earned flip of every item? |

The loop is bounded: at most 33 rounds (owner decision 2026-10-24; a round that returns no review does not count), and it ends early on severity: on the first round with no Critical (`P1`) or Important (`P2`) finding (`HIGHEST: none` or `HIGHEST: P3` before `OVERALL:`), the loop stops (`severity_stop: true` in the JSON output when `HIGHEST: P3`), any `P3` findings are filed through `add-todo`, and a `P3`-only final round stamps with `stopped-low-severity` and a `--cap-outcome` naming what was filed. Findings are fixed in new commits appended to the candidate, gates re-run, and the next round runs against the new tip. A unit patched three rounds running is stopped and re-thought instead of patched again. Round verdicts for the receipt: `clean` (no findings), `fixed` (findings found and fixed in a later round), `stopped-low-severity`, or `stopped-at-33`. When the dispatcher exits 1 (the reviewer returned no review), record the outage and stop without a stamp: a session-only pass is not a substitute.

### 5. Re-run the gates

After the last fix, re-run the section's Test checkpoint and its owed gates (`python3 scripts/check-all.py`, `python3 tests/test_correctness.py`, `python3 scripts/todo-graph.py validate`, `python3 scripts/todo-claims.py`, and the Freeze check in frozen files). Quote the outputs. A red gate reopens the candidate: fix, re-run to green, and run the next review round on the repaired tip.

### 6. Measurement, bitstream, surface, and frozen checks

- **Format and measurement sections:** Confirm the four places agree (`src/`, `tests/rfi_ref.py`, `spec/FOLD.md`, `KAT_FOLD`), old files decode or are refused cleanly, and every size/speed figure was re-run from its command on `photo`, `tp`, and `synth` with totals, geometric means, worst images, and thread counts stated. A compression or speed section that missed its stop condition stamps as a negative result with its row in `docs/research.md`'s "Already tried" table and nothing ported into `src/`.
- **Surface sections (`surface: yes`):** 100% of every control, menu item, dialog, and state on the Fidelity block is working or deferred to a named, resolving section.
- **Release sections:** Every backlog entry was promoted into a section or carries `reviewed: <tag> <YYYY-MM-DD> deferred by operator: "<words>"`.
- **Frozen files:** Every `**Freeze check:**` ran and passed with the result quoted.

### 7. Stamp

Tick nothing by hand. Confirm every checklist item in the section is `[x]`, then run:

```bash
python3 scripts/todo-graph.py stamp 'DNN TNN §N' --sha <full candidate tip SHA> --plan 'docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md' --completion 'docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md:complete' --review '1:docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md:clean' --check '<checkpoint command> -> <quoted result>'
```

One `--plan` (required, naming the section's own findings file), one `--completion` (required: `:complete`, `:completed`, or `:filed`), one `--review` per returned round (`<round>:<findings>:<verdict>`), and one `--check` per gate actually run. `stamp` replays the pin's `[schedule]` through `scripts/review-dispatch.py`'s `scheduled()` and refuses any pass whose effort or lenses differ from what the schedule gave it. A section carrying `> **Restarted:**` (see `todo/README.md`) stamps only when its line names the recorded plan review, that review was in the findings file at the restart commit, the stamped commit descends from the restart, and at least one returned round was recorded after the restart; a completion review recorded before the restart stays valid. With a `stopped-at-33` or `stopped-low-severity` verdict add `--cap-outcome '<what was done>'`. `stamp` writes the receipt `docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.json`, flips the Implementation Order row to `[x]`, appends the `Verified:` and `Review:` lines, marks the file `done` when its last section stamps, and syncs `todo/implementation-plan.md`. Commit the stamp, receipt, findings file, and regenerated plan together as `todo: stamp <ref>`.

### 8. Audit stance

On an already-`[x]` section: run steps 1, 2, and 4 to 6 against the section's own candidate and receipt, changing nothing in the section itself until `unstamp` has reopened it. Record the audit's rounds in a findings file of their own, `docs/reviews/<NN-domain>/audit-d<NN>-t<NN>-s<N>.md` (`--record` that path), never in the section's stamped findings file. Confirm the stamp's evidence still holds (re-run the checkpoint) or find the regression. The only permitted row change is `[x]` back to `[ ]`, and only through `python3 scripts/todo-graph.py unstamp '<ref>' --reason '<what regressed; repair -> XREF: <ref>>'`, which removes the stamp lines, unticks the row, reopens the file, keeps the receipt as `docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.revoked-<date>.json`, and adds a dated `Unstamped:` note.

## Guardrails

- Do not stamp without a findings file, its one plan review, its one completion review, and at least one independent round.
- Do not stamp an unaccounted control on a surface section, a skipped format duty, or a moved frozen behavior.
- Do not hand-write `Verified:` lines, hand-flip rows, or edit a receipt. `stamp` owns the record.
- Do not review the working tree when the candidate is a commit or range. Name the hashes.
- Do not transfer evidence across changed candidates: a new tip re-runs its gates.
