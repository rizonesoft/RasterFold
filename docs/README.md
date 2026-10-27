# RasterFold documentation

| Guide | Purpose |
| --- | --- |
| [Build and verify](building.md) | Repository-local toolchains, target artifacts, host requirements and packages |
| [Use the C library](library.md) | API ownership, threading, errors and linking |
| [Format specification](../spec/FOLD.md) | Normative `.fold` version 3 contract |
| [Examples](examples.md) | CLI workflows and C11 PPM encode/decode examples (`examples/`) |
| [Benchmarks](benchmarks.md) | Current and retained corpus/reference measurements, SVG charts and data tables |
| [Limitations](limitations.md) | Current format, pixel layout, dimension/file caps, memory model and platform scope |
| [Troubleshooting](troubleshooting.md) | Exit codes `0..5`, exact refusal diagnostics, output publication recovery and toolchain repair |
| [Licenses and provenance](licenses.md) | GPL-3.0 license, code of conduct, test photo provenance and pinned toolchain components |
| [Research record](research.md) | Measurement protocol, experiments and prior art |
| [Historical measurements](measurements.md) | Preserved pre-migration Windows comparison, with its limits |
| [WSL migration audit](wsl-migration-audit.md) | Actual build/test evidence and remaining runtime limits |
| [Document drift policy](document-drift.md) | Drift facts, review receipts, generated outputs and site verification |
| [Agent skills and rules](agent-skills-and-rules-structure.md) | Antigravity skills, rules, native Stop hook and adaptive review dispatch |
| [Runner intelligence](../.intelligence/README.md) | Family-owned observations and stopped maintenance |
| [Execution plan](../todo/implementation-plan.md) | Canonical planned work and section ownership |

The Jekyll documentation site sources are staged from the allowlist in `docs/site-content.json` by `scripts/build-docs.py` and published to GitHub Pages by `.github/workflows/pages.yml` (`D08 T01 §3`), which verifies the live deployment against the build job's `drift-build.json` receipt via `scripts/build-docs.py --verify-live`. The native converter application (`docs/vision.md`) remains separate planned work; these guides do not claim it has been built or published as a download.
