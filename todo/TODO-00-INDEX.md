# RasterFold -- TODO Index

The live execution plan for RasterFold. Format spec: [README.md](./README.md). Ordered plan: [implementation-plan.md](./implementation-plan.md).

## How to use this tree

- **This file** carries domain order and the active TODOs. Keep it at that altitude: no checklists.
- **Each domain's `INDEX.md`** lists its own TODO files and is the place to look for scope within a domain. Ideas that are not planned work live in [`backlog.md`](./backlog.md), never in a domain.
- Give every topic **one canonical home**. Cross-link with XREFs instead of duplicating scope.
- Numbering is local to a domain (`TODO-01`, `TODO-02`) and never reused.
- When work graduates to documentation, move it to the domain's Completed section rather than leaving a stale checklist here.

## What this plan is

RasterFold is a lossless RGB image codec whose target is to beat lossless JPEG XL in compression, speed, and memory on images in general. On 2026-09-29 it does so on photographs (2.9% smaller over 39 photographs, encode at 0.15 and decode at 0.52 to 0.58 of JPEG XL's time, a third of the memory) and loses on images with few colours. The plan was seeded on 2026-09-29 from the roadmap then kept in `suggestions.md`, when the TODO system, skills, hooks, and gates were ported from Isotone; that file became the research record [`../docs/research.md`](../docs/research.md) (measurement protocol, what was already tried, designs), and this tree is the executable plan.

## Domain order

Domains are numbered in **allocation order**. `DNN TNN §N` cross-references encode the domain number, so a remap rewrites every reference in the same commit. A new domain appends after the last one.

**Execution order lives in the dependency graph**, not in this column. Ask the graph: `python scripts/todo-graph.py query ready`. The **Phase** column below is the coarse sequencing.

| No. | Domain | Phase | Purpose |
| :-: | ------ | :---: | ------- |
| 00 | [Workspace](./00-workspace/INDEX.md) | 0, 8 | Toolchain, gates, hooks, CI, static analysis, the review panel, the TODO system, measurement references, and held-out validation. |
| 01 | [Compression](./01-compression/INDEX.md) | 2-7 | The palette method, the model experiments, format version 3, exact repeats, every photo below JPEG XL e9, and screen content against WebP. |
| 02 | [Speed](./02-speed/INDEX.md) | 1, 5, 10 | Compiler options, and the measurements of strips and the SIMD mixer. |
| 03 | [Memory](./03-memory/INDEX.md) | 1 | Buffer lifetimes and copies. |
| 04 | [Robustness](./04-robustness/INDEX.md) | 1, 2 | Early rejection of exhausted streams and a long fuzz campaign. |
| 05 | [Milestones](./05-milestones/INDEX.md) | 0-59 | The 60 milestone gates and the planning sections that author each milestone's work one milestone ahead. |
| 06 | [Library](./06-library/INDEX.md) | 9 | `rasterfold.dll`, the C API, and its tests and examples. |
| 07 | [Tool](./07-tool/INDEX.md) | 9 | The version 1 command line: commands, formats, messages, and exit codes. |
| 08 | [Docs](./08-docs/INDEX.md) | 0, 9 | The HTML documentation with comparison charts, and the drift and staleness gates. |
| 09 | [Release](./09-release/INDEX.md) | 9 | Versioning, the changelog, the package, and the 1.0 release. |
| 10 | [Converter](./10-converter/INDEX.md) | 9 | Native C application, bundled codecs, preview, comparisons, SVG interface and native delivery. |
| 99 | [Manual](./99-manual/INDEX.md) | 0 | Operator-only steps and decisions: the retired folders, the GitHub CLI login, GitHub Pages, and the recorded standing approval and missed-milestone policy. |

`99-manual` is numbered apart from the allocation sequence on purpose: it is the operator's phase, not a build area, and a new build domain still appends after `10`.

The Phase column is the coarse domain grouping, not an executable schedule. Current dependency-safe sequencing and live counts come only from [`implementation-plan.md`](./implementation-plan.md) plus `python scripts/todo-graph.py query ready`.

## Active TODOs

Native converter files are listed in [domain 10](./10-converter/INDEX.md); the complete product contract is [docs/vision.md](../docs/vision.md).

| TODO | Domain | Title |
| ---- | ------ | ----- |
| [TODO-01](./00-workspace/TODO-01-dev-automation.md) | 00-workspace | Dev-Automation Wiring |
| [TODO-01](./03-memory/TODO-01-buffer-lifetimes.md) | 03-memory | Buffer Lifetimes and Copies |
| [TODO-01](./04-robustness/TODO-01-decoder-hardening.md) | 04-robustness | Decoder Hardening |
| [TODO-02](./00-workspace/TODO-02-measurement-references.md) | 00-workspace | Measurement References: JPEG XL e9 and WebP |
| [TODO-01](./08-docs/TODO-01-html-docs.md) | 08-docs | HTML Documentation with Comparison Charts |
| [TODO-02](./08-docs/TODO-02-drift-gates.md) | 08-docs | Drift and Staleness Gates |
| [TODO-01](./05-milestones/TODO-01-beat-jxl-and-webp.md) | 05-milestones | Milestones 1 to 10 |

Every other TODO is listed in its domain's `INDEX.md`; the files above hold the rows of Phases 0 and 1 (milestones 1 and 2).
