---
name: process-intelligence
description: Review and dispose of measured runner lessons at a stopped boundary, preserving evidence, ownership and archive history. Use when asked to process intelligence or maintain this runner.
---

# Process runner intelligence

Read `.intelligence/README.md` and inspect both retained family registers before acting:

- `python3 scripts/intelligence.py --family claude list`
- `python3 scripts/intelligence.py --family codex list`

A new observation is captured with:

- `python3 scripts/intelligence.py --family <claude|codex> capture --title <title> --evidence <path/command/result> --proposal <correction> --owner <TODO-ref>`

Keep evidence concrete and avoid secrets. Capture during a run is allowed; it does not authorize changing runner policy mid-campaign.

Before disposition, verify the campaign guard is stopped (`build/claude-campaign-guard.json`, `build/claude-campaign-state.json`, and `build/claude-campaign-pending-cancel.json` absent).

For each open item in `claude` and `codex`, reproduce or inspect the evidence and determine the smallest correct action:

- Apply an authorized correction under its owning section and required review, then run `python3 scripts/intelligence.py --family <claude|codex> apply --id <id> --commit <reachable-commit> --reason <measured-proof>`.
- Reject a false premise with a factual reason via `python3 scripts/intelligence.py --family <claude|codex> reject --id <id> --reason <factual-reason>`.
- Defer with an owner, trigger, and reason via `python3 scripts/intelligence.py --family <claude|codex> defer --id <id> --reason <owner-and-trigger>`; it remains visible in `entries`.
- Do not label a proposal applied merely because a matching commit exists.

Use `python3 scripts/intelligence.py --family <claude|codex> archive --id <id> --reason <closure-note>` only for applied or rejected records. The archive remains in the same atomic register, retaining ids, source evidence, and history. Never hand-edit away an observation. Finish with `python3 scripts/intelligence.py validate` and report the actual dispositions and any unresolved owner.
