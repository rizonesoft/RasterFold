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

> **Goal:** RasterFold has an HTML documentation site, generated from the Markdown sources and the committed measurement data by one stdlib Python script: an overview, getting started, the command-line and API references, the format specification, the research notes, and a benchmarks section with charts comparing RasterFold with JPEG XL e7, JPEG XL e9, lossless WebP, and PNG on size, encode and decode speed, and memory. The committed site is always exactly what its sources generate.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** Documentation is Markdown only: `README.md`, `spec/FOLD.md`, `docs/research.md`, `research/README.md`, `todo/README.md`. Measurements live in `benchmarks/results.md` and `benchmarks/results.csv` (from `tests/benchmark.py`). There is no HTML, no chart, and no Pages workflow.
<!-- claim: absent docs/site -->
<!-- claim: absent scripts/build-docs.py -->
<!-- claim: exists benchmarks/results.csv -->

## Inputs

- [`README.md`](../../README.md), [`spec/FOLD.md`](../../spec/FOLD.md), [`docs/research.md`](../../docs/research.md) -- the page sources
- [`benchmarks/results.csv`](../../benchmarks/results.csv) -- the data the charts draw
- -> XREF: D00 T02 §3 -- `benchmarks/corpus.csv`, the per-image data the corpus charts draw
- -> XREF: D08 T02 §3 -- the `--check` gate that keeps the committed site identical to its sources
- -> XREF: D99 T01 §5 -- the operator enables GitHub Pages for §3
- -> XREF: D09 T01 §4 -- the 1.0 release, which links the published site

## Outcome

- `python scripts/build-docs.py` writes `docs/site/` (HTML pages, one stylesheet, inline SVG charts) with no network and no package beyond the Python standard library; `--check` exits 1 when the committed site differs from what the sources generate.
- The benchmarks page draws, per set (reference image, photographs, generated images, held-out once it exists): size ratios, encode and decode times at one and default threads, and peak memory, against JPEG XL e7, JPEG XL e9, lossless WebP, and PNG; plus a per-image size scatter and a size-against-decode-time chart.
- The site is published by the `pages` workflow on every push to `main`.

**Adjacency:** list=applicable @ D08 T01 §1; document=applicable @ D08 T01 §1; settings=not-applicable (the generator takes no configuration beyond its sources); reporting=applicable @ D08 T01 §2; notifications=not-applicable (a docs build reports on the console); permissions=not-applicable (a static site); audit=not-applicable (git history is the audit); exchange=applicable @ D08 T01 §3; reverse=not-applicable (a regenerated site replaces the last)

**Adjacency rationale:** The site lists its pages in one navigation; the pages are the documents; the charts are the reports; publishing exports the site.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | The site generator | -- |  [ ]   |
|   2   |   §2    | Comparison charts | §1, D00 T02 §3 |  [ ]   |
|   3   |   §3    | Publish with GitHub Pages | §2, D99 T01 §5 |  [ ]   |

---

## 1. The Site Generator

**Job:** A reader can browse a list of documentation pages in one navigation and read each document rendered as HTML.

- [ ] Write `scripts/build-docs.py`: a stdlib Markdown renderer for the subset the sources use (headings with GitHub anchors, paragraphs, lists, tables, fenced code, inline code, links, emphasis, blockquotes), page templates with a navigation list, and a stylesheet with light and dark themes. Done when: `python scripts/build-docs.py` writes `docs/site/index.html` and one page per source. Cheaper substitute: a third-party site generator that a fresh clone cannot run without installing it.
- [ ] Add pages: overview (from `README.md`), getting started, format specification (`spec/FOLD.md`), research notes (`docs/research.md`), and the development workflow (`todo/README.md` summary). Done when: every relative link between pages resolves.
- [ ] Add `--check` (regenerate in memory and compare with `docs/site/`). Done when: editing a source without regenerating makes `--check` exit 1.
- [ ] Commit: `"docs: generate the HTML documentation site"`

**Test checkpoint:** `python scripts/build-docs.py --check` exits 0 on the committed site and 1 after a source edit (quoted, then regenerated).

## 2. Comparison Charts

**Job:** A reader can see size, speed, and memory against JPEG XL e7, JPEG XL e9, lossless WebP, and PNG as charts that report the committed measurements.

- [ ] Draw inline SVG charts in `scripts/build-docs.py` from `benchmarks/results.csv` and `benchmarks/corpus.csv`: grouped bars of size ratio per set, encode and decode time per set at one and default threads, peak memory, a per-image size-ratio scatter against JPEG XL e9, and a size-against-decode-time chart per set. Every chart carries its source file, the codec revision, and the date from the data file's header. Done when: the benchmarks page shows every chart and each chart's numbers equal the CSV's. Cheaper substitute: screenshots of charts, which go stale without anyone noticing.
- [ ] Add a data table under every chart with the exact figures. Done when: each chart has its table.
- [ ] Commit: `"docs: comparison charts from the committed measurements"`

**Test checkpoint:** a test in `scripts/build-docs.py --self-test` parses the generated SVG and asserts each bar's value equals its CSV cell; `--check` exits 0.

## 3. Publish with GitHub Pages

**Job:** A reader can open the site the `pages` workflow exports to GitHub Pages.

- [ ] Add `.github/workflows/pages.yml`, which runs `python scripts/build-docs.py --check` and publishes `docs/site/` on every push to `main` that touches it. Done when: the workflow run is green and the site loads at its Pages address. Cheaper substitute: publishing a site CI never checked.
- [ ] Commit: `"docs: publish the site with GitHub Pages"`

**Test checkpoint:** the `pages` run's URL and the site's address quoted from `ci-wait` and a fetch of the index page.

## Verification

- [ ] `python scripts/build-docs.py --check` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
