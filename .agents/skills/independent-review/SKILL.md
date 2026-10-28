---
name: independent-review
description: Get an independent review of your work from the reviewer pinned for your writer in .conclave/reviewers.toml, through scripts/review-dispatch.py, then verify and act on every finding. Use before calling a nontrivial change done, before a commit that ships, or when a TODO section needs its review round.
---

# Independent review

Your own review is not enough for a nontrivial change: a different model reviews it. Codex-written work goes to Muse Spark 1.3 via Muse; Claude Code, Muse Code, Antigravity and OpenCode work goes to GPT-6 Astra via Codex. `.conclave/reviewers.toml` pins one reviewer per writer with no fallback. The shared `[schedule]` sets each pass's effort and lenses: plan and completion at `medium`, round 1 `high`, round 2 `medium`, round 3 `low`, then `low` with one lens per round.

| Work written by | Reviewer | Fallback |
| --- | --- | --- |
| Claude Code (`claude`) | GPT-6 Astra (`codex`, scheduled effort) | none |
| Muse Code (`muse`) | GPT-6 Astra (`codex`, scheduled effort) | none |
| Antigravity (`agy`) | GPT-6 Astra (`codex`, scheduled effort) | none |
| Codex (`codex`) | Muse Spark 1.3 (`muse`, scheduled effort) | none |
| OpenCode (`opencode`, Anthropic model) | GPT-6 Astra (`codex`, scheduled effort) | none |

The pin refuses any other writer, any other reviewer CLI, a second reviewer in a route, and a reviewer of the writer's own model family.

## Steps

1. Review your own change first and fix what you find.
2. Write the review prompt to a scratch file, not the repository, from `docs/review-round-prompt.md`: name the candidate commit or range, the governing documents, and the lenses that apply.
3. Run the round:

   ```bash
   python3 scripts/review-dispatch.py --writer <this session's key: claude|muse|agy|codex|opencode> --prompt <scratch>/review-prompt.md --record docs/reviews/<topic>.md
   ```

   On Windows use `python` instead of `python3`; the command is otherwise the same.
4. Read the review. Verify every finding against the code before acting: fix what is real, and record why anything else is not.
5. Commit fixes as new commits and run another round on the new tip, up to 33 rounds; the loop ends on the first round with no Critical (`P1`) or Important (`P2`) finding (`HIGHEST: none` or `HIGHEST: P3`), and any `P3` or advisory findings are filed as follow-up work. A unit that needs patching three rounds running gets re-thought, not patched again.

## Rules

- The dispatcher's JSON output and the `## Round N` section it appends are the evidence. A verdict with no record is an opinion.
- Exit 1 means the reviewer returned no review (not installed, failed, timed out, empty or blocked), and nothing else runs. That is never an approval: say so, record the outage, and do not ship on it.
- A reviewer that could not reach the work (its sandbox could not read the repository) replies `OVERALL: blocked`, which is recorded as no review. If its sandbox keeps failing, put the diff in the prompt and run the round again.
- The reviewer runs read-only. Never pass it a secret, and never ask it to edit files.
- For `--writer codex`, the Muse reviewer's shell is disabled: put the exact candidate diff in the review prompt or a readable scratch file named in the prompt, and identify the candidate SHA or range. Ensure source files read as context match that candidate, or provide candidate snapshots in scratch. A reviewer unable to inspect the candidate returns blocked.
- Change the reviewer only in `.conclave/reviewers.toml`, never by calling a model directly.
