# The unattended run

One double-click finishes RasterFold to 100%: every open TODO section shipped, reviewed, and stamped, the terminal release acceptance re-run, and the plan closed. This file is the contract the launcher and the run both follow.

## How it starts

The Windows Desktop shortcut `RasterFold Unattended Run.lnk` opens Windows Terminal into this WSL checkout and runs `scripts/unattended-run.sh`, which:

1. Runs `python3 scripts/unattended-preflight.py`. Any FAIL stops the launch with the reason on screen (the terminal stays open). Nothing starts against a dirty tree, a stale plan, a missing reviewer, or a low disk.
2. Starts Muse straight on `main` (never a branch, never a worktree) with `--yolo --disable-request-user-input --model muse-wonder-full-1.0 --reasoning-effort high` and the Spark/Ember agent definitions in `.agents/muse-agents.json`, seeded with the prompt below.

## The seed prompt (verbatim in unattended-run.sh)

> Unattended RasterFold campaign. First create the session goal: "Finish RasterFold 100% complete and excellent: every open TODO section built, gated, reviewed through the pinned three-pass schedule, stamped, and committed straight on main; then the terminal release acceptance passes and the plan closes. Best lossless RGB codec in its class on the research corpus, with no placeholders." Then load the process-plan skill and run it end to end: audit, start the campaign guard and the in-session heartbeat (muse.cron_create, 3-59/5 * * * *, canonical prompt from the skill), and ship section after section through process-todo-section plus review-todo-section until query ready prints 0 runnable now. Never ask the operator a question: where a section leaves a choice open, take the industry-standard option, record the decision and its reversal cost in the section, and carry on. Delegate near-mechanical lookups and gate runs to rf-spark-scout, exact lead-specified edits to rf-spark-hands, and verbatim literal work to rf-ember-literal; verify every line a subagent produces before it lands, and keep all judgment, codec work, reviews, stamps, and commits on the lead model. A commit is not a stop. A red gate is repaired at its owning layer, never bypassed. Stop only at plan closeout, a recorded park where every leftover row is blocked or runnable-elsewhere, or an escalation the tree cannot fix.

## Why it actually runs unattended

- `--yolo` disables approval and the sandbox for the run and trusts the workspace, so no tool call waits on a human. `--disable-request-user-input` withholds the question tool entirely: the run cannot park on a question. The AGENTS.md unattended rule covers the rest: standard option, recorded decision, carry on.
- The run guard is the campaign guard plus the in-session cron heartbeat from `process-plan`: a stalled or crashed turn is resumed inside the same session with full context, and six resumptions with no tree change trip the stall breaker instead of burning tokens forever. Muse has no Stop hook; the guard file plus heartbeat carry it, exactly as the skill maps.
- Reviews never wait on the operator: `scripts/review-dispatch.py` runs the pinned reviewer (muse routes to Astra via the Codex CLI) through plan, adversarial round, completion, and reconciliations up to the 33-pass cap per section.
- Every commit passes the local hooks (staged-tree TODO validate, plan audit, drift) with no `--no-verify`, straight on `main`, one section per logical candidate range.

## Spark and Ember, without losing quality

`.agents/muse-agents.json` defines three subagents, loaded by the launcher through `--agents`:

| Agent | Model | Work it may take | Work it may never take |
| --- | --- | --- | --- |
| `rf-spark-scout` | muse-spark-1.3 | Read-only searches, counts, inventories, claim re-measurement, one named gate run quoted verbatim | Any edit, judgment, review, stamp |
| `rf-spark-hands` | muse-spark-1.3 | An exact lead-written edit spec (rename, table sync, sanctioned regeneration) outside codec logic | Codec or format changes, stamps, commits, improvising past a mismatch |
| `rf-ember-literal` | muse-ember-1.0 | Verbatim string replacement, one exact command returned verbatim, list reshaping | Anything needing interpretation |

The quality rule is the lead's, not the subagent's: the lead (Wonder) reads every diff a Spark hand produces and independently re-runs or spot-checks every scout fact that a decision rests on, exactly as `.agents/skills` already require of cheaper implementers. A subagent that hits a decision point replies `NEEDS-LEAD` and stops. Before a new class of mechanical work is delegated, the lead replays it once on a known-answer input; a mismatch keeps that class on the lead.

## Stopping and resuming

- Operator stop in words: `python3 scripts/campaign_guard.py end --reason operator-stop`, delete the heartbeat job, done (process-plan spells the order).
- Resume: double-click the shortcut again. Preflight refuses while a guard from a live run exists; a dead run's guard is taken over only through `acquire --handover <reason>` as the skill requires, never by deleting files.
