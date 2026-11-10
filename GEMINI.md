# GEMINI.md

Follow [AGENTS.md](./AGENTS.md). This file holds only what is specific to Google Antigravity (the `agy` CLI, writer key `gemini`).

- Claude Code (`claude`), Codex (`codex`), Antigravity (`gemini`), Muse Code (`muse`) and OpenCode (`opencode`) can write in this repository.
- Independent reviews use `python3 scripts/review-dispatch.py --writer <claude|codex|gemini|muse|opencode> ...`; this harness's key is `gemini`: Codex work goes to Gemini 4 Carbon through the Antigravity CLI, and the other writers' work goes to GPT-6 Astra through the Codex CLI. Each route has one reviewer and no fallback.
- Skills live in `.agents/skills/<name>/`, rules live in `.agents/rules/`, and the native campaign `Stop` hook lives in `.agents/hooks.json`.
- During `process-plan` and `process-phase`, the in-session heartbeat runs through the native `schedule` (`CronExpression: "3-59/5 * * * *"`) and `manage_task` tools alongside `scripts/campaign_guard.py`.
