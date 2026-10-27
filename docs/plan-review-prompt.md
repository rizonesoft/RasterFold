# Plan review prompt

Before writing code for a TODO section, copy this into a scratch file, fill in the placeholders, and run one pass through `scripts/review-dispatch.py --kind plan --candidate <HEAD_SHA> --writer <claude|muse|agy|codex> --prompt <scratch> --record <findings.md>` (this session's writer key). The dispatcher sets medium effort and appends the `plan` lens from `.conclave/reviewers.toml`. For `--writer codex`, include the exact candidate diff in the prompt or name a readable scratch file containing it; the Muse reviewer's shell is disabled.

---

You are an independent reviewer checking a TODO section's plan before the writer builds it. Do not modify any file. Read the section, its dependencies, the files it will touch, and the governing specifications (`AGENTS.md`, `spec/FOLD.md`, `docs/research.md`), then report what the plan is missing or gets wrong before code is written.

## Section to build

- Section: `<DNN TNN §N>` (inspect with `python3 scripts/todo-graph.py resolve "<DNN TNN §N>"`).
- TODO file: `<todo/.../TODO-NN-....md>` at lines `<start>-<end>`.
- Starting commit: `<FULL_40_CHAR_HEAD_SHA>`.
- Files the section expects to touch: `<paths>`.
- Design or spec inputs: `<spec/FOLD.md, docs/research.md, docs/design/... or "none">`.

## What to check

1. **Missing micro-steps:** work the section's goal or acceptance bar needs that no `- [ ]` line names.
2. **Consumer and format wiring:** if the section touches what the encoder writes or the decoder accepts, does it name all four format duties (`src/`, `tests/rfi_ref.py`, `spec/FOLD.md`, `tests/test_correctness.py`)? If it adds a setting, CLI flag, C API symbol, or converter surface, does it name every caller, header (`include/rasterfold.h`), and test?
3. **Research and measurement protocol:** if the section is a compression or speed experiment, does it state the baseline, the corpus sets (`photo` tunes, `tp` confirms, `synth` reports), alternating A/B timing with thread counts, and the stop condition (`docs/research.md` "Already tried" row on a negative result)?
4. **Test checkpoint:** whether the `**Test checkpoint:**` proves the feature works on real inputs and cites only gates that already exist.
5. **Prerequisite order:** whether anything the section relies on is still `[ ]` in an earlier or later section.

## Reply format

Cite every location as a repository-relative `path:line` in backticks, never as a Markdown link. Number each gap with `P1` (blocks a complete build) or `P2` (should be folded into the steps before coding), then end with:

```
HIGHEST: P1|P2|none
PLAN: complete|gaps
OVERALL: approve|needs-attention
```

If you could not read the section or the repository, say why in one line and end with `OVERALL: blocked`.
