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

> **Goal:** `rasterfold-v1.0.0` ships as a signed-off, reproducible package: Windows/Linux x64 CLI packages, shared/static libraries, headers, applicable symbols and import libraries, complete HTML docs, notices and checksums, built by CI from the tag, with the format frozen at version 3 and the backlog reviewed.

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
- `dist/` contains versioned two-target CLI/SDK packages with headers, symbols, notices, changelog and `SHA256SUMS`, produced deterministically from the tag.
- The tag `rasterfold-v1.0.0` is pushed, the release published, and every backlog entry reviewed for it.

**Adjacency:** list=not-applicable (no records to browse); document=applicable @ D09 T01 §2; settings=not-applicable (no option); reporting=applicable @ D09 T01 §1; notifications=not-applicable (a release note, no messages); permissions=not-applicable (no refusal path); audit=applicable @ D09 T01 §1; exchange=applicable @ D09 T01 §3; reverse=not-applicable (a published release is superseded, not undone)

**Adjacency rationale:** The package is the document users download; `--version` reports what they run; the changelog is the audit history; the release workflow exports the package.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | One version source and the changelog | -- |  [x]   |
| 2 | §2 | The two-target release package | §1, D06 T01 §2 | [ ] |
|   3   |   §3    | The release workflow | §2 |  [ ]   |
| 4 | §4 | Release rasterfold-v1.0.0 | §3, D05 T01 §9, D08 T01 §7 | [ ] |

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

> **Verified:** 2026-11-09 | §1 | eb1881b8a8dd3a154a188e4b41d7718ea9386964
> **Review:** `docs/reviews/d09-t01-s1.json`
## 2. The Release Package

> **Started:** 2026-11-12T05:32:00Z

**Job:** A user can download one document package holding everything the release ships, with checksums to verify it.

**Corrected 2026-11-12 (validation at Started):** the item leaves five things open; they are decided here so the build invents nothing. Today `scripts/package.py` (96 lines) writes one development zip per target to `build/packages/rasterfold-<target>-<version>-dev+<revision>.zip` from `bin/<target>/build.json`, with `LICENSE`, `docs/building.md` and `docs/library.md`, a fixed 2026-01-01 timestamp and a sidecar `.sha256`; `build.yml` uses it and keeps doing so unchanged.

1. **Release mode.** `python3 scripts/package.py --release [--target <target>|all]` writes `dist/rasterfold-<version>-<target>.zip` (one self-contained zip per target, top directory `rasterfold-<version>-<target>/`) and `dist/SHA256SUMS` over the zips. `dist/` is added to `.gitignore`. The Job line's "one package" is read as one package per platform, as the Outcome states ("two-target CLI/SDK packages"): a Windows user does not download Linux binaries.
2. **A clean source revision** means all of: `git status --porcelain` prints nothing, the manifest's `revision` equals `git rev-parse --short=12 HEAD`, the manifest says `dirty: false`, and its `product_version` equals `VERSION`. Any other state is refused with the reason, before anything is written.
3. **Contents**, in sorted order: every artifact of `target_artifacts(<target>)` at its path (`bin/`, `lib/`, `include/`, and on Windows `symbols/` and the import libraries), `build.json`, `LICENSE`, `CHANGELOG.md`, `docs/` holding the public documentation set exactly as `scripts/build-docs.py --stage` selects it (the allowlisted pages, generated references, licence and provenance page, and assets), and `SHA256SUMS` at the top directory listing every other file in `sha256sum -c` form. No host has Jekyll outside CI, so the offline documentation is that staged set; when `--site-dir <directory>` names a built HTML site (the release workflow's Jekyll output, `§3`), it is added as `docs/html/`. Third-party notices: the package ships no third-party binary, and `docs/licenses.md` (in the staged set) is the provenance page; `testdata/` and its notice are not shipped.
4. **Determinism.** Entries are written in sorted name order with one timestamp, the committer date of HEAD in UTC (`git show -s --format=%ct HEAD`), fixed modes (0755 for `bin/rasterfold`, 0644 otherwise), deflate level 9, and no extra fields, so two runs on the same commit and the same build output are byte-identical. Whether two fresh builds of one commit are byte-identical is measured and recorded here, not promised: reproducible builds are Milestone 45's contract.
5. **Refusals** carried over from the development mode (tampered artifact, missing artifact, unsafe manifest path, stale version) apply unchanged, plus: a symbolic link or a path escaping its root under `--site-dir`, a `--site-dir` without `index.html`, and an existing `dist/` file that is not a package or checksum file of this version.

**Corrected 2026-11-12 (plan review `docs/reviews/09-release/D09-T01-s2.md`):** one P1 and two P2 gaps folded in; where this paragraph and the five points above disagree, this paragraph governs. (1) Offline documentation. The staged site files are not usable offline (their links are rewritten to the hosted `/RasterFold/*.html` paths), and the repository's own pages are not a closed set either (27 relative links leave the published pages, and `docs/benchmarks.md` carries 14 template includes). The package therefore ships an offline rendering made by the documentation generator itself: `scripts/build-docs.py` gains `render_offline_docs(root, revision)`, which takes every published page of `docs/site-content.json`, removes the Jekyll front matter, inlines each generated table and replaces each chart include with an image link to the chart file shipped beside the page, and rewrites every relative link: to the packaged copy when the target is a published page, an asset, or a file the package ships (`LICENSE`, `CHANGELOG.md`, `include/rasterfold.h`), otherwise to that path at the packaged commit on GitHub (`https://github.com/rizonesoft/RasterFold/blob/<full revision>/<path>`, `tree/` for a directory). It fails if any template tag, hosted `/RasterFold/` path, or relative link without a packaged target remains. The pages stay Markdown, readable in any editor or viewer with working links between them and `docs/index.md` as the entry page; HTML rendering needs Jekyll, which only CI has, so `--site-dir` and `docs/html/` are dropped from this section, and the Goal's HTML documentation is the site published for the tag (`§4`'s Pages read-back; `D08 T01 §7` owns versioned downloads). The checkpoint extracts a package and verifies, with no network, that every relative link in `docs/` resolves inside the extracted tree and that every published page is reachable from `docs/index.md`. (2) Release tests. `tests/test_package.py` gains release-mode cases of its own: the exact file list per target, the executable mode bit, `SHA256SUMS` listing every other file exactly once with the right digest, each refusal of points 2 and 5 (minus the dropped `--site-dir` ones) with `dist/` byte-identical before and after the refused run, the offline-documentation link check on an extracted package, and byte identity of two runs. (3) Publication semantics. Each zip is written to a temporary file in `dist/` and renamed into place only when complete, so an existing package is replaced whole or left untouched. `dist/SHA256SUMS` is recomputed at the end of every run, also a failed one, from the `rasterfold-<version>-<target>.zip` files then present, in sorted order, so successive single-target runs accumulate both entries and a failure in a later target leaves the earlier target's package and a checksum file that matches exactly what is on disk. A `dist/` entry that is not a package, a temporary, or the checksum file of this version is refused before anything is written. A sequential-target case and a failure-preservation case cover this.

**Corrected 2026-11-12 (completion review):** each package also carries a `README.md`, the repository's `PACKAGE.md`: what the extracted directory holds, how to verify it, first use of the tool with the user's own image, and how to link the library, for Linux and Windows, with package-relative paths. The packaged documentation alone described building from a checkout. The offline link check covers the README and requires it to link `docs/index.md`, and a link to `PACKAGE.md` from a packaged page points at the package's `README.md`.

**Corrected 2026-11-12 (round 1):** two things point 3 and the plan-review correction left implicit are now explicit. One release run at a time: a run creates `dist/.release.lock` exclusively and removes it when it ends, a second run is refused while it exists, and every temporary is exclusively created (`<final name>.<random>.tmp`), so no two writers ever share one. And the package's source files (the documentation, `LICENSE`, `CHANGELOG.md`, `VERSION`) are read from the commit itself through `git archive`, never from the working tree, so an edit made after the clean-tree check cannot reach a package; the clean-tree check stays as the early, plain refusal.

Decision recorded (2026-11-12): zip, not tar, on both platforms (one format, one verification command, and the existing development packages are zips). Reversal cost: one writer function.

- [x] Extend `scripts/package.py` to package a clean source revision into versioned target directories with executables, libraries, headers, applicable symbols/import libraries, offline docs, licenses/notices, `CHANGELOG.md` and `SHA256SUMS`. Done when: two runs on the same commit produce byte-identical zips. Cheaper substitute: zipping `bin/` by hand. Done 2026-11-12 at `1eafa71`: `release()` and its helpers in `scripts/package.py`, the offline documentation renderer and its checker in `scripts/build-docs.py` (`render_offline_docs`, `offline_link_report`, `--check-offline`), `dist/` in `.gitignore`, the release paragraph in `docs/building.md`, and the release cases of `tests/test_package.py`; two runs on that commit gave byte-identical packages (quoted in the checkpoint). Measured, not promised: a second fresh build of the same commit reproduced all 5 Linux artifacts and 7 of the 8 Windows artifacts by digest, `bin/rasterfold.dll` differing; byte-identical builds are Milestone 45's contract.
- [x] Commit: `"release: a reproducible release package"`

**Test checkpoint:** Driven run with evidence, 2026-11-12 on the clean commit `19b55d4962f7` with both targets built from it (`build.json` `revision` `19b55d4962f7`, `dirty` `false`): `python3 scripts/package.py --release --target all` printed `dist/rasterfold-1.0.0-windows-x64.zip  bae4c8129fc6560ee235f8a23d80d7a0da73b86261485b9b4040928f10a6a707` and `dist/rasterfold-1.0.0-linux-x64.zip  53aa625775499734a84c9971971bf8fc73f61879b94952c7791b5b3432fd4eca` on the first run and the same two lines on the second, and `cmp` found both zips and `dist/SHA256SUMS` byte-identical across the runs; `(cd dist && sha256sum -c SHA256SUMS)` printed `OK` for both packages. The packages were then extracted outside the checkout and the commands of their `README.md` run from the extracted directory. Linux: `sha256sum -c SHA256SUMS` printed `OK` for all 33 lines; `./bin/rasterfold version` printed `product version: 1.0.0`; `encode photo.png photo.fold`, `inspect` (`method: cm`, `160` by `120`), `verify` (`status: ok`) and `decode photo.fold roundtrip.png` exited 0 with `roundtrip.png` pixel-identical to `photo.png`, and a second `encode` onto the existing file exited 2 with `rasterfold: output file already exists`; `cc -Iinclude app.c lib/librasterfold.a -pthread -o app` and the shared-library line both built, and both programs printed `RasterFold API 0x10000`. Windows (PowerShell): the checksum line reported 36 files and no failure; `.\bin\rasterfold.exe version`, `encode`, `inspect`, `verify` and `decode` exited 0 with the round trip pixel-identical; `clang -DRF_SHARED -Iinclude app.c lib/librasterfold.dll.a -o app.exe` built with the pinned llvm-mingw clang and `app.exe`, with `rasterfold.dll` copied beside it, printed `RasterFold API 0x10000`. The MSVC `cl` line was not run: this host has no MSVC. `python3 scripts/build-docs.py --check-offline <extracted top directory>` printed `build-docs: offline: 16 page(s), 55 relative link(s), 0 unresolved, 0 unreachable from docs/index.md` for each package (the `README.md` links are included). Refusals, run on `8a5a1ed` and unchanged since: an untracked file printed `package: working tree is not clean (1 path(s), first: ?? untracked-release-probe.txt); commit or remove them before a release package`, an altered manifest revision printed `package: linux-x64: the build is of revision '000000000000', HEAD is 8a5a1ed80cf7; rebuild before packaging`, and a present lock file printed `package: another release run holds dist/.release.lock (pid 4242); wait for it, or remove the file if that run is gone`, `dist/` staying byte-identical after all three. `python3 -m unittest discover -s tests -p test_package.py` ran 32 tests `OK`. Earlier candidates of this section gave the same result with their own hashes (`1eafa71`: `1db2e6ad...`, `67e62bd0...`; `8a5a1ed`: `c835cc9a...`, `2e7fb099...`), each identical across two runs.

**Freeze check:** packaging changes no codec byte: `python3 tests/test_correctness.py` stays green with every known-answer file unchanged.

## 3. The Release Workflow

**Job:** A user can download the package and checksums the release workflow exports for each tag.

- [ ] Add `.github/workflows/release.yml`: on a `rasterfold-v*` tag, run every gate, build all target packages, and create the GitHub release with the zip and its checksums attached, exporting the release notes from `CHANGELOG.md`. Done when: a workflow_dispatch dry run on an immutable candidate ref or a prerelease tag matching VERSION completes. Cheaper substitute: uploading the zip by hand.
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
