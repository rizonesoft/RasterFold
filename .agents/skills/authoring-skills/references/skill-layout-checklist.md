# Skill Layout Checklist

Use this checklist whenever adding or changing a skill in Antigravity (`agy`).

## Antigravity Skill (`.agents/skills/`)

- [ ] Skill lives in `.agents/skills/<skill-name>/` as a real directory, not a symlink.
- [ ] `SKILL.md` exists with `name` and `description` frontmatter matching `<skill-name>`.
- [ ] Long docs live in `references/`, executable helpers in `scripts/`, assets in `assets/`.
- [ ] No `agents/openai.yaml` exists under `.agents/skills/<skill-name>/`.

## Reusable Kit

- [ ] `kits/base/templates/` is updated if new projects should inherit the skill.
- [ ] Kit templates use real files and directories, not symlinks.
- [ ] Kit docs explain any project-specific text that must be adapted after install.

## Rules

- [ ] Repo-wide durable instructions are in `AGENTS.md`.
- [ ] Agent-specific entry files (`GEMINI.md`) and rules (`.agents/rules/`) stay isolated.
- [ ] Markdown reference checklists live in `docs/` or skill `references/`.
