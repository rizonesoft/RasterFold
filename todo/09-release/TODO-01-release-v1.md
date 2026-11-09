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

> **Goal:** `rasterfold-v1.0.0` ships as a signed-off, reproducible package: Windows/Linux x64 and Android ARM64/x64 CLI and native converter packages, shared/static libraries, headers, applicable symbols and import libraries, complete HTML docs, notices and checksums, built by CI from the tag, with the format frozen at version 3 and the backlog reviewed.

> [!IMPORTANT]
> **Current state (verified 2026-11-09):** API 1.0 and format 3 are independent contracts; product SemVer 1.0.0 lives only in `VERSION` and feeds the CLI report, build manifests, development package names, facts, and the site through `scripts/version.py`, and `CHANGELOG.md` records changes since the version 2 codec. There is no release workflow or release tag yet (§3, §4). Release automation must extend this packager and keep product, format and ABI versions independent.
<!-- claim: exists CHANGELOG.md -->
<!-- claim: exists scripts/package.py -->
<!-- claim: absent .github/workflows/release.yml -->

## Inputs

- [`LICENSE`](../../LICENSE) -- GPL-3.0, shipped in the package
- -> XREF: D06 T01 §1 -- the API version remains a separate compatibility contract from the product release
- -> XREF: D08 T01 §3 -- the published docs the release links to

- -> XREF: D10 T04 §1 -- shared implementation or verification boundary.

## Outcome

- `rasterfold --version` reports product 1.0.0 and format versions; `rf_version()` keeps reporting the independent packed API version.
- `dist/` contains versioned four-target CLI/SDK and separate converter packages with headers, symbols, notices, changelog and `SHA256SUMS`, produced deterministically from the tag.
- The tag `rasterfold-v1.0.0` is pushed, the release published, and every backlog entry reviewed for it.

**Adjacency:** list=not-applicable (no records to browse); document=applicable @ D09 T01 §2; settings=not-applicable (no option); reporting=applicable @ D09 T01 §1; notifications=not-applicable (a release note, no messages); permissions=not-applicable (no refusal path); audit=applicable @ D09 T01 §1; exchange=applicable @ D09 T01 §3; reverse=not-applicable (a published release is superseded, not undone)

**Adjacency rationale:** The package is the document users download; `--version` reports what they run; the changelog is the audit history; the release workflow exports the package.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | One version source and the changelog | -- |  [ ]   |
| 2 | §2 | The four-target release package | §1, D06 T01 §2, D10 T04 §3 | [ ] |
|   3   |   §3    | The release workflow | §2 |  [ ]   |
| 4 | §4 | Release rasterfold-v1.0.0 | §3, D05 T01 §9, D10 T04 §4 | [ ] |

---

## 1. One Version Source and the Changelog

> **Started:** 2026-11-09T05:26:29Z

**Job:** A user can read a report of the version they run, and a history of what changed in each release.

- [x] Define product SemVer once in `VERSION`, feeding a generated codec-version header, `--version`, `docs/facts.json`, package names and the site; preserve the independent `rf_version()` ABI contract. Done when: `scripts/drift-check.py` fails when any of them disagree. Cheaper substitute: the version typed in four places.
- [x] Add `scripts/version.py` to derive development build metadata from the source revision and require release tags to match `VERSION`. Done when: dirty builds are labeled, contradictory tags fail, SemVer prerelease tags require the matching prerelease VERSION, and no push invents a semantic version increment.
**Corrected 2026-11-09 (plan review):** Four gaps are folded in before building. (1) Product identity is recorded at build time: `scripts/build.py` writes the product version, source revision, and dirty flag into each target's `build.json`, and `scripts/package.py` refuses to package a build whose recorded product version disagrees with `VERSION` (a stale build), with a refusal fixture; the archive is named from the manifest's identity, not from a live guess. (2) The development identity's consumers are specified: the build manifest and the development package name `rasterfold-<target>-<version>-dev+<revision>[.dirty].zip` derived from that manifest; `scripts/version.py --dev` computes the same label from a checkout. (3) `tests/test_version.py` is registered in `scripts/check-all.py`'s test list, and the checkpoint below names the full matrix. The exact tag grammar is pinned here for §3's workflow: `rasterfold-v<MAJOR>.<MINOR>.<PATCH>` with an optional `-<prerelease>` that must equal `VERSION`'s prerelease; SemVer build metadata (`+...`) in a tag is refused, and no command invents an increment. (4) The changelog obligation is wired at its source: the milestone gate checklist template in `research/milestones.py` directs each gate commit to append its summary to `CHANGELOG.md`'s Unreleased section, and the generated milestone files are regenerated; completion here means that wiring exists, not that future commits are proven.

- [x] Add version/ref mismatch fixtures in `tests/test_version.py`. Done when: packages, CLI and site agree while changing product SemVer does not change the ABI or file-format version.
- [x] Write `CHANGELOG.md` with an Unreleased section and the history since the version 2 codec. Done when: every milestone gate's commit appends to Unreleased.
- [x] Commit: `"release: one version source and a changelog"`

**Test checkpoint:** `tests/test_version.py` passes (a seeded header mismatch fails, header regeneration from a prerelease VERSION, the tag matrix including prerelease and build-metadata refusals, clean/dirty/unknown dev labels, and CLI plus build-manifest agreement); a seeded `docs/facts.json` product mismatch fails through drift's generated-facts comparison; `rasterfold --version` prints `product version: 1.0.0` beside format written 3 and API 1.0; changing a fixture manifest's product version makes packaging refuse the stale build; `python3 scripts/drift-check.py` exits 0 (2026-11-09).

## 2. The Release Package

**Job:** A user can download one document package holding everything the release ships, with checksums to verify it.

- [ ] Extend `scripts/package.py` to package a clean source revision into versioned target directories with executables, libraries, headers, applicable symbols/import libraries, offline docs, licenses/notices, `CHANGELOG.md` and `SHA256SUMS`. Done when: two runs on the same commit produce byte-identical zips. Cheaper substitute: zipping `bin/` by hand.
- [ ] Commit: `"release: a reproducible release package"`

**Test checkpoint:** the two runs' zip hashes quoted equal; the package's checksum file verifies.

## 3. The Release Workflow

**Job:** A user can download the package and checksums the release workflow exports for each tag.

- [ ] Add `.github/workflows/release.yml`: on a `rasterfold-v*` tag, run every gate including recorded exact-candidate Android native-device evidence and the already implemented D10 T04 §1 to §3 gates, build all target packages, and create the GitHub release with the zip and its checksums attached, exporting the release notes from `CHANGELOG.md`. Done when: a workflow_dispatch dry run on an immutable candidate ref or a prerelease tag matching VERSION completes without requiring the later D10 T04 §4 whole-product signoff. Cheaper substitute: uploading the zip by hand.
- [ ] Commit: `"release: build and publish releases from tags"`

**Test checkpoint:** the dry run's workflow URL and its attached assets quoted.

## 4. Release rasterfold-v1.0.0

Everything milestone 9 confirmed, frozen and shipped: format version 3 is final from this tag (the pre-release rule in `todo/README.md` ends here).

- [ ] Run `python tests/fuzz_decode.py` over 100,000 mutations across every method and input format, and the full gates. Done when: no crash, hang, or mismatch, and `pwsh scripts/check-all.ps1` is green.
- [ ] Move `CHANGELOG.md`'s Unreleased section to 1.0.0 and mark format version 3 final in `spec/FOLD.md`. Done when: `scripts/drift-check.py` exits 0.
- [ ] Run the backlog review for `rasterfold-v1.0.0`: promote each backlog entry into a section or record the operator's deferral on it. Done when: every entry carries a `reviewed: rasterfold-v1.0.0` field or has been promoted.
- [ ] Push the tag `rasterfold-v1.0.0`. Done when: the release workflow publishes the package and its checksums.
- [ ] Publish and read back the Pages artifact for the final tag through `.github/workflows/pages.yml`, dispatched from allowed `main` with the immutable tag commit as its source. Done when: the deployed receipt, downloads, CLI/API references and product/format/ABI labels match the released commit and assets.
- [ ] Commit: `"release: RasterFold 1.0.0"`

**Test checkpoint:** the fuzz summary, the green release workflow URL, and the published asset list quoted.

**Freeze check:** from this tag, format version 3 is frozen: `KAT_FOLD3` is recorded in the twenty-year compatibility corpus, and any later change to what version 3 writes needs a new version.

## Verification

- [ ] The release page lists the package and `SHA256SUMS`, and the checksums verify
- [ ] `python scripts/todo-graph.py validate` clean
