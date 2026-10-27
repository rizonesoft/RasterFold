---
name: process-todo-section
description: Process exactly one TODO section end to end -- resolve it, validate and fact-check the plan against the repository, correct every drifted claim, run the pre-build plan review, build it, run the gates, hand to review, and commit. Use whenever asked to process, implement, ship, continue, or finish a TODO section.
---

# Process TODO Section

The section is the contract, and a contract is checked before it is signed.

Processing a section is two jobs, in order. First establish that the plan is sound: internally consistent, still true of the code and of its governing documents, buildable as written, verifiable when built, and reviewed by the pinned reviewer (`--kind plan`) before any code is written. Then implement it. Do not improvise around the plan, and do not implement a plan you have found to be wrong. Correct it in the file, visibly, then build the corrected version.

## Use this skill when

- The user names a section **in any form**: a path and a section, a `DNN TNN §N` reference, or a row pasted straight out of `todo/implementation-plan.md`. Step 0 turns all of them into the same thing.
- An attended runner picks the next `[ ]` row from the plan.
- Do NOT use to audit already-shipped work: that is `review-todo-section` in audit stance.

## Step 0 -- resolve the argument before anything else

**Never hand-translate a reference into a filename.** Ask the graph:

```bash
python3 scripts/todo-graph.py resolve "$ARGUMENTS"
```

It prints the path, the section title, `surface` (`yes` or `no`), whether the TODO is frozen, and **which dependencies are unmet**. Its exit code is the instruction:

| Exit | Meaning | Do this |
| :-: | --- | --- |
| `0` | Resolved, open, dependencies met | Proceed to step 1 |
| `1` | No such TODO or no such section | Stop. Report the reference as unresolvable rather than guessing a near match. |
| `2` | No section reference in the input | Stop and ask which section. |
| `3` | The section is already `[x]` | Do **not** process it. This is `review-todo-section` in audit stance; say so and hand over. |
| `4` | Unmet dependencies | Stop at the dependency gate and name the unmet sections. |
| `5` | The section moved out of the tree (`> **Moved:**` under its heading) | Stop. Its open work is worked from the file the `moved` line names, by that file's own rules. |

Use the `skill arg` line it prints as the canonical form for the rest of the run, so the commit message and the review call name the section the same way.

**If `resolve` printed a `needs` line, verify that resource before writing `Started:`.** A Windows host gate from WSL uses `python3 scripts/windows.py --powershell build.ps1` and the Windows tests; a native Linux gate uses `python3 scripts/build.py`. Research comparisons need Windows ffmpeg with libjxl via the bridge in `docs/building.md`; merely having an unrelated ffmpeg on Linux is not that proof. Verify downloads before a network-bound experiment and keep the machine quiet for timing; record load and repeat under the research protocol. A missing target runtime stays an explicit unmet requirement, never a cross-build pass relabeled as execution.

**If `resolve` printed a `requires` line with requirements missing here, the section needs that environment.** Confirm it is reachable before writing `Started:`. If it is not, stop and say which capability the section waits on: starting an environment-bound section with no environment burns a session the same way.

**Write `Started:` now, at the first resolve.** If the section body carries no `> **Started:**` line, add one with the current UTC instant (`date -u +%Y-%m-%dT%H:%M:%SZ`). Do **not** overwrite an existing one on resume: review subtracts it from stamp time for `Duration:`, which is the whole working interval.

**Stop here if another writer holds the tree.** Check `git status` for unfamiliar uncommitted work you do not understand, and ask before building over it.

## Execution discipline

- **Validate before building.** Step 2 is a gate, not a formality. A section that cannot pass it gets fixed first.
- **Follow the corrected contract.** The checklist items define the scope. Do not widen because something adjacent looks wrong; file it instead with `add-todo`, whose admission test decides between an item on an open section, a new section (carrying its Origin line), and the backlog.
- **Commits follow logical steps.** A section ships as one logical change; plan corrections and `Started:` commit first so the pre-build `--kind plan` review reads the exact committed contract, and review fix-loop commits append to the section's candidate range (never amend).
- **Never mark `[x]` without evidence.** The Implementation Order row flips only through `python3 scripts/todo-graph.py stamp`, after review. This skill never flips a row itself.
- **Files are never damaged.** Output goes through `rf_write_atomic` (an exclusively created `.partial` file, flushed and published without replacing another destination), an existing destination is never overwritten, and every input is untrusted: the decoder bounds every size and count before using it and rejects rather than guesses. A section that touches reading or writing exercises the failure path in its Test checkpoint, not only the happy path.
- **The bitstream is frozen per version.** A change to what the encoder writes or the decoder accepts touches four places in one commit: `src/cm.c` (or `src/fold.c`), the independent oracle `tests/rfi_ref.py` written from the spec text, `spec/FOLD.md`, and the known-answer file `KAT_FOLD` in `tests/test_correctness.py`. A new coding method takes a new method number; a change to an existing method takes a new format version (`docs/research.md` section 2.2). The oracle is never translated line by line from the C: it would repeat the C's bugs.
- **Measure, never guess, and never tune to one image.** A compression or speed claim follows the protocol in `docs/research.md` sections 2.3 and 2.4: prototype in `build/research/try.c`, measure with `python3 scripts/windows.py research/ab.py` on the `photo`, `tp`, and `synth` sets, alternate A/B runs for speed, and state the thread counts. No rule, constant, or branch may look at the dimensions, hash, file name, or colour statistics of a particular image, and `measure/image.bmp` alone never decides anything.
- **Encode and decode are proven together.** The decoder is the reverse of the encoder: a section that changes one proves the round trip, byte identity with the oracle, identical output for one thread and three threads, and rejection of the malformed inputs it now has to refuse (`python3 tests/fuzz_decode.py`).
- **Output discipline:** bound every command (`build.ps1` output filtered to errors and warnings, `test_correctness.py -k` on the touched cases, `tail`/`head` on logs and on `ab.py -v` tables, field extraction on JSON). Full logs go under `build/` (gitignored).

## Workflow

### 1. Read the whole contract

Step 0 gave you the path, the section, and the dependency verdict. Read the entire TODO file, not just the section. The Goal, Current state, and Inputs carry context the section assumes. Read the sections this one depends on: their `Verified:` stamps and receipts tell you what actually shipped versus what was planned.

Confirm every dependency in the `Depends On` column is `[x]`. If one is not, stop and say so.

### 2. Validate the plan before building it

Check that the section still holds against the repository and its governing documents:

| Check | What you are asking | The failure it catches |
| ----- | ------------------- | ---------------------- |
| **Legitimacy** | Does a real source demand this: an observed defect, a measured gap against JPEG XL, the format specification, a paper or reference codec, a user need? | Work invented by the plan, built faithfully, wanted by nobody |
| **Currency** | Do the files, functions, constants, line numbers, command options, and measured ratios it names still exist and still hold? Re-run a quoted ratio before relying on it. | A section pinned to a path or count that moved |
| **Consistency** | Do its own items agree with each other, with the file's Current state block, and with the sections it depends on? | Two items specifying different things; the later one silently wins |
| **Correctness** | Are the behaviors, API names, and format details it states actually what the source says? | A stale behavior ported confidently into code |
| **Sufficiency** | Is there enough here to build without inventing? Are the decisions made, or deferred into the implementer's lap? | A section that becomes a design session for an unattended executor |
| **Verifiability** | Can the `Test checkpoint` be executed and can it fail? Does a `Freeze check` name fixtures that exist? | A checkpoint that passes by being unfalsifiable |
| **Accuracy** | Is every concrete claim still literally true: every reference, path, count, ID, and deferral? | A section that reads as authoritative while quietly citing things that moved |

Ground each answer:

```bash
python3 scripts/todo-graph.py validate          # graph integrity, XREF reciprocity
python3 scripts/todo-claims.py                  # the TODOs' measured claims still hold
python3 scripts/todo-graph.py query deferred    # deferrals this section owns or wrote
```

### 3. Correct the section when validation fails

Fix findings in the TODO file, before implementation, so the plan and the build never disagree in the record. Stale facts are corrected in place and marked `**Corrected YYYY-MM-DD:**` with what they said before and what the source says now. Contradictions resolve toward the source and the later decision. Items already true in the codebase are ticked with the XREF or commit SHA that shipped them. Genuinely wrong work is struck with a stated reason, never silently dropped. An unfalsifiable checkpoint is rewritten so it can fail. A **frozen** behavior that looks wrong is not changed in code or plan: record the question for the operator and continue around it.

### 3a. Plan review by the pinned reviewer (one pass, before any code)

Once the section is corrected and `Started:` is written, commit the TODO file (with `plan --sync` if any heading or deliverable moved) so the reviewer reads the exact contract you will build, then have the reviewer pinned for this writer judge whether that contract produces a 100% complete feature or codec change. Write the prompt to a scratch file under `build/` from `docs/plan-review-prompt.md`, then run:

```bash
python3 scripts/review-dispatch.py --kind plan --candidate <full SHA of that commit> --writer <this session's key: claude|muse|agy|codex> --prompt <scratch-plan-prompt.md> --record docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md
```

The dispatcher sets the effort (`medium`) and the `plan` lens from the pin's `[schedule] plan` step and appends a `## Plan review` section; it is not a round, it does not move the round schedule, and a findings file takes one returned plan review. Fix every `P1` and `P2` gap in the section the same way as step 3 (corrected in place, dated), and file a `P3` you do not take through `add-todo`. Under the plan review in the findings file, add one line per gap: `- Planned: <gap> in <commit>` or `- Filed: DNN TNN §N (<gap>)`. When the dispatcher exits 1, run the pass again before building. `stamp` requires this review as `--plan 'docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md'`.

### 4. Build the corrected contract

Work the checklist top to bottom. Tick each item as its Done-when becomes true, in the file, as you go.

- Count discipline: Dones carry per-item test deltas (which tests this item added, which cases it changed), never suite totals. Totals are quoted once, from the commit being created (step 7).
- Build the cheaper substitute's failure into the work: the checkpoint must be able to catch the wrong thing, so build the test that distinguishes them.
- Keep the diff to the section. Adjacent wrongness gets filed with `add-todo`, not fixed in passing.
- One copy of each mechanism: the range coder, the counters, and the quantizers live once and are shared between methods.
- The stack is fixed (`AGENTS.md`): C11 with a freestanding Win32 CLI/DLL and hosted Linux/Android libc platform layer, built by pinned local LLVM, Ninja and Android NDK; Python standard library plus Pillow for tests and research.

### 4a. Measure before porting (compression and speed sections)

A section whose deliverable is a size or speed change is an experiment first:

1. **Prototype in research.** Copy `src/cm.c` to `build/research/try.c` and make the change there. `python3 scripts/windows.py research/ab.py` compiles any `cm.c` variant that keeps the `rf_cm_encode` / `rf_cm_decode` interface.
2. **Measure by the protocol.** Size: `python3 scripts/windows.py research/ab.py size build/research/try.c photo`, then `tp` and `synth`, against the same commands on `src/cm.c`; quote `total`, `geomean`, and `worst`. Speed: `python3 scripts/windows.py research/ab.py ab src/cm.c build/research/try.c -n5 measure kodim01` at `-t1` and `-t3`; a minimum-time ratio below 0.97 is real, 0.97 to 1.03 is noise.
3. **Apply the section's stop condition.** If the change misses its bar, it ships as a negative result (`todo/README.md`, Completion-first): nothing is ported into `src/`, the numbers go into the stamp and into the "Already tried" table of `docs/research.md`, and the porting items are struck with a pointer to the stamp.
4. **Port only what passed.** Port into `src/cm.c`, then write the oracle change into `tests/rfi_ref.py` from the spec change, then `spec/FOLD.md`, then regenerate `KAT_FOLD` (`docs/research.md` section 2.2); a speed-only change must leave `KAT_FOLD` and every oracle comparison byte-identical.

### 5. Bitstream, surface, and feature completeness

- **Format sections:** Confirm the four places agree (`src/`, `tests/rfi_ref.py`, `spec/FOLD.md`, `KAT_FOLD`), `python3 tests/test_correctness.py` is green, old files decode or are refused as unsupported, and `python3 tests/fuzz_decode.py 300` reports no crash, hang, or oracle mismatch.
- **Surface sections (`surface: yes`):** Account for 100% of every control, menu item, dialog, and state on the Fidelity block and `**Design:**` specification: each is working or deferred to a named, resolving section (`python3 scripts/todo-graph.py resolve '<ref>'` must not exit 1 or 2).

### 6. Run the Test checkpoint, for real

Execute the checkpoint command and read the output. Quote the result in the commit body. If the checkpoint cannot run, the section is not done: record what ran, what did not, and why, and stop without a stamp.

Then run **every gate the section owes** (`.agents/skills/process-todo-section/gates.md`):

```bash
python3 scripts/check-all.py            # build, correctness suite, fuzz, TODO gates, tool self-tests
python3 tests/test_correctness.py       # the whole suite, not only the touched cases
python3 scripts/todo-graph.py validate
python3 scripts/todo-claims.py          # the TODOs' measured claims still hold
```

### 7. Commit, then hand to review

Commit the section work, recording the checkpoint evidence in the implementation commit body:

```text
<area>: <what now works> (<DNN TNN §N>)

<checkpoint output, quoted>
<plan corrections, if any>
```

Then invoke `review-todo-section` on the same ref. It runs the one-pass completion and excellence review first (`scripts/review-dispatch.py --kind completion`), then the adaptive review rounds (`scripts/review-dispatch.py --writer <this session's key>`), and stamps the section through `python3 scripts/todo-graph.py stamp`, which writes the receipt `docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.json`, flips the row, and syncs the plan. Commit the stamp, receipt, findings file, and regenerated plan as `todo: stamp <ref>`.

### 8. Report

Tell the user plainly: what was built, what the checkpoint proved (quoted), what the plan review, completion review, and adaptive review rounds found and what was done about each finding, what plan corrections were made, what was filed rather than fixed, and where the stamp stands.

## Guardrails

- Do not implement a section you judged wrong. Correct it or stop.
- Do not widen the section. File adjacent work.
- Do not flip the Implementation Order row or hand-write `Verified:` lines. `stamp` owns both.
- Do not claim a checkpoint passed without running it: quote the output.
- Do not change what the encoder writes without the oracle, the spec, and the known-answer file in the same commit, or without a new method number or format version.
- Do not port a compression or speed change that missed its stop condition, and do not tune a constant on `measure/image.bmp` alone.
- Do not end with the plan unsynced.
