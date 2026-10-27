# GEMINI.md

Follow [AGENTS.md](./AGENTS.md). This file holds only what is specific to Google Antigravity (`agy`).

- Claude Code (`claude`), Muse Code (`muse`) and Antigravity (`agy`) write in this repository, and Codex never writes (2026-10-27 operator decision).
- Every independent review (`--kind plan` pre-build, `--kind completion` post-build, and the adaptive code review rounds) is dispatched with `python3 scripts/review-dispatch.py --writer <claude|muse|agy> ...` and routes to GPT-6 Astra (`gpt-6-astra` via `codex`) as pinned in `.conclave/reviewers.toml` (one route per writer, each to `astra`, no fallback).
- Skills live in `.agents/skills/<name>/`, rules live in `.agents/rules/`, and the native campaign `Stop` hook lives in `.agents/hooks.json`.
- During `process-plan` and `process-phase`, the in-session heartbeat runs through the native `schedule` (`CronExpression: "3-59/5 * * * *"`) and `manage_task` tools alongside `scripts/campaign_guard.py`.
