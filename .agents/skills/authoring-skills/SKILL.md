---
name: authoring-skills
description: Use when creating, moving, reviewing, or documenting project skills, skill references, or Antigravity (.agents/skills) skill folders and rules (.agents/rules/).
---

# Authoring Skills

## Overview

Keep project skills under `.agents/skills/<skill-name>/` and rules under `.agents/rules/`.

## Workflow

1. Put skill content under `.agents/skills/<skill-name>/`.
2. Require `SKILL.md` in each skill directory with `name` and `description` frontmatter.
3. Put heavy reference material under `references/` inside the skill folder.
4. Keep `SKILL.md` concise and link to `references/*.md` using relative paths.
5. Keep `.agents/skills/<skill-name>/` as real files and directories, never symlinks.
6. Use `.agents/rules/*.md` for repository rules.

## Verification

- Confirm `SKILL.md` exists in `.agents/skills/<skill-name>/`.
- Confirm relative links inside `SKILL.md` resolve from the skill folder root.
- Confirm no symlinks or provider-specific `agents/openai.yaml` files exist in `.agents/skills`.
