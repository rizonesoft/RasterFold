# AGENTS.md

Agent instructions for this repository. Human orientation lives in `README.md`. Claude Code loads this file through the `CLAUDE.md` import stub.

## What this project is

**RasterFold** is a lossless RGB image codec for Windows x64: a freestanding C program (`Bin/rasterfold.exe`, no C runtime) that encodes 24-bit BMP files into `.fold` files and back. The format is version 2 ([`spec/FOLD.md`](spec/FOLD.md)); its main method is a context-mixing coder (`src/cm.c`). The goal is to beat lossless JPEG XL in compression, encode and decode speed, and memory, on images in general and never by tuning to one test image.

| Path | Purpose |
| ---- | ------- |
| `src/` | The codec: `cm.c` (method 1, the CM coder and its threads), `fold.c` (the container), `bmp.c`, `crc32.c`, `sys.c` (Win32), `main.c`, `mem.c`, `rf.h` |
| `spec/FOLD.md` | The normative format specification |
| `build.ps1`, `build.ninja` | The build: Ninja over the pinned local Clang, output `Bin/rasterfold.exe` |
| `tools/bootstrap.ps1` | Downloads and verifies the pinned toolchain into `tools/` (llvm-mingw, Ninja, libjxl tools); everything else under `tools/` is ignored except `tools/githooks/` and `tools/provision.ps1` |
| `tools/provision.ps1` | Sets up a clone: toolchain, `core.hooksPath`, Python and Pillow, ffmpeg check; `-Verify` checks without changing anything |
| `tools/githooks/` | `pre-commit` (the TODO gate on the staged tree) and `commit-msg` (strips AI attribution) |
| `tests/` | `rfi_ref.py` (the independent Python oracle of the format), `test_correctness.py`, `fuzz_decode.py`, `measure_final.py` (the JPEG XL reference comparison), `benchmark.py` |
| `research/` | Measurement tools: `corpus.py`, `ab.py` (sizes and A/B timing of `cm.c` variants), `bench.c`, `prof.py`, `vsjxl.py`; data lands in `build/research/` |
| `testdata/photos/` | The ten-photo test pack with its provenance and licences |
| `measure/` | The reference image and the comparison verdict |
| `benchmarks/` | `results.md` and `results.csv` from `tests/benchmark.py` |
| `docs/research.md` | The research record: where the codec stands, the measurement protocol, the "Already tried" table, the designs the plan's sections implement, and prior art |
| `scripts/` | `check-all.ps1` (every local gate) and the stdlib Python TODO tooling (`todo-graph.py`, validator, claims, findings, runs, review prompt, panel slots, campaign guard) |
| `todo/` | The live execution plan; read `todo/README.md` before authoring or implementing |
| `todo/implementation-plan.md` | Ordered phase plan; its boxes are synchronized through `scripts/todo-graph.py` |
| `todo/budget.json`, `todo/backlog.md` | The budget decisions (no caps) and the list of deferred ideas that are not sections, which loses nothing without a trace |
| `docs/reviews/` | Per-section review findings, attestations, the derived findings ledger, run records |
| `docs/phase-runs/` | One findings file per phase run, written as the run goes |
| `.claude/` | Claude Code skills, the Stop hook, and settings |
| `.conclave/panel.toml` | The review panel wiring: writer, model registry, and every review slot |
| `Bin/`, `build/` | Build output, scratch, caches, the research corpus, and campaign guard state; ignored, never authoritative |

Everything here is Windows-only. The TODO tooling is stdlib Python 3 (`python` on Windows) and needs no install step; the tests need Pillow, and the JPEG XL comparisons need ffmpeg with libjxl on `PATH`.

## The decisions this project runs on

- **Freestanding C11, no C runtime.** Entry `rf_start`, Win32 through `kernel32` and `shell32` only, built by the pinned llvm-mingw Clang with Ninja (`build.ps1`), target `x86-64-v2`. Assembly is not used: every assembly routine was measured against C and C won or tied (`docs/research.md` section 3.4). Reintroducing assembly needs an A/B run that shows a gain.
- **The format is frozen per version.** A change to what the encoder writes touches four places in one commit: the C code, the oracle `tests/rfi_ref.py` (written from the spec, never translated from the C), `spec/FOLD.md`, and the known-answer file in `tests/test_correctness.py`. A new coding method takes a new method number; a change to an existing method takes a new format version. Old files decode or are refused as unsupported, never misread.
- **Files are never damaged.** Output goes to `destination.partial` (`CREATE_NEW`), is flushed, and is renamed only after success; an existing destination is never overwritten. The decoder bounds every field before use and rejects malformed input.
- **Same bytes for any thread count.** The three channel threads and the one-thread pipeline produce identical files.
- **Measured, general claims only.** Size and speed claims follow `docs/research.md` section 2: the research corpus (`photo` tunes, `tp` confirms, `synth` reports), alternating A/B runs for time, thread counts stated. No rule, constant, or branch looks at the dimensions, hash, name, or colour statistics of a particular image, and `measure/image.bmp` alone never decides anything.
- **A dependency is a decision.** Adding a library, a runtime import, or a Python package beyond the standard library and Pillow records the reason in the section that adds it.

## The TODO system

`todo/` is the live execution plan; **format spec: `todo/README.md`.** Markdown is canonical and `build/` holds derived, gitignored projections.

Domains are flat-numbered from `00`, allocated in order, and read from the tree: `00-workspace` (toolchain, gates, CI, this system), `01-compression`, `02-speed`, `03-memory`, `04-robustness`, `05-milestones` (the 60 milestone gates, generated from `research/milestones.py`), `06-library`, `07-tool`, `08-docs`, `09-release`, and `99-manual` (operator-only rows and the operator's recorded decisions). The plan runs one phase per milestone; see `todo/README.md`, Milestones. Numbers are stable addresses: a new domain appends after the last one. `todo/TODO-00-INDEX.md` is the authority for which exist.

**Nothing in the plan is capped.** Sections the operator asks for in an attended session carry nothing extra. A section a campaign files on its own carries `**Origin:** discovered run=<run id> <YYYY-MM-DD>` under its heading (provenance for reporting, never a quota) and must be a defect in shipped or in-flight work, something an acceptance-bar aim requires, or a prerequisite of a planned row; anything else goes to `todo/backlog.md` (never runnable, never capped). A backlog entry leaves only by promotion (a live section carries its source key), by merge (the survivor keeps ``merged: `<key>` ``), or with the operator's words in a removal record under `## Removed with operator approval`; `validate` fails anything else as `backlog-dropped`. Every release section (it pushes `rasterfold-vX.Y.Z`) carries a backlog review item and stamps only when every backlog entry was promoted or carries a `reviewed: <tag> <date> ...` field quoting the operator's deferral. `todo/budget.json` records the decisions (schema 3, both caps `null`); only the operator changes it, in words a new history entry quotes. The structural limits (30 items per section, 55 sections per file) apply.

Files are `todo/NN-domain/TODO-NN-short-name.md`. The **Implementation Order table is the dependency graph**: every `## N.` section has exactly one row and vice versa, and a row flips to `[x]` only when a `Verified:` stamp covers it. Cross-references use section marks in the forms `§N`, `TNN §N`, and `DNN TNN §N` as spelled out in `todo/README.md`, and must be bidirectional. Every section sits in exactly one phase table of `todo/implementation-plan.md`.

## Choose the work contract

For TODO work, read the target section, `todo/README.md`, its dependencies, and the applicable skill under `.claude/skills/`: capture through `add-todo`, author a file through `create-todo`, plan a codec change through `plan-experiment`, build through `process-todo-section` (gate card: `.claude/skills/process-todo-section/gates.md`), stamp through `review-todo-section`, close a file through `process-todo-file`, run a phase or the plan through `process-phase` or `process-plan`, harden the tree through `groom-plan`.

The lifecycle is: capture, author, validate the plan and source claims, record `Started:`, implement, run the section's gates, commit, independent review, panel review and stamp, then sync the plan. Each section must be executable with zero conversation context. **One section = one commit.** Preserve section addresses and bidirectional cross-references. A TODO Implementation Order row turns `[x]` only with its `Verified:` stamp through the review skill. Do not rewrite a stamped checklist or transfer evidence across changed candidates. File new work with its own owner and dependency.

`/process-plan` is the campaign front door: it audits the plan, starts the run guard (the Stop hook in `.claude/settings.json`, the guard file under `build/`, and a heartbeat job), and runs phase after phase until every row is shipped, parked, or blocked.

## What counts as proof

A Test checkpoint cites one or more of six proofs, spelled out in `todo/README.md`:

1. **Builds clean**: `build.ps1` exits 0 with no compiler warning on the touched files.
2. **Static analysis clean**: nothing new from `clang-tidy` (the gate lands with `D00 T01 §3`; until then no checkpoint may cite it).
3. **Correctness test**: a named case of `tests/test_correctness.py`, or a `tests/fuzz_decode.py` run with its seed.
4. **Driven run with evidence**: `Bin/rasterfold.exe` output quoted (`--time` statistics, `inspect` fields, a refusal's exit code).
5. **Format fidelity proof**: the round trip and byte identity with the oracle; every change to what the encoder writes or the decoder accepts owes this one.
6. **Corpus measurement**: sizes and times by the protocol in `docs/research.md` section 2, with sets, totals, geometric means, worst images, and thread counts.

A checkpoint citing a gate that does not exist yet is unfalsifiable and is not allowed.

## Working rules

- **Output discipline:** bound every command (`build.ps1` filtered to errors and warnings, `test_correctness.py -k` while iterating, `tail`/`head` on logs and on `ab.py -v` tables, field extraction on JSON). Keep full logs in ignored scratch under `build/`.
- **Act, then report:** complete authorized work and report evidence. Explicit operator stop instructions take effect immediately.
- **One writer, and it never reviews itself:** Claude Code is the only writer (operator decision); no other harness edits, commits, or runs campaigns here. Every other model takes part only as a headless reviewer through a slot in `.conclave/panel.toml`, run by `python scripts/panel_slots.py exec <slot>` (GPT review slots run through the `codex` CLI), which names the writer, registers the models, and pins every review role; `scripts/panel_slots.py` refuses a table where any slot shares the writer's family. There are no fallback slots: a failed round waits for the operator. Skills name slots, never models: a re-pin is one edit to that file plus a fresh probe date.
- **Writes are serial:** one session owns the working tree. Check `git status` before building over unfamiliar work.
- **Section atomicity is the candidate range:** one section ships as one logical change, and review fix-loop commits append to that range (never amend); each fix is re-reviewed and the stamp names the whole range. "One section = one commit" never means "one hash".
- **Experiments end in a result, not a change:** a compression or speed section that misses its stop condition ships the negative result (stamped measurement, a row in `docs/research.md`'s "Already tried" table), and its change is not ported.
- **No em dashes** in authored prose; use `--`, a colon, or a new sentence. One line per paragraph and list item in Markdown.
- **Source of truth:** the format via `spec/FOLD.md` and the oracle, other codecs via their specifications, reference implementations, and published papers, measurements via the commands that produced them, plan state via `todo/`. Disagreements are recorded decisions, not silent reinterpretations.
- **Completion-first, no partial ships:** a section is complete only when every micro-step is `[x]`, its checkpoint is quoted from a real run, and review stamped it. Anything genuinely unshippable now files through `add-todo` with an owner (debt with a collector, never a silent drop), and waiting is never a strategy. Reviewers refuse stamps on partial scope.

## The commit hook

`tools/githooks/pre-commit` refuses a commit whose **staged** tree fails `python scripts/todo-graph.py validate`, checked in a temp checkout of the index. It must stay a POSIX shell script with LF endings and index mode `100755`. `tools/githooks/commit-msg` strips any AI-assistant attribution (a `Co-Authored-By:` trailer naming Claude, Anthropic, or another assistant, and "Generated with" footers) from every commit message, so GitHub never lists an assistant as a contributor; never add such lines to commits or pull requests, and keep `.claude/settings.json`'s `includeCoAuthoredBy: false` and empty `attribution`. `tools/provision.ps1` points `core.hooksPath` at `tools/githooks` for each clone; run it once after cloning. A red hook is a defect to fix, never a gate to skip.

## A measurement recorded is a measurement re-checked

`validate` proves the tree is internally consistent. It cannot tell whether a `Current state` block is still **true**, because a stale figure and a correct one look identical as prose. So **a TODO that measures something records it as a claim**, and `scripts/todo-claims.py` re-measures it:

```
<!-- claim: exists src/cm.c -->
<!-- claim: absent src/pal.c -->
<!-- claim: count "^#define NMODEL 5$" src/cm.c = 1 -->
```

**When a claim goes stale the TODO is wrong, not the repository.** Fix the claim and the sentence it supports in the same commit, and if the change invalidates the section's reasoning, say so rather than quietly updating a number. Size and speed figures cannot be re-measured by `todo-claims.py`: a TODO quotes them with the command and date that produced them, and the implementer re-runs them before relying on them.

## Unknowns and questions

Answer from source first: the code, the format specification, a reference codec or paper, or a measurement. When an unanswered question would change implementation, take a justified default, record that it is a default with its cost of changing, and carry on. Do not stall a section waiting for an answer; do not silently reinterpret a section into something buildable.

## Validation

```powershell
pwsh scripts/check-all.ps1                    # build, correctness, fuzz, and every TODO and tool gate below that it runs
powershell -ExecutionPolicy Bypass -File build.ps1   # the build alone
python tests\test_correctness.py              # the whole suite; iterate with -k <case>
python tests\fuzz_decode.py 300               # mutated files must be rejected cleanly
python tests\measure_final.py                 # the JPEG XL reference comparison
python tests\benchmark.py -n 3                # size, speed, and memory against PNG, WebP, JPEG XL
```

```bash
python scripts/todo-graph.py self-test       # must stay green; the run prints its own total
python scripts/todo-graph.py validate        # FATAL blocks; new WARN blocks until fixed or accepted
python scripts/todo-graph.py plan --sync     # re-derive the plan projection after TODO edits
python scripts/todo-graph.py plan --check    # fail if the projection went stale
python scripts/todo-graph.py query ready     # dependency-safe work right now
python scripts/todo-graph.py query blocked   # sections waiting on something
python scripts/todo-graph.py query stats     # tree health
python scripts/todo-graph.py query budget    # sections and discovered sections per phase, the backlog count
python scripts/todo-graph.py query growth --since <ref>   # a run's added sections and backlog changes
python scripts/todo-graph.py resolve 'DNN TNN §N'   # ref -> file, section, deps, status
python scripts/todo-claims.py                # re-measure what the TODOs claim about the repo
python scripts/todo-claims.py --coverage     # name every Current state block nothing re-measures
python scripts/todo-findings.py --check      # fail if docs/reviews/findings.md is stale
python scripts/todo-runs.py --check          # run records agree with the review files
python scripts/campaign_guard.py --self-test # the Stop hook and guard lifecycle, in a throwaway workspace
python scripts/panel_slots.py validate       # the review panel wiring holds
```

Run checks owed by the task. Report only commands actually run, and distinguish static evidence, test output, driven-run output, fidelity output, measurements, and review proof.

Use trunk-based `main` for routine work and concise imperative commits. Respect exact candidate identity and one-section scope. Never bypass hooks with `--no-verify`, amend a recorded candidate, or force-push.

## Credentials

Credentials never enter tracked files, arguments, logs, or handoff prose.
