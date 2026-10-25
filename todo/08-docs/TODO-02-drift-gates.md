---
schema_version: 1
id: drift-gates
domain: 08-docs
status: draft
title: "TODO-02 -- Drift and Staleness Gates for Docs, READMEs, and Supporting Files"
depends_on: []
track: D2
---

# TODO-02 -- Drift and Staleness Gates for Docs, READMEs, and Supporting Files

> **Goal:** No document in the repository can quietly disagree with the code or the data. Names, versions, limits, exit codes, and messages come from one facts file; every measured figure in a README or doc is bound to the committed data it came from; generated files are checked against their sources; every page states when it was verified against the files it describes and goes stale when they change; and every link resolves. The commit hook, `scripts/check-all.ps1`, and CI all refuse drift.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** `scripts/drift-check.py` and `docs/drift.json` are implemented by the attended 2026-09-30 maintenance task and extended in §1-§3, together with staged hooks, local gates and CI. They cover all supported tracked/new information files, explicit source facts, data bindings (`csv`, `csv-meta`, and `csv-aggregate`), generated blocks, source review hashes, references, built-site receipts and deterministic repair. Historical measurements retain their dates and limitations; absent future data and a not-yet-built Pages site cannot be called current proof. Formal section stamps remain separate from this implementation.
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
|   4   |   §4    | Staleness: verified dates and data revisions | §2 |  [ ]   |
|   5   |   §5    | Links, and the gates wired everywhere | §3, §4 |  [ ]   |
| 6 | §6 | Deterministic repair and complete file coverage | §1, §3 | [ ] |
| 7 | §7 | Commit and built-site provenance | §5, §6 | [ ] |

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

**Job:** A maintainer can read a report of every stale page and data file, and why it is stale.

- [ ] Register source hashes and review evidence in `docs/drift.json` for current prose. Done when: a same-day source change fails the receipt and `--fix` cannot renew it; explicit review is required.
- [ ] Have the measurement producers record actual codec revision and `KAT_FOLD` hash alongside new `benchmarks/results.csv` and `benchmarks/corpus.csv` data, and fail when the bitstream changed after the data was produced (the known-answer hash differs), naming the data files to regenerate. Done when: regenerating `KAT_FOLD` without rerunning the benchmarks makes the check exit 1.
- [ ] Commit: `"docs: staleness from verified dates and data revisions"`

**Test checkpoint:** the two seeded staleness cases quoted; the report names each stale page and changed source; no date-only freshness inference is accepted.

## 5. Links, and the Gates Wired Everywhere

**Job:** A contributor meets the policy at every step: the commit hook, the local gates, and CI refuse drift the same way.

- [ ] Check every relative link and heading anchor in tracked Markdown, and in the allowlisted Jekyll artifact in CI (`build/site-artifact/`). Done when: a dead link makes the check exit 1; extend source-anchor fixtures to cover Setext headings before such headings are admitted, while built HTML IDs remain authoritative.
- [ ] Run `python scripts/drift-check.py` in `tools/githooks/pre-commit` (on the staged tree), in `scripts/check-all.ps1`, and in `.github/workflows/plan.yml`. Done when: each of the three refuses a seeded drift.
- [ ] Commit: `"docs: refuse drift in the hook, the local gates, and CI"`

**Test checkpoint:** a scratch-clone commit with a seeded drift refused by the hook (quoted); `pwsh scripts/check-all.ps1` shows the `drift` gate `PASS` on the tree.

## 6. Deterministic repair and complete file coverage

**Job:** A maintainer can render source facts into generated documents and repair stale output while preserving authored and historical evidence.

- [ ] Inventory supported Markdown, text, HTML, structured configuration, data and SVG/XML in `docs/drift.json`. Done when: all tracked and newly added supported files have a visible role and applicable checks, with explicit legacy encodings.
- [ ] Implement compare-before-write atomic repair in `scripts/drift-check.py --fix`. Done when: only registered generated files/blocks and generator outputs change; concurrent edits and paths outside the checkout are refused.
- [ ] Keep review receipts, measurements and commit history out of automatic repair. Done when: a stale receipt or unresolved semantic mismatch remains a failing report after safe repairs.
- [ ] Add `tests/test_drift.py` fixtures. Done when: wrong facts, missing coverage, malformed contracts, historical preservation, symlink escapes and idempotent repair are exercised.
- [ ] Commit: `"docs: deterministic repair and complete file coverage"`

**Test checkpoint:** Run `python3 -m unittest discover -s tests -p test_drift.py -v`; quote wrong-fact failure, narrow repair diff and a second no-op repair.

## 7. Commit and built-site provenance

**Job:** A maintainer can audit commit history and prove documentation and the published site correspond to their cited revisions and current inputs.

- [ ] Validate explicit commit/path references and declared commit facts in `scripts/drift-check.py`. Done when: an unknown revision/path fails; facts in old commits are evaluated against that commit, never rewritten to match HEAD.
- [ ] Generate and verify `build/site-artifact/drift-build.json` from allowlisted source hashes. Done when: a changed source, missing artifact or broken HTML fragment blocks publishing.
- [ ] Wire the staged document gate and commit-message declaration checks into `tools/githooks/`. Done when: unstaged fixes cannot hide a bad staged document or a false staged commit fact.
- [ ] Add exact-candidate CI checks to `.github/workflows/plan.yml`. Done when: every supported information-file change runs drift checks, including READMEs and Pages sources.
- [ ] Commit: `"docs: commit and built-site provenance"`

**Test checkpoint:** Seed a bad staged document, bad commit reference and stale/broken built site in fixtures; each fails. Verify a correct historical commit fact against its own tree.

**Groomed 2026-09-30:** Mechanical coverage is explicit, not a claim that arbitrary prose can be understood perfectly. Future measurement producers remain prerequisites of complete live measurement coverage. Repair never fabricates a benchmark, a review stamp or a deployment.

## Verification

- [ ] `python scripts/drift-check.py` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
