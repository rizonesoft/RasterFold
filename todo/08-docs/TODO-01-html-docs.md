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

> **Goal:** RasterFold has an HTML documentation site built by Jekyll on GitHub Pages from `docs/`: an overview, getting started, the command-line and API references, the format specification, the research notes, and a benchmarks section with charts comparing RasterFold with JPEG XL e7, JPEG XL e9, lossless WebP, and PNG on size, encode and decode speed, and memory. Jekyll renders the pages, theme, and navigation; a stdlib Python script (`scripts/build-docs.py`) generates what Jekyll cannot check (the SVG charts and data tables from the committed CSVs, and the pages mirrored from sources outside `docs/`), and its `--check` fails when any generated file differs from its source.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** Documentation is Markdown only: `README.md`, `spec/FOLD.md`, `docs/research.md`, `research/README.md`, `todo/README.md`. Measurements live in `benchmarks/results.md` and `benchmarks/results.csv` (from `tests/benchmark.py`). There is no Jekyll configuration, no chart, and no Pages workflow. Operator decision 2026-09-29: "let's go with Jekyll" (the hybrid: Jekyll renders, a Python script generates the checked charts and mirrored pages). GitHub Pages' source is to be set to GitHub Actions (`D99 T01 §5`), without the starter workflow GitHub suggests.
<!-- claim: absent docs/_config.yml -->
<!-- claim: absent scripts/build-docs.py -->
<!-- claim: exists benchmarks/results.csv -->

## Inputs

- [`README.md`](../../README.md), [`spec/FOLD.md`](../../spec/FOLD.md), [`docs/research.md`](../../docs/research.md) -- the page sources
- [`benchmarks/results.csv`](../../benchmarks/results.csv) -- the data the charts draw
- [Jekyll docs](https://jekyllrb.com/docs/) and [GitHub Pages with a custom workflow](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages) -- the renderer and the deployment §1 and §3 use
- -> XREF: D00 T02 §3 -- `benchmarks/corpus.csv`, the per-image data the corpus charts draw
- -> XREF: D08 T02 §3 -- the `--check` gate that keeps the generated files identical to their sources
- -> XREF: D99 T01 §5 -- the operator sets the Pages source to GitHub Actions for §3
- -> XREF: D09 T01 §4 -- the 1.0 release, which links the published site

## Outcome

- `docs/` is a Jekyll site (`docs/_config.yml`, the Just the Docs theme through `remote_theme`, a navigation list), and CI builds it with `actions/jekyll-build-pages` from `./docs` only; nothing outside `docs/` is published.
- `python scripts/build-docs.py` writes the generated parts (the pages mirrored from `spec/FOLD.md` and `README.md` with front matter, and the SVG charts and data tables under `docs/_includes/generated/`) with no package beyond the Python standard library; `--check` exits 1 when a committed generated file differs from what its sources produce.
- The benchmarks page shows, per set (reference image, photographs, generated images, held-out once it exists): size ratios, encode and decode times at one and default threads, and peak memory against JPEG XL e7, JPEG XL e9, lossless WebP, and PNG; plus a per-image size scatter and a size-against-decode-time chart.
- The `pages` workflow checks the generated files, builds, and deploys on every push to `main` that touches the docs or the data.

**Adjacency:** list=applicable @ D08 T01 §1; document=applicable @ D08 T01 §1; settings=not-applicable (the site's configuration is a committed file, not a user setting); reporting=applicable @ D08 T01 §2; notifications=not-applicable (a docs build reports on the console); permissions=not-applicable (a static site); audit=not-applicable (git history is the audit); exchange=applicable @ D08 T01 §3; reverse=not-applicable (a redeployed site replaces the last)

**Adjacency rationale:** The site lists its pages in one navigation; the pages are the documents; the charts are the reports; the workflow exports the site to GitHub Pages.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | The Jekyll site and its generated pages | -- |  [ ]   |
|   2   |   §2    | Comparison charts | §1, D00 T02 §3 |  [ ]   |
|   3   |   §3    | Publish with GitHub Pages | §2, D99 T01 §5 |  [ ]   |

---

## 1. The Jekyll Site and Its Generated Pages

**Job:** A reader can browse a list of documentation pages in one navigation and read each document rendered as HTML.

Jekyll renders what it is good at (Markdown, layouts, the theme, navigation, search). Pages whose source lives outside `docs/` are mirrored into it by `scripts/build-docs.py`, never copied by hand, so they cannot drift from their sources.

- [ ] Add `docs/_config.yml` (title, `remote_theme: just-the-docs/just-the-docs` with the `jekyll-remote-theme` plugin, search on, `exclude:` for `reviews/` and `phase-runs/` so review records are never published) and `docs/Gemfile` for local previews. Done when: `actions/jekyll-build-pages` builds `./docs` in CI without an error. Cheaper substitute: building from the repository root, which publishes `todo/`, `AGENTS.md`, and the skills.
- [ ] Write the pages that live in `docs/`: `index.md` (overview), `getting-started.md`, `benchmarks.md`, with front matter placing them in the navigation, and front matter on `docs/research.md`. Done when: the built site lists them in its navigation.
- [ ] Write `scripts/build-docs.py`, which mirrors `spec/FOLD.md` to `docs/format.md` and the command and limits parts of `README.md` to `docs/usage.md`, each with front matter and a generated-file banner, and `--check`, which regenerates in memory and compares. Done when: editing `spec/FOLD.md` without rerunning makes `python scripts/build-docs.py --check` exit 1.
- [ ] Commit: `"docs: a Jekyll site with pages mirrored from their sources"`

**Test checkpoint:** `python scripts/build-docs.py --check` exits 0 on the tree and 1 after a source edit (quoted, then regenerated); the CI build log of `./docs` is quoted with no warning.

## 2. Comparison Charts

**Job:** A reader can see size, speed, and memory against JPEG XL e7, JPEG XL e9, lossless WebP, and PNG as charts that report the committed measurements.

Charts are generated as static SVG by `scripts/build-docs.py`, not drawn by JavaScript in the browser, so their numbers can be checked in CI against the data.

- [ ] Generate SVG charts into `docs/_includes/generated/charts/` from `benchmarks/results.csv` and `benchmarks/corpus.csv`: grouped bars of size ratio per set, encode and decode time per set at one and default threads, peak memory, a per-image size-ratio scatter against JPEG XL e9, and a size-against-decode-time chart per set. Every chart carries its source file, the codec revision, and the date from the data file's header. Done when: `docs/benchmarks.md` includes every chart with `{% include generated/charts/<name>.svg %}` and each chart's numbers equal the CSV's. Cheaper substitute: a JavaScript chart library drawing at view time, which no gate can check.
- [ ] Generate a Markdown data table under every chart into `docs/_includes/generated/tables/`. Done when: each chart has its table on the page.
- [ ] Add `--self-test`, which parses each generated SVG and asserts every bar's value equals its CSV cell. Done when: `python scripts/build-docs.py --self-test` passes and a hand-edited bar makes it fail.
- [ ] Commit: `"docs: comparison charts generated from the committed measurements"`

**Test checkpoint:** `python scripts/build-docs.py --self-test` and `--check` exit 0; the failure on a hand-edited bar quoted, then regenerated.

## 3. Publish with GitHub Pages

**Job:** A reader can open the site the `pages` workflow exports to GitHub Pages.

The workflow is GitHub's Jekyll starter adapted to this repository: it builds `./docs` only, checks the generated files first, pins every action to a full commit SHA like the other workflows, and runs only when the docs or their data change.

- [ ] Add `.github/workflows/pages.yml`: on pushes to `main` touching `docs/**`, `spec/**`, `README.md`, `benchmarks/**`, or `scripts/build-docs.py`, and on `workflow_dispatch`; permissions `contents: read`, `pages: write`, `id-token: write`; concurrency group `pages` without cancelling; a `build` job (checkout, `python scripts/build-docs.py --check`, `actions/configure-pages`, `actions/jekyll-build-pages` building `./docs` into `./_site`, `actions/upload-pages-artifact`) and a `deploy` job (`actions/deploy-pages`, environment `github-pages`); every action pinned to a full SHA with its version in a comment. Done when: the workflow run is green and the site loads at https://rizonesoft.github.io/RasterFold/. Cheaper substitute: the unmodified starter workflow, which builds the whole repository and skips the check.
- [ ] Commit: `"docs: publish the site with GitHub Pages"`

**Test checkpoint:** the `pages` run's URL from `python scripts/review_prompt.py ci-wait <sha> --workflow pages --workflow-path .github/workflows/pages.yml`, and a fetch of the site's index page returning 200.

## Verification

- [ ] `python scripts/build-docs.py --check` and `--self-test` exit 0
- [ ] `python scripts/todo-graph.py validate` clean
