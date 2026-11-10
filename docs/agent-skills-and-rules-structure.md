# Agent Skills and Rules Structure

This repository uses Claude Code (`claude`), OpenAI Codex (`codex`), Google Antigravity (`gemini`), Muse Code (`muse`) and OpenCode (`opencode`, running an Anthropic model) as writers. Independent reviews route Codex work to Gemini 4 Carbon through the Antigravity CLI and other writers' work to GPT-6 Astra through the Codex CLI through `scripts/review-dispatch.py` and `.conclave/reviewers.toml`.

## Skill Locations

All project skills live directly under `.agents/skills/`:

```text
.agents/skills/<skill-name>/
  SKILL.md
  references/
  scripts/
  assets/
```

- `SKILL.md` is required in every skill folder.
- `references/` holds longer checklists or gate cards.
- `scripts/` holds executable helpers when a skill needs one.
- `assets/` holds templates or static resources.

## Naming and placement

- Name a skill for its capability (`authoring-skills`, `local-tool-safety`, `process-todo-section`, `review-todo-section`, `process-plan`), not with a project namespace prefix. Project-specific routing context belongs in the description and instructions, where it is read on demand, rather than in the name, which is paid for in the always-on catalog by every session.
- All skills live under `.agents/skills/` as real files.

## Rules and Hooks

Use the word "rules" carefully:

- Durable project behavior belongs in [`AGENTS.md`](../AGENTS.md).
- Antigravity-specific entry instructions live in [`GEMINI.md`](../GEMINI.md) and rules in `.agents/rules/`.
- The campaign `Stop` hook is wired in `.agents/hooks.json`, `.claude/settings.json`, `.codex/hooks.json`, and the OpenCode plugin `.opencode/plugins/rasterfold-campaign/index.js` (which also delivers OpenCode's heartbeat from `scripts/opencode-heartbeat.py`); each uses `scripts/campaign_guard.py`.
- Markdown checklists or rule references belong in a skill `references/` folder or in `docs/`.

## Adding a New Skill

1. Create `.agents/skills/<skill-name>/SKILL.md` as a real file.
2. Keep the `name` field equal to `<skill-name>`, using lowercase letters, numbers, and hyphens only.
3. Name the skill for the capability, not the project namespace (`authoring-skills`, `local-tool-safety`, `process-todo-section`).
4. Keep `description` specific enough for automatic selection.
5. Put long checklists in `references/`.
6. Update `AGENTS.md` if the skill changes project behavior.
7. Validate all skill files and references (`python3 scripts/todo-graph.py validate`).

## Core Skills

| Skill | Path |
| ----- | ---- |
| `local-tool-safety` | `.agents/skills/local-tool-safety/` |
| `authoring-skills` | `.agents/skills/authoring-skills/` |
| `independent-review` | `.agents/skills/independent-review/` |
| `process-todo-section` | `.agents/skills/process-todo-section/` |
| `review-todo-section` | `.agents/skills/review-todo-section/` |
| `process-phase` | `.agents/skills/process-phase/` |
| `process-plan` | `.agents/skills/process-plan/` |
