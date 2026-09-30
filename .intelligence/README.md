# Runner intelligence

Each family owns its own `register.json` and implementation. The register keeps open/deferred entries and an archive of applied/rejected entries in one file, so a single atomic replacement preserves both sides of a disposition. IDs never migrate between families or get reused. Capture records evidence and an owner; a proposal alone is not a finding.

Use `.codex/runner/intelligence.py` for Codex and `.claude/runner/intelligence.py` for Claude. Both provide `list`, `validate`, `capture`, `apply`, `reject`, `defer` and `archive`. Capture takes `--title`, `--evidence`, `--proposal` and `--owner`. Dispositions take `--id` and `--reason`; applying also requires `--commit`. `scripts/intelligence.py validate` checks both without mutating them.

Capture is allowed during a run. All dispositions hold the shared admission lock and require both families to be stopped. A parked/paused run or malformed ownership file is busy. Archive only closed entries. Reject with a factual reason, defer with a named owner and trigger, and apply only after the correction has been implemented and checked. The command verifies commit reachability; the reviewer verifies that the commit actually addresses the evidence.
