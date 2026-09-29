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
> **Current state (verified 2026-09-29):** `research/corpus.py` records JPEG XL e7 sizes only (`jxl_refs.json` keys `<image>:e7`), and `research/ab.py size` compares against e7 only. `tests/benchmark.py` runs `rasterfold`, `rasterfold-1t`, `jxl`, `jxl-1t` (both e7), `webp`, and `png`. A one-off script measured e9 and WebP on the corpus on 2026-09-29: photos 0.983 of e9 (28 of 39 below) and 0.904 of WebP; generated images geometric mean 2.29 of e9 and 3.66 of WebP. No per-image corpus results are committed.
<!-- claim: count "effort\", \"7\"" research/corpus.py = 1 -->
<!-- claim: count "\"jxl-e9\"" tests/benchmark.py = 0 -->
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
|   1   |   §1    | e9 and WebP references in the research tools | -- |  [ ]   |
|   2   |   §2    | JPEG XL e9 in the benchmark | -- |  [ ]   |
|   3   |   §3    | The committed corpus results file | §1 |  [ ]   |

---

## 1. e9 and WebP References in the Research Tools

**Needs:** ffmpeg with libjxl (benchmarks)

**Job:** A developer can see every size ratio against JPEG XL e7, JPEG XL e9, and WebP in one report.

The roadmap measures milestones against JPEG XL e9 and WebP, so the tool that decides every compression experiment must report them.

- [ ] Encode every corpus image with JPEG XL effort 9 (`-distance 0 -effort 9 -threads 1`) and lossless WebP (`-lossless 1 -quality 100 -compression_level 6 -threads 1`) in `research/corpus.py`, recording `<image>:e9` and `<image>:webp` in `build/research/jxl_refs.json`. Done when: every image has all three keys. Cheaper substitute: measuring e9 on `measure` alone.
- [ ] Print `e9=` and `webp=` totals, geometric means, and worst images beside `jxl=` in `research/ab.py size`, and the three per-image ratios with `-v`. Done when: `python research/ab.py size photo` reports photos 0.983 of e9 and 0.904 of WebP (the 2026-09-29 figures).
- [ ] Commit: `"research: report JPEG XL e9 and WebP beside e7"`

**Test checkpoint:** `python research/ab.py size synth -v` lists `syn_checker` with e9 210 and WebP 82 bytes; deleting one `:e9` key makes the tool name the missing reference instead of printing a ratio.

## 2. JPEG XL e9 in the Benchmark

**Needs:** ffmpeg with libjxl (benchmarks)

- [ ] Add a `jxl-e9` codec (`-effort 9 -threads 1`) to `CODECS` in `tests/benchmark.py`, decoded like `jxl`. Done when: `python tests/benchmark.py measure -n 1` writes a `jxl-e9` column in `benchmarks/results.md`. Cheaper substitute: quoting e9 from the research tool, which has no times.
- [ ] Commit: `"test: benchmark JPEG XL effort 9"`

**Test checkpoint:** the `measure` table shows `jxl-e9` at 1,440,564 bytes with its encode and decode times.

## 3. The Committed Corpus Results File

**Job:** A reader of the docs can check every quoted corpus figure against the committed file the size tool exports, which names the revision it came from.

- [ ] Add `--csv <path>` to `research/ab.py size`, writing per-image rows (set, image, width, height, rasterfold, jxl_e7, jxl_e9, webp) and a header line naming the codec revision (`git rev-parse HEAD`) and the `KAT_FOLD` hash. Done when: `python research/ab.py size all --csv benchmarks/corpus.csv` writes every research image.
- [ ] Commit `benchmarks/corpus.csv`. Done when: the file is tracked and its figures match `docs/research.md` section 1.
- [ ] Commit: `"research: commit the per-image corpus results"`

**Test checkpoint:** the committed file's photo totals reproduce `docs/research.md` section 1 (0.9712 of e7) when summed by a one-line script quoted in the stamp.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
