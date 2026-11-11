---
schema_version: 1
id: converter-delivery
domain: 10-converter
status: draft
title: "TODO-04 -- Converter Validation and Delivery"
depends_on: []
frozen: true
track: GUI4
---

# TODO-04 -- Converter Validation and Delivery

> **Goal:** The complete native converter ships as tested, documented, self-contained packages with an exact capability and device matrix.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the Windows/Linux x64 build exist (the Android targets were removed on 2026-11-11). There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native C application with bundled codec source and no additional user installs.
<!-- claim: exists include/rasterfold.h -->
<!-- claim: exists converter/include/rfc_codec.h -->

## Inputs

- [Product vision](../../docs/vision.md) and [native design](../../docs/design/converter.md): scope, exactness, platform and interaction contracts.
- [C API](../../include/rasterfold.h): RasterFold ownership and error boundary.

- -> XREF: D10 T01 §4 -- shared implementation or verification boundary.

- -> XREF: D10 T03 §8 -- shared implementation or verification boundary.

- -> XREF: D10 T02 §12 -- shared implementation or verification boundary.

- -> XREF: D09 T01 §3 -- shared implementation or verification boundary.

- -> XREF: D08 T01 §8 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §5 -- safe delivery of verified output documents.


## Outcome

- The complete native converter ships as tested, documented, self-contained packages with an exact capability and device matrix.
- Clean Windows/Linux x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T01 §1; document=applicable @ D10 T04 §3; settings=applicable @ D10 T03 §7; reporting=applicable @ D10 T04 §1; notifications=applicable @ D10 T03 §5; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T04 §4; exchange=applicable @ D10 T04 §3; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Full capability and device matrix | D10 T02 §1, D10 T02 §2, D10 T02 §3, D10 T02 §4, D10 T02 §5, D10 T02 §6, D10 T02 §7, D10 T02 §8, D10 T02 §9, D10 T02 §10, D10 T02 §11, D10 T02 §12, D10 T03 §8 | [ ] |
| 2 | §2 | Hostile input and resource limits | §1 | [ ] |
| 3 | §3 | Self-contained native packages | §1, §2, D09 T01 §1 | [ ] |
| 4 | §4 | Whole-product release acceptance | §3, D08 T01 §8, D09 T01 §3 | [ ] |

---

## 1. Full capability and device matrix

**Requires:** display-session, linux-display -- The complete capability matrix needs driven evidence from both native targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A maintainer can read reports proving every promised format and interaction on each supported target.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Build a licensed conformance corpus manifest in `tests/converter/corpus.json`. Done when: each format includes ordinary, boundary, metadata, transparency, high-depth and malformed cases that apply to it.
- [ ] Add the native device matrix to `.github/workflows/converter.yml`. Done when: Windows and Linux runtime tests produce separate named results.
- [ ] Add capability completeness checks to `scripts/check-converter.py`. Done when: a required provider missing from one target is a failure, never silently omitted.
- [ ] Record exact OS/runtime evidence in `docs/converter-validation.md`. Done when: every supported platform has a clean-install, UI, file-safety and fresh decoding result and separately identified differential fixtures.
- [ ] Commit: `"converter: full capability and device matrix"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --all-targets` and quote native results for both targets; an absent native runtime remains a release blocker.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 2. Hostile input and resource limits

**Requires:** display-session, linux-display -- Native safety, installation and final product evidence are required on every advertised target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can open untrusted images without unbounded resource use or damaged files.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Add bounded fuzz harnesses in `tests/converter/fuzz_adapters.c`. Done when: every decoder and metadata/frame parser is covered with retained seeds.
- [ ] Run sanitizer and timeout cases in `scripts/check-converter.py`. Done when: truncated, huge, recursive metadata and excessive-frame cases fail cleanly.
- [ ] Add dependency/source advisory review to `converter/vendor.json` maintenance. Done when: each provider has an update owner and the release records reviewed upstream security notices.
- [ ] Add `tests/converter/test_limits.py`. Done when: configured memory/thread/input limits and cancellation leave source and destination hashes intact.
- [ ] Commit: `"converter: hostile input and resource limits"`

**Test checkpoint:** Quote seed, execution counts, sanitizer results, time/memory limits and retained crash reproductions; unresolved crashes block release.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 3. Self-contained native packages

**Requires:** display-session, linux-display -- Native safety, installation and final product evidence are required on every advertised target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can download and install a versioned application document package with all required runtime assets.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Extend `scripts/package.py` with separate converter packages. Done when: CLI/library SDK packages keep their established bin/lib/include/symbols layout and do not acquire GUI dependencies.
- [ ] Package Windows and Linux application assets and non-system libraries. Done when: a clean offline target launches, opens samples, converts and exports without a missing import.
- [ ] Attach notices, SBOM, checksums and build-source metadata. Done when: users can identify the version and retrieve corresponding bundled sources.
- [ ] Add `tests/converter/test_packages.py`. Done when: archive traversal, missing license/provider, mismatched version and missing runtime assets fail.
- [ ] Commit: `"converter: self-contained native packages"`

**Test checkpoint:** Install and drive each package on a clean target, check import inventories and verify all checksums; signing credentials remain outside source/logs.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 4. Whole-product release acceptance

**Requires:** display-session, linux-display -- Native safety, installation and final product evidence are required on every advertised target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can audit release history and obtain documented, verified native apps and codec SDKs from one coherent release.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Write `docs/release-checklist.md` from the vision matrix. Done when: converter formats, native UI, documentation, SDKs, no-extra-install checks and runtime evidence each name a concrete gate.
- [ ] Run the prerelease package and Pages readback in `scripts/check-converter.py`. Done when: downloaded artifacts match checksums and the site identifies the same source/release.
- [ ] Check current versus planned claims in `docs/converter-formats.md`. Done when: every advertised capability is demonstrated on every advertised target or blocks acceptance.
- [ ] Record release acceptance in `docs/converter-validation.md`. Done when: all required evidence names the exact candidate and no fixture is relabeled as a native run.
- [ ] Commit: `"converter: whole-product release acceptance"`

**Test checkpoint:** Use a prerelease candidate to drive install, sample, conversion, comparison and exported-file readback on all targets and inspect the matching deployed documentation.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## Verification

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report native device coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
