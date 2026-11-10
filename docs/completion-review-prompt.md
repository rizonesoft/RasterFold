# Completion review prompt

Fill this template with `python3 scripts/review-prompt.py --kind completion --ref '<DNN TNN §N>' --base <BASE_SHA> --candidate <TIP_SHA> --summary '<what was built>'` (the text after the `---` line is sent whole. A review prompt never limits the reviewer's reply length or finding count.), and pass the file it writes to `scripts/review-dispatch.py --kind completion --candidate <FULL_SHA> --writer <claude|codex|gemini|muse|opencode> --prompt <file> --record docs/reviews/<domain>/<slug>.md` (this session's writer key). It runs once per TODO section, last: after the adversarial round, its fixes and a green gate run, on the exact final candidate. The stamp must name that same commit.

---

You are an independent product and codec completeness reviewer. Do not modify any file. A TODO section has just been implemented. The adversarial round has already looked for bugs in what was written. Check whether the finished feature, with that round's fixes, is complete, wired end to end, and up to the bar of reference codecs and tools in this space (JPEG XL `libjxl`, lossless WebP, PNG/`oxipng`, CALIC/paq8 context mixers, and for Domain 10 converter surfaces, XnConvert and ImageGlass).

## Candidate

- TODO section: `<DNN TNN §N>` (read its contract with `python3 scripts/todo-graph.py resolve "<DNN TNN §N>"` and read the surrounding file).
- Commit or range: `<FULL_SHA>` or `<BASE_SHA>..<TIP_SHA>` (inspect the supplied patch, or use `git show` or `git diff` when shell access is available).
- What was built: `<two or three sentences naming the files, symbols, format paths, CLI flags, or UI controls>`.
- Governing documents: `AGENTS.md`, `spec/FOLD.md`, `docs/research.md`, and `<the design doc or plan file that applies>`.
- The adversarial round and what was done with each finding: `<the section's findings file>`.

## What to check

Give one section with your findings, each numbered and tagged `in-scope` (belongs in this section before it can be called done) or `out-of-scope` (real follow-up work that belongs in its own TODO section):

1. **End-to-end reach:** can a caller, CLI user, or converter user actually reach what was built (CLI argument parsing, `include/rasterfold.h` public API, single-thread and 3-thread pipeline parity, `.partial` safe publication, error reporting, and cleanup on failure)? A module that passes its unit test while no real path reaches it is `in-scope`.
2. **Format, oracle, and corpus completeness:** if the section touches the codec format or coding methods, do `src/`, `tests/rfi_ref.py`, `spec/FOLD.md`, and `tests/test_correctness.py` agree byte-for-byte? If it is a compression, speed, or memory change, are the corpus measurements (`photo`, `tp`, `synth`), geometric means, worst images, and thread counts complete and honest?
3. **Edge cases and platform parity:** truncated or hostile `.fold` and BMP inputs, 1x1 and odd-dimension images, zero-byte files, allocation failure paths, Windows freestanding (`rf_start`, no CRT in CLI/DLL) vs Linux/Android POSIX/pthreads parity, and x64 vs ARM64 portability.
4. **Reference codec / competitor comparison:** what would a developer or user switching from `cjxl`/`djxl` (`libjxl`), `cwebp`, `oxipng`, or a native batch converter notice is missing from this feature right away?
5. **The round's fixes:** was every real finding from the adversarial round fixed without a regression, and is every finding not acted on correctly dismissed? A fix that is wrong or incomplete, or a real finding left open, is `in-scope`.

## Reply format

Cite every repository location as `path:line` in backticks, never as a Markdown link: the review is recorded byte for byte under `docs/reviews/`, where `scripts/drift-check.py` checks every link. Number every gap with its `in-scope` or `out-of-scope` tag and what is missing. End with a final line: `COMPLETION: complete` when no `in-scope` item is listed (`out-of-scope` items alone do not force gaps), or `COMPLETION: gaps` when any `in-scope` item is listed. A section on a pin older than the three-pass policy (2026-11-06) ends `COMPLETION: gaps` when any item of either kind is listed. If you could not read the candidate or the TODO section at all, explain why in one line and end with `COMPLETION: blocked`.
