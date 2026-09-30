---
schema_version: 1
id: release-v1
domain: 09-release
status: draft
title: "TODO-01 -- Release 1.0"
depends_on: []
frozen: true
track: R1
---

# TODO-01 -- Release 1.0

> **Goal:** `rasterfold-v1.0.0` ships as a signed-off, reproducible package: `rasterfold.exe`, `rasterfold.dll`, `rasterfold.h`, both import libraries, the HTML docs, the licence, and a checksum file, built by CI from the tag, with the format frozen at version 3 and the backlog reviewed.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** There is no version number in the code, no changelog, no packaging script, no release workflow, and no tag.
<!-- claim: absent CHANGELOG.md -->
<!-- claim: absent scripts/package.ps1 -->

## Inputs

- [`LICENSE`](../../LICENSE) -- GPL-3.0, shipped in the package
- -> XREF: D06 T01 §1 -- the API version macros §1 ties to the release version
- -> XREF: D08 T01 §3 -- the published docs the release links to

## Outcome

- `rasterfold --version` and `rf_version()` report 1.0.0, format version 3, API version 1.
- `dist/rasterfold-1.0.0-win-x64.zip` holds the executable, DLL, header, import libraries, docs site, licence, changelog, and `SHA256SUMS`, produced identically by CI from the tag.
- The tag `rasterfold-v1.0.0` is pushed, the release published, and every backlog entry reviewed for it.

**Adjacency:** list=not-applicable (no records to browse); document=applicable @ D09 T01 §2; settings=not-applicable (no option); reporting=applicable @ D09 T01 §1; notifications=not-applicable (a release note, no messages); permissions=not-applicable (no refusal path); audit=applicable @ D09 T01 §1; exchange=applicable @ D09 T01 §3; reverse=not-applicable (a published release is superseded, not undone)

**Adjacency rationale:** The package is the document users download; `--version` reports what they run; the changelog is the audit history; the release workflow exports the package.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | One version source and the changelog | -- |  [ ]   |
|   2   |   §2    | The release package | §1, D06 T01 §2 |  [ ]   |
|   3   |   §3    | The release workflow | §2 |  [ ]   |
|   4   |   §4    | Release rasterfold-v1.0.0 | §3, D05 T01 §9 |  [ ]   |

---

## 1. One Version Source and the Changelog

**Job:** A user can read a report of the version they run, and a history of what changed in each release.

- [ ] Define the codec version once (`RF_CODEC_VERSION` in `src/rf.h`), feeding `--version`, `rf_version()`, `docs/facts.json`, and the package name. Done when: `scripts/drift-check.py` fails when any of them disagree. Cheaper substitute: the version typed in four places.
- [ ] Write `CHANGELOG.md` with an Unreleased section and the history since the version 2 codec. Done when: every milestone gate's commit appends to Unreleased.
- [ ] Commit: `"release: one version source and a changelog"`

**Test checkpoint:** a seeded version mismatch fails the drift check; `rasterfold --version` output quoted.

## 2. The Release Package

**Job:** A user can download one document package holding everything the release ships, with checksums to verify it.

- [ ] Write `scripts/package.ps1`, which builds from a clean tree and writes `dist/rasterfold-<version>-win-x64.zip` with the executable, DLL, header, both import libraries, the docs site, `LICENSE`, `CHANGELOG.md`, and `SHA256SUMS`. Done when: two runs on the same commit produce byte-identical zips. Cheaper substitute: zipping `bin/` by hand.
- [ ] Commit: `"release: a reproducible release package"`

**Test checkpoint:** the two runs' zip hashes quoted equal; the package's checksum file verifies.

## 3. The Release Workflow

**Job:** A user can download the package and checksums the release workflow exports for each tag.

- [ ] Add `.github/workflows/release.yml`: on a `rasterfold-v*` tag, run every gate, build the package, and create the GitHub release with the zip and its checksums attached, exporting the release notes from `CHANGELOG.md`. Done when: a dry run on a scratch tag in a fork or a pre-release tag completes. Cheaper substitute: uploading the zip by hand.
- [ ] Commit: `"release: build and publish releases from tags"`

**Test checkpoint:** the dry run's workflow URL and its attached assets quoted.

## 4. Release rasterfold-v1.0.0

Everything milestone 9 confirmed, frozen and shipped: format version 3 is final from this tag (the pre-release rule in `todo/README.md` ends here).

- [ ] Run `python tests/fuzz_decode.py` over 100,000 mutations across every method and input format, and the full gates. Done when: no crash, hang, or mismatch, and `pwsh scripts/check-all.ps1` is green.
- [ ] Move `CHANGELOG.md`'s Unreleased section to 1.0.0 and mark format version 3 final in `spec/FOLD.md`. Done when: `scripts/drift-check.py` exits 0.
- [ ] Run the backlog review for `rasterfold-v1.0.0`: promote each backlog entry into a section or record the operator's deferral on it. Done when: every entry carries a `reviewed: rasterfold-v1.0.0` field or has been promoted.
- [ ] Push the tag `rasterfold-v1.0.0`. Done when: the release workflow publishes the package and its checksums.
- [ ] Commit: `"release: RasterFold 1.0.0"`

**Test checkpoint:** the fuzz summary, the green release workflow URL, and the published asset list quoted.

**Freeze check:** from this tag, format version 3 is frozen: `KAT_FOLD3` is recorded in the twenty-year compatibility corpus, and any later change to what version 3 writes needs a new version.

## Verification

- [ ] The release page lists the package and `SHA256SUMS`, and the checksums verify
- [ ] `python scripts/todo-graph.py validate` clean
