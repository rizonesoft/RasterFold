---
name: process-plan
description: Run the RasterFold plan with Codex as writer.
---
Read AGENTS.md and the shared workflow in `.agents/skills/process-plan/SKILL.md`, together with its referenced section and review procedures. Preserve all repository gates, review rounds, milestone caps and stop conditions. This Codex runtime replaces harness-specific instructions: writer key `codex`, native `CODEX_THREAD_ID`, guard `--cron-id none`, and the pinned Carbon reviewer. Never invoke Claude CronCreate or Antigravity schedule tools. Wait inside the session while reviews or usage limits clear. Set the session goal from the launch prompt. An operator stop ends the run cleanly with a handoff and no automatic restart.
