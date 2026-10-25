---
schema_version: 1
id: measurement-references
domain: 00-workspace
status: draft
title: "TODO-02 -- Measurement References: JPEG XL e9 and WebP"
depends_on: []
track: W2
---

# TODO-02 -- Measurement References: JPEG XL e9 and WebP

> **Goal:** Every measurement RasterFold makes reports JPEG XL effort 7, JPEG XL effort 9, and lossless WebP side by side, from the research tools, the benchmark, and a committed per-image corpus file the docs and drift gates read, so no target in the roadmap is measured against a reference nobody reran.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** `research/corpus.py` records JPEG XL e7 (`<image>:e7`), JPEG XL e9 (`<image>:e9`), and lossless WebP (`<image>:webp`) sizes in `build/research/jxl_refs.json`, and `research/ab.py` (`size` and `rsize`) compares against all three references via `summarize_sizes`. `tests/benchmark.py` runs `rasterfold`, `rasterfold-1t`, `jxl`, `jxl-1t` (both e7), `jxl-e9` (effort 9), `webp`, and `png`, merges subset runs with existing `benchmarks/results.csv` rows via `load_existing_rows`/`merge_rows`, and labels retained sets in `benchmarks/results.md`. On the 39-photo corpus, `research/ab.py size photo` reports 0.9712 of e7, 0.9829 (0.983) of e9, and 0.9041 (0.904) of WebP; on `synth`, geometric mean is 1.1986 of e7, 2.2860 (2.29) of e9, and 3.6626 (3.66) of WebP. No per-image corpus results are committed yet.
<!-- claim: count "effort\", \"7\"" research/corpus.py = 1 -->
<!-- claim: count "effort\", \"9\"" research/corpus.py = 1 -->
<!-- claim: count "\"libwebp\"" research/corpus.py = 1 -->
<!-- claim: exists tests/test_research_refs.py -->
<!-- claim: count "\"jxl-e9\"" tests/benchmark.py = 1 -->
<!-- claim: absent benchmarks/corpus.csv -->

## Inputs

- [`research/corpus.py`](../../research/corpus.py), [`research/ab.py`](../../research/ab.py) -- the corpus builder and the size tool §1 extends
- [`tests/benchmark.py`](../../tests/benchmark.py) -- the benchmark §2 extends
- [`docs/research.md`](../../docs/research.md) -- section 1, whose figures §3's file will back
- -> XREF: D08 T02 §2 -- binds the docs' figures to the corpus file §3 commits
- -> XREF: D08 T01 §2 -- the charts that draw the corpus file §3 commits

## Outcome

- `python research/ab.py size photo` prints ratios against JPEG XL e7, JPEG XL e9, and WebP.
- `python tests/benchmark.py` has a `jxl-e9` codec column.
- `benchmarks/corpus.csv` holds every research image's RasterFold, JPEG XL e7, JPEG XL e9, and WebP size, with the codec revision that produced it.

**Adjacency:** list=not-applicable (a measurement tool has no records to browse); document=not-applicable (no printed output); settings=not-applicable (no new option); reporting=applicable @ D00 T02 §1; notifications=not-applicable (a tool run reports on the console); permissions=not-applicable (no refusal path); audit=not-applicable (git history records each corpus file); exchange=applicable @ D00 T02 §3; reverse=not-applicable (measurements are regenerated, not undone)

**Adjacency rationale:** Reporting is the size tool's new columns; exchange is the committed CSV the docs and drift gates read.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | e9 and WebP references in the research tools | -- |  [x]   |
|   2   |   §2    | JPEG XL e9 in the benchmark | -- |  [x]   |
|   3   |   §3    | The committed corpus results file | §1 |  [ ]   |

---

## 1. e9 and WebP References in the Research Tools

> **Started:** 2026-10-25T00:45:00Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A developer can see every size ratio against JPEG XL e7, JPEG XL e9, and WebP in one report.

The roadmap measures milestones against JPEG XL e9 and WebP, so the tool that decides every compression experiment must report them.

**Corrected 2026-10-25:** Per the pre-build `--kind plan` review (`docs/reviews/00-workspace/D00-T02-s1.md`), `research/ab.py` shares `summarize_sizes` across both `size` and `rsize` modes, validates every image's `:e7`, `:e9`, and `:webp` entry in `jxl_refs.json` as a strict positive integer (`type(v) is int and v > 0`, rejecting missing, zero, negative, non-integer, and boolean values) before printing any `-v` ratio lines, and `research/corpus.py` (`update_refs` / `encode_ref`) fails non-zero if `ffmpeg` fails or writes a non-positive output file, reloads `build/research/jxl_refs.json` to verify all three positive integer keys per corpus stem, and is covered alongside `size`/`rsize` independent arithmetic checks in `tests/test_research_refs.py`.

- [x] Encode every corpus image with JPEG XL effort 9 (`-distance 0 -effort 9 -threads 1`) and lossless WebP (`-lossless 1 -quality 100 -compression_level 6 -threads 1`) in `research/corpus.py` (`REFS_SPEC`, `encode_ref`, `update_refs`), recording `<image>:e9` and `<image>:webp` beside `<image>:e7` in `build/research/jxl_refs.json`, failing non-zero on `ffmpeg` failure without corrupting `jxl_refs.json`, reloading `jxl_refs.json` to verify every corpus stem has positive integer `:e7`, `:e9`, and `:webp` keys, and updating this file's `Current state` and claims. Done when: all 55 stems in `build/research/ppm` have positive integer `:e7`, `:e9`, and `:webp` keys matching the encoded files on disk (`python3 scripts/windows.py research/corpus.py`), and `python3 scripts/todo-claims.py` exits 0. Cheaper substitute: measuring e9 on `measure` alone.
- [x] Print `e9=` and `webp=` totals, ratios, geometric means, and worst images beside `jxl=` in `research/ab.py` (`size` and `rsize` via shared `summarize_sizes`), and the three per-image ratios with `-v`, validating all three references as strict positive integers before printing any ratios and verifying against independent arithmetic in `tests/test_research_refs.py`. Done when: `python3 research/ab.py size photo` reports `total=30865211 jxl=31782058 ratio=0.9712 geomean=0.9728 worst=1.0037 (kodim17) e9=31400714 e9_ratio=0.9829 e9_geomean=0.9883 e9_worst=1.0193 (kodim16) webp=34138176 webp_ratio=0.9041 webp_geomean=0.8932 webp_worst=1.0104 (ic_deer) measure=1409028 (0.9581 e9=0.9781 webp=0.8869)` (`0.9829` / `0.983` of e9 and `0.9041` / `0.904` of WebP, matching the 2026-09-29 `ffmpeg 8.1.2-full_build-www.gyan.dev` figures and independent arithmetic), and `tests/test_research_refs.py` passes on Linux and Windows.
- [x] Commit: `"research: report JPEG XL e9 and WebP beside e7"`

**Test checkpoint:** `python3 research/ab.py size synth -v` lists `syn_checker` with e9 210 and WebP 82 bytes (`syn_checker 5709 jxl= 316 18.0665 e9= 210 27.1857 webp= 82 69.6220`, matching `build/research/jxl/syn_checker.e9.jxl` and `syn_checker.webp` produced by `ffmpeg 8.1.2-full_build-www.gyan.dev`); deleting `syn_checker:e9` makes `python3 research/ab.py size synth -v` exit 1 with `missing reference in jxl_refs.json: syn_checker:e9` and empty stdout instead of printing a ratio; `python3 -m unittest discover -s tests -p 'test_research_refs.py' -v` and `python3 scripts/windows.py tests/test_research_refs.py -v < /dev/null` exit 0.

> **Verified:** 2026-10-25 | §1 | 92ce6dcc484e9cca3b016144767f273ede8c87c1
> **Review:** `docs/reviews/d00-t02-s1.json`
## 2. JPEG XL e9 in the Benchmark

> **Started:** 2026-10-25T01:01:00Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Corrected 2026-10-25:** Per the pre-build `--kind plan` review (`docs/reviews/00-workspace/D00-T02-s2.md`), `tests/benchmark.py` defines typed CSV loading (`parse_csv_row`, `load_existing_rows`, `write_csv_rows`) with strict `pixels_match` boolean parsing (`"True"` / `"False"`) and nullable integer `bytes`, merges subset runs with unrequested sets in `benchmarks/results.csv` via `merge_rows` without duplicating rows or inventing results (`report()` renders `--` for unmeasured codec cells in retained sets), explicitly labels retained sets in `benchmarks/results.md` as retained from `benchmarks/results.csv` without claiming a single run/repeat count across retained sets, and covers subset merge, failure preservation, and CSV/Markdown agreement in `tests/test_research_refs.py`.

- [x] Add a `jxl-e9` codec (`-effort 9 -threads 1`) to `CODECS` in `tests/benchmark.py`, decoded like `jxl`, together with typed CSV persistence (`parse_csv_row`, `load_existing_rows`, `write_csv_rows`, `merge_rows`), `--` rendering for unmeasured codec cells in retained sets, explicit retained-set provenance notes in `report()`, and regression coverage in `tests/test_research_refs.py`. Done when: `python3 scripts/windows.py tests/benchmark.py measure -n 1` updates `measure` in `benchmarks/results.csv` and `benchmarks/results.md` with a `jxl-e9` column while retaining `photos` and `synthetic` rows unchanged with explicit provenance notes, and `tests/test_research_refs.py` passes on Linux and Windows. Cheaper substitute: quoting e9 from the research tool, which has no times.
- [x] Commit: `"test: benchmark JPEG XL effort 9"`

**Test checkpoint:** the `measure` table in `benchmarks/results.md` (and `benchmarks/results.csv`) shows `jxl-e9` at 1,440,564 bytes (`0.980` of `jxl`) with its encode time (`10361742` us, `5.369x` `jxl`) and decode time (`573121` us, `1.073x` `jxl`), while `photos` and `synthetic` retain their 126 six-codec rows in `benchmarks/results.csv` (`133` total rows) and render `--` for `jxl-e9` under explicit `Retained from existing benchmarks/results.csv (not re-measured in this run).` notes; `python3 -m unittest discover -s tests -p 'test_research_refs.py' -v` and `python3 scripts/windows.py tests/test_research_refs.py -v < /dev/null` exit 0.

> **Verified:** 2026-10-25 | §2 | 98ce3193d3be595a103e0628372f00d3818cf5f4
> **Review:** `docs/reviews/d00-t02-s2.json`
## 3. The Committed Corpus Results File

**Job:** A reader of the docs can check every quoted corpus figure against the committed file the size tool exports, which names the revision it came from.

- [ ] Add `--csv <path>` to `research/ab.py size`, writing per-image rows (set, image, width, height, rasterfold, jxl_e7, jxl_e9, webp) and a header line naming the codec revision (`git rev-parse HEAD`) and the `KAT_FOLD` hash. Done when: `python research/ab.py size all --csv benchmarks/corpus.csv` writes every research image.
- [ ] Commit `benchmarks/corpus.csv`. Done when: the file is tracked and its figures match `docs/research.md` section 1.
- [ ] Commit: `"research: commit the per-image corpus results"`

**Test checkpoint:** the committed file's photo totals reproduce `docs/research.md` section 1 (0.9712 of e7) when summed by a one-line script quoted in the stamp.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
