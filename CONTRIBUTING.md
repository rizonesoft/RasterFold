# Contributing to RasterFold

Start with [AGENTS.md](AGENTS.md), the [build guide](docs/building.md) and the [TODO format](todo/README.md). The project uses trunk-based `main`, focused changes and independent review. Keep unrelated working changes intact.

Describe the problem, affected target and a reproducible input before changing the codec. Corpus-wide improvement matters more than a result on one photograph. Compression and speed experiments follow [the research protocol](docs/research.md), including negative results and explicit thread counts.

## Build and test

```bash
python3 tools/provision.py
source .venv/bin/activate
./build.sh --target all
python3 scripts/check-all.py --all-targets
```

The Windows commands are in the build guide. Never claim a cross-build as a runtime test or a local run as GitHub CI. Keep complete gate logs under ignored `build/`; put concise, reproducible evidence in the review record.

## Preserve the contracts

- A format change updates the C implementation, independent oracle, specification and known-answer file together, with the appropriate new format version or method number.
- The CLI never replaces an existing destination, and malformed input must be bounded and rejected without a crash or hang.
- New dependencies need a reason recorded by the owning TODO section. Compiler archives belong in the ignored local toolchain, not Git.
- One section is one logical candidate range. Forward review fixes are allowed; do not amend a recorded candidate, force-push, bypass hooks or stamp unreviewed work.
- A TODO row becomes complete only with verified evidence and the required independent reviews. Keep the implementation plan synchronized through its tooling.

Run `python3 scripts/drift-check.py` after changing source or documentation. `--fix` refreshes registered generated output; stale prose review receipts require a real source review. The [drift guide](docs/document-drift.md) covers repairs, commit declarations and site artifacts. `python3 scripts/plan-audit.py` checks dependency order and release coverage.

Use clear imperative commit subjects. Do not add assistant attribution trailers. Write Markdown paragraphs and list items on one logical line and avoid decorative hard wrapping.

## Report responsibly

For a normal defect, use the bug report form with platform, compiler/build identity and reproduction steps. Share only images you have permission to redistribute. For memory safety, data loss or other security concerns, use [private reporting](SECURITY.md).
