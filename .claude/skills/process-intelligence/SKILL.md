---
name: process-intelligence
description: Review and dispose of Claude's measured runner lessons at a stopped boundary, preserving evidence, ownership and archive history. Use when asked to process intelligence or maintain this runner.
---

# Process Claude intelligence

Read `.intelligence/README.md` and `.claude/runner/intelligence.py list`. This family owns only `.intelligence/claude/register.json`. A new observation is captured with `capture --title <title> --evidence <path/command/result> --proposal <correction> --owner <TODO-ref>`. Keep evidence concrete and avoid secrets. Capture during a run is allowed; it does not authorize changing runner policy mid-campaign.

Before disposition, verify both campaigns are stopped. Paused, parked and unreadable ownership are busy. The command holds the shared admission lock across this check and its atomic write. Do not stop another campaign just to get through this gate.

For each open item, reproduce or inspect the evidence and determine the smallest correct action. Apply an authorized correction under its owning section and required review, then `apply --id <id> --commit <reachable-commit> --reason <measured-proof>`. Reject a false premise with a factual reason. Defer with an owner, trigger and reason; it remains visible. Do not label a proposal applied merely because a matching commit exists.

Use `archive --id <id> --reason <closure-note>` only for applied or rejected records. The archive remains in the same atomic register, retaining ids, source evidence and history. Never hand-edit away an observation or borrow another family's id. Finish with `python3 scripts/intelligence.py validate` and report the actual dispositions and any unresolved owner.
