---
schema_version: 1
id: drift-gates
domain: 08-docs
status: done
title: "TODO-02 -- Drift and Staleness Gates for Docs, READMEs, and Supporting Files"
depends_on: []
track: D2
---

# TODO-02 -- Drift and Staleness Gates for Docs, READMEs, and Supporting Files

> **Goal:** No document in the repository can quietly disagree with the code or the data. Names, versions, limits, exit codes, and messages come from one facts file; every measured figure in a README or doc is bound to the committed data it came from; generated files are checked against their sources; every page states when it was verified against the files it describes and goes stale when they change; and every link resolves. The commit hook, `scripts/check-all.ps1`, and CI all refuse drift.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** `scripts/drift-check.py` and `docs/drift.json` are implemented, verified, and stamped across §1-§7, together with staged hooks (`tools/githooks/pre-commit`, `tools/githooks/commit-msg`), local gates (`scripts/check-all.py`, `scripts/check-all.ps1`) and CI (`.github/workflows/plan.yml`). They cover all supported tracked/new information files, explicit source facts, data bindings (`csv`, `csv-meta`, and `csv-aggregate`), generated blocks, source review hashes, commit/path references, `Drift-Fact:` commit fact declarations, built-site receipts (`build/site-artifact/drift-build.json`) bound to `docs/site-content.json`, and deterministic repair. Historical measurements retain their dates and limitations; live GitHub Pages deployment enforcement and deployed receipt read-back are owned by `D08 T01 §3`.
<!-- claim: exists scripts/drift-check.py -->
<!-- claim: exists docs/drift.json -->

## Inputs

- [`include/rasterfold.h`](../../include/rasterfold.h), [`src/rf.h`](../../src/rf.h), [`src/main.c`](../../src/main.c) -- the C headers and CLI entry point `docs/facts.json` derives its values from (product SemVer `VERSION` is reserved for `todo/09-release/TODO-01-release-v1.md` section 1)
- [`scripts/todo-claims.py`](../../scripts/todo-claims.py) -- the claim checker §1 extends to the whole repository
- [`README.md`](../../README.md), [`docs/research.md`](../../docs/research.md), [`research/README.md`](../../research/README.md), [`spec/FOLD.md`](../../spec/FOLD.md), [`AGENTS.md`](../../AGENTS.md) -- the documents under the gates
- [`tools/githooks/pre-commit`](../../tools/githooks/pre-commit), [`scripts/check-all.ps1`](../../scripts/check-all.ps1), [`.github/workflows/plan.yml`](../../.github/workflows/plan.yml) -- where §5 wires the gates
- -> XREF: D00 T02 §3 -- `benchmarks/corpus.csv`, the corpus data §2 binds figures to
- -> XREF: D08 T01 §1 -- the site generator whose `--check` §3 runs

## Outcome

- `python scripts/drift-check.py` exits 0 on the tree and 1 on each seeded kind of drift (a wrong fact, a figure unlike its data, a stale generated file, a page whose reviewed source hashes differ, a dead link).
- The commit hook, `scripts/check-all.ps1`, and the `plan-gates` workflow run it.

**Adjacency:** list=not-applicable (a gate has no records to browse); document=applicable @ D08 T02 §6; settings=not-applicable (no option); reporting=applicable @ D08 T02 §4; notifications=not-applicable (a gate reports on the console and in CI); permissions=applicable @ D08 T02 §5; audit=applicable @ D08 T02 §7; exchange=not-applicable (nothing imported or exported); reverse=not-applicable (repair uses normal Git diffs; it never rewrites history)

**Adjacency rationale:** Reporting is the staleness report; permissions is the policy that refuses a drifting commit.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | One facts file and claims everywhere | -- |  [x]   |
|   2   |   §2    | Figures bound to their data | §1, D00 T02 §3 |  [x]   |
|   3   |   §3    | Generated files match their sources | §1, D08 T01 §1 |  [x]   |
|   4   |   §4    | Staleness: verified dates and data revisions | §2 |  [x]   |
|   5   |   §5    | Links, and the gates wired everywhere | §3, §4 |  [x]   |
| 6 | §6 | Deterministic repair and complete file coverage | §1, §3 | [x] |
| 7 | §7 | Commit and built-site provenance | §5, §6 | [x] |

---

## 1. One Facts File and Claims Everywhere

> **Started:** 2026-10-25T01:58:00Z

- [x] Write `docs/facts.json`: the product and format names, the public C API (`RF_API_VERSION_MAJOR`, `RF_API_VERSION_MINOR`) and format (`RF_VERSION`) versions (product SemVer `VERSION` is reserved for `todo/09-release/TODO-01-release-v1.md` section 1), the size limits (`RF_HEADER_SIZE`, `RF_MAX_DIM`, `RF_MAX_PIXELS`, `RF_MAX_FILE`), the exit codes and their meanings, the command names, and the file extensions. Done when: every value matches `include/rasterfold.h`, `src/rf.h`, and `src/main.c`, checked by `scripts/drift-check.py`. Cheaper substitute: a list in the README.
- [x] Implement explicit fact bindings, generated current-fact blocks and supported prose assertions (`format_version`, `api_major`, `api_minor`, `max_dimension`, `max_pixels`, `max_file_bytes`) in `scripts/drift-check.py` and `docs/drift.json`. Done when: a wrong current format/API major/API minor/limit value and a malformed binding fail; unbound arbitrary prose is reported as outside semantic proof, never silently repaired.
- [x] Reuse `scripts/todo-claims.py` claim evaluation from `scripts/drift-check.py` across supported information files. Done when: a stale or unterminated claim in `README.md` fails, while fenced examples and historical/planned roles remain explicit.
- [x] Add regression tests in `tests/test_drift.py` for the expanded `docs/facts.json` derivation against the live repository (`include/rasterfold.h`, `src/rf.h`, `src/main.c`), wrong format/API major/API minor/limit prose assertions, malformed bindings, stale and unterminated claims in `README.md`, and preservation of fenced examples and historical/plan roles. Done when: `python3 -m unittest tests/test_drift.py -v` passes.
- [x] Commit: `"docs: one facts file and claims checked in every document"`

**Test checkpoint:** `python3 -m unittest tests/test_drift.py -v` passes; `python3 scripts/drift-check.py` exits 0 on the clean tree; independent seeded CLI runs against `README.md` for (a) wrong format, API major, API minor, and limit prose assertions, (b) a malformed fact binding, and (c) a stale and an unterminated claim each exit 1 (quoted, then reverted with a clean `python3 scripts/drift-check.py` exit 0 read-back).

> **Verified:** 2026-10-25 | §1 | 8c4abb36805458823cd883f3bd3dce91857c99e2
> **Review:** `docs/reviews/d08-t02-s1.json`
## 2. Figures Bound to Their Data

> **Started:** 2026-10-25T04:36:30Z

- [x] Add `csv` (exact row/column selector) and `csv-aggregate` (`count`, `sum`, `ratio`, `geomean`, `max`, and `csv-meta` header provenance, with explicit corpus group membership `photo` 39, `tp` 10 overlapping, `tp-only` 5 exclusive, `synth` 11, `all` 55, ratio denominators, and display precision) providers in `docs/drift.json` and `scripts/drift-check.py`, place `<!-- drift:fact <provider-key> = <value> -->` beside every current measured figure in `docs/benchmarks.md`, and verify both that the provider recomputes to `<value>` and that the visible figure immediately preceding the annotation matches `<value>`. Done when: every current published measurement carries a binding and provenance, and legacy research (`docs/measurements.md`, `docs/research.md`, `docs/research-summary.md`) remains labeled `historical` rather than falsely bound to newer data. Cheaper substitute: rounding the figures vaguely so they never look wrong.
- [x] Require current measurement summary blocks (`benchmarks-measurements` in `docs/benchmarks.md`) to be generated from registered providers in `docs/drift.json` `blocks`, keeping historical reports outside current-performance claims, and add regression tests in `tests/test_drift.py` for independent aggregate expectations, changing a visible figure while retaining its annotation, editing generated measurement content, removing required block markers, missing/ambiguous CSV selections, and preserving historical text. Done when: editing a current generated measurement block or visible figure makes `python3 scripts/drift-check.py` exit 1 and `python3 -m unittest tests/test_drift.py -v` passes.
- [x] Commit: `"docs: bind every measured figure to its data"`

**Test checkpoint:** `python3 -m unittest tests/test_drift.py -v` passes (`Ran 49 tests in 10.027s, OK`); changing one CSV cell (`benchmarks/results.csv` `1409028 -> 1409999`: `fact: docs/benchmarks.md: line 20: measurement_reference_bytes must equal 1409999; review the surrounding sentence`, `generated: docs/facts.json: deterministic output differs; run --fix`, `rc=1`), changing a visible figure while keeping its `drift:fact` annotation (`docs/benchmarks.md` `` `1,409,028` -> `1,409,999` ``: `fact: docs/benchmarks.md: line 20: visible figure before measurement_reference_bytes does not match 1409028; review the surrounding sentence`, `generated: docs/benchmarks.md: deterministic output differs; run --fix`, `rc=1`), editing the generated `benchmarks-measurements` block (`RGB8, -> RGB24,`: `generated: docs/benchmarks.md: deterministic output differs; run --fix`, `rc=1`), and removing its block markers (`block: docs/benchmarks.md: required block benchmarks-measurements is missing; restore its markers`, `rc=1`) each make `python3 scripts/drift-check.py` exit 1 naming the mismatched figure/block, then restored to exit 0 on the tree (`drift: 210 supported documents; 0 error(s); 0 deterministic repair(s)`).

> **Verified:** 2026-10-25 | §2 | e0e662d2de0bf0f5dd26b1a69067957d6a0d8bc0
> **Review:** `docs/reviews/d08-t02-s2.json`
## 3. Generated Files Match Their Sources

> **Started:** 2026-10-25T03:12:00Z

- [x] Run every generator's `--check` from `scripts/drift-check.py`: `python scripts/build-docs.py --check`, `python research/milestones.py --check`, `python scripts/todo-graph.py plan --check`, and (once they exist) the command-line and API references (`planned_outputs` for `docs/cli.md` and `docs/api.md` in `docs/drift.json`), verifying that declared generator `outputs` exist and remain read-only during `--check`. Done when: a stale output of each makes the check exit 1. Cheaper substitute: regenerating in CI without failing.
- [x] Add generator integration regression tests in `tests/test_drift.py` that independently stale `scripts/build-docs.py`, `research/milestones.py`, and `scripts/todo-graph.py` outputs, assert top-level `scripts/drift-check.py` exits 1 with the corresponding generator diagnostic without modifying files, and verify restoration and `--fix` repair to exit 0. Done when: `python3 -m unittest tests/test_drift.py -v` passes.
- [x] Commit: `"docs: every generated file checked against its source"`

**Test checkpoint:** `python3 -m unittest tests/test_drift.py -v` passes (`Ran 47 tests in 5.519s, OK`); each seeded stale output quoted from `python3 scripts/drift-check.py` with its failure line and exit 1 (`docs/format.md`: `generator: scripts/build-docs.py: build-docs: docs/format.md: generated file differs from source (run python3 scripts/build-docs.py)`, `rc=1`; `todo/05-milestones/TODO-01-beat-jxl-and-webp.md`: `generator: research/milestones.py: milestones: todo/05-milestones/TODO-01-beat-jxl-and-webp.md is stale; run python research/milestones.py --write`, `rc=1`; `todo/implementation-plan.md`: `generator: scripts/todo-graph.py: ::error::todo/implementation-plan.md is stale -- D08 T01 §1: plan says [ ], graph says [x]`, `rc=1`), then restored with `python3 scripts/drift-check.py` exiting 0 (`drift: 192 supported documents; 0 error(s); 0 deterministic repair(s)`).

> **Verified:** 2026-10-25 | §3 | 98d424b95225e798d795d4e31d92e46d2074d2ad
> **Review:** `docs/reviews/d08-t02-s3.json`
## 4. Staleness: Verified Dates and Data Revisions

> **Started:** 2026-10-25T06:28:30Z

**Job:** A maintainer can read a report of every stale page and data file, and why it is stale.

- [x] Register source hashes and dated review evidence in `docs/drift.json` (`reviews`) for current prose (`README.md`, `AGENTS.md`, `GEMINI.md`, `CONTRIBUTING.md`, `docs/building.md`, `docs/library.md`, `docs/index.md`, `docs/getting-started.md`, `docs/benchmarks.md`, `docs/document-drift.md`, `docs/README.md`, `docs/agent-skills-and-rules-structure.md`, `spec/FOLD.md`, `research/README.md`, `testdata/photos/README.md`). Done when: a same-day source change fails the receipt and `--fix` cannot renew it; explicit `--review <doc> --evidence ...` is required and no date-only freshness inference is accepted.
- [x] Have the measurement producers (`tests/benchmark.py` and `research/ab.py`) verify a clean git `HEAD` and `provenance_snapshot` (`src`, `include`, `scripts/build.py` / `research/bench.c`, `tests/test_correctness.py`), rebuild the measured executable from the verified snapshot before measuring, record actual codec revision, `kat_fold_sha256`, and per-set `set_kat_hashes` / `retained_kat_fold_sha256` alongside `benchmarks/results.csv` and `benchmarks/corpus.csv` via atomic `.tmp` publication with read-back verification, and fail in `scripts/drift-check.py` when `tests/test_correctness.py` `KAT_FOLD` differs from the file-level or per-set KAT hash, naming the data files and any stale retained sets to regenerate while leaving them untouched under `--fix`. Done when: regenerating `KAT_FOLD` without rerunning the benchmarks makes `python3 scripts/drift-check.py` exit 1 naming `benchmarks/corpus.csv` and `benchmarks/results.csv` (and any stale sets after a partial rerun).
- [x] Add regression tests in `tests/test_drift.py` and `tests/test_research_refs.py` covering same-day source receipt invalidation, date-only freshness rejection, both CSV producers' pre/post-measurement provenance snapshots, dirty-tree/mid-run modification refusals, atomic `.tmp` write and read-back verification, missing/malformed provenance headers, `KAT_FOLD` mismatch, partial reruns leaving unmeasured sets stale, `--fix` preserving stale receipts and data files, and clean recovery after restoration or full regeneration. Done when: `python3 -m unittest tests/test_drift.py tests/test_research_refs.py -v` passes.
- [x] Commit: `"docs: staleness from verified dates and data revisions"`

**Test checkpoint:** `python3 -m unittest tests/test_drift.py tests/test_research_refs.py -v` passes (`Ran 59 tests in 17.8s, OK`); the two seeded staleness cases quoted from `python3 scripts/drift-check.py` with exit 1 (`scripts/build.py` same-day source change -> `stale: README.md: scripts/build.py changed since review ...`, `stale: CONTRIBUTING.md: scripts/build.py changed since review ...`, `stale: docs/building.md: scripts/build.py changed since review ...`, `stale: docs/getting-started.md: scripts/build.py changed since review ...`, `rc=1`, `after --fix rc=1`; `KAT_FOLD` change in `tests/test_correctness.py` -> `stale: benchmarks/corpus.csv: KAT_FOLD changed in tests/test_correctness.py since benchmarks/corpus.csv was produced at c8c1f78d74a423844c77da10d7d0dcde25f73056 (recorded b3012508fdcadc58e768baaba1c9fc3cf5cc2dd837643888b1ca2314fe79bb32, current b373321199f501d08ebd2d7b87cb5f0f6d7b8a749874ed945dc81eac1a02a369); regenerate benchmarks/corpus.csv with python3 research/ab.py size all --csv benchmarks/corpus.csv` and `stale: benchmarks/results.csv: KAT_FOLD changed in tests/test_correctness.py since benchmarks/results.csv was produced (stale sets: measure, photos, synthetic; ...); regenerate benchmarks/results.csv with python3 tests/benchmark.py`, `rc=1`, `after --fix rc=1`; partial `measure`-only rerun -> `stale: benchmarks/results.csv: KAT_FOLD changed in tests/test_correctness.py since benchmarks/results.csv was produced (stale sets: photos, synthetic; ...)`, `rc=1`); restored with `python3 scripts/drift-check.py` exiting 0 (`drift: 212 supported documents; 0 error(s); 0 deterministic repair(s)`).

> **Verified:** 2026-10-25 | §4 | 9cd7129f592737a1a3f9c92594f63c63bc8e9447
> **Review:** `docs/reviews/d08-t02-s4.json`
## 5. Links, and the Gates Wired Everywhere

> **Started:** 2026-10-25T07:25:30Z

**Job:** A contributor meets the policy at every step: the commit hook, the local gates, and CI refuse drift the same way.

- [x] Check every relative link (including nested bracket labels, balanced-parenthesis destinations, angle-bracket destinations, and reference link definitions) and heading anchor (ATX including up to three leading spaces and linked headings, and Setext `===` and `---` headings excluding YAML front matter, thematic breaks, list items, blockquotes, and tables) in tracked Markdown, and check relative links, `/RasterFold/` base-path links, and built HTML IDs in the allowlisted Jekyll artifact in CI (`build/site-artifact/`), updating `docs/document-drift.md` and its review receipt. Done when: a dead link or missing anchor makes `python3 scripts/drift-check.py` exit 1 while built HTML IDs remain authoritative.
- [x] Run `python scripts/drift-check.py` in `tools/githooks/pre-commit` (on the staged tree), in `scripts/check-all.ps1` / `scripts/check-all.py`, and in `.github/workflows/plan.yml` (both repository-wide and `--site-dir build/site-artifact`), and add regression tests in `tests/test_drift.py` (`test_setext_nested_links_and_gate_entrypoints`) covering source Markdown link forms, ATX/Setext heading anchors, built-site HTML link/fragment failures, and entrypoint refusal and recovery. Done when: each of the three entrypoints refuses a seeded drift and `python3 -m unittest tests/test_drift.py -v` passes.
- [x] Commit: `"docs: refuse drift in the hook, the local gates, and CI"`

**Test checkpoint:** `python3 -m unittest tests/test_drift.py -v` passes (`Ran 51 tests in 23.1s, OK`); a scratch-clone commit with a seeded broken link (`[broken [nested] link](missing-target(1).md#bad)`) is refused by `tools/githooks/pre-commit` (`link: README.md: missing target: missing-target(1).md#bad`, `stale: README.md: README.md changed since review ...`, `pre-commit: staged documentation drift; repair, review and stage the corrected files`, `rc=1`, `HEAD` unchanged) and succeeds (`rc=0`, `HEAD` advances) after restoring a valid link; `pwsh scripts/check-all.ps1 -Gate "document drift"` fails on the seeded broken link (`FAIL document drift (1.6s)`, `link: README.md: missing target: missing-target(1).md#bad`, `check-all: 0/1 passed`, `rc=1`) and passes after restoration (`PASS document drift (1.7s)`, `check-all: 1/1 passed`, `rc=0`; `pwsh scripts/check-all.ps1 -Quick` `check-all: 27/27 passed`); seeded built-artifact failures in `build/site-artifact/` fail `python3 scripts/drift-check.py --site-dir build/site-artifact` with `rc=1` (`link: index.html: missing target: missing-page.html`, `site-link: index.html: URL outside Pages base path: /outside/page.html`, `anchor: index.html: missing heading or id: /RasterFold/format.html#missing-html-id`) and recover to `rc=0`.

> **Verified:** 2026-10-25 | §5 | 3588c3d7b69546b56d7364837a87194c8ebe15d1
> **Review:** `docs/reviews/d08-t02-s5.json`
## 6. Deterministic repair and complete file coverage

> **Started:** 2026-10-25T08:13:00Z

**Job:** A maintainer can render source facts into generated documents and repair stale output while preserving authored and historical evidence.

- [x] Inventory supported Markdown, text, HTML, structured configuration (`.gitattributes`, `.gitignore`, `.json`, `.jsonl`, `.toml`, `.yml`, `.yaml`, `.ini`, `.cfg`), data (`.csv`) and SVG/XML in `docs/drift.json` (including explicit `generated` role for `docs/facts.json` and `configuration` roles for `.gitattributes`, `.gitignore`, `*.ini`, `*.cfg`), and validate manifest contracts (`coverage`, `facts`, `blocks`, `assertions`, `reviews`, `generators`, `encodings`) in `scripts/drift-check.py`. Done when: all tracked and newly added supported files have a visible role and applicable checks, with explicit legacy encodings and deterministic contract validation.
- [x] Implement compare-before-write atomic repair with read-back verification in `scripts/drift-check.py --fix` and `research/milestones.py --write`, running generator repairs in an isolated staging tree (`check.generators(fix=True)`) and publishing only declared `outputs` through `Checker.fix()`. Done when: only registered generated files (`docs/facts.json`), registered generated block interiors (`blocks`), and declared generator `outputs` can change; undeclared generator writes, generator failures, concurrent edits on either direct or generator repair targets, and paths or symlinks outside the checkout are refused while preserving original bytes and file modes and cleaning up temporary files.
- [x] Keep review receipts, measurements and commit history out of automatic repair, and update `docs/document-drift.md` and renew its review receipt (`--review docs/document-drift.md --evidence ...`) after reviewing the updated repair and coverage contract. Done when: a stale receipt or unresolved semantic mismatch remains a failing report after safe repairs, and `python3 scripts/drift-check.py` exits 0 on the clean tree.
- [x] Add `tests/test_drift.py` fixtures (`test_deterministic_repair_and_complete_file_coverage`) exercising wrong facts, missing coverage, malformed manifest contracts, historical and legacy-encoding preservation, symlink/traversal escapes, non-generated repair target refusal, undeclared generator writes and generator failure during `--fix`, concurrent edits on both direct and generator repair paths, injected `os.fsync`/`os.replace` failures preserving original bytes and modes with `.drift-*` cleanup, and idempotent repair with read-back verification. Done when: `python3 -m unittest discover -s tests -p test_drift.py -v` passes.
- [x] Commit: `"docs: deterministic repair and complete file coverage"`

**Test checkpoint:** `python3 -m unittest discover -s tests -p test_drift.py -v` passes (`Ran 52 tests in 25.875s, OK`); wrong-fact failure quoted (`fact: README.md: line 6: version must equal 2; review the surrounding sentence`, `rc=1`, still failing after `--fix` with `drift: 4 supported documents; 1 error(s); 0 deterministic repair(s)`); narrow repair diff quoted on both a generated block (`README.md`: `-Version 1.` / `+Version 2.`, authored `Authored intro.` preserved) and a staged generator output (`todo/implementation-plan.md`: `-| Phase 0 | stale |` / `+| Phase 0 | D08 T02 §6 |`); injected publication failure quoted (`INJ_RC= 2 ERR= drift: simulated replace failure TEMPS= [] MODE= 0o640`, original bytes and `0o640` file modes preserved, `.drift-*` temp files cleaned up, followed by clean recovery on retry); and second no-op repair quoted (`FIX2_RC= False drift: 4 supported documents; 0 error(s); 0 deterministic repair(s)`).

> **Verified:** 2026-10-25 | §6 | 93cb9b2ec428b8d14e6ac31bde5ed2c319bca9d9
> **Review:** `docs/reviews/d08-t02-s6.json`
## 7. Commit and built-site provenance

> **Started:** 2026-10-25T09:16:58Z

**Job:** A maintainer can audit commit history and prove documentation and the published site correspond to their cited revisions and current inputs.

- [x] Validate explicit commit/path references (`<!-- drift:commit <sha> [<path>] -->`, requiring `<sha>^{commit}` to resolve to a commit and any `<path>` to be a normalized relative repository path resolving in that commit's tree) and declared `Drift-Fact:` commit facts (catching malformed `Drift-Fact` trailer lines) in `scripts/drift-check.py`. Done when: an unknown revision, non-commit object, or invalid/missing committed path fails; facts in old commits are evaluated against that commit's own manifest and source tree, never rewritten to match `HEAD`.
- [x] Generate and verify `build/site-artifact/drift-build.json` from allowlisted source hashes (`site_sources` in `docs/drift.json` including `docs/site-content.json`, `docs/Gemfile`, and `src/rf.h`, cross-checked against every allowlisted `config`, `pages`, and `assets` input in `docs/site-content.json` and `.agents`/`gemini.md` in `forbidden_site_paths`). Done when: a changed source or allowlist mapping (`docs/site-content.json`), missing artifact/page/asset, symlink, or broken HTML fragment is refused by `python3 scripts/drift-check.py --site-dir build/site-artifact` (live Pages deployment enforcement and deployed receipt read-back are owned by `D08 T01 §3`).
- [x] Wire the staged document gate and commit-message declaration checks into `tools/githooks/pre-commit` and `tools/githooks/commit-msg` (including `--git-dir` forwarding and refusal if a tracked gate script in `HEAD` is removed from the index), and update `docs/document-drift.md` and renew its review receipt. Done when: unstaged working-tree fixes cannot hide a bad staged document in `pre-commit` or a false staged `Drift-Fact:` declaration in `commit-msg`, `HEAD` remains unchanged on refusal, and staging the correction allows the commit to succeed.
- [x] Pin exact-candidate CI checkout (`ref: ${{ github.event.pull_request.head.sha || github.sha }}` with `HEAD == $HEAD_SHA` verification) and `--commit-range` fallback (`$HEAD_SHA^..$HEAD_SHA` when `BASE_SHA` is absent or all-zeros on `workflow_dispatch` or initial branch push) in `.github/workflows/plan.yml`, and add regression tests in `tests/test_drift.py`. Done when: every supported information-file change runs drift checks against the exact candidate commit and its commit-fact range, and `python3 -m unittest discover -s tests -p test_drift.py -v` passes.
- [x] Commit: `"docs: commit and built-site provenance"`

**Test checkpoint:** `python3 -m unittest discover -s tests -p test_drift.py -v` passes (`Ran 53 tests in 28.978s, OK`); seeded failures quoted for (1) a bad staged document concealed by a clean unstaged working copy in `pre-commit` (`rc=1`, `pre-commit: staged documentation drift; repair, review and stage the corrected files`, `HEAD` unchanged, followed by recovery after staging), (2) a false staged `Drift-Fact:` declaration concealed by an unstaged source/manifest fix in `commit-msg` / `git commit` (`rc=1`, `commit-fact: Drift-Fact: version=2: differs from that commit's facts; correct the new message or add a corrective commit, never rewrite history`, `HEAD` unchanged, followed by recovery after staging `Drift-Fact: version=3`), (3) bad `<!-- drift:commit ... -->` revision and path references (`commit: history.md: line 1: unresolved commit or committed path deadbeefdeadbeef^{commit}` and `commit: history.md: line 1: invalid committed path ./src/version.h`, `rc=1`), and (4) a stale/broken built site in `build/site-artifact` after mutating `docs/site-content.json` (`site: build/site-artifact: site source receipt differs from current inputs; rebuild before publishing`, `rc=1`), omitting `docs/site-content.json` from `site_sources` (`site: build/site-artifact: allowlisted site source not covered by site_sources: docs/site-content.json`, `rc=1`), removing an allowlisted built page (`site: build/site-artifact: missing allowlisted built output: usage.html`, `rc=1`), or seeding a broken HTML fragment and private paths (`anchor: index.html: missing heading or id: /RasterFold/usage.html#cli`, `site-private: build/site-artifact: private path in artifact: .agents`, `rc=1`); and historical `Drift-Fact: version=2` commit verified against its own tree after `HEAD` advanced to `version=3` (`HIST_RANGE: False False`, `rc=0`).

**Groomed 2026-09-30:** Mechanical coverage is explicit, not a claim that arbitrary prose can be understood perfectly. Future measurement producers remain prerequisites of complete live measurement coverage. Repair never fabricates a benchmark, a review stamp or a deployment.

> **Verified:** 2026-10-25 | §7 | 9155346a2220a432d9074f5b92e3e4227751263b
> **Review:** `docs/reviews/d08-t02-s7.json`
## Verification

- [x] `python scripts/drift-check.py` exits 0 (`drift: 219 supported documents; 0 error(s); 0 deterministic repair(s)`)
- [x] `python scripts/todo-graph.py validate` clean (`32 todos, 227 sections -- 0 fatal, 0 warning(s)`)
