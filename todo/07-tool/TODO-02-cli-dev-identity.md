---
schema_version: 1
id: cli-dev-identity
domain: 07-tool
status: draft
title: "TODO-02 -- Standalone CLI Development Build Identity"
depends_on: []
track: T2
---

# TODO-02 -- Standalone CLI Development Build Identity

> **Goal:** Let a developer identify the exact git commit and dirty state of a standalone development `rasterfold` executable from `rasterfold --version` / `rasterfold version`, while keeping tagged release packages reproducible and reporting the clean release SemVer (`RF_PRODUCT_VERSION`).

> [!IMPORTANT]
> **Current state (verified 2026-11-13):** `src/main.c` prints `product version: <RF_PRODUCT_VERSION>` from committed `src/product_version.h`, while `scripts/version.py --dev` computes `<version>-dev+<sha>[.dirty]` only for `bin/<target>/build.json` and development zip filenames. A bare development binary copied outside `bin/<target>/` carries no source revision line.
<!-- claim: exists src/product_version.h -->
<!-- claim: exists scripts/version.py -->

## Inputs

- [`scripts/version.py`](../../scripts/version.py), [`scripts/build.py`](../../scripts/build.py) -- product SemVer vs checkout development identity (`--dev`) and target build flags
- [`src/main.c`](../../src/main.c), [`scripts/verify_release.py`](../../scripts/verify_release.py), [`scripts/package.py`](../../scripts/package.py) -- CLI `version` / `--version` output and release-package reproducibility/version checks
- [`scripts/build-docs.py`](../../scripts/build-docs.py) -- generated CLI reference (`docs/cli.md`)
- -> XREF: D09 T01 §4 -- release backlog review for `rasterfold-v1.0.0` that promoted `B-005`

## Outcome

- Development builds pass a build-generated revision identity (kept in `build/` / `bin/<target>/`, never dirtying tracked `src/product_version.h`) so `rasterfold version` / `rasterfold --version` prints a revision report alongside `product version: <SemVer>`, whereas `--release` builds (`scripts/package.py --release` / release builds) omit `-dev+` or print a clean release build so `scripts/verify_release.py` and reproducible release zip hashes hold.

**Adjacency:** all=not-applicable (read-only `rasterfold --version` build-provenance line; no collection, document write, setting, permission, or format I/O workflow)

**Adjacency rationale:** Only the read-only `version` / `--version` text report and per-target untracked build metadata change.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Report development revision in standalone CLI version output | D09 T01 §1, D05 T01 §10 |  [ ]   |

---

## 1. Report Development Revision in Standalone CLI Version Output

**Origin:** discovered run=36a212a58ece 2026-11-13 -- promoted from `B-005` during `D09 T01 §4` release backlog review

-> SOURCE: D09-T01-S1-completion

**Job:** A developer or user running `rasterfold --version` or `rasterfold version` reads an audit report naming both the product version and, on development builds, the checkout's commit revision and dirty status without needing the surrounding build directory.

`D09 T01 §1` kept `src/product_version.h` committed and revision-free so every commit did not rewrite a tracked header; injecting a generated `build_identity.h` from untracked `bin/<target>/` during `scripts/build.py` (with `--release` / `package.py --release` passing release mode so `product version: <VERSION>` and reproducible release zips remain unchanged) gives standalone development binaries exact provenance without tracked-header churn.

- [ ] Emit an untracked per-target header in `scripts/build.py` carrying `scripts/version.py --dev` on development builds and clean release identity under `--release` (`scripts/package.py --release`), wire `src/main.c` (`version` / `--version`) to print `build identity: <dev-label>` on development builds while keeping `product version: <VERSION>` intact for `scripts/verify_release.py`, update `docs/cli.md` via `python3 scripts/build-docs.py --write` if `help version` changes, and cover both development and `--release` outputs in `tests/test_version.py` and `tests/test_package.py`. Done when: a bare development binary reports `<version>-dev+<12-hex>[.dirty]` and `python3 scripts/package.py --release --target all` + `scripts/verify_release.py` remain reproducible and green on Linux and Windows. Cheaper substitute: writing the commit SHA into tracked `src/product_version.h`, which dirties every checkout after each commit.
- [ ] Commit: `"cli: report development build identity in version output"`

**Test checkpoint:** `./bin/linux-x64/bin/rasterfold --version` and `python3 scripts/windows.py bin/windows-x64/bin/rasterfold.exe --version` quote `build identity:`, and `python3 -m unittest discover -s tests -p test_version.py` plus `-p test_package.py` pass on both platforms.

## Verification

- [ ] `python3 -m unittest discover -s tests -p test_version.py` exits 0
- [ ] `python3 -m unittest discover -s tests -p test_package.py` exits 0
- [ ] `python3 scripts/check-all.py --all-targets` exits 0
- [ ] `python3 scripts/todo-graph.py validate` clean
