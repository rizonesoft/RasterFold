# RasterFold's Codex automation

This is an independent Codex implementation of the repository's plan workflow. Skills, native hooks, controller, review tooling and intelligence are editable here without changing Claude's policy. It uses the existing checkout; no worktree isolation, snapshot chain or recovery checkpoint layer is created.

## Start here

Use Codex in WSL/Linux at the repository root. The native harness discovers skills through `.agents/skills/`, whose symlinks point only to `.codex/skills/`. Those are real private copies, not links into `.claude/`. All nine original lifecycle skills are present, plus `process-intelligence`.

```bash
python3 .codex/runner/campaign.py status
python3 .codex/runner/harness.py
python3 .codex/runner/panel_slots.py validate
python3 -m unittest discover -s .codex/tests -v
```

Invoke `$process-plan --audit` for a read-only audit or `$process-plan` to execute the plan. A named phase stays within that phase. Before starting, review the current native hook definitions in Codex `/hooks` and trust the ones you approve. `harness.py trust-check` reads native discovery and refuses missing, disabled or untrusted definitions. It never modifies trust, user configuration or credentials. Hook trust is a native Codex requirement, not an extra approval invented by the runner.

## What is isolated

| Concern | Codex | Claude |
| --- | --- | --- |
| Skills | `.codex/skills/` | `.claude/skills/` |
| Hook configuration | `.codex/hooks.json` | `.claude/settings.json` |
| Campaign lifecycle | `.codex/runner/campaign.py` | `scripts/campaign_guard.py` and `.claude/hooks/campaign-stop.ps1` |
| Review dispatch | `.codex/runner/panel_slots.py`, private `panel.toml` | `scripts/panel_slots.py`, `.conclave/panel.toml` |
| Review evidence tools | `.codex/runner/review_prompt.py` | `scripts/review_prompt.py` |
| Transient state | `.codex/state/` | `build/claude-campaign-*` |
| Intelligence code | `.codex/runner/intelligence.py` | `.claude/runner/intelligence.py` |
| Intelligence records | `.intelligence/codex/register.json` | `.intelligence/claude/register.json` |

Canonical TODO Markdown, format specifications, source code and committed review evidence belong to the project. Both families must satisfy those contracts. Do not remove transient ownership directories while a campaign is active; in particular, Claude retains its established guard files under `build/`. Each admission implementation is private; its small `.runner/admission.lock` and sibling ownership check are the shared handshake. Run both agent clients in the same WSL/Linux filesystem environment. This does not promise cross-host locking between independent Windows and WSL agent processes.

## Lifecycle guarantees

`start` requires a real session identity and an existing run file. It refuses any active, paused, parked or unreadable competing ownership record. Stop continuation applies only to the owning active session. Interrupt and explicit stop are terminal; SessionEnd is advisory. Pause and park retain ownership until explicit resume or stop. Type `resume the campaign` in the native session to grant one resume of its current generation; an agent cannot resume itself merely by supplying a reason. Stale queued heartbeats are blocked at prompt submission. Stop-hook errors report a diagnostic without creating an unbounded continuation loop. Every resume/phase transition changes the generation, so stale queued messages must be discarded.

Each stop or pause event is published as an immutable request before waiting for admission. Unhandled stop requests take precedence over pause, independent of wall-clock order. Requests are acknowledged by id and retained through resume; a new interrupt cannot be deleted by an older resume. `status` folds pending operator requests into the state it reports. A fresh start records its request boundary before native trust inspection, so an interrupt during that inspection cancels the start. Run ids use the canonical TODO Origin format of twelve lowercase hex digits; generation tokens remain independent full UUIDs.

Admission waits are bounded for both threads and processes. Source fingerprinting runs outside admission, followed by a fresh check of ownership, pending requests and tool activity. Phase advance rejects previously used filenames, filesystem identities including hard-link aliases, and copied closeout or park records for the current run. CLI activation and supervisor launch share one admission: launch failure parks the owner before releasing it. On inspection failure the supervisor stops queuing and retries contended admission until it can park a readable matching owner or observe that ownership changed. It never overwrites a terminal, replaced or malformed ownership record.

Pre/PostToolUse activity suppresses heartbeat nudges during an executing tool for up to thirty minutes. Run-log bookkeeping and intelligence captures do not count as source progress. The heartbeat supervisor queues a generation-bound message to the same Codex session every five minutes when there has been no intervening heartbeat. Two nudges without repository progress park the run; three unchanged Stop events also park it. Delivery failure records a reason and parks. No timer may turn a stopped run active. These checks prevent accidental continuation; they are not a security boundary against an agent with shell access to its own source and state. Only the supervisor may send generated campaign messages; never use `codex queue` to manufacture operator consent. Inspect `.codex/state/heartbeat.log` at section boundaries and report a dead supervisor instead of silently assuming recovery works.

The checked-in hooks do not start a campaign. Tests use disposable fixtures and fake queue delivery. Native trust/discovery and the real reviewer probe are separate from a live campaign proof.

For stopped maintenance, tool admission first checks both ownership records through the small admission module. When neither runner owns the checkout, it does not import the campaign controller. This keeps an in-progress controller edit repairable while preserving refusal on unreadable ownership. Add a dependency before importing it from another module, and run the lifecycle fixtures after changing either module. Active-owner controller failures still deny tool admission.

## Review and delivery

Codex work is reviewed by the private Claude slots; Claude work keeps its GPT slots. The Codex reviewer has no tools or MCP servers and receives an explicit candidate bundle on stdin. Provide enough source and contract context to support its verdict. Model, effort and timeout cannot be overridden at dispatch. Failures are outages, never invented approvals or same-family fallbacks.

Every new Codex stamp records `Implementer: Codex (<actual registered model>)` and a `Claude panel` heading. Historical stamps retain their original review rules. No existing stamp is transferred to this implementation. The source-only review of a dirty working tree is not a verified section stamp or remote CI evidence.

Private CI repair records live in the canonical run findings file, with three attempts per episode. See the [delivery procedure](skills/review-todo-section/delivery.md). No automatic push is authorized merely by starting Codex; the task's delivery scope still governs external actions.

## Intelligence

Capture a measured runner problem or useful lesson with an owner, source evidence and proposed correction. Both families may capture during their own run. Applying, rejecting, deferring or archiving requires both families to be stopped; unreadable records block maintenance. An applied record cites a reachable implementation commit. Archived entries remain in the same atomically replaced register, preserving ids and disposition history.

```bash
python3 .codex/runner/intelligence.py list
python3 scripts/intelligence.py validate
```

Use `$process-intelligence` at a stopped boundary. Changes to policy are deliberate reviewed work, not automatic rewriting based on speculative feedback.
