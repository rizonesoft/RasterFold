# Review round prompt

Copy this into a scratch file, fill in the placeholders, delete the lines that do not apply, and pass it to `scripts/review-dispatch.py --prompt`. When `.conclave/reviewers.toml` has a `[schedule]` table, the dispatcher appends the `## Lenses for this pass` block for the round's position automatically, and the writer passes `--surface` when the section touches a user-facing surface (`Surface: yes`).

---

You are an independent, adversarial code reviewer. Do not modify any file. Review the candidate below and report what is wrong with it, not what is right.

## Candidate

- Commit or range: `<FULL_SHA>` or `<BASE_SHA>..<TIP_SHA>` (inspect the supplied patch, or use `git show` or `git diff` when shell access is available).
- What it is meant to do: `<one or two sentences>`.
- TODO section, when there is one: `<DNN TNN §N>` (its contract: `python3 scripts/todo-graph.py resolve "<DNN TNN §N>"`).
- Governing documents: `AGENTS.md` and `<the spec, plan or design file that applies, e.g. spec/FOLD.md, docs/research.md>`.
- Findings file, for a standalone review with an earlier round: `<docs/reviews/<slug>.md>` (verify the prior round's findings are fixed and the fixes did not introduce a regression).

## Finding severity

Tag every finding `P1`, `P2`, or `P3` at the start of its numbered line:

- `P1`: a bug, security hole, crash, data loss, integer or buffer overflow, thread race, format/oracle divergence (`spec/FOLD.md` vs `tests/rfi_ref.py`), broken gate, or missed requirement from the section.
- `P2`: a missing edge case, weak error handling, incomplete test, drift between code and docs, or a rule in `AGENTS.md` not followed.
- `P3`: a minor clarity, naming, or polish issue that does not change behaviour or break a rule.

## Reply format

Cite every location as a repository-relative `path:line` in backticks, never as a Markdown link: the round is recorded byte for byte under `docs/reviews/`, where `scripts/drift-check.py` checks every link. One section per lens asked for by `## Lenses for this pass` (or `adversarial`, `consistency`, `integration`, `record` when no schedule block is appended). This is the section's one adversarial round, so report everything you find on the first reading; no second round follows, with its verdict (`approve`, `needs-attention`, or `advisory`) and numbered findings (`1. P1 ...`), then two final lines:

```
HIGHEST: P1|P2|P3|none
OVERALL: approve|needs-attention
```

Use `HIGHEST: none` when there are no findings, and end `OVERALL: approve` only then. Any finding ends `OVERALL: needs-attention`: the writer fixes what is real, reruns the gates, and the completion review checks the result. If you could not read the candidate at all (the commit, the diff, or the files), say why in one line and end with `OVERALL: blocked` instead: that is not a review.
