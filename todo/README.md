# TODO System -- Format Spec

The `todo/` tree is the live execution plan for RasterFold, the lossless RGB image codec in this repository. Markdown is canonical; the graph cache is a derived read-only projection rebuilt by `scripts/todo-graph.py`.

One rule governs everything below: **a TODO section must be implementable by someone with zero conversation context.** A fresh session starts with none, and a session that hits the usage limit resumes cold. If a section only makes sense to someone who was in the room, it is not done.

This system is a port of the Isotone TODO system (2026-09-29), which was itself a port of the Resolute, ScratchPad, and Intelligent Notepad ones. History comments in `scripts/` name sections from those repos (`D00 T04 §21` and the like); those are provenance for why a rule exists, not live references here.

## Tree shape

```
todo/
├── README.md               this file
├── TODO-00-INDEX.md        root index -- domain order + active work
├── implementation-plan.md  phase plan, the front door for a run (boxes derived)
├── budget.json             the budget decisions and their history (no caps)
├── backlog.md              deferred ideas, one line each; never sections, never runnable, never dropped silently
├── .warning-baseline       accepted warnings; a NEW warning fails validate
├── .design-baseline        empty: planned surfaces have explicit design contracts (see "Surfaces")
├── 00-workspace/
│   ├── INDEX.md            domain index -- every TODO in this domain
│   ├── TODO-01-<short-name>.md
│   └── TODO-02-<short-name>.md
├── 01-compression/
└── …
```

Domains are flat-numbered and ordered by allocation sequence. Each maps to a build area; the mapping lives in `TODO-00-INDEX.md` and in each domain's `INDEX.md`. The tooling reads domain names from the tree and never hardcodes them: a directory `NN-kebab-name/` holding an `INDEX.md` is a domain. Numbers are stable addresses: a new domain appends after the last one, because `DNN` cross-references encode them.

The domains are `00-workspace` (toolchain, gates, CI, this system, measurement references, held-out validation), `01-compression` (the models, predictors, and methods that make files smaller), `02-speed` (threads, compiler options, and hot-loop work that make coding faster without changing bytes, or with a stated cost), `03-memory` (buffer lifetimes and peaks), `04-robustness` (decoder hardening, fuzzing, and format scope), `05-milestones` (the 60 milestone gates of the roadmap and their planning sections, generated from `research/milestones.py`), `06-library` (the DLL and its C API), `07-tool` (the command line), `08-docs` (the HTML documentation and the drift and staleness gates), `09-release` (versioning, packaging, releases), `10-converter` (the native C converter and bundled codec adapters), and `99-manual` (operator-only rows and recorded decisions, numbered apart from the allocation sequence; each of its rows sits in the phase that waits on it). `TODO-00-INDEX.md` is the authority for which exist today.

**Naming:** `TODO-NN-short-name.md`, where `NN` is the next free number *within that domain*. Numbers are local to the domain and never reused. Renaming a file is safe: the stable `id` in frontmatter is what cross-references resolve against.

## Frontmatter

Every TODO file opens with YAML frontmatter. Five required fields, four optional. That is the whole schema: resist adding more.

```yaml
---
schema_version: 1
id: palette-method                    # stable, kebab-case, globally unique, survives renames
domain: 01-compression                # must match the containing directory
status: draft                        # draft | active | blocked | done | superseded
title: "TODO-01 -- Palette Method"
depends_on: []                       # optional -- whole-TODO edges; prefer section edges
frozen: false                        # optional -- true when the file changes a frozen behavior
track: C1                            # optional -- build-area reference
superseded_by: other-todo-id         # optional -- set with status: superseded
---
```

`id` rules: lowercase `a-z0-9-`, 2-60 chars, starts with a letter, no trailing dash. It is the key every cross-reference and the graph cache resolve against, so it must not change once other files point at it.

## File anatomy

```markdown
---
(frontmatter)
---

# TODO-01 -- Palette Method

> **Goal:** One paragraph. What is true when this file is finished, in plain terms.

> [!IMPORTANT]
> **Current state (verified YYYY-MM-DD):** What exists RIGHT NOW, before this TODO runs. Without this the implementer has to grep the repo to find the starting line. Name real files and real gaps.

## Inputs

- [`src/cm.c`](…) -- the range coder §1 moves into a shared header
- [`docs/research.md`](…) -- section 4.1, the design this file implements
- -> XREF: D04 T01 §2 -- the fuzz seeds this file's decoder needs

## Outcome

- Bullet list. Each bullet is an observable end state, not an activity.
- "`syn_checker` codes to fewer bytes than JPEG XL e7" -- good.
- "Improve compression" -- bad.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no new option); reporting=applicable @ D01 T01 §4; notifications=not-applicable (a command-line tool reports through its exit code and message); permissions=not-applicable (file safety is unchanged); audit=not-applicable (git history is the audit); exchange=applicable; reverse=applicable

## Implementation Order

| Order | Section | Deliverable                   | Depends On | Status |
| :---: | :-----: | ----------------------------- | ---------- | :----: |
|   1   |   §1    | Shared range coder header     | --         |  [x]   |
|   2   |   §2    | Palette encoder and decoder   | §1         |  [ ]   |

---

## 1. Shared Range Coder Header

One paragraph of context: why this section exists and what it must not break.

- [ ] Move the range coder from `src/cm.c` into `src/rc.h` unchanged. Done when: `python tests/test_correctness.py` prints `33 passed, 0 failed` and the known-answer file is unchanged. Cheaper substitute: a second copy of the coder in `src/pal.c`.
- [ ] Another concrete item. Max 30 per section. See Work items below.
- [ ] Commit: `"codec: share one range coder between methods"`

**Test checkpoint:** `python tests/test_correctness.py` exits 0 with `33 passed, 0 failed`; reverting the header to a copy with one changed constant makes the known-answer case fail.

> **Verified:** 2026-09-29 | §1 | build clean · 33 passed · KAT unchanged

## 2. …

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0: build, correctness, fuzz, and TODO gates green
- [ ] `python scripts/todo-graph.py validate` clean
```

### Sections appear in numerical order, and `## Verification` comes last

`## 1.` through `## N.` run in order down the file, with `## Verification` after all of them. New sections go in numerical position, not appended after `## Verification`: appending is what strands a section where no top-to-bottom reader finds it. The validator does not check this yet, so check it by eye when you add a section.

### The Implementation Order table IS the dependency graph

Every `## N.` body section has exactly one table row, and every row has exactly one body section. The graph script enforces both directions. `Depends On` uses the cross-reference notation below; `--` means no dependency.

`Status` is the section's own completion state and flips to `[x]` **only** when a `Verified:` stamp covering that section exists. The two are checked together.

## Cross-reference notation

| Form         | Means                                 | Example      |
| ------------ | ------------------------------------- | ------------ |
| `§N`         | Section N of this same file           | `§3`         |
| `TNN §N`     | TODO-NN in the same domain, section N | `T02 §1`     |
| `DNN TNN §N` | Domain NN, TODO-NN, section N         | `D01 T01 §4` |

Never a bare number, and never a cross-TODO reference without a section. `D01 T01` alone is not a dependency: it is a vague gesture at one.

**Cross-references are bidirectional.** If this file's Inputs point at `D01 T01 §4`, then `01-compression/TODO-01-….md` must point back at this file. One-sided XREFs are broken XREFs, and the validator flags them FATAL.

Skills under `.agents/skills/` may cite only full `DNN TNN §N` refs, and only to live sections: `validate` fails a skill that cites a short form or a dead section.

## Proof: what a Test checkpoint may cite

RasterFold is C11 built by `scripts/build.py` and its host wrappers with pinned local toolchains; Windows codec artifacts are freestanding, Linux uses libc, and the separately packaged converter may bundle audited codec/support libraries, with a Python oracle (`tests/rfi_ref.py`) and Python test drivers. A checkpoint cites one or more of these six, and **it must be able to fail**:

| Proof | What it is | What it cannot prove |
| ----- | ---------- | -------------------- |
| **Builds clean** | `python3 scripts/build.py` (Windows: `python scripts/build.py`) exits 0 and the compiler prints no warning for the touched files (`-Wall -Wextra`). The baseline gate every code section owes. | That the code does the right thing. It is a compile gate, nothing more. |
| **Static analysis clean** | `clang-tidy` from `tools/llvm-mingw` reports nothing new on the touched files. This gate does not exist yet (`D00 T01 §3` builds it); until it ships, no checkpoint may cite it. | Runtime behavior. Clean analysis over wrong logic is still wrong logic. |
| **Correctness test** | A named case of `python tests/test_correctness.py` (the codec against the independent oracle `tests/rfi_ref.py`, the frozen known-answer file, rejection cases), or `python tests/fuzz_decode.py N` for decoder hardening. Cite the case name and the pass count. | Speed and size. A passing round trip says nothing about how many bytes or cycles it took. |
| **Driven run with evidence** | Run `bin/windows-x64/bin/rasterfold.exe` on a real file and quote what it printed: the `rf_stat` lines of `--time`, the `inspect` fields, the exit code of a refusal. | Repeatability. A driven run is evidence of one run, so the section says what was driven and what it produced. |
| **Format fidelity proof** | An encode and decode round trip that compares pixels, and byte identity with the oracle: `test_correctness.py` does both for every case, and `tests/benchmark.py` checks decoded pixels for every image it measures. **Every change to what the encoder writes or the decoder accepts owes this one.** | Anything about speed. Fidelity proves bytes and pixels, not how fast they were produced. |
| **Corpus measurement** | A size or speed number measured by the protocol in `docs/research.md` section 2: `python research/ab.py size <variant> photo` (and `tp`, `synth`) for bytes, `python research/ab.py ab A.c B.c -n5` for time, `python tests/benchmark.py` or `python tests/measure_final.py` against JPEG XL. Quote totals, geometric means, and the worst image, and state thread counts. | Anything about one image in particular, and anything about a machine that was busy: a timing on a loaded machine is noise until repeated. |

Until the workspace domain ships a gate, a section says which gates do not exist yet. A checkpoint that cites a gate which does not exist is unfalsifiable, which is the one thing a checkpoint may never be.

**Every code section owes a clean build and the correctness suite.** A section that changes a `.c` or `.h` file and cites only a size number has skipped the cheapest gates it had.

### No image-specific tuning

A compression or speed claim is a claim about images in general. A section never adds a rule, constant, or branch that looks at the dimensions, hash, file name, or colour statistics of a particular test image, and never judges a change on `measure/image.bmp` alone. Tuning uses the `photo` research set; the test pack (`tp`) confirms it. `docs/research.md` section 2.3 is the protocol.

## Stamps

A stamp is a blockquote recording verified work, written **at the end of the section it covers**: after the test checkpoint, with that section's deferrals beneath it. It is the section's conclusion: you read what was asked, then what was actually proven.

```
## 3. Two More Context Models

One paragraph of context, then the checklist.

**Test checkpoint:** …

> **Verified:** 2026-10-02 | §3 | build clean · 35 passed · photo total 0.9691 (was 0.9712) · worst kodim17 1.0031 · tp 0.9725 · decode 3t 0.61 of JPEG XL
> **Deferred:** int16 weights for the wider mixer -> XREF: D02 T02 §1 (item: "…") -- needs the SIMD mixer first
> **Review:** round 1, fingerprint `a3f91c2e5b04` -- `adversarial` approve · `consistency` approve · `integration` needs-attention (1). Raw findings: docs/reviews/01-compression/D01-T02-s3.md
> **Plan review:** gpt high, no findings (run 20261002-D01-T02-S3-gpt)
> **CRUD:** not applicable (a model change has no write path beyond the file format, which the fidelity proof covers)
> **Implementer:** assistant name (model-id)
```

Stamp prose cites full `DNN TNN §N` refs, including the section's own: no same-section exemption, and multi-commit Review lines tag every candidate `` `oid`(round N) ``. Ownership pointers (`Deferred:`, `Resolved:`, `Depends On`, `-> XREF:`) keep their short XREF grammar.

Alternative markers, one per stamp (the last marker line governs, so these never stack):

```md
> **Plan review:** gpt high, filed D00 T01 §6 (run 20261002-D00-T01-S6-gpt)
> **Plan review:** gpt outage, outage: gpt rung (owner ann, due 2026-10-09)
> **Plan review:** gpt high, filed D00 T01 §6 (run 20261002-D00-T01-S6-gpt-r2, supersedes 20261002-D00-T01-S6-gpt)
```

- `Verified:` -- date, sections covered, and the *evidence*: real command output, not "it works".
- `Deferred:` -- one line per deferral, each naming a concrete owner via XREF. A deferral without an owner is an abandonment.
- `Review:` -- the independent review's cost and outcome: rounds, each lens's verdict with its finding count, then a link to the raw findings under `docs/reviews/`. A `Review:` line names its round and its candidate fingerprint, so a stale record cannot satisfy a freshness check for a different candidate.
- `Plan review:` -- the plan-review round's completion marker: either the pre-build `review-dispatch.py --kind plan` record (`<model> <effort>, <verdict> (<count>) -- docs/reviews/<domain>/D<dd>-T<tt>-s<n>.md#plan-review`) written by `python3 scripts/todo-graph.py stamp`, or a legacy marker naming which family ran it and the filings it produced (`no findings`, `outage: <rung> (owner <name>, due <YYYY-MM-DD>)`, or `retry-owed` / `partial:`). Legacy lineage: every legacy marker over a review record carries `(run <YYYYMMDD-DNN-TNN-SN-family[-rN]>)`, the run's date prefix is its timestamp; within one date base the bare base is run 1 and `-rN` is run N for N >= 2 (`-r1` is run 1's synonym, `-r0` is outside the shape); genesis is a singleton marker, and a rerun marker chains with `supersedes <prior-run>` or with `follows-outage` when the immediately preceding marker is an outage marker; runs never repeat within a section and the last run is one the manifest carries. A rerun opens a new `Plan review` record (ledger rows number past the file max across records). Grammar: the last marker line governs; `no findings` never sits beside filings; `outage:` never sits beside filings or `no findings`.
- `Completion:` -- the final `review-dispatch.py --kind completion` marker, run after the section's one adversarial round (`<model> <effort>, <verdict> (<count>) -- docs/reviews/<domain>/D<dd>-T<tt>-s<n>.md#completion-review`) written by `python3 scripts/todo-graph.py stamp`.
- `Reopened:` -- `<YYYY-MM-DD> | <finding ref> | <reason>`, naming the audit locus whose finding voids the stamp. A reopened section reads as unverified everywhere downstream: its row must be `[ ]` and its stamped dependents park until it re-stamps.
- `Retired:` -- `<YYYY-MM-DD> | <section ref> | <reason>`, migrating a grandfathered stamp without asserting a review that never ran. Validator-silent prose by design.
- `CRUD:` -- behavioral evidence for the section: the write path exercised and read back, not just checked for syntax. For this codec the write path is the file: an encode read back by decode (and by the oracle) is the read-back. Either `applicable | <what ran and what it proved>` or `not applicable (<reason>)`.
- `Started:` -- optional UTC instant written when implementation starts. Do not overwrite on resume.
- `Restarted:` -- `<YYYY-MM-DDTHH:MM:SSZ> | plan review <sha> | <reason>`, on the line directly under `Started:`, at most one per section. It recovers a build whose `--kind plan` review read a commit after the one that recorded `Started:` (a correction committed late), which `stamp` otherwise refuses, because the build start is the first commit carrying `Started:` and the plan review's candidate must be it or an ancestor. Write it only after that plan review is recorded and committed; `<sha>` is 7 to 40 lowercase hex digits of the review's candidate. `Started:` is never overwritten and `Duration:` still runs from it. With the line present the build start is the first commit carrying it, and `stamp` also requires that the line names the recorded plan review's candidate, that the findings file at the restart commit already holds that review, that the restart commit is the stamped commit or an ancestor of it, and that the findings file holds a returned review round whose recorded `Candidate:` (`review-dispatch.py --candidate` on a round) descends from the restart commit and is the stamped commit or an ancestor of it (a round of an older candidate does not count, whenever it ran or was committed). A completion review recorded before the restart stays valid. Items ticked before the restart are re-run after it and their evidence re-quoted. A malformed, repeated, or misplaced line is refused, never ignored.
- `Duration:` -- optional minutes or instant range from `Started:` to stamp (implement, review, and stamp): either integer minutes (`7`, `45m`) or `<start> to <end>` Zulu instants (`2026-09-26T14:54:33Z to 2026-09-26T16:35:19Z`). The range end orders clearance: when both reviews carry ends the target must complete strictly after the finding's review, else day stamps rule and same-day fails closed.
- Clearance tokens -- a section clearing a filed critical names `fix <sha>` (or `fix <base>..<tip>` for a multi-commit loop, base excluded) and `proof <finding-id> <path>[::<test>]` in its own text; the query proves the fix against git and the proof against the fix tree, and a clearance missing either stays listed.
- `Implementer:` -- optional `Name (model-id)` recording who built the section.
- `Resolved:` -- a deferral that has been closed. Replaces the `Deferred:` marker **in place**, keeping the original text and XREF and adding the date and what closed it. Closure is a state change, not a deletion: what was owed, and who paid it, both stay on the record.

### A deferral cannot be left to rot

A deferral hands work to another section, and the parser resolves the owner and notices when the owner ships. Four rules close the loop, and the important one is **fatal**:

| Condition                                                                                                   | Severity    |
| ----------------------------------------------------------------------------------------------------------- | ----------- |
| The `-> XREF:` owner does not resolve to a real section                                                     | `FATAL`     |
| The `(item: "…")` names a checklist item the owner does not have                                            | `FATAL`     |
| **The owner has shipped it -- item `[x]`, or the section's row `[x]` -- and the line still says `Deferred:`** | **`FATAL`** |
| Marked `Resolved:` while the owner has *not* shipped it                                                     | `WARN`      |
| No `-> XREF:` owner at all                                                                                  | `FATAL`     |

The stale case is fatal because advisory is what lets rot happen. The moment a section ships, any deferral waiting on it turns the build red until someone closes it: so **the section that resolves a deferral is the section that closes it**, which is also the only moment anyone has the information to write the closure line.

A deferral whose owner has *not* shipped is not stale; it is pending, and it stays open silently. Name the `(item: "…")` always: without it closure can only be detected when the whole target section completes, which is much later and much coarser.

Write a closure as:

```
> **Resolved:** 2026-10-02 | <the original text> -> XREF: D02 T02 §1 (item: "…") | closed by `ac90135`
```

`query deferred` lists open and resolved separately. Staleness is not reported there, because a stale deferral cannot reach that list: `validate` fails first.

### The struck-item deferral contract

A struck checklist item (`- [ ] ~~…~~`) hands its work to an owner section, and three rules make the handoff checkable, all FATAL under the partial-flip family. A stamp line matches by containment, but a deferred item must match word for word, because a containment match would bless a reworded item as the same debt.

1. The forward `-> XREF:` names the item: `(item: "…")` rides the XREF line. A bare XREF fails: the handoff names no debt.
2. The owner carries those exact words in order and consecutively, as whole words. `net` inside `network`, or the same words shuffled, fails: the item was reworded or removed.
3. A shipped owner records the debt done: `> **Resolved:**` citing the deferrer with `(item: ...)` matching the deferred item verbatim after stripping. The closure lives in the owner, beside the shipped work.

**Two kinds of rot the validator cannot see**, both owned by `process-todo-section`'s fact-check: a deferral whose *description* has drifted while its owner is still legitimately open, and one whose work was quietly done by someone who never ticked the owning box. Closing the second means ticking the owner's item too; leaving it unticked moves the rot up a level rather than removing it.

**The stamp lives with its section, not in a block at the top of the file.** The parser finds stamps anywhere in the file and maps each to its section by the `§N` in the body, so the `§N` stays in the text even though the heading above it already says so. That redundancy is what lets one stamp cover a range (`§1-§3`) and what keeps the format position-independent.

For the whole file at a glance, read the Implementation Order table: `[x]` cannot exist without a stamp covering it, and the validator makes that a FATAL. Re-verification replaces the stamp line in place. Never accumulate duplicates.

## Frozen behavior

Two behaviors are dangerous to get wrong because a mistake there does not fail loudly, it damages somebody's data:

1. **The `.fold` bitstream.** What the encoder writes and what the decoder accepts for a given format version (`spec/FOLD.md`, `RF_VERSION` in `src/rf.h`). A decoder that reads version 2 files differently tomorrow silently corrupts every file written today. A change to what method 1 writes needs a new format version; a new coding method needs a new method number; old versions and methods keep decoding or are rejected as unsupported, never misread.
2. **File safety.** An existing destination is never overwritten: output goes to `destination.partial` with `CREATE_NEW`, is flushed, and is renamed only after success (`rf_write_atomic` in `src/sys.c`).

**A format version is frozen from the release that first ships it.** Before that release, a version under development may be extended in place (version 3 across milestones 4 to 8), each extension regenerating its known-answer file; every earlier released or committed version keeps decoding unchanged. New versions are approved by the operator's standing approval (`D99 T01 §3`): a version takes effect when its combined measurement clears its milestone's bar, the held-out set confirms it, and every older version still decodes.

Any TODO touching one sets `frozen: true` in frontmatter, and every section that changes a frozen behavior carries a freeze check alongside its test checkpoint:

```
**Freeze check:** The version 2 known-answer file `KAT_FOLD` in `tests/test_correctness.py` still decodes to its pixels; the new method's files carry method number 2, and a version 2 decoder without the change rejects them as `unsupported fold: method`, never misreads them.
```

Restructuring frozen code is allowed when the bytes do not change (the known-answer case proves they did not). Changing what it *writes*, or what an existing version accepts, requires operator approval: the TODO records the approval, it does not grant it.

The frozen set as of this file is the two behaviors above.

## Surfaces

The Pages site and native converter are planned user-facing surfaces. Their sections declare `**Fidelity:**`, `**Design:**`, `**Treatment:**` and `**Chrome:**`, backed by `docs/design/documentation.md` or `docs/design/converter.md`. Design lines use `-- states: all in spec -- themes: all four -- density: both`. The design baseline remains empty: new surfaces receive explicit contracts, never exemptions.

The one surface line that is live is `**Job:**`: one sentence naming what a user can do once the section ships (`**Job:** A user can export ... and import it back ...`). It is the section's positive statement of what it delivers, and `query adjacency` reads it to find the section that owns an `applicable @ DNN TNN §N` declaration.

A section that cannot run without a live host or tool the plan cannot otherwise see carries one more line, anywhere in its body:

```
**Needs:** ffmpeg with libjxl (benchmarks)
```

The value comes from a closed list (`todo-graph.py` `NEEDS_ALLOWED`; `validate` refuses any other): `Windows host (build/test)`, `ffmpeg with libjxl (benchmarks)`, `Network (downloads)`, `Idle machine (timing)`, `GPU (compute)`, `ARM64 device`, `FPGA board`. A section carries one `Needs:` line (the last one counts), naming the most constraining host. `resolve` prints it as `needs`, so a runner can skip the row while the host is not reachable and take the next unblocked row instead. `Idle machine (timing)` marks a section whose checkpoint is a speed ratio: its measurement is repeated until two runs agree, or the section parks with the load named.

A section that cannot run without an environment capability the plan cannot otherwise see carries one more line, anywhere in its body:

`**Requires:** display-session -- convicted by <measurement pointer>`

Values are comma-separated closed vocabulary (`todo-graph.py` `REQUIRES_ALLOWED`); the reason after ` -- ` is required and cites the measurement that convicted the section. `validate` refuses an unknown value (`requires-unknown`) and a mark without its reason (`requires-no-reason`), both FATAL. `query ready` splits dependency-ready rows into runnable-now (requirements met by the runner's context) versus runnable-elsewhere (requirements named per row); `resolve` prints the line with the local verdict. `query ready --context` evaluates a declared set instead, for planning.

| Value | Means | Detected how (local context) |
| ----- | ----- | ---------------------------- |
| `display-session` | A Windows interactive session able to show windows | `sys.platform == "win32"` with `SESSIONNAME` naming an interactive session (not `Services`, never empty); every other context evaluates False |
| `windows-runtime` | Native Windows CLI/library execution | Explicit after a native execution probe; no GUI session required |
| `linux-runtime` | Native Linux CLI/library execution | Explicit after a native execution probe; no GUI session required |
| `linux-display` | Driven Linux display and accessibility evidence | Explicit after a real session probe; never inferred from DISPLAY or a build |
| `operator` | A step only the operator can take: a login behind the owner's account, a deletion the agent's safety rules refuse, an approval of a frozen-behavior change. Every section in `todo/99-manual/` carries it | Never detected: no agent self-reports an operator's keyboard, so only a declared `query ready --context operator` lists these rows as runnable now |

A section whose open work is worked OUTSIDE this tree carries a `Moved:` marker under its heading:

```
> **Moved:** 2026-10-02 to docs/plans/<plan>.md (operator instruction); worked there by its named owner.
```

The section keeps its Implementation Order row (`[ ]`, never ticked without a stamp) and every cross-reference, so addresses stay stable and `validate`'s reciprocity still holds. `query ready` and `query blocked` skip it, `query stats` counts it on its own `moved` line, `resolve` prints a `moved` line and exits 5, a dependency ON a moved section counts as met, and `plan --sync` replaces its plan row with one `> **Moved:** \`DNN TNN §N\` -- ...` line at the end of the table the row sat in, which `plan --check` requires and the progress arithmetic never counts. The body's first `path/to/file.md` must exist, or `validate` is FATAL (`moved-target-missing`).

## Milestones

The plan runs as 60 milestones (`docs/research.md` section 9), one phase each, in order. A milestone's phase holds its work sections and, last, its gate (`todo/05-milestones/`), which measures the milestone's target and the no-regression rule against the milestone before it. Milestones 2 to 4 held no photo size more than 0.1% worse, no encode or decode time more than 3% worse, no higher peak memory, and every older file still decoding; on 2026-10-30 the operator replaced that rule with a cost envelope (`D99 T01 §4`), stated in each gate and in `docs/research.md` section 9: Milestone 5 carries its own envelope, and from milestone 6 set totals hold size within 0.1% and each photograph within 0.5%, time within 1.10x (1.20x on `synth`, 1.5x per image of at least 16,384 pixels, 1 ms below that) from pooled alternating runs, codec working memory within 5%, and every older file still decoding. From milestone 11, the phase opens with a planning section that authors the milestone's work sections through `plan-experiment` from the previous milestone's measured results, one milestone ahead, so no section is written against numbers that do not exist yet. A missed target follows the missed-milestone policy (`D99 T01 §4`): up to three follow-up experiments are filed, and the operator is asked only when all three miss. The milestone files are generated from `research/milestones.py`; `python research/milestones.py --check` fails when they drift from its table.

## Section sizing

Max 30 checklist items per section. The validator warns above that; it is a ceiling, not a target.

Size a section by what holds together, not by a number. Two forces pull in opposite directions and both are real:

- **Too large** exhausts a fresh worker's context halfway through, and gives review more surface than it can cover well in one pass.
- **Too small** multiplies cost. A section is the unit the review contract prices, so three thin sections cost three review cycles where one coherent section costs one.

Split where the work genuinely divides: a measurement before an implementation, the C codec separate from the Python oracle only when they can ship apart (they usually cannot: a bitstream change touches both in one commit), a dependency boundary. Split at **authoring** time, not during implementation: splitting mid-flight costs a wasted context.

This guidance is not derived from measurement. `python scripts/todo-graph.py query calibration` compares each stamped section's item count against what it actually cost, and refuses to quote a correlation until enough sections have stamped; when it stops refusing, this paragraph is the thing to revisit.

### Feature adjacency: what a change owes beyond its core

Section sizing above decides how work is split. This decides whether the work is *all there*, and it is answered **before the Implementation Order table is final**, not at review.

So a TODO file's `## Outcome` carries an **Adjacency** line naming which of these apply, and which do not, with a one-line reason. For a codec the keys read like this:

- **List and filters** (`list`) -- can somebody find one of these without knowing its identifier? Usually not applicable to a codec.
- **Document or print** (`document`) -- what does a user carry away? For RasterFold: the written `.fold` or BMP file, or a benchmark report.
- **Settings with a named consumer** (`settings`) -- every command-line option (`--threads`, `--time`), and every encoder choice a user can make. An option nothing reads is worse than none.
- **Reporting** (`reporting`) -- what the tool tells about its own work: `inspect` fields, `--time` statistics, `benchmarks/results.md`.
- **Lifecycle notifications** (`notifications`) -- in a command-line tool, the exit code and the one-line error message.
- **Permissions, exercised on refusal** (`permissions`) -- an existing destination, an unreadable input, a path too long: the tool proves what it does when it is refused.
- **Audit and history** (`audit`) -- usually git history and the benchmark record.
- **Import or export** (`exchange`) -- wherever the tool reads or writes a file somebody else authored: the BMP subset, and every `.fold` version and method a decoder must keep reading.
- **The reverse of every create** (`reverse`) -- the decoder is the reverse of the encoder: every file the encoder can write must decode to the same pixels, and every file the decoder rejects must be one the encoder never writes.

**"Not applicable" is a declaration, not silence.** A gate script has no list; say so in one line. The check is advisory (`query adjacency`), because the tool cannot know which kinds a domain genuinely lacks: but a file that stays silent and a file that decided are not the same thing, and only one of them is a plan.

**Executable declaration:** place one `**Adjacency:**` line inside `## Outcome`. Write every key exactly once, separated by semicolons: `key=applicable`, `key=applicable @ DNN TNN §N` for an explicit cross-domain owner, or `key=not-applicable (source-backed reason)`. A narrow non-feature file may instead declare `all=not-applicable (reason covering its entire scope)`. It cannot mix the blanket form with individual entries. Preserve substantive decisions in an `**Adjacency rationale:**` paragraph; that prose is evidence for human review, not a second machine declaration. A missing/unknown/duplicate key, missing NA reason or declaration outside Outcome remains visible and prevents explicit closeout.

`python scripts/todo-graph.py query adjacency --json` derives owner candidates from positive Job, checklist and Build-order clauses, retaining source anchors. A reference must uniquely identify a real, non-Moved section with the claimed capability. These semantic matches require source review and do not prove runtime completeness.

Ordinary query and `validate` print `WARN [adjacency advisory]` without entering the structural warning ratchet. Before closing a file, run `python scripts/todo-graph.py query adjacency --file 'todo/NN-domain/TODO-NN-name.md' --require-owned`; refuse closure on missing/undeclared/unowned obligations and name the diagnostic.

## Work items: one checkbox is one buildable step

A section that only names an outcome ("make it faster") leaves a cold agent to invent the function, the constant, the test, and the cheaper substitute. That is how a model change ships that nobody measured, and how an option ships that nothing reads.

Each checklist item except `Commit:` is a **micro-step**. Required on new work:

1. **One action.** One file, function, constant, command, or test case. If you need "and then" to describe it, it is two items, or one item with numbered sub-steps.
2. **A named path** in backticks (`src/cm.c`, `code_bit()`, `python research/ab.py size photo`). A verb with no object ("improve prediction") is not an item.
3. **Done when.** The observable end state in the same bullet. Example: "Done when: `python research/ab.py size build/research/try.c photo` prints a total at least 0.1% below `0.9712` and a worst image no worse than `1.0047`."
4. **The cheaper substitute** on any item a measurement could fake, so the Test checkpoint can fail on it (tuning on `measure/image.bmp` alone, a speed number from one run on a busy machine, a format change without the oracle).
5. **A source cite** when behavior is copied: a file and line, a paper or reference implementation (libjxl, lpaq, paq8, CharLS), or `spec/FOLD.md` for the format.

Numbered sub-steps (`1.` `2.` `3.`) under an item are the **procedure** for that one checkbox. They are not extra Implementation Order rows and they do not get their own review cycle. Use them when the action has a fixed order (measure in `build/research/try.c`, then port to `src/cm.c`, then the oracle, then the spec).

**Build order** (a numbered list of files above the checklist) is the allowed retrofit on an already-written open section: it directs a cold agent without rewriting item text a deferral may name. Every numbered Build-order stage carries the same two non-negotiable anchors as a new micro-step: at least one intended implementation path, function, runnable command, or concrete source/evidence artifact in backticks, plus a literal observable `Done when:` in that stage. Prefer micro-step items on new work.

Split into a new `## N.` when the work has a different dependency or would push the parent past 30 items. **Split into a new FILE at 55 sections**, which `validate` enforces as FATAL: a section number is a permanent address, so a file only ever grows and can never be renumbered to tidy up. Split by subject, and leave every existing section exactly where it is. Do not split a stamped section.

**Do not rewrite a `[x]` section's checklist.** That is the contract the stamp covers. New granularity on shipped work is a new section.

## Measured claims

`validate` proves the tree is internally consistent. It cannot tell whether a `Current state` block is still **true**. So a TODO that measures something records it as a claim, an HTML comment on its own line (`exists`, `absent`, `lines`, and `count` forms; grammar and examples in `AGENTS.md` and the `scripts/todo-claims.py` docstring), and `python scripts/todo-claims.py` re-measures every claim under `todo/`, fenced or not, so this spec carries no literal example.

Claims are invisible in rendered Markdown and inert to every other parser. When a claim goes stale the TODO is wrong, not the repository: fix the claim and the sentence it supports in the same commit.

Write the block as `**Current state (verified YYYY-MM-DD):**` so `python scripts/todo-claims.py --coverage` can find it, count whether it carries a claim, and flag a block whose cited files changed after the date it states. Coverage is ratcheted by `COVERAGE_FLOOR` in `scripts/todo-claims.py`: raise it deliberately as coverage grows, never lower it to pass.

Size and speed numbers are claims too, but not ones `todo-claims.py` can re-measure. A `Current state` block that quotes a ratio names the command that produced it and the date, so the implementer re-runs it before relying on it (`process-todo-section`'s currency check).

## Completion-first

A section ships whole or it does not ship. Complete means every micro-step `[x]`, the Test checkpoint quoted from a real run, and a `Verified:` stamp written by `review-todo-section`: a `[x]` row with an open checklist reads as done while work remains, so it is a defect, not a head start. The `Commit:` micro-step is the one exception to the ticked rule: it is ticked in the implementation commit, and the row determines the reading of an unticked one (`[ ]` means doing, `[x]` means excused by syntax, the commit being proven by history rather than by the checkbox).

What cannot ship now is filed, not dropped: through `add-todo` with an owner, as a new section, an item on an open section, or a deferral the owner closes. Waiting is never a strategy: a blocked row parks with its blocker named, and work that can proceed does.

**A measured failure is a result, not a failure to ship.** Many compression and speed sections are experiments with a stop condition: "keep only if the photo total improves by at least 0.1%". A section whose experiment misses its bar ships as a negative result: the code change is reverted (or never ported from `build/research/`), the measurement is quoted in the stamp, the numbers are added to the "Already tried" table in `docs/research.md`, and every micro-step that would have ported the change is struck with a one-line reason pointing at the stamp. That is complete; quietly keeping a change that missed its bar is not.

Reviewers refuse stamps on partial scope. The mechanical check (a `[x]` row with an open checklist fails `validate`) is `partial-flip-shipped` below.

## Tooling

```bash
python scripts/todo-graph.py build      # parse todo/ -> build/todo-cache.json
python scripts/todo-graph.py validate   # structural + graph integrity checks
python scripts/todo-graph.py self-test  # the script's own suite; must stay green
python scripts/todo-graph.py query ready        # sections with all deps met, split into runnable-now versus runnable-elsewhere by the runner's context
python scripts/todo-graph.py query blocked      # sections waiting on something
python scripts/todo-graph.py query stats        # tree health
python scripts/todo-graph.py query plan-health  # review-loop governance: --json for machines; --check/--fail-on gate automation
python scripts/todo-graph.py query summary      # operator digest: incomplete runs, blocked clearances, overdue owners, next action, gate verdict
python scripts/todo-graph.py query run <id>     # one run ID resolves to candidate, scope, findings, lineage, outage, artifacts
python scripts/todo-graph.py query budget       # sections and discovered sections per phase, discovered sections per run (provenance), the backlog count, the budget history
python scripts/todo-graph.py query backlog      # every backlog entry: id, date, app, title, needs
python scripts/todo-graph.py query growth --since <ref>   # sections and backlog entries added or removed since a commit; reports, never fails
python scripts/todo-graph.py render             # mermaid dependency graph
python scripts/todo-graph.py plan --sync        # re-derive the checkboxes AND re-align every table (except one under <!-- no-align -->)
python scripts/todo-graph.py plan --check       # fail if the boxes are stale or a section has no row
python scripts/todo-graph.py resolve 'D01 T01 §3'   # ref -> file, section, deps, status
```

A table whose preceding line is `<!-- no-align -->` (blank lines between are fine) is left exactly as written, so wide prose tables such as the plan's acceptance bar wrap naturally instead of being padded to their longest cell; `plan --check` never fails on alignment either way.

The scripts are stdlib-only by design: no install step stands between a fresh clone and validating the plan. On Windows, `python` is the interpreter; use `python3` where that is what your shell has.

`resolve` is the front door for the two section skills, and it takes whatever you already had in front of you: a `DNN TNN §N` reference, a `<path> §N` pair, or a row pasted straight out of `implementation-plan.md`, backticks, pipes and all. Its exit code carries the verdict: `3` means the section is already `[x]` (audit stance, not implementation), `4` means a dependency is unmet, `5` means the section moved out of the tree. So

```
process todo section: | [ ] | `D01 T01 §1` | Shared range coder header | none | 3 |
```

is a complete instruction: nobody has to translate domain `01` and TODO `01` into a filename, which is the step that gets done wrong at 3am.

`build/` is gitignored: the cache is always reproducible from the markdown.

### The implementation plan: phases and rows

[`implementation-plan.md`](./implementation-plan.md) is the second derived artefact, and the only one that is committed: it is prose a person reads, so it cannot live in `build/`. Its structure is authored; its boxes, item counts, and progress line are derived.

- A phase is a level-3 heading of exactly the shape `### Phase <N> -- <Title>`, numbered from 0 in run order, followed by one paragraph saying why it runs where it does, then one table.
- The table header is `| ✔ | Section | Deliverable | Needs | Items |`. Each row is `| [ ] | \`DNN TNN §N\` | <deliverable> | <needs> | <items> |`: the box, the needs cell and the items cell are rewritten by `plan --sync`, the deliverable is prose you author (usually the Implementation Order row's deliverable). A new row may be typed without the two derived cells' values (`none` and `0` do); the next sync fills them.
- The Needs cell is what a runner must have before it takes the row: the closed-list key of the section's `**Needs:**` line (`windows-host`, `ffmpeg-jxl`, `network`, `idle-machine`, `gpu`, `arm64-device`, `fpga-board`), then every `**Requires:**` value, comma-separated, or `none`. It is derived from the section, never typed: change the section's line and sync. `plan --check` fails on a stale or missing Needs cell and on a phase table whose header lacks the column.
- **Every section in the tree appears in exactly one row of exactly one phase.** Phases have no size limit. Authoring a TODO file therefore includes placing each of its sections in a phase table; `plan --check` fails on a section with no row, a row naming no section, a duplicated row, or a stale box.
- A section's phase is chosen so every `Depends On` edge points at a section in the same or an earlier phase, and within a phase rows run in table order; `groom-plan` checks both.
- The `> **Progress:**` line under the title is rewritten by `plan --sync`, including the tree's size (sections, and how many of them a campaign discovered) and the backlog's entry count, in the form `<N> sections (<M> discovered); backlog <K> entries.`, and `plan --check` fails when the line is stale. Never type a total into prose.

The boxes **are never ticked by hand**; `plan --check` runs in CI and in the pre-commit hook's validate path, so a stale projection fails instead of quietly misinforming whoever reads it next. Run `plan --sync` after any row flips.

`plan --check` always validates the canonical Markdown projection in the selected working tree or staged export. It also verifies `build/todo-progress.json` and `build/todo-operator.json` when present; these ignored caches may be absent in a fresh clone or staged export. Checking creates neither cache and never rewrites the plan. `plan --sync` regenerates all three projections.

**CI enforces this contract on every main-branch push and pull request.** The plan workflow runs `self-test`, `validate`, `plan --check`, dependency/vision coverage and document drift checks, so a FATAL (or a NEW warning) fails the build like any other defect.

### The budget and the backlog

Nothing in the plan is capped. The rules here are about provenance and loss, not size: every discovered section says which run filed it, nothing leaves the backlog without a trace, and no release ships without reviewing the backlog. `validate` enforces all of it as FATAL, so the commit hook and CI refuse a breach. The structural limits (30 items per section, 55 sections per file) are separate.

**Operator-directed and discovered sections.** A section written in an attended session at the operator's request is operator-directed: it carries nothing extra. A section an agent files on its own during a campaign (a `process-phase` gap audit, a `review-todo-section` research or plan-review finding, a `groom-plan` gap scan, an adjacent defect or prerequisite filed through `add-todo` mid-run) is discovered, and carries one line in its body, directly under its heading:

```
**Origin:** discovered run=<run id> <YYYY-MM-DD> -- <what found it>
```

The run id is the 12-hex-digit id `campaign_guard.py` minted for the run (the one its run file's `run=<id>` markers carry); a groom outside a campaign mints one with `python scripts/campaign_guard.py mint-generation` and records it in the groom record. The ` -- <what found it>` note is optional. No line means operator-directed. The line is provenance, not a quota: `query growth` and `query budget` report discovered sections by run so the operator can see what a campaign added, and no count of them fails anything. `validate` refuses a near miss (a malformed id or date, a missing word, `**Origin:** operator`), a second Origin line in one section, and one outside a numbered section (`origin-malformed`), so a discovered section cannot pass as operator-directed.

**`todo/budget.json`** (schema 3) records the budget decisions:

- `backlog_cap` and `per_run_discovered_sections`: both `null`, meaning no cap. A whole number here is `budget-malformed`.
- `history`: an append-only list of `{date, change, reason, approved_by, snapshot}` entries; the first entry (2026-09-29) records the port with both caps `null`. A history only moves forward, and it must hold a schema 3 entry.

The live values must equal the latest entry's `snapshot` (`budget-unrecorded-change` otherwise). The history is checked against the committed file (`git show HEAD:todo/budget.json`): an entry edited or removed fails. An entry that raises a cap (removing one counts as a raise) must say `approved_by: operator` and quote the operator's words in `reason`. No skill changes the budget on its own initiative.

**`todo/backlog.md`** is a flat list of ideas worth keeping that are not planned work. It is not in the plan, never runnable, never counted by plan parity, never capped, and nothing may defer to it. One entry per line:

```
- [B-NNN] <title> -- source: <key> -- added: YYYY-MM-DD -- why deferred: <text> -- promote when: <text>
```

with optional `app:` (`rasterfold` or `suite`; no field reads as `suite`, and RasterFold's one app is also the suite, so its release reviews every entry), `summary:`, and `needs:` fields (`needs:` cites full `DNN TNN §N` refs to live sections, or other backlog ids), and the repeatable `merged:` and `reviewed:` fields below. `source` is one key token: a finding ID, a `docs/research.md` item, or the `-> SOURCE:` key the section would carry, so a scan that runs twice finds its own entry. Ids are taken in order and never reused. `validate` refuses a malformed line, a line that reads as a section, a dead or short ref, a duplicate id or source, and an entry whose source a live section already carries. `query backlog` lists it.

**The admission test** applies to discovered work only. A campaign files a discovered section only when the work is one of:

1. a defect in shipped or in-flight work;
2. something an aim in the plan's acceptance bar requires;
3. a prerequisite of an existing planned row.

Everything else (an idea from a paper, a nice-to-have from a review) goes to the backlog, where it is kept, not lost. Work the operator asks for skips the test: the operator's request is the admission. A discovered item that passes the test lands where it fits best: an item on an open section whose scope holds it, or a new section with its Origin line.

**No drop without operator approval.** Every source key HEAD's `todo/backlog.md` holds (an entry's `source`, a merge marker, or a removal record) must still be accounted for in the working file, or `validate` fails with `backlog-dropped` (checked against `git show HEAD:todo/backlog.md`, skipped when HEAD has no such file). An entry leaves the backlog in exactly three ways:

1. **Promotion**: `add-todo` on the entry. The promoting commit writes the section with the entry's source as its `-> SOURCE:` line and deletes the entry. A promotion the operator asks for is operator-directed; one a campaign makes on its own passes the admission test and carries an Origin line.
2. **Merge**: two entries for one real-world thing become one. The survivor (the lower id) keeps the absorbed entry's key as a merge marker, in its `summary:` or as its own `merged:` field, and keeps it for as long as it lives:

   ```
   merged: `<source-key>` (B-NNN, YYYY-MM-DD)
   ```

   The backticks are required; the parenthesis (the absorbed id and the day) is recommended. A marker naming a key a live entry still carries is `backlog-duplicate`: the merge deletes the absorbed entry.
3. **Operator removal**: only when the operator says so, in words. The removing commit deletes the entry and appends one line under `## Removed with operator approval` at the end of `todo/backlog.md`:

   ```
   - B-NNN <source> -- removed YYYY-MM-DD -- operator: "<the operator's words, verbatim>"
   ```

   A record's id and source are never reused, and the record itself is never deleted (deleting it is `backlog-dropped` too). No skill removes an entry on its own judgement that it is stale; it asks the operator.

**The release-time backlog gate.** Every release section (one whose checklist pushes a tag, ``Push the tag `rasterfold-vX.Y.Z` ``) carries the item

```
- [ ] Run the backlog review for `rasterfold-vX.Y.Z`: ...
```

or `validate` fails with `release-backlog-review-missing`. Running it means walking every backlog entry and, for each, either promoting it into a section (through `add-todo`; a full promotion deletes the entry) or asking the operator whether it may wait and recording the answer on the entry:

```
-- reviewed: rasterfold-vX.Y.Z YYYY-MM-DD promoted DNN TNN §N
-- reviewed: rasterfold-vX.Y.Z YYYY-MM-DD deferred by operator: "<the operator's words>"
```

`promoted` is for an entry only part of which became a section (the rest stays); the ref must be live. The words must not contain ` -- `. One `reviewed:` field per release per entry, appended after the entry's other fields; fields already committed are never edited or removed (`backlog-review-rewritten`). Once a release section is stamped `[x]`, every in-scope entry added on or before the stamp day must carry a `reviewed:` field for that tag, or `validate` fails with `release-backlog-unreviewed`.

### FATAL blocks; WARN is ratcheted

`validate` reports two severities under a **two-layer contract**: a FATAL always exits `1` and is never ackable; a WARN is ratchet-managed, so one already in `todo/.warning-baseline` or the ack ledger exits `0`, while a NEW one prints as `WARN*` and exits `1` until it is fixed or deliberately accepted with `warnings --accept`. "Advisory" therefore describes only warnings that are already baselined or acked, never a new one.

| Severity | Exit code | Meaning |
| -------- | :-------: | ------- |
| `FATAL`  |    `1`    | The graph or the format is broken: a `Depends On` that does not resolve, a section without its Implementation Order row, a row without its section, a one-sided XREF, a deferral with no owner, a `superseded` TODO with no successor, and every other structural class in the per-class table below. Fix it before doing anything else. |
| `WARN`   | see below | Ratchet-managed: a warning already in `todo/.warning-baseline` or the ack ledger exits `0`; a NEW one prints as `WARN*` and exits `1` until fixed or deliberately accepted with `warnings --accept`. |

**The per-class map, mirrored from `SEVERITY_MAP` in `scripts/todo-graph.py`: the self-test compares this table to the map row-for-row, so editing one without the other fails `self-test`.** The rule is push-time actionability: FATAL where the fix is mechanical and the defect is a structural-integrity break; WARN where the fix is a judgement call a red build cannot resolve. The `fidelity-*` and `design-*` classes apply to the planned site and converter surfaces (see "Surfaces").

| Class | Severity | Why |
| ----- | -------- | --- |
| `over-section-cap` | FATAL | A TODO file may hold at most **55** sections. Past that the next work opens a NEW file in the same domain, split by subject. Section numbers are permanent addresses, so a file can never be renumbered or made smaller. |
| `duplicate-source-key` | FATAL | Two sections claim the same `-> SOURCE: <key>`. An automated filer stamps what it filed FROM, so a scanner that runs twice cannot open a second row for one build failure. |
| `evidence-citation-short-form` | FATAL | New evidence cites full `DNN TNN §N` refs on stamps, findings files, and attestations. |
| `review-citation-role-less` | FATAL | A multi-commit Review line tags every candidate with its round; round-to-commit mapping must be mechanical. |
| `review-citation-role-mismatch` | FATAL | A round tag matching no recorded panel round, or two candidates sharing one round; the mapping must correspond, not merely exist. |
| `superseded-no-successor` | FATAL | A superseded TODO must name where its work went; mechanical. |
| `filter-overclaim-open` | FATAL | An open checkpoint that cannot detect its promised regression; naming the tests or scripts is a two-minute fix. |
| `filter-overclaim-stamped` | WARN | The stamp must not be reopened; the fix-forward channel owns it. |
| `no-commit-item` | FATAL | One section = one commit is the format's core contract. |
| `partial-flip-shipped` | FATAL | An unticked micro-step outside the exemptions inside a `[x]` section breaks the shipped claim itself. |
| `commit-history-unreadable` | WARN | History the `Commit:` binding cannot read is unverified, not broken; a bare-tree export must still validate. |
| `fidelity-missing-lines-open` | FATAL | A Fidelity surface with no Job/Treatment/Chrome is unimplementable in house style. |
| `fidelity-missing-lines-stamped` | WARN | Fix-forward: the gap is real, the stamp stays. |
| `no-checklist-items` | FATAL | An empty section is unimplementable (co-emits `no-commit-item`). |
| `over-30-items` | WARN | Sizing is a judgement call; a red build cannot split a section. |
| `frozen-no-freeze-check` | FATAL | A frozen TODO without its check is a safety-marker mismatch; mechanical either way. |
| `freeze-check-not-frozen` | FATAL | A Freeze check in a TODO whose frontmatter does not say `frozen: true` is the same safety-marker mismatch in the other direction; mechanical fix. |
| `bare-todo-ref` | WARN | Prose legitimately mentions a TODO file without a section. |
| `one-sided-xref` | FATAL | This spec calls it broken; the validator agrees. |
| `deferral-no-owner` | FATAL | A deferral with no owner is an abandonment. |
| `resolved-owner-unshipped` | WARN | Recording early resolution is evidence, not a defect. |
| `missing-from-index` | FATAL | Two-line mechanical fix; discoverability is structural. |
| `malformed-stamp` | FATAL | A `Verified:` line the parser refused. It reads as evidence while verifying nothing, so it is worse than a missing stamp; the fix is to write the line correctly. |
| `needs-unknown` | FATAL | A `**Needs:**` value outside the closed list in `todo-graph.py` (`NEEDS_ALLOWED`). The list is closed so a misspelt host cannot silently unmark a section that cannot run without it. |
| `moved-target-missing` | FATAL | A `> **Moved:**` marker that names no file, or a file that does not exist. The marker takes the section out of `query ready`, the plan and the progress totals on the strength of that pointer, so a dead pointer would hide work. |
| `pending-control-contract` | FATAL | Reserved: the coming-soon inspector is not ported yet, so this class cannot fire until it lands. |
| `stamp-no-opus-panel` | FATAL | A stamp whose findings carry no panel verdicts. It reads as reviewed evidence while verifying nothing. The governing record is the last `GPT panel` section; Codex-written candidates use the independent `Claude panel`; Claude-written candidates retain the `GPT panel`. The writer family never reviews its own candidate. The class keeps its historical name and the legacy date rules the ported validator carries. |
| `requires-unknown` | FATAL | A `**Requires:**` value outside the closed list in `todo-graph.py` (`REQUIRES_ALLOWED`). The list is closed so a misspelt capability cannot silently unmark a section. |
| `requires-no-reason` | FATAL | A `**Requires:**` mark without its `-- reason`. The citation is what makes the mark auditable instead of vibes. |
| `stamp-no-plan-review` | FATAL | A stamp that carries no `Plan review:` completion marker. The plan-review round is required procedure. |
| `plan-review-malformed` | FATAL | A plan-review record the query cannot parse: a `Plan review` section without its Manifest line or its Ledger block, a non-row line inside the block, or a content-illegal row. |
| `filed-target-no-backlink` | FATAL | A filed ledger row whose target file carries no back-link. The filing is untraceable from the target side, so remediation cannot be attributed to the finding. |
| `stamp-reopened` | FATAL | A `> **Reopened:**` line outside its shape, on a still-checked row, or with a still-stamped dependent. A reopen that does not void proof downstream lets work continue on invalid evidence. |
| `plan-review-duplicate-id` | FATAL | A finding ID appearing twice in one ledger. Multi-target findings ride one row with every target, never split rows. |
| `plan-review-no-lineage` | FATAL | A `Plan review:` marker without run lineage: no run ID, a reused run ID, a rerun marker naming no superseded run, or a run the manifest does not carry. |
| `ledger-history-violation` | FATAL | A ledger row whose disposition moved the forbidden way against the committed record, or a row that vanished. Later evidence amends via a new row, never by rewriting the old one. |
| `provenance-malformed` | FATAL | A findings file without a well-formed `Provenance:` line, or a provenance line outside the field shape, without a shaped run ID, with an unresolving candidate, a missing path, or a run no marker of its section carries. |
| `ledger-supersession-broken` | FATAL | A ledger row whose `supersedes` link names no row of its block, crosses review namespaces, or closes a cycle. |
| `risk-acceptance-malformed` | FATAL | A `Risk accepted:` line outside the record shape, with an uncoverable target, expiring before it is recorded, or reviewed outside its record-expiry window. |
| `risk-acceptance-silent-edit` | FATAL | A risk acceptance whose owning record changed after the evidence commit without a superseding record. |
| `risk-acceptance-chain-broken` | FATAL | A `supersedes <date>` link that names no earlier record on its target, points forward, branches, or cycles. |
| `skill-citation-unresolved` | FATAL | A skill citing a full section ref (`DNN TNN §N`) that resolves to no live section. |
| `skill-citation-short-form` | FATAL | A skill citing a short section form (a bare section mark, `TNN` plus a section, or a file plus a section) instead of a full `DNN TNN §N` ref. |
| `budget-malformed` | FATAL | `todo/budget.json` missing, unparseable, or outside schema 3: an unknown key (a phase ceiling included), a live cap that is not `null`, a history entry out of shape, out of date order, or older in schema than the one before it, or a history with no schema 3 entry. |
| `budget-unrecorded-change` | FATAL | A live value that differs from the latest history snapshot, a history entry edited or removed against the committed file, or a raise (removing a cap included) of `backlog_cap` or `per_run_discovered_sections` without `approved_by: operator` and the operator's quoted words. |
| `origin-malformed` | FATAL | A `**Origin:**` line outside the grammar `**Origin:** discovered run=<12-hex run id> <YYYY-MM-DD>` (optional ` -- <note>`), a second Origin line in one section, or one outside a numbered section: a discovered section whose provenance nobody can read. |
| `backlog-malformed` | FATAL | A `todo/backlog.md` line outside the entry grammar, a line that reads as a section (checkbox, numbered heading, stamp, XREF), an entry citing a dead or short section ref, or a merge marker, `reviewed:` field, or operator removal record outside its grammar (or a record outside its heading). |
| `backlog-duplicate` | FATAL | A backlog id or source key used twice (entries, merge markers, and removal records share the namespace), or a source key a live section already carries: a promoted entry leaves the backlog in the promoting commit. |
| `backlog-dropped` | FATAL | A source key HEAD's backlog holds (an entry, a merge marker, or a removal record) that the working file no longer accounts for: not carried by a live section's `-> SOURCE:` line, a surviving entry's merge marker, or an operator removal record. Nothing leaves the backlog without the operator's approval. |
| `backlog-review-rewritten` | FATAL | A `reviewed:` field HEAD's entry carries that the working entry lost or changed: a release's backlog review is append-only. |
| `release-backlog-review-missing` | FATAL | A release section (it pushes an app tag) without its ``Run the backlog review for `<tag>` `` checklist item. |
| `release-backlog-unreviewed` | FATAL | A release section stamped `[x]` while a backlog entry in its scope (its app or `suite`; every entry for `rasterfold`), added on or before the stamp, has no `reviewed:` field for that tag. |
| `design-missing` | FATAL | A section whose Fidelity line names a surface carries no `**Design:**` line and is not listed in `todo/.design-baseline`: a surface built from a guess instead of the spec it implements 1:1. |
| `design-malformed` | FATAL | A `**Design:**` or `**Design deviation:**` line outside its grammar (near misses included), a second Design line in one section, a line outside a numbered section, or `new surface:` without the item that adds the spec to `docs/design/` first. |
| `design-dead-ref` | FATAL | A Design ref whose file does not exist or whose anchor matches no heading (slugged GitHub-style) or HTML id, or a deviation whose follow-up names no live section. |
| `design-baseline-stale` | FATAL | A `todo/.design-baseline` ref that now has its Design line, is stamped, is no longer a surface, names no section, is listed twice, or is not a ref: the list only shrinks, in the commit that makes the entry unnecessary. |
| `design-baseline-grown` | FATAL | A `todo/.design-baseline` ref HEAD does not list: the baseline records the gaps of 2026-09-27, and a new gap is never exempted. |
| `design-deviation-open-at-release` | FATAL | A `**Design deviation:**` still open (its follow-up row not `[x]`) in a file whose app has a release section (the one that pushes its tag) stamped on or after the day the deviation opened. |

Treat a warning as a decision to make rather than noise to clear. The tree starts at zero FATAL and zero non-baselined warnings, and it is worth keeping there.

## Skills

| Skill                   | Use                                                                              |
| ----------------------- | -------------------------------------------------------------------------------- |
| `add-todo`              | **Front door.** Route new work to the right domain, file, and section            |
| `create-todo`           | Author a whole new TODO file, wire XREFs, update indexes                         |
| `plan-experiment`       | Turn a codec idea into a measured, falsifiable TODO section with a stop condition |
| `groom-plan`            | Harden the tree for a weaker executor: sequence, drift, gaps, complete features  |
| `process-todo-section`  | **Fact-check the plan, correct what has drifted, then ship exactly one section** |
| `review-todo-section`   | Quality gate after implementation; writes the stamp                              |
| `process-todo-file`     | Loose-end sweep and closure when every section is `[x]`                          |
| `process-phase`         | Attended phase runner: repair + gap-audit the phase, then ship it to 100%        |
| `process-plan`          | Front door for `implementation-plan.md`; chains ready phases                     |

When something needs doing, start at `add-todo`: it decides whether the work belongs in an existing section, needs a new one, warrants a whole new file (delegating to `create-todo`), or is already covered. Reaching for `create-todo` directly tends to produce a second TODO over an existing one.

### Native converter execution contexts

`windows-runtime`, `linux-runtime` and `linux-display` are explicit `Requires` values. Declare them with `query ready --context` only after recording a working native display probe. Windows desktop proof uses `display-session`. An environment variable, installed SDK, cross build or explicit context flag is never runtime proof. The relevant sections retain their required driven tests, accessibility checks and evidence even when admitted.
