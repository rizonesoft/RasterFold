# Runner intelligence

Each retained family (`claude` and `codex`) owns its own `.intelligence/<family>/register.json`. The register keeps open/deferred entries and an archive of applied/rejected entries in one file, so a single atomic replacement preserves both sides of a disposition. IDs are scoped to their family register and never migrate between families or get reused. Capture records evidence and an owner; a proposal alone is not a finding.

Use `python3 scripts/intelligence.py --family <claude|codex> <action>` for all operations, governed by `.agents/skills/process-intelligence/SKILL.md`:

- `python3 scripts/intelligence.py validate` validates both `.intelligence/claude/register.json` and `.intelligence/codex/register.json` without mutating them.
- `python3 scripts/intelligence.py --family claude list` and `python3 scripts/intelligence.py --family codex list` inspect the two registers.
- `python3 scripts/intelligence.py --family <claude|codex> capture --title <title> --evidence <evidence> --proposal <proposal> --owner <TODO-ref>` records an observation.
- `python3 scripts/intelligence.py --family <claude|codex> apply --id <id> --commit <reachable-commit> --reason <measured-proof>` marks an entry applied.
- `python3 scripts/intelligence.py --family <claude|codex> reject --id <id> --reason <factual-reason>` marks an entry rejected.
- `python3 scripts/intelligence.py --family <claude|codex> defer --id <id> --reason <owner-and-trigger>` marks an entry deferred.
- `python3 scripts/intelligence.py --family <claude|codex> archive --id <id> --reason <closure-note>` moves an applied or rejected entry into `archive`.

Capture is allowed during a run. All dispositions (`apply`, `reject`, `defer`, `archive`) hold the campaign guard lock (`scripts/campaign_guard.py`) continuously across stopped-boundary verification, register read, mutation, and atomic `os.fsync` + `replace` persistence, and require `build/claude-campaign-guard.json`, `build/claude-campaign-state.json`, and `build/claude-campaign-pending-cancel.json` to be absent. An active run, a pending heartbeat cancellation, a held guard lock, or an unreadable `build/` directory refuses disposition without modifying any register.
