---
name: process-phase
description: Attended runner that takes one phase of todo/implementation-plan.md to 100% -- repair the phase, gap-check it, then ship section after section via process-todo-section plus review-todo-section, parking only when every leftover row is blocked or runnable-elsewhere in this context. Use when the user says process, run, or finish a phase.
---

# Process Phase

One phase, start to 100%, or parked when the rest of it is blocked or runnable-elsewhere here. You do not stop in between.

**Exactly three endings.** Zero open rows and a written closeout. Every leftover row blocked or runnable-elsewhere here, so the phase is **parked** and `process-plan` moves to the next ready phase. Or the operator's own pause. There is no fourth, and a parked phase is neither complete nor a stall.

The whole plan is `process-plan`, not this skill. This skill is one named phase. When the session entered through `process-plan` with no phase argument, return to it after closeout or park so it can start the next ready phase; a session pinned to one phase ends here.

The user is present but is not the engine. Talk to them when something genuinely needs them; never wait on them for anything you can decide, verify, or fix yourself.

**Completion-first never buys completion with a bypass.** `--no-verify`, `--amend`, and force-push are forbidden to this run. If a gate refuses, the run fixes the cause. It does not push, and it does not pause: a red gate is work. A failed push is a red gate, not a skip: diagnose, fix, retry. Never leave an unpushed stack on the theory that CI will catch up later.

**Attended means interruptible, not stoppable.** When the user sends a message mid-run, answer it briefly and continue the loop in the same turn. The one exception outranks everything: **if the user tells you to stop or pause the run, obey immediately**, confirm, and wait. Their instruction beats completion-first, always. Stopping or pausing deletes the run guard first, so no heartbeat resumes against the operator's instruction; resume recreates it before any other step.

## Step 0 -- open the run

Check that no other writer holds the tree (`git status`, and ask about unfamiliar uncommitted work). Then open the run's findings file: `docs/phase-runs/<YYYY-MM-DD>-phase-<N>.md` (create `docs/phase-runs/` if absent). **Every finding this run produces is appended there the moment it is made, not at the end**: the file survives session death where chat scrollback does not, and it is what the user reads during and after the run. Structure:

```markdown
# Phase run: <heading>

## Phase repairs              (Step 1: what was wrong with the plan, what was corrected, where)
## Shipped-row verification   (Step 1b: each [x] row -- stamp ok / checkpoint re-run result / audit outcome)
## Gap audit                  (Step 2: gaps found, where each was filed, rows pulled in; the park record lands here)
## Sections                   (Step 3: one entry per section -- ref, outcome, review verdict, corrections)
## Critical events            (every stop, pause, resume, and session death)
## Lessons                    (what was learned this run, worth keeping)
```

Read the most recent prior file in `docs/phase-runs/` for this phase, if one exists: anything unresolved there is this run's first input.

Record the run's start commit as the first Critical events line, `- run start: <git rev-parse HEAD>`, and the phase's line from `python scripts/todo-graph.py query budget`. The start commit is what the run's growth report counts from. Every section the run files on its own (the gap audit, the review research passes, the plan-review rounds, and every other filing it makes unasked) carries `**Origin:** discovered run=<this run's id> <YYYY-MM-DD>` directly under its heading, the id being the guard's `run_id`, so the closeout can show what the run added. Nothing caps how many (operator decision 2026-09-27: "the cap is worrying me, because I'm worried features will be left behind."). Sections the operator asks for mid-run are operator-directed: no Origin line.

Run guard: if this session entered through `process-plan`, the plan owns the guard; verify the guard file `build/claude-campaign-guard.json` names this session and the heartbeat job exists (`CronList`), and record the check, but do not create a second. If pinned to this phase standalone, start the guard exactly as the `process-plan` skill specifies (Stop hook, guard file, heartbeat), with this phase's run file.

## Step 1 -- repair the phase before running it

The phase table is a plan, and plans drift. Fix it before building on it. In order:

1. `python scripts/todo-graph.py validate`: fix every FATAL now.
2. `python scripts/todo-graph.py plan --check`: if stale, `plan --sync`.
3. For EVERY open row in the phase: `python scripts/todo-graph.py resolve '<ref>'`. Record the exit code.
   - Exit 4 with unmet deps **outside this phase** is a **leftover, not a stall**. Leave the row here, ship every exit-0 runnable-now row, and park when only leftovers remain. Do not drag a later phase's dependency into this one, and do not loop back hoping the answer changes.
   - A row whose `resolve` verdict is runnable-elsewhere **in this context** is a leftover, not a shippable row, whatever the exit code: it stays visible, never ships here, and parks with the rest when only leftovers remain. Re-run `resolve` rather than trusting a previous verdict.
   - Exit 1/2: the row cites a section that does not exist: repair the reference against the TODO file. A broken ref is repairable work, so it blocks a park.
4. Read each open section's TODO file top to bottom, looking for **phase-level** staleness only (per-section validation happens again inside `process-todo-section`): sections whose work already shipped elsewhere, sections made moot by a decision since, callouts whose blocker no longer exists. Correct with dated `**Corrected YYYY-MM-DD:**` notes.

### Step 1b -- shipped rows are verified, not trusted

`[x]` rows in the phase are claims, and a claim is checked. At run start, re-check shipped rows that an OPEN row in this phase depends on: the dependency spine the new work builds on. For each spine row:

1. Confirm a `Verified:` stamp covers it (`resolve` exits 3 and the stamp names the section).
2. Re-run its `Test checkpoint` command if it names one. A checkpoint that no longer passes means the section regressed after shipping: treat it as this phase's work (diagnose, fix forward, re-review with `review-todo-section` in audit stance).
3. If anything about the implementation looks wrong against today's source, invoke `review-todo-section` in audit stance on that section. It either re-confirms the row or downgrades it to `[ ]`, and a downgraded row rejoins the loop like any other.

## Step 2 -- gap-audit the phase

Read the phase as a user would use it, end to end, and ask what is missing: a format change with no oracle or spec work, a speed change with no size check, a decoder path with no rejection case, handoffs between domains nobody specified. File each gap with `add-todo` (with evidence and an owner), which applies the admission test and stamps the Origin line: only a defect in shipped or in-flight work, something an acceptance-bar aim requires, or a prerequisite of a planned row becomes a section, and nice-to-haves default to the backlog. An admission-test pass lands as an item on an open section that can hold it, or as a new section with its Origin line; there is no overflow, because there is no cap. The Gap audit log records each filing with its ref or backlog id. Pull any new rows into the phase table where they belong, and sync the plan. A gap found is a gap filed the same turn: the audit that only lists gaps in chat has not audited.

## Step 3 -- ship the phase, one row at a time

In table order, for each open row: `process-todo-section`, then `review-todo-section`. Record each outcome in the findings file's Sections log. After each stamp, sync the plan, and read the run's growth at the section boundary: `python scripts/todo-graph.py query growth --since <run start>`, with its summary line (sections added, discovered ones by run, backlog entries added and removed) recorded in the Sections log entry. A backlog entry the run removed must have left by promotion, merge marker, or operator record, or `validate` is already red (`backlog-dropped`). When the section is a release (it pushes an app tag), its backlog review item runs like any other: every backlog entry for that app or `suite` (every entry for an `isotone` release) is promoted into a section through `add-todo`, or the operator is asked whether it may wait and the answer is recorded on the entry as `-- reviewed: <tag> <YYYY-MM-DD> deferred by operator: "<their words>"`; an attended run asks, and an unattended run parks with the question rather than deciding, because `validate` refuses the stamp while an entry is unreviewed (`release-backlog-unreviewed`). After each stamp, also check the heartbeat as `process-plan` states (`campaign_guard.py health` and `expiry` against `CronList`), replacing a missing or expiring job before the next section starts. Commit per section; push per the two-push discipline (ship push, then stamp push). After every push, `python scripts/review_prompt.py ci-wait <pushed head> --since <remote head before the push>` reads CI back (a push whose paths miss the workflow's filter reads `not triggered`) and the run file records its line; the next section waits for green, because every later section would otherwise build on it. A red read-back is a failed gate, and the run repairs it rather than waiting on it: `ci-wait` prints the failing job and step and a bounded excerpt of the failed log, the run diagnoses from that evidence, fixes the cause forward in a repair commit (or, when the red is the just-stamped section's own change, reopens that section through audit stance), pushes through this same block, and reads CI back again. Every repair commit has an owner: a red the just-stamped section caused is repaired under that section, reopened; a red caused anywhere else is repaired under a section of its own, the reopened owner of the broken code or a new section filed through `add-todo` with a checkpoint and review, and the repair commit's message names that section, never an unowned commit. A reopened section is repaired, re-reviewed, and re-stamped through this skill before anything continues, and a red on a SHIP push makes the repair commit the new candidate: its checkpoint, independent review, and panel run against the repaired head, never the pre-repair sha, because evidence never transfers across a changed candidate. On green, with any reopened section re-stamped, it continues with the next section in the same turn. The repair is bounded per episode: an episode runs from the first red to the next green, and it gets at most three repair attempts per repair episode across its consecutive reds, so a new red never resets the count; the run file numbers each attempt within the episode (`attempt N of 3`) with its commit and its `ci-wait` line, and the count persists across restarts in `build/claude-campaign-repair.json`: every repair push first runs `python scripts/campaign_guard.py repair attempt --red <red sha> --commit <repair sha> --workflow <workflow> --run-file <run file>`, which binds the episode to its repository, branch, workflow, and run file, counts a repeated commit once, and refuses a fourth attempt (exit 1: escalate); the green read-back runs `repair close --green <sha> --workflow <workflow> --run-file <run file> --evidence "<the green ci-wait line>"`, which needs that line on a descendant of the last attempt; and a resumed runner reads `repair status --run-file <run file>` before its next repair, restoring a lost episode file with `repair restore`, and a unit patched in three consecutive attempts is rethought rather than patched again. `ci-wait` prints a red's `cause:` line, correlated with the failing step (a platform signal and a repository error in different steps read `cause: unknown`), and the cause decides, not the step's name. An unknown cause owes bounded evidence gathering before any repair or escalation: the run page (`gh run view <id>`), the full log, and one local re-run of the printed command; if the cause is still unknown after those three, the run escalates it as a cause the tree cannot establish: a failing repository-controlled step (the workflow file, a pinned action, a setup script, the repository's own commands) is repairable even when it failed during job setup, and a red whose failed-step log cannot be fetched is diagnosed from the full log, or by re-running locally at the pushed commit the step command `ci-wait` prints. The run escalates to the operator only for a cause the tree cannot fix: a runner or platform fault (`cause: platform fault`, such as a lost runner or a GitHub outage), a read-back still unverifiable after `ci-wait`'s one retry (GitHub unreachable, `gh` missing or unauthenticated, runner quota; a run still queued or in progress is waited on to its ceiling first, because a slow run is not an unreachable one; `ci-wait` exit 3 means it stayed pending past the ceiling, so run `python scripts/campaign_guard.py repair ceiling --run-id <GitHub run id>` and re-run `ci-wait` once; the second exit 3 on the same run is refused there, a queue outside the tree: escalate), or the repair bound exhausted; the run file records the cause before the report, and the report is the only stop. An operator-authorized workflow retirement or trigger change reads back with `ci-wait --expect-no-run <reason> --authorized-by <who> --authorized-at <UTC time> --approved-range <base>..<head>`, which exits 4 `NOT GREEN` on silence and is never evidence for `repair close`; and a record quotes `ci-wait` only as printed, its secret shapes already masked. The report ends the run first: a column-0 `PARKED <UTC> run=<run id> escalation: <cause>` line in the run file, then `python scripts/campaign_guard.py end --session $CLAUDE_CODE_SESSION_ID --reason escalation` and `CronDelete` of the heartbeat it names, so the Stop hook lets the report turn end and no heartbeat resumes against it.

Skip rows whose `resolve` is not exit 0 or whose verdict is runnable-elsewhere here, and re-check them after each stamp: the graph moves as rows flip. When every remaining open row is exit 4 (or otherwise unshippable here), the phase parks: write the park record (each leftover, what blocks it, where the blocker lives), led by its column-0 `PARKED <UTC> run=<run id> <reason>` line (the guard's `run_id`), commit the findings file, and if pinned standalone end the guard (`python scripts/campaign_guard.py end --session $CLAUDE_CODE_SESSION_ID --reason park`, then `CronDelete` the job it names) and record its deletion. Then return to `process-plan` (or end, if pinned).

## Step 4 -- closeout

When the table is all `[x]`, close the files before closing the phase: for every TODO file with a row this phase shipped, invoke `process-todo-file`. It runs the file-level Verification block, sweeps loose ends, reconciles deferrals, and sets `status: done` where the whole file is exhausted; a file with rows in later phases gets the sweep and keeps its status. A finding the sweep produces is filed before the closeout, never carried silently.

Then re-run the full suite once, confirm the plan shows the phase complete, write the closeout under a `## Closeout run=<run id>` heading (what shipped, what was repaired, what was learned, which files closed, and the growth: the phase's sections and discovered sections from `query budget`, the run's growth line from `query growth --since <run start>` with its discovered sections by run, and every backlog id the run added, promoted, or merged), commit, delete the guard if pinned standalone (the plan deletes it when chained), and report. A phase is complete when its table says so, its touched files are closed out, and the closeout is written: not before.

## Guardrails

- Do not invent a side loop that ships rows outside `process-todo-section` plus `review-todo-section`.
- Do not tick `implementation-plan.md` by hand. Sync it.
- Do not claim a phase complete while its table has `[ ]` rows.
- Do not call a parked phase complete, and do not call it a stall.
- Do not end the turn on the audit. Ship, park, or close out.
- Do not leave a run guarded after it ends, and do not pause with the guard live: stop deletes first, resume recreates.
- Do not file a discovered section without its Origin line, and do not change `todo/budget.json`.
- Do not delete a backlog entry except by promotion, a merge that keeps its key, or the operator's recorded words, and do not stamp a release section before its backlog review is recorded.
