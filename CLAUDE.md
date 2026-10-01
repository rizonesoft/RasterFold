# CLAUDE.md

This file exists so Claude Code loads the shared agent instructions, which live in one place: `AGENTS.md`.

See @AGENTS.md for the full working rules.

## Delegation (Claude only)

Delegate bounded research, implementation, testing, documentation, and routine pre-review to the Sonnet fleet in `.claude/agents/` (effort `high`), following `.claude/agent-routing.md`: owned paths granted per dispatch (`agent_routing.py grant`), builds and measurements serialized, every implementer slice accepted only through the five anti-weakening layers (enforced ownership, `weaken-scan`, the lead's full-diff read, red-then-green tests, unchanged gates), one corrective follow-up then the lead takes over or escalates. The lead keeps design, plan corrections, consequential-surface review, commits, pushes, panel rounds, stamps, and final acceptance, and re-quotes every gate from evidence on disk. A hook refuses unrouted or model-overridden dispatches, and `python3 scripts/agent_routing.py verify` proves the served model and effort.
