---
schema_version: 1
id: html-docs
domain: 08-docs
status: draft
title: "TODO-01 -- HTML Documentation with Comparison Charts"
depends_on: []
track: D1
---

# TODO-01 -- HTML Documentation with Comparison Charts

> **Goal:** RasterFold has an HTML documentation site built by Jekyll on GitHub Pages from explicitly allowlisted sources staged in `build/site-source/`: an overview, getting started, the command-line and API references, the format specification, the research notes, and a benchmarks section with charts comparing RasterFold with JPEG XL e7, JPEG XL e9, lossless WebP, and PNG on size, encode and decode speed, and memory. Jekyll renders the pages, theme, and navigation; a stdlib Python script (`scripts/build-docs.py`) generates what Jekyll cannot check (the SVG charts and data tables from the committed CSVs, and the pages mirrored from sources outside `docs/`), and its `--check` fails when any generated file differs from its source.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** `docs/_config.yml`, `docs/Gemfile`, `docs/site-content.json`, `scripts/build-docs.py`, the initial site pages (`docs/index.md`, `docs/getting-started.md`, `docs/benchmarks.md`, `docs/research-summary.md`), and the mirrored pages (`docs/format.md`, `docs/usage.md`) are implemented by `D08 T01 §1`, with `actions/jekyll-build-pages` verifying `build/site-source/` in `.github/workflows/plan.yml`. Measurements live in `benchmarks/results.md`, `benchmarks/results.csv`, and `benchmarks/corpus.csv`. SVG comparison charts (§2) and the dedicated `.github/workflows/pages.yml` deployment workflow (§3) remain planned. Operator decision 2026-09-29: "let's go with Jekyll" (the hybrid: Jekyll renders, a Python script generates the checked charts and mirrored pages). GitHub Pages' source is set to GitHub Actions (`D99 T01 §5`).
<!-- claim: exists docs/_config.yml -->
<!-- claim: exists scripts/build-docs.py -->
<!-- claim: exists benchmarks/results.csv -->

## Inputs

- [`README.md`](../../README.md), [`spec/FOLD.md`](../../spec/FOLD.md), [`docs/research.md`](../../docs/research.md) -- the page sources
- [`benchmarks/results.csv`](../../benchmarks/results.csv) -- the data the charts draw
- [Jekyll docs](https://jekyllrb.com/docs/) and [GitHub Pages with a custom workflow](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages) -- the renderer and the deployment §1 and §3 use
- -> XREF: D00 T02 §3 -- `benchmarks/corpus.csv`, the per-image data the corpus charts draw
- -> XREF: D08 T02 §3 -- the `--check` gate that keeps the generated files identical to their sources
- -> XREF: D99 T01 §5 -- the operator sets the Pages source to GitHub Actions for §3
- -> XREF: D09 T01 §4 -- the 1.0 release, which links the published site

- -> XREF: D10 T04 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T03 §8 -- shared implementation or verification boundary.

- -> XREF: D08 T03 §2 -- shared implementation or verification boundary.

## Outcome

- `docs/` holds public sources and internal records. `scripts/build-docs.py --stage build/site-source` selects only the explicit product allowlist, and CI builds that staging directory with Jekyll; raw `docs/` is never the publication source.
- `python scripts/build-docs.py` writes the generated parts (the pages mirrored from `spec/FOLD.md` and `README.md` with front matter, and the SVG charts and data tables under `docs/_includes/generated/`) with no package beyond the Python standard library; `--check` exits 1 when a committed generated file differs from what its sources produce.
- The benchmarks page shows, per set (reference image, photographs, generated images, held-out once it exists): size ratios, encode and decode times at one and default threads, and peak memory against JPEG XL e7, JPEG XL e9, lossless WebP, and PNG; plus a per-image size scatter and a size-against-decode-time chart.
- The `pages` workflow checks the generated files, builds, and deploys on every main push and explicit release dispatch, with exact-source dispatch available for release readback.

**Adjacency:** list=applicable @ D08 T01 §1; document=applicable @ D08 T01 §1; settings=not-applicable (the site's configuration is a committed file, not a user setting); reporting=applicable @ D08 T01 §2; notifications=not-applicable (a docs build reports on the console); permissions=not-applicable (a static site); audit=not-applicable (git history is the audit); exchange=applicable @ D08 T01 §3; reverse=not-applicable (a redeployed site replaces the last)

**Adjacency rationale:** The site lists its pages in one navigation; the pages are the documents; the charts are the reports; the workflow exports the site to GitHub Pages.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | The Jekyll site and its generated pages | D08 T02 §1 |  [x]   |
|   2   |   §2    | Comparison charts | §1, D00 T02 §3 |  [x]   |
| 3 | §3 | Publish with GitHub Pages | §2, §4, §5, D08 T02 §5, D08 T02 §7, D99 T01 §5 | [ ] |
| 4 | §4 | Complete current documentation map | §1, D08 T02 §7 | [ ] |
| 5 | §5 | Accessible navigation and site states | §4 | [ ] |
| 6 | §6 | Converter user guide and capability reference | §4, D10 T03 §8, D10 T04 §1 | [ ] |
| 7 | §7 | Versioned downloads and complete references | D09 T01 §1, D09 T01 §2, D08 T03 §1, D08 T03 §2 | [ ] |
| 8 | §8 | Published whole-product documentation acceptance | §3, §6, §7, D10 T04 §1 | [ ] |

---

## 1. The Jekyll Site and Its Generated Pages

> **Started:** 2026-10-25T02:23:14Z

**Fidelity:** Documentation site -- the committed product documentation design.

**Design:** docs/design/documentation.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Readable technical content, original RasterFold branding and current-version labels.

**Chrome:** Jekyll navigation, local search, page outline and edit-source link.

**Job:** A reader can browse a list of documentation pages in one navigation and read each document rendered as HTML.

Jekyll renders what it is good at (Markdown, layouts, the theme, navigation, search). Pages whose source lives outside `docs/` are mirrored into it by `scripts/build-docs.py`, never copied by hand, so they cannot drift from their sources.

- [x] Record an explicit page/asset allowlist in `docs/site-content.json`. Done when: a test refuses agent instructions, TODOs, review records, intelligence and private/local paths in the built site; excluding two current directories alone is insufficient.
- [x] Add `docs/_config.yml` (title, `remote_theme: just-the-docs/just-the-docs` with the `jekyll-remote-theme` plugin, search on, `exclude:` for `reviews/` and `phase-runs/` so review records are never published) and `docs/Gemfile` for local previews. Done when: `actions/jekyll-build-pages` builds `build/site-source/` in CI without an error; configuration exclusions are defense in depth, never the publication allowlist. Cheaper substitute: building from the repository root, which publishes `todo/`, `AGENTS.md`, and the skills.
- [x] Write the pages that live in `docs/`: `index.md` (overview), `getting-started.md`, `benchmarks.md`, with front matter placing them in the navigation, and a public research summary (`docs/research-summary.md`) sourced from selected approved research sections. Done when: the built site lists them in its navigation.
- [x] Write `scripts/build-docs.py`, which mirrors `spec/FOLD.md` to `docs/format.md`, the command and limits parts of `README.md` to `docs/usage.md`, and selected public sections of `docs/research.md` to `docs/research-summary.md`, each with front matter and a generated-file banner, plus `--check` (which regenerates in memory and compares); remove `"planned_owner": "D08 T01 §1"` from `docs/drift.json` so `scripts/build-docs.py --check` is a mandatory drift generator. Done when: editing `spec/FOLD.md` without rerunning makes `python scripts/build-docs.py --check` exit 1, and removing `scripts/build-docs.py` makes `scripts/drift-check.py` fail.
- [x] Implement `scripts/build-docs.py --stage build/site-source` with strict destination confinement (allowing only non-symlink subdirectories under `build/`, refusing `.`, `docs`, `..` traversal, or symlink ancestors/targets without modifying existing files, and removing obsolete staged files on repeated runs) and add regression fixtures in `tests/test_drift.py`. Done when: the clean staging directory contains exactly the allowed generated pages/assets from `docs/site-content.json`, rebases links, rejects undeclared/private inputs and unsafe destinations without modifying existing files, and removes obsolete staged files on re-staging.
- [x] Wire a build-only Jekyll verification step in `.github/workflows/plan.yml` (`python3 scripts/build-docs.py --check`, `python3 scripts/build-docs.py --stage build/site-source`, `actions/jekyll-build-pages@44a6e6beabd48582f863aeeb6cb2151cc1716697` into `build/site-artifact`, and rendered HTML navigation verification). Done when: the `plan.yml` CI run builds `build/site-source/` cleanly and verifies the rendered navigation.
- [x] Commit: `"docs: a Jekyll site with pages mirrored from their sources"`

**Test checkpoint:** `python3 scripts/build-docs.py --check` exits 0 on the tree and 1 after a source edit (quoted, then regenerated); `python3 -m unittest tests/test_drift.py -v` passes; the CI build log of `build/site-source/` from `.github/workflows/plan.yml` is quoted with no warning.

> **Verified:** 2026-10-25 | §1 | 03a81db61afa9bcc7ada2c2549e17d93d9b7f127
> **Review:** `docs/reviews/d08-t01-s1.json`
## 2. Comparison Charts

> **Started:** 2026-10-25T03:51:30Z

**Job:** A reader can see size, speed, and memory against JPEG XL e7, JPEG XL e9, lossless WebP, and PNG as charts that report the committed measurements.

Charts are generated as static SVG by `scripts/build-docs.py`, not drawn by JavaScript in the browser, so their numbers can be checked in CI against the data.

- [x] Record verified `# codec_revision=... kat_fold_sha256=... date=... measured_sets=... retained_sets=... retained_revision=... retained_date=...` header metadata in `benchmarks/results.csv` and `tests/benchmark.py` (`read_results_csv` / `load_existing_rows` / `write_csv_rows`), preserving measured-set (`measure`: `225ba888a41e09a89d65f82829098fd8c7d41c3c`, `2026-10-25`) versus retained-set (`photos`, `synthetic`: `0159404532cf944c4265ecb8ea46c54f637bdfc2`, `2026-09-29`) provenance without misattributing retained sets to a newer date or revision. Done when: `benchmarks/results.csv` and `benchmarks/corpus.csv` both carry machine-readable header provenance and `tests/test_research_refs.py` passes.
- [x] Generate static SVG charts into `docs/_includes/generated/charts/` from `benchmarks/results.csv` and `benchmarks/corpus.csv`: `corpus-size-ratio.svg` (total size ratio vs `jxl_e7` across `photo` 39, `tp` 10 overlapping, `tp-only` 5 exclusive, `synth` 11, and `all` 55), `benchmark-size-ratio.svg` (geometric-mean size ratio vs `jxl` across `measure`, `photos`, `synthetic` with explicit `N/A` markers where `jxl-e9` is absent in retained sets), `encode-time.svg` and `decode-time.svg` (geometric-mean time ratio vs `jxl` per set at default and 1 thread), `peak-memory.svg` (maximum encode and decode peak working set per set), `scatter-e9.svg` (55-image corpus raw bytes vs `rasterfold / jxl_e9` size ratio), and `size-vs-decode.svg` (per-set geometric-mean size ratio vs decode-time ratio). Every chart carries its source file, codec revision(s), date(s), corpus/set membership, thread counts, and limitations. Done when: `docs/benchmarks.md` includes every chart with `{% include generated/charts/<name>.svg %}` and each chart's numbers equal the CSV's. Cheaper substitute: a JavaScript chart library drawing at view time, which no gate can check.
- [x] Generate a matching Markdown data table under every chart into `docs/_includes/generated/tables/` (`corpus-size-ratio.md`, `benchmark-size-ratio.md`, `encode-time.md`, `decode-time.md`, `peak-memory.md`, `scatter-e9.md`, `size-vs-decode.md`), allowlist all generated chart and table includes in `docs/site-content.json` and `docs/drift.json`, and verify in `.github/workflows/plan.yml` that the built HTML (`benchmarks.html`) renders every SVG chart and table. Done when: each chart has its table on the page and `scripts/build-docs.py --stage build/site-source` stages every generated include.
- [x] Add `--self-test` to `scripts/build-docs.py` and regression tests in `tests/test_drift.py`, which independently recompute all aggregates and per-image values from the CSVs, parse each generated SVG (`bar`, `na`, and `point` elements plus SVG geometry) and Markdown table, and assert complete series membership, provenance metadata, and numerical agreement. Done when: `python3 scripts/build-docs.py --self-test` and `python3 -m unittest tests/test_drift.py -v` pass, and hand-editing a bar, scatter point, deleted series, or table cell makes `--self-test` fail.
- [x] Commit: `"docs: comparison charts generated from the committed measurements"`

**Test checkpoint:** `python3 scripts/build-docs.py --self-test` prints `build-docs: self-test: 7 chart(s) and 7 table(s) match CSV data` (exit 0) and `--check` prints `build-docs: current` (exit 0); `python3 -m unittest tests/test_drift.py tests/test_research_refs.py -v` passes (`Ran 56 tests in 10.067s OK`); seeded mutations fail `python3 scripts/build-docs.py --self-test` with exit 1 on a hand-edited bar (`build-docs: self-test: docs/_includes/generated/charts/corpus-size-ratio.svg: ('photo', 'rasterfold') data-value='0.1234' != '0.9712'`), scatter point (`build-docs: self-test: docs/_includes/generated/charts/scatter-e9.svg: ('photo', 'measure') point data-x/data-y=('4720056', '0.5000') != ('4720056', '0.9781')`), deleted series (`build-docs: self-test: docs/_includes/generated/charts/benchmark-size-ratio.svg: missing series element for ('photos', 'jxl-e9')`), and table cell (`build-docs: self-test: docs/_includes/generated/tables/corpus-size-ratio.md: table rows do not match CSV calculation`), then regenerate cleanly to `build-docs: self-test: 7 chart(s) and 7 table(s) match CSV data` (exit 0).

> **Verified:** 2026-10-25 | §2 | aada421fbfbf53389764a342b19aba159d6af0be
> **Review:** `docs/reviews/d08-t01-s2.json`
## 3. Publish with GitHub Pages

**Job:** A reader can open the site the `pages` workflow exports to GitHub Pages.

The workflow is GitHub's Jekyll starter adapted to this repository: it builds the allowlisted staging directory, checks generated output first, pins every action to a full commit SHA and runs on every main push and explicit release dispatch so all generators see their current inputs.

- [ ] Add `.github/workflows/pages.yml`: on every push to `main` and `workflow_dispatch` with an explicit immutable source ref; permissions `contents: read`, `pages: write`, `id-token: write`; concurrency group `pages` without cancelling; a `build` job (checkout the requested commit/tag, build the native CLI and its references using pinned tools, `python scripts/build-docs.py --check`, `python scripts/build-docs.py --stage build/site-source`, `actions/configure-pages`, `actions/jekyll-build-pages` with `source: build/site-source` into `./_site`, copy container output to a runner-owned artifact directory before receipt writes, `python3 scripts/drift-check.py --site-dir build/site-artifact --write-site-receipt`, a second read-only `--site-dir build/site-artifact` check, `actions/upload-pages-artifact` with `path: build/site-artifact`) and a `deploy` job (`actions/deploy-pages`, environment `github-pages`); every action pinned to a full SHA with its version in a comment. Done when: the workflow run is green and the site loads at https://rizonesoft.github.io/RasterFold/. Cheaper substitute: the unmodified starter workflow, which builds the whole repository and skips the check.
- [ ] Commit: `"docs: publish the site with GitHub Pages"`

**Test checkpoint:** the `pages` run's URL from `python3 scripts/ci_status.py --workflow pages.yml`, a `workflow_dispatch` run on `main` supplying a distinct immutable source commit ref (`D99 T01 §5`), and a fetch of the site's index page returning 200 and its published source receipt matching the deployed commit.

**Groomed 2026-09-30:** The initial publication documents current capabilities. The converter manual and final complete-product site are later sections in Phase 9; the existing Phase 0 publication is not made dependent on future converter implementation.

## 4. Complete current documentation map

> **Started:** 2026-10-25T09:47:37Z

**Job:** A reader can find accurate documentation for every currently shipped command, API and target.

**Fidelity:** Documentation site -- the committed documentation design.

**Design:** docs/design/documentation.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Readable, accessible technical references with current-version labels.

**Chrome:** Shared Jekyll navigation, search, page outline and edit-source link.

- [x] Add the complete current page inventory in `docs/site-content.json` and enforce `REQUIRED_PAGE_IDS` in `scripts/build-docs.py`. Done when: overview (`docs/index.md`), getting-started (`docs/getting-started.md`), four-target installation/build (`docs/building.md` -> `docs/installation.md`), current CLI (`README.md` -> `docs/usage.md`), current C API (`docs/library.md` -> `docs/c-api.md`), format (`spec/FOLD.md` -> `docs/format.md`), examples (`docs/examples.md`), measurements (`docs/benchmarks.md` and `docs/research.md` -> `docs/research-summary.md`), limitations (`docs/limitations.md`), troubleshooting (`docs/troubleshooting.md`), contribution (`CONTRIBUTING.md` -> `docs/contributing.md`), security (`SECURITY.md` -> `docs/security.md`), and licenses (`docs/licenses.md`) each name one canonical source and shrinking the required inventory fails `scripts/build-docs.py --check`.
- [x] Mirror `docs/building.md`, `docs/library.md`, `spec/FOLD.md`, `CONTRIBUTING.md`, and `SECURITY.md` into distinct `output` files through `scripts/build-docs.py`, author `docs/examples.md`, `docs/limitations.md`, `docs/troubleshooting.md`, and `docs/licenses.md`, and register all new generated outputs and review receipts in `docs/drift.json`. Done when: generated pages retain working links under `/RasterFold/`, canonical sources are never overwritten in place, examples document `examples/encode.c`, `examples/decode.c`, and `examples/ppm.h`, troubleshooting documents exit codes `0..5` and refusal/recovery paths from `src/main.c`, `src/fold.c`, `src/bmp.c`, `src/api.c`, and `src/sys_posix.c`, and current facts come from the source contracts.
- [x] Add current/planned/historical labels to `docs/index.md`, `docs/benchmarks.md`, and `docs/README.md`. Done when: current (`v2` format, `1.0` C API, 4-target CLI and libraries), historical (`2026-09-29` retained sets and `docs/measurements.md`), and planned (`D10` native converter and `D08 T03` generated reference pages) scope are explicitly labeled and the native converter is not advertised as downloadable until its package exists.
- [x] Add `tests/test_docs.py` page-coverage fixtures and wire them into `scripts/check-all.py` and `.github/workflows/plan.yml`. Done when: removing a required page, shrinking the topic inventory, mutating a mirrored source without regenerating, or exporting a private source fails `tests/test_docs.py` and the build.
- [x] Commit: `"docs: complete current documentation map"`

**Test checkpoint:** Run the page-coverage fixtures added here, build Jekyll and run `python3 scripts/drift-check.py --site-dir build/site-artifact`; quote missing-page and leaked-private-file failures.
- Correctness tests: `python3 -m unittest discover -s tests -p 'test_docs.py' -v` passed `Ran 5 tests ... OK`; `python3 scripts/build-docs.py --self-test` passed `build-docs: self-test passed (7 checks)`.
- Driven run with evidence: `python3 scripts/build-docs.py --stage build/site-source` staged `32 file(s) into build/site-source`; `docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/github/workspace" -w /github/workspace -e GITHUB_WORKSPACE="/github/workspace" -e PAGES_REPO_NWO="rizonesoft/RasterFold" -e INPUT_SOURCE="build/site-source" -e INPUT_DESTINATION="build/site-artifact" ghcr.io/actions/jekyll-build-pages:v1.0.13` built `build/site-artifact` cleanly (`Remote Theme: Using theme just-the-docs/just-the-docs`, `done in 5.912 seconds`); `python3 scripts/drift-check.py --site-dir build/site-artifact --write-site-receipt && python3 scripts/drift-check.py --site-dir build/site-artifact` printed `drift: 228 supported documents; 0 error(s); 0 deterministic repair(s)`. Removing `build/site-artifact/troubleshooting.html` failed `python3 scripts/drift-check.py --site-dir build/site-artifact` with `site: build/site-artifact: missing allowlisted built output: troubleshooting.html`, `site: build/site-artifact: invalid source receipt drift-build.json: missing allowlisted built output in build/site-artifact: troubleshooting.html`, and 15 `link: ...: missing target: /RasterFold/troubleshooting.html` errors (`drift: 228 supported documents; 17 error(s)`). Injecting `todo/08-docs/TODO-01-html-docs.md` into `build/site-artifact/todo/08-docs/TODO-01-html-docs.md` failed `python3 scripts/drift-check.py --site-dir build/site-artifact` with `site-private: build/site-artifact: private path in artifact: todo/08-docs/TODO-01-html-docs.md` (`drift: 228 supported documents; 4 error(s)`).

## 5. Accessible navigation and site states

**Requires:** linux-display -- The checkpoint requires observed browser/accessibility or native-guide evidence, not only a source build; source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A reader can search and navigate the site on desktop and mobile using a keyboard or assistive technology.

**Fidelity:** Documentation site -- the committed documentation design.

**Design:** docs/design/documentation.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Readable, accessible technical references with current-version labels.

**Chrome:** Shared Jekyll navigation, search, page outline and edit-source link.

- [ ] Implement navigation and local search in `docs/_config.yml` and `docs/_includes/`. Done when: every allowed page is reachable and search indexes only published content.
- [ ] Style documented states in `docs/assets/`. Done when: four themes, both densities, narrow/mobile layout, visible focus, empty search and missing-page states match the design.
- [ ] Add project-base-path and anchor checks in `tests/test_docs.py`. Done when: all local links, images, search assets and fragments work under `/RasterFold/`.
- [ ] Record browser and accessibility evidence in `docs/site-validation.md`. Done when: keyboard-only navigation and a screen-reader pass cover overview, references, tables and search.
- [ ] Commit: `"docs: accessible navigation and site states"`

**Test checkpoint:** Build the site and drive its documented states at desktop/mobile widths; quote internal-link and accessibility evidence. A source-only review is insufficient.

## 6. Converter user guide and capability reference

**Requires:** display-session, linux-display, android-arm64-runtime, android-x64-runtime -- The checkpoint requires observed browser/accessibility or native-guide evidence, not only a source build; source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can install, use, compare and troubleshoot the native converter from complete documentation.

**Fidelity:** Documentation site -- the committed documentation design.

**Design:** docs/design/documentation.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Readable, accessible technical references with current-version labels.

**Chrome:** Shared Jekyll navigation, search, page outline and edit-source link.

- [ ] Generate `docs/converter-formats.md` from `converter/codecs.json`. Done when: read/write, depth, alpha, animation, metadata and exactness classes match actual enabled adapters.
- [ ] Write `docs/converter.md`. Done when: single conversion, previews, side-by-side/wipe/difference, saving, cancel/retry and Try sample each have tested instructions.
- [ ] Write target-specific help in `docs/converter-troubleshooting.md`. Done when: permissions, display drivers, Android providers, unsupported modes, memory limits and larger output have concrete recovery steps.
- [ ] Capture UI examples from the candidate in `docs/assets/converter/`. Done when: every screenshot names its source revision, target and state and contains no private source paths.
- [ ] Register the generated capability page in `docs/drift.json`. Done when: stale registry values fail its generator check; authored definitions live separately in `docs/converter-exactness.md` and are never overwritten.
- [ ] Commit: `"docs: converter user guide and capability reference"`

**Test checkpoint:** Follow every guide on a clean target and check that the generated capability table exactly equals the registry; record native results for all four targets.

## 7. Versioned downloads and complete references

**Job:** A user can identify the current release, choose the right artifact and read matching references.

**Fidelity:** Documentation site -- the committed documentation design.

**Design:** docs/design/documentation.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Readable, accessible technical references with current-version labels.

**Chrome:** Shared Jekyll navigation, search, page outline and edit-source link.

- [ ] Generate `docs/downloads.md` from verified release metadata. Done when: each target links executable/GUI and library packages, symbols, checksums and license/source information.
- [ ] Generate release/API/format labels in `scripts/build-docs.py`. Done when: independent compatibility versions cannot be confused with the product SemVer.
- [ ] Wire current CLI/API, converter and release-history pages in `docs/site-content.json`. Done when: all public entry points show matching release or development labels; later main builds keep downloads linked to the actual released assets rather than nonexistent development packages.
- [ ] Register generated downloads/version pages in `docs/drift.json`. Done when: release metadata changes invalidate the generated pages before publication.
- [ ] Commit: `"docs: versioned downloads and complete references"`

**Test checkpoint:** Seed a version mismatch and a missing target asset and show the docs gate failing; then build and inspect the matching reference pages.

## 8. Published whole-product documentation acceptance

**Requires:** linux-display -- The checkpoint requires observed browser/accessibility or native-guide evidence, not only a source build; source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user reaches the complete documentation for the exact product version they downloaded.

- [ ] Add publication readback to `tests/test_docs.py`. Done when: the public project URL, source receipt, navigation, downloads and sampled references agree with the deployment candidate.
- [ ] Exercise the full documentation inventory in `docs/site-validation.md`. Done when: installation, use, format, API, converter, benchmark limitations, accessibility, troubleshooting and licensing all have observed page evidence.
- [ ] Verify the artifact allowlist and asset origins in `scripts/build-docs.py`. Done when: the published tree contains no runner, TODO, review, intelligence, private data or third-party tracking asset.
- [ ] Commit: `"docs: published whole-product documentation acceptance"`

**Test checkpoint:** Quote the successful Pages run plus deployed receipt/hash and page checks. HTTP 200 on the home page alone does not prove completeness.

## Verification

- [ ] `python scripts/build-docs.py --check` and `--self-test` exit 0
- [ ] `python scripts/todo-graph.py validate` clean
