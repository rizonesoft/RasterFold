# Claude agent routing

Claude sessions in this repository run as a lead plus a delegated fleet. The lead is the writer pinned in `.conclave/panel.toml` (Opus) and stays accountable for architecture, integration, consequential review, commits, and final acceptance. Bounded research, implementation, testing, documentation, and routine pre-review go to the agents in `.claude/agents/`, which run on the `sonnet` alias at explicit `effort: high`. The alias follows the newest Sonnet that Claude Code serves; the probe record below proves which model and effort actually ran. Codex keeps its own mechanics (`.codex/README.md`) and never reads this file.

## Routing table

`scripts/agent_routing.py validate` checks every row against the agent's frontmatter, so this table cannot drift from what runs.

| Agent | Model | Effort | Class | Use it for |
| ----- | ----- | ------ | ----- | ---------- |
| `code-explorer` | `sonnet` | `high` | explorer | Mapping the files, callers, and consumers a section touches; fact-checking a section's paths, symbols, counts, XREFs, and deferrals (process-todo-section step 2). Read-only. |
| `prior-art-researcher` | `sonnet` | `high` | researcher | The review skill's research pass and plan-experiment's prior-art step, with primary-source citations. Read-only plus web. |
| `section-implementer` | `sonnet` | `high` | implementer | One bounded slice of a section: code, tests, tools, or docs, writing only the paths it is given. |
| `oracle-writer` | `sonnet` | `high` | implementer | The `tests/rfi_ref.py` change for a format change, written from `spec/FOLD.md` alone so the oracle stays independent of the C. |
| `checks-runner` | `sonnet` | `high` | runner | Builds, the correctness suite, fuzz, check-all, TODO gates, and research measurements whose output would flood the lead. Never edits. |
| `routine-reviewer` | `sonnet` | `high` | reviewer | An advisory pre-pass over a candidate diff (failure modes, consistency, integration, Markdown rules) before the lead's self-review and the GPT panel. Never a verdict. |

`checks-runner` runs with `omitClaudeMd: true` because its dispatch carries everything it needs; the others load `CLAUDE.md` and `AGENTS.md` because they apply the repository's writing and format rules.

## What the lead never delegates

- The section's validation verdict and every plan correction (process-todo-section steps 2 and 3), and any design decision.
- Review of consequential surfaces, done by the lead itself: the `.fold` bitstream and its version and method numbers, the oracle's independence, decoder bounds and malformed input, file writing and `rf_write_atomic`, thread scheduling and byte-identical output, credentials and privacy, and the review, stamp, and campaign tooling.
- Commits, pushes, `scripts/panel_slots.py` rounds, the independent review, stamps, row flips, and `plan --sync`.
- Final acceptance: the lead reads the integrated diff and re-quotes every gate from the log on disk or a fresh run. An agent's "passed" is a pointer to evidence, never the evidence.

Review verdicts stay where `.conclave/panel.toml` pins them (GPT through Codex). A `routine-reviewer` pass adds a cheap first net and replaces nothing.

## How to dispatch

- **Bounded:** one slice per dispatch, with the section reference, the items, the context the agent cannot infer (decisions already made, the stamps it builds on), and the acceptance commands.
- **Owned paths:** before every writing dispatch, `python3 scripts/agent_routing.py grant <agent> <path>...` grants the exact paths the agent owns (literal files or directories, never patterns, never overlapping another type's grant) and snapshots the tree; the dispatch prompt repeats them. Two concurrent agents never own the same path, and nobody owns a path the lead is editing. A grant binds to the first dispatch that writes under it, so at most one writer of each type runs at a time (a `section-implementer` and an `oracle-writer` may run together); parallelism costs no extra tokens, so this limits wall time only. Contract and record files (`AGENTS.md`, `CLAUDE.md`, `.gitignore`, `.gitattributes`, `todo/`, `docs/reviews/`, `docs/phase-runs/`, `docs/drift.json`, `.claude/`, `.codex/`, `.agents/`, `.conclave/`, `.intelligence/`, `tools/githooks/`, `build/agent-routing/`) are never granted.
- **Parallel where independent:** read-only agents run in parallel freely. Builds, tests, and measurements are serialized because they share `bin/`, `build/`, and a quiet machine: at most one agent builds or measures at a time, and the dispatch says so.
- **No duplicate investigation:** pass the explorer's findings into the implementer's prompt instead of letting each agent rediscover them.
- **Format changes split the work:** `section-implementer` owns the C, `oracle-writer` owns `tests/rfi_ref.py` from the spec, and the lead owns `spec/FOLD.md`, `KAT_FOLD`, and the four-place agreement. The oracle's independence is an instruction, not a tool restriction (the agent can technically read `src/`), so the lead reviews every oracle diff for C-translated structure itself, and never pastes C into an `oracle-writer` dispatch.
- **Check the tree around writing dispatches:** `weaken-scan` (below) compares the tree with the grant snapshot; a changed path the agent did not own is a failed dispatch, whatever the agent reported.

## Accepting an implementer's work

A Sonnet implementer must never leave the implementation weaker than the section demands. No single check guarantees that, so acceptance stacks five, and a slice is accepted only when all five pass. Delegation saves the writing, never the judging.

1. **Ownership is enforced, not requested.** The `protect` hook refuses any fleet `Edit`, `Write`, or `NotebookEdit` outside the granted paths and on every contract or record file, so an implementer cannot tick a box, edit the section it is judged by, or touch a stamp.
2. **Weakening is scanned.** `python3 scripts/agent_routing.py weaken-scan` diffs every file changed since the grant (including writes made through `Bash`, which the hook cannot see, staged renames, anything staged in the index (writers never stage, and staged content is scanned too), and ignored control files such as `.claude/settings.local.json`, which it compares by hash) and flags removed assertions, raises, refusals, and bounds checks; removed tests; added skips, stubs, `TODO`s, `pass`, `#if 0`, `noqa`, or `NOLINT`; changed limits and known answers; deleted files; changes outside the grant; and any change to a gate, the oracle, or the spec. Exit 1 means flags exist, and the lead adjudicates every flag by reading the line. A flag is cleared only by a section item that requires the change (quoted in the agent's `TOUCHED CHECKS`) or by the lead's written reason in the run record.
3. **The lead reads the whole diff,** not the summary, item by item against each Done-when, looking for what a scan cannot see: a narrowed algorithm, a cheaper substitute the section names as wrong, a test that asserts too little, a missing failure path.
4. **New tests are proven red then green.** The agent quotes each new test failing before the change and passing after; the lead re-runs the green side itself, and on a consequential surface reproduces the red side too (the grant snapshot under `build/agent-routing/baseline/` holds the pre-dispatch files).
5. **Nothing weaker reaches review.** The section's gates run unchanged (the lead quotes them from its own run or the log on disk), the lead reviews every consequential surface itself, and the GPT panel then reviews the candidate as before. Delegation adds layers in front of the existing gates and removes none.

`python3 scripts/agent_routing.py release` clears the grants after acceptance. A slice that fails any layer gets the one corrective follow-up below, or the lead writes it.

## Escalation

- A `BLOCKED` return is answered by the lead from source (code, spec, measurement) or escalated to the operator. It is never resolved by narrowing the contract.
- A result that misses its acceptance criteria gets one corrective follow-up through `SendMessage` to the same agent, naming the exact failure. A second miss means the lead does the slice itself or escalates: no third dispatch of the same slice, and never acceptance of a weaker result.
- A failed gate, an unresolved finding, or a `CRITICAL-SURFACE` mark from the reviewer goes to the lead at once.

## Enforcement and proof

- **Guard** (`PreToolUse` on `Agent` in `.claude/settings.json`): refuses `general-purpose`, `Explore`, `Plan`, `claude`, or an omitted type (they run at an unpinned model and effort), refuses a `model` argument that differs from a fleet agent's frontmatter, refuses `isolation` on a fleet agent (AGENTS.md creates no worktrees), and refuses dispatch while `CLAUDE_CODE_SUBAGENT_MODEL`, `CLAUDE_CODE_SUBAGENT_MODEL_FORCE`, or `ANTHROPIC_DEFAULT_SONNET_MODEL` is set. Everything else passes untouched: `fork` (lead-grade, inherits the lead's model), `claude-code-guide` and `statusline-setup` (Claude Code's own helpers, not repository work), and plugin agents such as `codex:codex-rescue`.
- **Ledger** (`SubagentStop`): reads the finished subagent's transcript and appends the served model, the recorded effort, and the turn count to `build/agent-routing/ledger.jsonl`. A fleet agent served outside its family or below its effort makes the hook exit 1, which Claude Code shows to the operator as a non-blocking hook error; `verify` is the authoritative check.
- **Verify:** `python3 scripts/agent_routing.py verify [--session <id>]` checks a session's ledger against the frontmatter and against the newest model per family in the installed Claude Code catalog. `--record` writes the passing results to `.claude/agents/routing-probe.json`.
- **Ownership** (`PreToolUse` on `Edit|Write|NotebookEdit`): `protect` reads the subagent's `agent_type` from the hook payload and refuses fleet writes outside `grant`ed paths or on contract and record files. Writes outside the repository (scratch) and under `build/` (logs, except `build/agent-routing/`) pass.
- **Gate:** `python3 scripts/agent_routing.py validate` and `--self-test` run in `scripts/check-all.py` and the `plan-gates` workflow. Validation fails on a model other than the `sonnet` alias, effort below `high`, `Edit`, `Write`, or `NotebookEdit` on a read-only class, unwired hooks (guard, protect, or ledger), a table row that disagrees with the frontmatter, or an agent with no probe or a probe whose recorded evidence contradicts the policy (served family or effort, no served turns, or a served model other than the catalog newest it recorded).
- **Attribution:** an optional `Implementer:` stamp line names the lead session's model, which is accountable for the candidate. The delegated legs, with served model and effort, are in the ledger; quote `verify --session <id>` in the run record when a section used the fleet.

## Re-probing

Re-probe after a Claude Code update, when a newer Sonnet ships, or when an agent file changes: dispatch each affected agent on a real bounded task in a new session, then run `python3 scripts/agent_routing.py verify --record` and commit the probe file. `verify` fails if the served Sonnet is not the catalog's newest, so a stale alias resolution is caught rather than assumed.

## Limits

- The catalog check reads the `latest_per_family` table embedded in the installed Claude Code binary. If a future release moves it, `verify` prints the catalog as unknown and the newest-model check is skipped; the family and effort checks still run.
- CI cannot dispatch agents, so it checks the recorded probe, not a live one. A probe is evidence of the day it ran.
- `Bash` writes are not seen by the `protect` hook, on any class. `weaken-scan` sees their result in the tree and is the backstop.
- `weaken-scan` is a pattern net: it can miss a weakening written in an unusual shape and can flag a harmless line. That is why the lead's full-diff read (step 3) stays mandatory however clean the scan.
- Claude Code itself makes small background calls on Haiku (the 2026-10-01 probe session billed 74k Haiku input tokens with no Haiku subagent). Those are the harness's own, not routable by agent files, and the ledger shows no fleet agent ran on Haiku.
- Cost and quality are recorded per dispatch in the ledger, not yet aggregated into run records.
