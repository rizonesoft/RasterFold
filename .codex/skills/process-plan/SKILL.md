---
name: process-plan
description: Audit or execute RasterFold's implementation plan through the private Codex controller. Use when asked to process the plan; --audit never arms a run, and a named phase stays within that phase.
---

# Process the plan with Codex

Read `AGENTS.md`, `todo/README.md`, `.codex/README.md` and `.intelligence/codex/register.json`. Work in this checkout. Claude and Codex have separate controllers and policy; only one campaign may own the tree. Do not create worktrees, snapshots or a checkpoint recovery stack.

## Audit and route

Run `git status -sb`, `python3 .codex/runner/campaign.py status`, then the graph's `validate`, `plan --check`, `query ready`, `query blocked` and `query budget`. Preserve unrelated working changes. Fix plan defects before arming. Record actual outputs in the run findings file; never tick the derived plan by hand. `--audit` reports and stops without creating state. A named phase invokes `$process-phase` with phase scope and never chains.

A request to execute the plan starts its first runnable phase in the same turn and continues through subsequent runnable phases. Blocked and runnable-elsewhere rows stay visible; no work is silently dropped or substituted.

## Arm the native lifecycle

Create `docs/phase-runs/<date>-codex-phase-<N>.md` with phase, starting commit, graph audit, critical events and sections log. Use the actual native `$CODEX_THREAD_ID`; do not invent a session id. Verify hook discovery with `python3 .codex/runner/harness.py trust-check`. If exact project hooks are untrusted, the native `/hooks` review is required by Codex; explain that requirement and keep the campaign stopped until the user trusts them. Never bypass trust.

```bash
python3 .codex/runner/campaign.py start --session "$CODEX_THREAD_ID" --phase <N> --run-file <run-file> --scope plan
```

Record the returned run id and generation. `start` admits one owner, creates private state and launches a heartbeat supervisor. Stop hooks continue only the owning active session. Three unchanged Stop events park it. The supervisor uses `codex queue` for the same session, checks generation, and parks after two unanswered nudges or a delivery failure. This is bounded recovery, not a license to resume after an operator stop. A generated message is discarded unless its session, run and generation still match active state.

At each section boundary, inspect controller status and `.codex/state/heartbeat.log`; an active state with a dead supervisor is a defect to diagnose. Keep observations as intelligence evidence. Use `$process-todo-section` then `$review-todo-section`, one logical candidate range per section, and record gates/reviews/CI as actually observed. Never review Codex work with a Codex slot.

## Advance or stop

Each phase ends with `## Closeout run=<id>` when its table is complete, or `PARKED <UTC> run=<id> <blockers>` when the remaining rows cannot run here. A parked phase is not completed work. For a plan run, inspect readiness again; when another phase is ready, create its run file and execute:

```bash
python3 .codex/runner/campaign.py advance --session "$CODEX_THREAD_ID" --phase <next> --run-file <next-file> --reason 'previous phase closed or parked; next ready phase'
```

For terminal completion, record the matching closeout heading and use `complete --reason '<measured outcome>'`. The controller checks the heading and, for plan scope, that readiness reports zero runnable rows. Completion of the campaign can include named blocked rows; it never means those rows are verified. A fully shipped plan additionally owes fresh clean-machine acceptance for the Windows/Linux executables and libraries and both Android ABIs in the release domain.

An explicit operator stop first writes a durable request, even if admission is busy, and takes effect immediately with `stop --reason '<operator words>'`; a pause uses `pause`. Native Interrupt stops the owner too. A parked or paused run retains ownership and requires a native operator `resume the campaign` prompt; `resume --reason '<instruction>'` consumes that native grant and rechecks hook trust and launches a new generation. Stop releases ownership. Never delete state files manually or adopt another session.

When genuinely blocked, record the specific cause and use `park --reason '<cause>'`, or `stop` when ending the campaign. A missing reviewer, failed external gate or exhausted repair episode is not a success. Follow [delivery.md](../review-todo-section/delivery.md) for CI repair bounds. Every disposition of intelligence waits until both families are stopped.
