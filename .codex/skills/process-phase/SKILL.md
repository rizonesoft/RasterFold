---
name: process-phase
description: Execute one RasterFold phase through dependency-safe sections and independent reviews, with the Codex run guard. A standalone phase does not chain to another phase.
---

# Process one phase

Read `todo/README.md`, the target phase and `$process-plan`'s lifecycle. If the plan owns an active run, reuse that owner and its run file. Otherwise audit and start the private controller with `--scope phase`. Do not create a second supervisor or use Claude's cron/guard commands.

Before implementing, resolve every open row using `python3 scripts/todo-graph.py resolve '<ref>'`. Read its complete section and dependencies. Reconcile stale claims, missing prerequisites and acceptance-bar gaps through `$add-todo` or `$create-todo`; no silent skip or substitute. Record discovered work with `**Origin:** discovered run=<id> <date>`; operator-directed work carries no discovered quota. Validate and synchronize the plan after edits.

In table order, select only runnable-now rows whose dependencies are satisfied. Invoke `$process-todo-section`, then `$review-todo-section`. Record the actual checkpoint, candidate range, independent findings, panel decisions and run costs. Keep mutations serial. A format change owes spec, oracle, known-answer and C code together. An unsuccessful experiment records its negative result instead of porting a losing change.

After each stamp, run graph validation, `plan --sync`, `query ready` and `query growth --since <start-commit>`. Inspect controller status and heartbeat log. Follow [delivery.md](../review-todo-section/delivery.md) for authorized pushes and CI readback. No later section builds on unresolved red CI. A release row reviews every backlog item under the operator's recorded decisions; the agent cannot invent a deferral.

When every phase row is verified, write `## Closeout run=<id>` with outcomes and evidence. When all remaining rows are blocked or runnable elsewhere, write `PARKED <UTC> run=<id> <specific blockers>`. Commit the canonical run evidence through the normal section lifecycle. If plan-scoped, return to `$process-plan` for the next phase. If standalone, end the controller with `complete` only for a completed phase, or `park` with its reason. Operator stop instructions take effect before any closeout work.

## Declared native contexts

The default readiness query is conservative. For a native section, first record successful host/display/device probes in the run file, then use `python3 scripts/todo-graph.py query ready --context <capability> ...` with only the capabilities actually demonstrated. Re-probe at the section boundary and retain the section's full native checkpoint. The declaration is routing information, never proof or operator approval. Never add `operator` to an agent context; unresolved operator rows remain for the owner.
