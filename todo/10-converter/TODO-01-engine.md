---
schema_version: 1
id: converter-engine
domain: 10-converter
status: draft
title: "TODO-01 -- Native Converter Engine"
depends_on: []
frozen: true
track: GUI1
---

# TODO-01 -- Native Converter Engine

> **Goal:** A bounded C conversion engine preserves source files and verifies exact output through a versioned adapter contract.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the four-target build exist. There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native C application with bundled codec source and no additional user installs.
<!-- claim: exists include/rasterfold.h -->
<!-- claim: exists converter/include/rfc_codec.h -->

## Inputs

- [Product vision](../../docs/vision.md) and [native design](../../docs/design/converter.md): scope, exactness, platform and interaction contracts.
- [C API](../../include/rasterfold.h): RasterFold ownership and error boundary.

- -> XREF: D10 T02 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T04 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T03 §1 -- shared implementation or verification boundary.

- -> XREF: D06 T01 §1 -- shared implementation or verification boundary.

- -> XREF: D00 T04 §1 -- shared implementation or verification boundary.

## Outcome

- A bounded C conversion engine preserves source files and verifies exact output through a versioned adapter contract.
- Clean Windows/Linux x64 and Android ARM64/x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T01 §1; document=applicable @ D10 T03 §5; settings=applicable @ D10 T01 §3; reporting=applicable @ D10 T01 §6; notifications=applicable @ D10 T01 §3; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T01 §6; exchange=applicable @ D10 T01 §5; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Pixel contract and capability registry | D06 T01 §1 | [ ] |
| 2 | §2 | Pinned bundled codec builds | §1, D00 T04 §1 | [ ] |
| 3 | §3 | Bounded asynchronous jobs | §1, §2 | [ ] |
| 4 | §4 | Exactness verification and differential fixtures | §1, §3 | [ ] |
| 5 | §5 | Safe import and export | §3, §4 | [ ] |
| 6 | §6 | Comparable measurements and reports | §3, §4 | [ ] |

---

## 1. Pixel contract and capability registry

> **Started:** 2026-11-09T06:57:03Z

**Job:** A developer can list supported formats and choose settings from a versioned capability registry before conversion allocates or writes anything.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [x] Define owned pixel/frame/metadata types in `converter/include/rfc_image.h`. Done when: stride, byte order, sample depth, alpha mode, orientation, ICC and frame duration have explicit invariants.
- [x] Define adapter lifecycle and result codes in `converter/include/rfc_codec.h`. Done when: C callers can enumerate, probe, encode, decode, cancel and free through one versioned ABI.
- [x] Write `converter/codecs.json` from the format matrix in `docs/vision.md`. Done when: every required format names a provider, native sample support, read/write modes, an availability state (`planned` visible with a refusal reason and not selectable for execution, or `usable` only when its provider section has shipped), a per-capability owning section (APNG with the PNG family in D10 T02 §1, animation and multi-page extensions in D10 T02 §12), and its settings: identifiers, types, defaults, allowed values, combination constraints and refusal reasons, with no setting able to silently reduce precision, alpha, metadata or frames (`docs/vision.md`).
- [x] Document exactness classes in `docs/converter-exactness.md`. Done when: pixel equality, metadata preservation, coefficient optimization and original-byte reconstruction cannot share an ambiguous lossless label.
- [x] Implement the registry in `converter/src/registry.c` over the `rfc_codec.h` ABI. Done when: C callers can enumerate formats, query capabilities and settings, validate a setting combination with a named refusal reason, and register an adapter lifecycle (probe, encode, decode, cancel, free) through the versioned ABI; planned providers cannot be selected for execution.
- [x] Add `tests/converter/test_contract.c`, compiled and linked against `converter/src/registry.c` and executed by the checkpoint. Done when: a valid registry query succeeds, and invalid strides, overflow, opaque ownership and unsupported capability combinations fail before allocation, proven by a counting allocator that records zero allocations on every refusal path.
- [x] Add a registry validator to `scripts/check-converter.py`. Done when: a missing format, missing owner or advertised unsupported mode fails with the exact row.
- [x] Commit: `"converter: pixel contract and capability registry"`

**Test checkpoint:** Build the C header as C11 and C++; run `python3 scripts/check-converter.py --case contract` with overflow and unsupported-mode fixtures.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 2. Pinned bundled codec builds

**Requires:** windows-runtime, linux-runtime, android-arm64-runtime, android-x64-runtime -- Native loader smoke tests require each target runtime; source checkpoint audit in docs/todo-vision-audit.md.

**Job:** A maintainer can build every bundled codec from verified source with no downloaded runtime on the target.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Add `converter/vendor.json` with exact upstream commits, archive hashes, licenses and enabled features. Done when: every transitive source has an origin and checksum; moving branch names are refused.
- [ ] Extend `tools/bootstrap.py` with converter-only source staging. Done when: bad hashes and archive traversal are refused and codec dependencies never enter RasterFold-only targets.
- [ ] Add `scripts/build-converter.py` over local LLVM/Ninja. Done when: all four target graphs compile application files as C11 and isolate vendored C++ behind C interfaces.
- [ ] Generate `converter/THIRD-PARTY-NOTICES.txt` and an SBOM from the source manifest. Done when: each linked provider and transitive dependency appears with license text and source retrieval instructions.
- [ ] Add import/dependency checks to `tests/converter/test_bundle.py`. Done when: clean target smoke tests load no unshipped non-system DLL or shared object; no external CLI is required.
- [ ] Document Linux display/font and Android SDK/NDK packaging choices in `docs/converter-building.md`. Done when: OS prerequisites and bundled support libraries are distinguished from build-time tools.
- [ ] Pin each vendor-required build tool in `toolchain.json` and its local bootstrap path. Done when: CMake, Meson, NASM or other required tooling is checksum-verified where used, and SIMD/assembly build features are recorded for fair comparisons.
- [ ] Register generated notices/SBOM and their check commands in `docs/drift.json`. Done when: source-pin changes invalidate generated licensing output; ignored upstream source caches remain excluded, and any tracked upstream documentation receives a provenance-only policy before admission.
- [ ] Commit: `"converter: pinned bundled codec builds"`

**Test checkpoint:** Run `python3 scripts/build-converter.py --target all` and the bad-download/import fixtures; quote runtime results from clean target installations.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 3. Bounded asynchronous jobs

**Job:** A user can configure resource settings, receive progress notifications, cancel and retry asynchronous conversion without freezing the interface.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement job states and immutable job IDs in `converter/src/job.c`. Done when: import, encode, verify and export transitions reject stale completions.
- [ ] Implement bounded worker and memory budgets in `converter/src/scheduler.c`. Done when: codec threads and job admission obey a combined CPU budget and conservative memory reservations; allocator hooks enforce limits where supported, and provider limitations are named instead of claiming an unobservable hard allocation cap.
- [ ] Add cancellation points in `converter/src/job.c`. Done when: cancellation frees partial buffers and never publishes output.
- [ ] Implement size and decompression limits in `converter/src/limits.c`. Done when: hostile dimensions, frame counts and metadata sizes fail before excessive allocation.
- [ ] Add `tests/converter/test_jobs.c`. Done when: rapid restart, out-of-memory, stale callback and cancellation races have deterministic assertions.
- [ ] Commit: `"converter: bounded asynchronous jobs"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case jobs`; quote peak allocation and verify no callback publishes a cancelled job.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 4. Exactness verification and differential fixtures

**Job:** A user can prove that decoding reverses an encode exactly, independently of a color-managed preview.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement sample/frame comparison in `converter/src/verify.c`. Done when: dimensions, depth, layout, transparent RGB, frame timing and every native sample participate in equality.
- [ ] Implement metadata policy comparison in `converter/src/metadata.c`. Done when: preserve, explicitly strip and unsupported are distinct outcomes; a default conversion cannot silently drop data.
- [ ] Add independent reference decoding to `tests/converter/reference.py`. Done when: tests identify each decoder implementation and its shared dependencies; independent differential proof is distinguished from same-provider round trips, with explicit JPEG coefficient, OpenEXR bit-pattern and LibRaw rendering contracts.
- [ ] Keep preview color transforms outside verification in `converter/src/preview.c`. Done when: display conversion cannot influence a lossless verdict.
- [ ] Add `tests/converter/test_exactness.py`. Done when: one changed sample, hidden transparent RGB, changed frame duration or lost ICC fails the appropriate claim.
- [ ] Freshly decode the produced file in `converter/src/verify.c` using a new decoder instance. Done when: runtime success requires the agreed sample/metadata comparison and never claims a second independent implementation.
- [ ] Commit: `"converter: exactness verification with explicit decoder provenance"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case exactness`; quote seeded mismatch failures and same-sample passes. Cheaper substitute rejected: visual similarity or quality=100.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 5. Safe import and export

**Requires:** windows-runtime, linux-runtime -- The file checkpoint drives Windows and Linux file-provider refusal cases. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can export a document to a chosen destination under a safe permissions policy, and deliver a verified file without damaging an existing one.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement file sniffing and bounded reads in `converter/src/input.c`. Done when: content and extension disagreement is reported; a filename never selects an unsafe decoder blindly.
- [ ] Implement exclusive staged publication in `converter/src/output.c`. Done when: an existing destination, input alias, symlink alias or hard-link alias is refused by default.
- [ ] Implement disk-full and cancellation cleanup in `converter/src/output.c`. Done when: only the job-owned temporary file is removed and no partial output is called successful.
- [ ] Define portable input/output callback ownership in `converter/include/rfc_io.h`. Done when: the engine accepts bounded read, seek and staged-publication callbacks without requiring an Android activity; the concrete SAF bridge belongs to D10 T03 §4.
- [ ] Add `tests/converter/test_file_safety.py`. Done when: existing targets, same-file inputs, truncated streams, permission denial and disk exhaustion preserve before/after hashes.
- [ ] Commit: `"converter: safe import and export"`

**Test checkpoint:** Run the file-safety fixtures on POSIX and Windows and drive native Windows/POSIX destination refusals; Android SAF proof belongs to D10 T03 §4; capture before/after hashes.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 6. Comparable measurements and reports

**Job:** A user can read comparison reports and an auditable per-run history of sizes, timings, parameters and verification.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement measurements in `converter/src/metrics.c`. Done when: reported bytes come from produced files; encode, decode/verify and wall time are separate.
- [ ] Implement sequential comparison scheduling in `converter/src/compare.c`. Done when: codecs use the same source, thread budget, preservation policy and warm-up protocol.
- [ ] Write a versioned result report in `converter/src/report.c`. Done when: source hash, codec version, parameters, exactness, byte count and measurement conditions are exported as JSON.
- [ ] Add growth and unsupported states to `converter/src/compare.c`. Done when: a larger file is reported as growth and unsupported codecs do not rank as zero-byte winners.
- [ ] Add `tests/converter/test_reports.py`. Done when: seeded inconsistent byte counts, missing provenance and failed verification cannot enter a successful comparison.
- [ ] Include compiler, provider build flags, enabled SIMD/assembly and threading features in `converter/src/report.c`. Done when: reports cannot compare differently configured providers without showing the configurations.
- [ ] Commit: `"converter: comparable measurements and reports"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case reports` and drive a comparison using the same licensed sample at one and default threads.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## Verification

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report native device coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
