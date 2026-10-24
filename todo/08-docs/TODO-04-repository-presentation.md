---
schema_version: 1
id: repository-presentation
domain: 08-docs
status: done
title: "TODO-04 -- Repository Presentation and Developer Entry Points"
depends_on: []
frozen: true
track: D1
---

# TODO-04 -- Repository Presentation and Developer Entry Points

> **Goal:** Give RasterFold a distinctive and accurate GitHub entry point with original branding, clear platform/library guidance, community files and artifact CI.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** The operator requested the same presentation care as `R:\GitHub\Isotone` and Impossible OS. Their checked-out READMEs and community/workflow files supplied structural references, not RasterFold branding or performance evidence. RasterFold's committed cover, API guide and cross-platform workflow are owned here; the existing Jekyll site plan remains in its original file.
<!-- claim: exists assets/brand/cover.svg -->
<!-- claim: exists docs/building.md -->
<!-- claim: exists docs/library.md -->

## Inputs

- [`README.md`](../../README.md), [`docs/research.md`](../../docs/research.md), [`spec/FOLD.md`](../../spec/FOLD.md): product and format authority.
- [`docs/building.md`](../../docs/building.md), [`docs/library.md`](../../docs/library.md): new developer entry points.
- -> XREF: D00 T04 §1 -- local toolchains and target artifacts.

## Outcome

- Original vector identity, a focused README, community templates and accurate native/cross-build instructions.
- CI builds and packages all requested targets, with runtime coverage distinguished from compilation.

**Adjacency:** list=not-applicable (repository landing page); document=not-applicable (technical guides only); settings=not-applicable (no product settings); reporting=applicable @ D08 T04 §1; notifications=not-applicable (no application notifications); permissions=not-applicable (no new data access); audit=applicable @ D08 T04 §1; exchange=applicable @ D08 T04 §1; reverse=not-applicable (no user data mutation)

**Adjacency rationale:** Build reports and artifact metadata make delivery reviewable; the source and API guides describe the exchange contract.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Repository presentation and developer entry points | D00 T04 §1 | [x] |

---

## 1. Repository Presentation and Developer Entry Points

> **Started:** 2026-10-24T22:25:00Z

**Job:** A developer reads build reports and the migration audit, then downloads artifacts with an explicit platform and API contract.

- [x] Add original SVG cover/icon, an accurate README and preserved historical measurement context. Done when: local links resolve and no old Windows measurement is presented as a new cross-platform claim (landed in `2c89e2e`; updated `README.md` and `docs/building.md` for the 2026-10-24 Antigravity consolidation).
- [x] Add build/API guides, community policy, issue forms and PR guidance. Done when: commands name actual artifacts and runtime limits, with no fabricated releases or contacts (`docs/building.md`, `docs/library.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `.github/ISSUE_TEMPLATE/`, `.github/pull_request_template.md`).
- [x] Update artifact CI and create local development packages. Done when: target manifests match packaged bytes; local tests and future remote CI are described separately (`.github/workflows/build.yml` and `scripts/package.py`).
- [x] Commit: `"docs: present RasterFold and its cross-platform developer workflow"`

**Test checkpoint:** `python3 scripts/package.py --target all` verifies all artifact hashes; `python3 tests/test_artifacts.py` validates the declared ABI and native consumers. Visual inspection and local-link checks are recorded in `docs/wsl-migration-audit.md`; these do not substitute for runtime tests or remote CI.

**Freeze check:** Existing Jekyll work and historical measurements stay owned by their original sections; presentation does not change the format, codec claims or verified plan stamps.

> **Verified:** 2026-10-25 | §1 | 1c3fbc10a283bff0468c15c9efa4c95a6a2a80cc
> **Review:** `docs/reviews/d08-t04-s1.json`
## Verification

- [x] `python3 scripts/todo-graph.py validate` and `plan --check` pass.
- [x] The README's target and API claims match the measured migration audit.
