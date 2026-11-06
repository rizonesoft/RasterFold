# Reconciliation review prompt

Copy this into a scratch file, fill in the placeholders, and pass it to `scripts/review-dispatch.py --kind reconciliation --candidate <FULL_SHA of the repaired candidate> --writer <claude|codex|gemini|muse|opencode> --prompt <file> --record docs/reviews/<domain>/<slug>.md` (this session's writer key). It runs at most once per TODO section, only after a completion review ended `COMPLETION: gaps` and its gaps were repaired and the gates rerun. The dispatcher sets medium effort and appends its lenses from `.conclave/reviewers.toml`, and refuses a second one.

---

You are an independent reviewer. Do not modify any file. A completion review of this TODO section returned gaps, and the writer says they are repaired. Decide whether the repair really closes them. The obligations are the ones that review held the section to: they cannot be narrowed, reclassified or waived to obtain approval.

## Candidate

- TODO section: `<DNN TNN §N>` (read its contract with `python3 scripts/todo-graph.py resolve "<DNN TNN §N>"`).
- Findings file holding the failed completion review and the writer's `- Implemented:` lines: `<docs/reviews/<domain>/<slug>.md>`.
- The failed completion's candidate: `<FULL_SHA>`.
- The repaired candidate: `<FULL_SHA>`. The repair is `<COMPLETION_SHA>..<REPAIRED_SHA>`, read all of it.
- Gate results on the repaired candidate: `<quoted output>`.
- Governing documents: `AGENTS.md`, `spec/FOLD.md`, and `<the design doc or plan file that applies>`.

## What to check

1. Every `in-scope` gap in the failed completion review: closed, with the `path:line` or test that shows it, or still open.
2. Each repair: does it introduce a regression, a weakened test, or a narrowed behaviour?
3. The section's contract: are its promises the same as when the completion review read them?

## Reply format

Cite every repository location as `path:line` in backticks, never as a Markdown link. Number each gap with `closed` or `open` and its evidence, then any new finding from the repair. End with a final line: `RECONCILIATION: complete` only when every gap is closed and the repair introduced no real defect, otherwise `RECONCILIATION: gaps`. If you could not read the candidate or the findings file, explain why in one line and end with `OVERALL: blocked`.
