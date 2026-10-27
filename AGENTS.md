# AGENTS.md

Agent instructions for this repository. Human orientation lives in `README.md`. Antigravity (`agy`) loads this file through `GEMINI.md`.

## What this project is

**RasterFold** is a lossless RGB image codec in C11 for Windows/Linux x64 and Android ARM64/x64. It supplies a CLI plus shared and static libraries through `include/rasterfold.h`. Windows codec artifacts remain freestanding; Linux and Android use libc and pthreads. The format is version 2 ([`spec/FOLD.md`](spec/FOLD.md)); its main method is a context-mixing coder (`src/cm.c`). The goal is general improvements in compression, speed and memory, measured on a corpus rather than tuned to one image.

| Path | Purpose |
| ---- | ------- |
| `src/` | The codec: `cm.c` (method 1, the CM coder and its threads), `pal.c` (methods 2 and 3, the palette coder), `rc.h` (the shared range coder), `fold.c` (the container), `bmp.c`, `crc32.c`, `sys.c` (Win32), `main.c`, `mem.c`, `rf.h` |
| `spec/FOLD.md` | The normative format specification |
| `build.sh`, `build.ps1`, `scripts/build.py` | Native and cross builds over pinned local LLVM/Ninja, output `bin/<target>/`; root `build.ninja` is a compatibility front door |
| `tools/bootstrap.py`, `tools/bootstrap.ps1`, `toolchain.json` | Verified repository-local Linux/Windows LLVM, Ninja and Android NDK pins; downloaded tools and archives are ignored |
| `tools/provision.py`, `tools/provision.ps1` | Clone setup and read-only verification; Linux test dependencies use `.venv/` |
| `tools/githooks/` | `pre-commit` (TODO, vision and drift gates on the staged tree) and `commit-msg` (strips AI attribution and validates declared facts) |
| `tests/` | `rfi_ref.py` (the independent Python oracle of the format), `test_correctness.py`, `fuzz_decode.py`, `measure_final.py` (the JPEG XL reference comparison), `benchmark.py` |
| `research/` | Measurement tools: `corpus.py`, `ab.py` (sizes and A/B timing of `cm.c` variants), `bench.c`, `prof.py`, `vsjxl.py`; data lands in `build/research/` |
| `testdata/photos/` | The ten-photo test pack with its provenance and licences |
| `measure/` | The reference image and the comparison verdict |
| `benchmarks/` | `results.md` and `results.csv` from `tests/benchmark.py` |
| `docs/research.md` | The research record: where the codec stands, the measurement protocol, the "Already tried" table, the designs the plan's sections implement, and prior art |
| `scripts/` | `check-all.ps1` (every local gate) and the stdlib Python TODO tooling (`todo-graph.py`, validator, claims, findings, runs, `review-dispatch.py`, `ci_status.py`, `campaign_guard.py`) |
| `todo/` | The live execution plan; read `todo/README.md` before authoring or implementing |
| `todo/implementation-plan.md` | Ordered phase plan; its boxes are synchronized through `scripts/todo-graph.py` |
| `todo/budget.json`, `todo/backlog.md` | The budget decisions (no caps) and the list of deferred ideas that are not sections, which loses nothing without a trace |
| `docs/reviews/` | Per-section review findings, attestations, the derived findings ledger, run records |
| `docs/phase-runs/` | One findings file per phase run, written as the run goes |
| `.agents/` | Antigravity-owned skills (`.agents/skills/`), rules (`.agents/rules/`), and native `Stop` hook (`.agents/hooks.json`) |
| `.intelligence/` | Retained runner intelligence registers and archives; disposition requires the campaign guard stopped |
| `.conclave/kits.json`, `GEMINI.md`, `.agents/rules/` | The Conclave-SDK kits (`base`, `reviewer`, `automations`) this checkout took from `rizonetech/Conclave-SDK` through its `forge.py`, the Antigravity entry file, and the kits' output and safety rules; see Conclave-SDK kits below |
| `.conclave/reviewers.toml`, `scripts/review-dispatch.py`, `docs/review-round-prompt.md`, `docs/plan-review-prompt.md`, `docs/completion-review-prompt.md` | The reviewer pin, the 3-stage adaptive review schedule (`plan`, `completion`, and adaptive rounds), and the dispatcher |
| `.conclave/automations/` | The automations kit: `tasks.json` (folder `\\RasterFold\\`, no tasks yet), the runner, the failure email, and the Task Scheduler and systemd installers; tests in `scripts/tests/` |
| `bin/`, `build/` | Build output, scratch, caches, the research corpus, and campaign guard state; ignored, never authoritative |

The build/tooling runs in WSL/Linux and Windows. Use `python3` in WSL/Linux and `python` on Windows. TODO tooling needs Python 3.11+ and no third-party package; image tests need pinned Pillow. Comparative measurements need ffmpeg with libjxl. Agent automation is operated in the shared WSL/Linux checkout; Android runtime validation is separate from cross-compilation. See `docs/building.md` for host SDK requirements.

## The decisions this project runs on

- **C11 with a platform boundary.** Windows entry `rf_start`, Win32 through `kernel32` and `shell32`, and no C runtime in the shipped CLI/DLL. Hosted Windows static consumers supply their own CRT. Linux/Android use libc and pthreads. x64 targets use `x86-64-v2`; ARM64 uses the tested portable arithmetic path. New codec assembly or SIMD optimizations require measured A/B evidence. The Windows freestanding memory primitives are platform plumbing, not a performance claim.
- **The format is frozen per version.** A change to what the encoder writes touches four places in one commit: the C code, the oracle `tests/rfi_ref.py` (written from the spec, never translated from the C), `spec/FOLD.md`, and the known-answer file in `tests/test_correctness.py`. A new coding method takes a new method number; a change to an existing method takes a new format version. Old files decode or are refused as unsupported, never misread.
- **Files are never damaged.** Output goes to exclusive `destination.partial`, is flushed, and is published without replacement (Win32 rename; POSIX no-replace rename with an exclusive-link fallback); an existing destination is never overwritten. The decoder bounds every field before use and rejects malformed input.
- **Same bytes for any thread count.** The three channel threads and the one-thread pipeline produce identical files.
- **Measured, general claims only.** Size and speed claims follow `docs/research.md` section 2: the research corpus (`photo` tunes, `tp` confirms, `synth` reports), alternating A/B runs for time, thread counts stated. No rule, constant, or branch looks at the dimensions, hash, name, or colour statistics of a particular image, and `measure/image.bmp` alone never decides anything.
- **A dependency is a decision.** Adding a library, a runtime import, or a Python package beyond the standard library and Pillow records the reason in the section that adds it.

## The TODO system

`todo/` is the live execution plan; **format spec: `todo/README.md`.** Markdown is canonical and `build/` holds derived, gitignored projections.

Domains are flat-numbered from `00`, allocated in order, and read from the tree: `00-workspace` (toolchain, gates, CI, this system), `01-compression`, `02-speed`, `03-memory`, `04-robustness`, `05-milestones` (the 60 milestone gates, generated from `research/milestones.py`), `06-library`, `07-tool`, `08-docs`, `09-release`, `10-converter` (the native C application and bundled codecs), and `99-manual` (operator-only rows and the operator's recorded decisions). The plan runs one phase per milestone; see `todo/README.md`, Milestones. Numbers are stable addresses: a new domain appends after the last one. `todo/TODO-00-INDEX.md` is the authority for which exist.

**Nothing in the plan is capped.** Sections the operator asks for in an attended session carry nothing extra. A section a campaign files on its own carries `**Origin:** discovered run=<run id> <YYYY-MM-DD>` under its heading (provenance for reporting, never a quota) and must be a defect in shipped or in-flight work, something an acceptance-bar aim requires, or a prerequisite of a planned row; anything else goes to `todo/backlog.md` (never runnable, never capped). A backlog entry leaves only by promotion (a live section carries its source key), by merge (the survivor keeps ``merged: `<key>` ``), or with the operator's words in a removal record under `## Removed with operator approval`; `validate` fails anything else as `backlog-dropped`. Every release section (it pushes `rasterfold-vX.Y.Z`) carries a backlog review item and stamps only when every backlog entry was promoted or carries a `reviewed: <tag> <date> ...` field quoting the operator's deferral. `todo/budget.json` records the decisions (schema 3, both caps `null`); only the operator changes it, in words a new history entry quotes. The structural limits (30 items per section, 55 sections per file) apply.

Files are `todo/NN-domain/TODO-NN-short-name.md`. The **Implementation Order table is the dependency graph**: every `## N.` section has exactly one row and vice versa, and a row flips to `[x]` only when a `Verified:` stamp covers it. Cross-references use section marks in the forms `§N`, `TNN §N`, and `DNN TNN §N` as spelled out in `todo/README.md`, and must be bidirectional. Every section sits in exactly one phase table of `todo/implementation-plan.md`.

## Choose the work contract

For TODO work, read the target section, `todo/README.md`, its dependencies, and the applicable skill under `.agents/skills/`: capture through `add-todo`, author a file through `create-todo`, plan a codec change through `plan-experiment`, build through `process-todo-section` (gate card: `.agents/skills/process-todo-section/gates.md`), stamp through `review-todo-section`, close a file through `process-todo-file`, run a phase or the plan through `process-phase` or `process-plan`, harden the tree through `groom-plan`.

The lifecycle is: capture, author, validate the plan and source claims, record `Started:`, run the pre-build `--kind plan` review, implement, run the section's gates, commit, run the post-build `--kind completion` review, run adaptive code review rounds and stamp through `review-todo-section`, then sync the plan. Each section must be executable with zero conversation context. **One section = one commit.** Preserve section addresses and bidirectional cross-references. A TODO Implementation Order row turns `[x]` only with its `Verified:` stamp through `python3 scripts/todo-graph.py stamp`. Do not rewrite a stamped checklist or transfer evidence across changed candidates. File new work with its own owner and dependency.

`process-plan` is the campaign front door. It audits the plan, starts `scripts/campaign_guard.py` with the writer's available `Stop` hook and a native in-session heartbeat (`3-59/5 * * * *`), and processes dependency-safe phases. Codex campaigns require the hook and heartbeat preflight in `process-plan`. No isolated worktrees or checkpoint recovery layer is required or created.

## What counts as proof

A Test checkpoint cites one or more of six proofs, spelled out in `todo/README.md`:

1. **Builds clean**: `python3 scripts/build.py --target <target>` (or the host wrapper) exits 0 with no compiler warning on the touched files.
2. **Static analysis clean**: nothing new from `clang-tidy` (the gate lands with `D00 T01 §3`; until then no checkpoint may cite it).
3. **Correctness test**: a named case of `tests/test_correctness.py`, or a `tests/fuzz_decode.py` run with its seed.
4. **Driven run with evidence**: `bin/windows-x64/bin/rasterfold.exe` output quoted (`--time` statistics, `inspect` fields, a refusal's exit code).
5. **Format fidelity proof**: the round trip and byte identity with the oracle; every change to what the encoder writes or the decoder accepts owes this one.
6. **Corpus measurement**: sizes and times by the protocol in `docs/research.md` section 2, with sets, totals, geometric means, worst images, and thread counts.

A checkpoint citing a gate that does not exist yet is unfalsifiable and is not allowed.

## Working rules

- **Output discipline:** bound every command (`build.ps1` filtered to errors and warnings, `test_correctness.py -k` while iterating, `tail`/`head` on logs and on `ab.py -v` tables, field extraction on JSON). Keep full logs in ignored scratch under `build/`.
- **Act, then report:** complete authorized work and report evidence. Explicit operator stop instructions take effect immediately.
- **Writers never review themselves:** Claude Code (`claude`), Muse Code (`muse`), Google Antigravity (`agy`) and Codex (`codex`) can write in this repository (operator decision 2026-10-27). Codex-written work routes to Muse Spark 1.3 (`muse-spark-1.3` via `muse`); the other writers route to GPT-6 Astra (`gpt-6-astra` via `codex`). All independent reviews run through `scripts/review-dispatch.py` and `.conclave/reviewers.toml` with one reviewer and no fallback. All routes use medium for plan and completion, then `high -> medium -> low -> low ...` for rounds. A writer never reviews its own candidate. Historical stamps retain their original rules. A failed review stops or parks the run.
- **Writes are serial:** one session owns the working tree. Check `git status` before building over unfamiliar work.
- **Section atomicity is the candidate range:** one section ships as one logical change, and review fix-loop commits append to that range (never amend); each fix is re-reviewed and the stamp names the whole range. "One section = one commit" never means "one hash".
- **Experiments end in a result, not a change:** a compression or speed section that misses its stop condition ships the negative result (stamped measurement, a row in `docs/research.md`'s "Already tried" table), and its change is not ported.
- **No em dashes** in authored prose; use `--`, a colon, or a new sentence. One line per paragraph and list item in Markdown.
- **Source of truth:** the format via `spec/FOLD.md` and the oracle, other codecs via their specifications, reference implementations, and published papers, measurements via the commands that produced them, plan state via `todo/`. Disagreements are recorded decisions, not silent reinterpretations.
- **Completion-first, no partial ships:** a section is complete only when every micro-step is `[x]`, its checkpoint is quoted from a real run, and review stamped it. Anything genuinely unshippable now files through `add-todo` with an owner (debt with a collector, never a silent drop), and waiting is never a strategy. Reviewers refuse stamps on partial scope.

## Conclave-SDK kits

The `base`, `reviewer` and `automations` kits came from `rizonetech/Conclave-SDK` at `b1ead32` (2026-10-21) through `python3 <sdk>/forge.py kit . <kit> --agent all`. `.conclave/kits.json` stamps `reviewer` and `automations`. The SDK's `todo` kit is not installed: RasterFold's TODO system, skills in `.agents/skills/`, `.agents/hooks.json`, and `scripts/campaign_guard.py` are its own.

These local adaptations differ from the kit files, and `forge.py sync` would undo them (it rewrites every kit's skills and rules, and with `--include-scripts` its scripts), so a sync always runs `--dry-run` first and keeps them:

- Claude Code, Muse Code, Antigravity and Codex write (`WRITERS` in `scripts/review-dispatch.py`); `.claude/` holds the campaign `Stop` hook and skill shims, `.codex/` holds Codex's campaign `Stop` hook, and `.agents/skills/` holds the canonical skill files directly.
- `.conclave/reviewers.toml` pins `codex -> spark` and the other writers to `astra`, without fallback, and defines the shared `[schedule]` table for `--kind plan`, `--kind completion`, and adaptive code review rounds.
- `GEMINI.md` and `docs/agent-skills-and-rules-structure.md` describe this repository's layout, not the SDK default.

## The commit hook

`tools/githooks/pre-commit` refuses a commit whose **staged** tree fails `python scripts/todo-graph.py validate`, checked in a temporary export of the index. It also enforces `scripts/plan-audit.py` and `scripts/drift-check.py` on the staged bytes. Explicit `Drift-Fact:` commit declarations are checked against staged source by `commit-msg`; historical declarations use their own commit tree in CI. It must stay a POSIX shell script with LF endings and index mode `100755`. `tools/githooks/commit-msg` strips any AI-assistant attribution (a `Co-Authored-By:` trailer naming Claude, Anthropic, or another assistant, and "Generated with" footers) from every commit message, so GitHub never lists an assistant as a contributor; never add such lines to commits or pull requests. `tools/provision.ps1` points `core.hooksPath` at `tools/githooks` for each clone; run it once after cloning. A red hook is a defect to fix, never a gate to skip.

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

WSL/Linux: `python3 scripts/check-all.py --all-targets`. Target artifacts and full logs are ignored build output; report actual runtime coverage separately. Review and automation unit tests: `python3 -m unittest discover -s scripts/tests -v`.

```powershell
pwsh scripts/check-all.ps1                    # native build, correctness, API, fuzz and repository gates
powershell -ExecutionPolicy Bypass -File build.ps1   # the build alone
python tests\test_correctness.py              # the whole suite; iterate with -k <case>
python tests\fuzz_decode.py 300               # mutated files must be rejected cleanly
python tests\measure_final.py                 # the JPEG XL reference comparison
python tests\benchmark.py -n 3                # size, speed, and memory against PNG, WebP, JPEG XL
```

```bash
python3 scripts/todo-graph.py self-test       # must stay green; the run prints its own total
python3 scripts/todo-graph.py validate        # FATAL blocks; new WARN blocks until fixed or accepted
python3 scripts/todo-graph.py plan --sync     # re-derive the plan projection after TODO edits
python3 scripts/todo-graph.py plan --check    # fail if the projection went stale
python3 scripts/todo-graph.py query ready     # dependency-safe work right now
python3 scripts/todo-graph.py query blocked   # sections waiting on something
python3 scripts/todo-graph.py query stats     # tree health
python3 scripts/todo-graph.py query budget    # sections and discovered sections per phase, the backlog count
python3 scripts/todo-graph.py query growth --since <ref>   # a run's added sections and backlog changes
python3 scripts/todo-graph.py resolve 'DNN TNN §N'   # ref -> file, section, deps, status
python3 scripts/drift-check.py                # documents, generated output and source review receipts
python3 scripts/plan-audit.py                 # prerequisite order and product coverage
python3 scripts/todo-claims.py                # re-measure what the TODOs claim about the repo
python3 scripts/todo-claims.py --coverage     # name every Current state block nothing re-measures
python3 scripts/todo-findings.py --check      # fail if docs/reviews/findings.md is stale
python3 scripts/todo-runs.py --check          # run records agree with the review files
python3 scripts/campaign_guard.py --self-test # the Stop hook and guard lifecycle, in a throwaway workspace
python3 scripts/review-dispatch.py --check-pin # the reviewer pin and schedule table hold
python3 scripts/review-dispatch.py --list-rounds # every recorded review round parses cleanly
python3 scripts/ci_status.py                  # read build.yml and plan.yml on main
```

Run checks owed by the task. Report only commands actually run, and distinguish static evidence, test output, driven-run output, fidelity output, measurements, and review proof.

Use trunk-based `main` for routine work and concise imperative commits. Respect exact candidate identity and one-section scope. Never bypass hooks with `--no-verify`, amend a recorded candidate, or force-push.

## Credentials

Credentials never enter tracked files, arguments, logs, or handoff prose.
