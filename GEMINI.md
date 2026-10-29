# GEMINI.md

Follow [AGENTS.md](./AGENTS.md). This file holds only what is specific to Google Antigravity (`agy`).

- Claude Code (`claude`), Muse Code (`muse`), Antigravity (`agy`), Codex (`codex`), OpenCode (`opencode`) and Copilot CLI (`copilot`) can write in this repository.
- Independent reviews use `python3 scripts/review-dispatch.py --writer <claude|muse|agy|codex|opencode|copilot> ...`: Codex work goes to Muse Spark 1.3, and the other writers' work goes to GPT-6 Astra. Each route has one reviewer and no fallback.
- Skills live in `.agents/skills/<name>/`, rules live in `.agents/rules/`, and the native campaign `Stop` hook lives in `.agents/hooks.json`.
- During `process-plan` and `process-phase`, the in-session heartbeat runs through the native `schedule` (`CronExpression: "3-59/5 * * * *"`) and `manage_task` tools alongside `scripts/campaign_guard.py`.
