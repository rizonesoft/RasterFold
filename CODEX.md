# CODEX.md

Follow [AGENTS.md](./AGENTS.md). This file holds only what is specific to OpenAI Codex (`codex`).

- Skills live in `.codex/skills/<name>/` (with `agents/openai.yaml` metadata). `.agents/skills/<name>` is a symlink to each, which is how the Codex harness discovers them.
- The lifecycle skills are Codex's own copies and differ from `.claude/skills/` by design ([`.codex/README.md`](./.codex/README.md)). The Conclave-SDK kit skills (`adopt-conclave`, `authoring-skills`, `independent-review`, `local-tool-safety`, `scheduled-automations`) are identical in `.codex/skills/` and `.claude/skills/`: change both together, or let `forge.py sync` refresh them.
- Use `.codex/skills/local-tool-safety` before local tool, Windows/WSL, connector, mail, cloud, SQL, or outbound work.
- Rules live in `.codex/rules/`. Campaign, hook and review mechanics live in `.codex/README.md`.
