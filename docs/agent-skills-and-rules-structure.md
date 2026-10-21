# Agent Skills and Rules Structure

This repo keeps skill content honest by separating tool-specific skill files from shared operating intent across Google Antigravity (`agy`), Claude Code (`claude`), and OpenAI Codex (`codex`).

## Tool-Specific Skill Locations

Antigravity-facing skills live under:

```text
.agents/skills/<skill-name>/
  SKILL.md
  references/
  scripts/
  assets/
```

Claude-facing skills live under:

```text
.claude/skills/<skill-name>/
  SKILL.md
  references/
  scripts/
  assets/
```

Codex-facing skills live under:

```text
.codex/skills/<skill-name>/
  SKILL.md
  references/
  agents/openai.yaml
```

- `SKILL.md` is required in every tool folder.
- `references/` is standard Agent Skills structure for longer documentation.
- `scripts/` is for executable helpers.
- `assets/` is for templates or resources.
- `agents/openai.yaml` is OpenAI/Codex-specific metadata. It belongs only in `.codex/skills/<skill-name>/`, never in `.claude/skills`.
- RasterFold differs from the Conclave-SDK default here: every `.agents/skills/<skill-name>` is a symlink to `../../.codex/skills/<skill-name>`, because the Codex harness discovers skills through `.agents/skills/` ([`.codex/README.md`](../.codex/README.md)). Never link `.claude` to `.codex` or the reverse: those copies may diverge in wording, metadata, and operational detail.

## Naming and placement

- Name a skill for its capability, not with a project namespace prefix. Project-specific routing context belongs in the description and instructions, where it is read on demand, rather than in the name, which is paid for in the always-on catalog by every session.
- Claude skills live under `.claude/skills/` and Codex skills under `.codex/skills/` as real files, `.agents/skills/` holds symlinks to the Codex copies, reusable starter templates live in Conclave-SDK's `kits/base/`, and Codex-specific `agents/openai.yaml` metadata lives only in `.codex/skills/`.

## Reusable Skills Kit

Reusable starter files for new projects live under `kits/base/`.

The kit contains copied templates for `AGENTS.md`, `GEMINI.md`, `CLAUDE.md`, `CODEX.md`, `.agents/skills/`, `.claude/skills/`, and `.codex/skills/`, plus an additive-only starter installer (`kits/base/scripts/install.sh` or `python forge.py kit <target> base`). The starter copies missing files, preserves every existing tuned file, rejects destructive force mode and symlink-parent traversal, and is not an updater.

## Rules

Use the word "rules" carefully:

- Durable project behavior belongs in `AGENTS.md`.
- Agent-specific entry files (`GEMINI.md`, `CLAUDE.md`, `CODEX.md`) and rules (`.agents/rules/`, `.claude/rules/`, `.codex/rules/`) hold tool-specific wiring.
- Markdown checklists or rule references belong in a skill `references/` folder or in `docs/`.
- Codex command execution policy files belong in `.codex/rules/*.rules` when using the Codex Starlark `.rules` format.

## Adding a New Skill

1. Create `.claude/skills/<skill-name>/SKILL.md` and `.codex/skills/<skill-name>/SKILL.md` as real files, and link `.agents/skills/<skill-name>` to `../../.codex/skills/<skill-name>`.
2. Keep the `name` field equal to `<skill-name>`, using lowercase letters, numbers, and hyphens only.
3. Name the skill for the capability, not the project namespace (`authoring-skills`, `local-tool-safety`, `process-todo-section`).
4. Keep `description` specific enough for automatic selection.
5. Put long checklists in `references/`.
6. Put OpenAI/Codex metadata in `.codex/skills/<skill-name>/agents/openai.yaml`.
7. Update Conclave-SDK's `kits/base/templates/` when new projects should inherit the skill.
8. Update `AGENTS.md` if the skill changes project behavior.
9. Validate all skill files, references, and metadata.

## Current Starter Kit Skills

| Skill | Agent paths (`<agent>` in `.agents`, `.claude`, `.codex`) |
| ----- | --------------------------------------------------------- |
| `local-tool-safety` | `<agent>/skills/local-tool-safety/` |
| `authoring-skills` | `<agent>/skills/authoring-skills/` |
