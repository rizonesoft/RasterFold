# GEMINI.md

Follow [AGENTS.md](./AGENTS.md). This file holds only what is specific to Google Antigravity (`agy`).

- Antigravity is not a writer in this repository. It runs only as a reviewer through `scripts/review-dispatch.py`, as the pinned fallback for Claude's work in `.conclave/reviewers.toml`.
- `.agents/skills/<name>` entries are symlinks to `.codex/skills/<name>`, the Codex harness's discovery path, so the skills seen there are Codex's copies.
- Rules live in `.agents/rules/`.
