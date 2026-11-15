---
schema_version: 1
id: converter-engine
domain: 10-converter
status: active
title: "TODO-01 -- Native Converter Engine"
depends_on: []
frozen: true
track: GUI1
---

# TODO-01 -- Native Converter Engine

> **Goal:** A bounded C conversion engine preserves source files and verifies exact output through a versioned adapter contract.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the Windows/Linux x64 codec build exist. The converter itself targets Windows x64 only (operator decisions 2026-11-11: Android removed, Linux interface dropped; either can return as a fork). There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native C application with bundled codec source and no additional user installs.
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
- Clean Windows x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T01 §1; document=applicable @ D10 T03 §5; settings=applicable @ D10 T01 §3; reporting=applicable @ D10 T01 §6; notifications=applicable @ D10 T01 §3; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T01 §6; exchange=applicable @ D10 T01 §5; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Pixel contract and capability registry | D06 T01 §1 | [x] |
| 2 | §2 | Pinned bundled codec builds | §1, D00 T04 §1 | [x] |
| 3 | §3 | Bounded asynchronous jobs | §1, §2 | [x] |
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

> **Verified:** 2026-11-09 | §1 | c059106524b5bed845c2b48098f1841246f920f2
> **Review:** `docs/reviews/d10-t01-s1.json`
## 2. Pinned bundled codec builds

> **Started:** 2026-11-15T01:32:14Z

**Requires:** windows-runtime -- Native loader smoke tests require each target runtime; source checkpoint audit in docs/todo-vision-audit.md.

**Job:** A maintainer can build every bundled codec from verified source with no downloaded runtime on the target.

**Clarified 2026-11-15:** Target scope follows the 2026-11-11 operator decisions (`AGENTS.md`, `docs/vision.md`, `docs/codec-distribution.md`) where the converter ships only for `windows-x64`, so `python3 scripts/build-converter.py --target all` builds every advertised converter target (`windows-x64`) while leaving `scripts/build.py` and RasterFold-only artifacts untouched. Build tools in `toolchain.json` add `cmake` (`4.5.0`) and `nasm` (`3.02`, `nasm-3.02-win64.zip` on Windows and `nasm-3.02.tar.xz` built locally into `tools/nasm-linux` on Linux x64) beside `llvm-mingw` and `ninja`, both marked `"scope": "converter"` so ordinary `tools/bootstrap.*` and `tools/provision.*` runs stay isolated while `--converter` / `-Converter` installs and verifies converter tooling (`libjpeg-turbo` and `libaom` SIMD assembly; Meson is unused and omitted because every bundled library builds with CMake and Ninja). Source staging in `tools/bootstrap.py --converter` and `tools/bootstrap.ps1 -Converter` populates gitignored `tools/converter-src/` only on converter requests, verifies byte counts, SHA-256 digests, archive traversal, and symlink safety, refuses moving branch refs, and configures CMake with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`. `converter/vendor.json` pins every direct and transitive library across D10 T02 (`zlib` `1.4`, `libpng` `1.6.59` with `libpng-apng` patch `1.6.59`, `libwebp` `1.6.0`, `highway` `1.4.0`, `brotli` `1.2.1`, `lcms2` `2.20`, `libjxl` `0.13.0`, `libaom` `3.15.3` AV1 encode/decode, `libavif` `1.4.2`, `libjpeg-turbo` `3.2.1`, `libdeflate` `1.26`, `libtiff` `4.7.2`, `giflib` `6.1.3`, `qoi` immutable commit `ffb2d2cb74a1de60819b21b939f7209aa53e91c1`, `openjpeg` `2.5.4`, `imath` `3.2.3`, `openjph` `0.33.0`, `openexr` `3.5.4`, and `libraw` `0.22.3`) plus `heif`/`heic` as system-only Windows Imaging Component codecs with zero HEVC source or binary bundled. Engine, adapter, and `converter/src/bundle_smoke.c` sources compile as C11 (`-std=c11`); C++ libraries compile into static archives isolated behind C headers (`jxl/*.h`, `OpenEXRCore/*.h`, `libraw/libraw.h`) and link statically (`-static-libstdc++ -static-libgcc -Wl,-Bstatic -lwinpthread`) into `bin/converter/windows-x64/rfc_bundle_smoke.exe` with no shipped MinGW runtime DLLs and headroom for the C++23 interface target of D10 T03 §1, while verbatim licenses live under `converter/licenses/` so `converter/THIRD-PARTY-NOTICES.txt` and `converter/sbom.spdx.json` verify offline under `scripts/drift-check.py`.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [x] Add `converter/vendor.json` with exact upstream commits, archive hashes, licenses and enabled features. Done when: every transitive source has an origin and checksum; moving branch names are refused.
- [x] Extend `tools/bootstrap.py`, `tools/bootstrap.ps1`, `tools/provision.py`, `tools/provision.ps1`, and `scripts/toolchain-latest.ps1` with converter-only source staging and `"scope": "converter"` filtering (`--converter` / `-Converter`). Done when: bad hashes, archive traversal, symlink escapes, and moving branch names are refused, ordinary provisioning succeeds without converter-scoped tools installed, converter provisioning installs and verifies converter tools and sources (`tests/test_toolchain.py`), and codec dependencies never enter RasterFold-only targets.
- [x] Pin CMake in `toolchain.json` beside the existing llvm-mingw and Ninja, and add `converter/CMakeLists.txt` and `converter/src/bundle_smoke.c` with `scripts/build-converter.py` as its driver. Done when: the Windows x64 target compiles the engine, adapters, and smoke harness as C11 (`-std=c11`), links `bin/converter/windows-x64/rfc_bundle_smoke.exe` statically against every bundled provider and `rasterfold` while isolating vendored C++ behind C interfaces and leaving room for the C++23 interface target of D10 T03 §1, and the codec targets in `scripts/build.py` are unchanged.
- [x] Generate `converter/THIRD-PARTY-NOTICES.txt` and an SBOM from the source manifest. Done when: each linked provider and transitive dependency appears with license text and source retrieval instructions.
- [x] Add import/dependency checks and negative fixtures to `tests/converter/test_bundle.py` and `scripts/check-converter.py --case bundle`. Done when: `rfc_bundle_smoke.exe` exercises representative operations from every bundled provider (failing on any missing provider), PE import inspection proves zero unshipped non-system DLLs (including MinGW runtime DLLs), clean-directory Windows execution with restricted `PATH` succeeds with no external CLI required, and bad-download/import/HEVC fixtures fail loudly.
- [x] Document the Windows packaging choices in `docs/converter-building.md`. Done when: OS prerequisites and bundled support libraries are distinguished from build-time tools.
- [x] Pin each vendor-required build tool in `toolchain.json` and wire `"scope": "converter"` across local bootstrap and provisioning consumers (`tools/bootstrap.py`, `tools/bootstrap.ps1`, `tools/provision.py`, `tools/provision.ps1`, `scripts/toolchain-latest.ps1`). Done when: CMake and NASM (`"scope": "converter"`) are checksum-verified where used, Meson omission is justified because all pinned sources build with CMake and Ninja, and SIMD/assembly build features are recorded for fair comparisons.
- [x] Register generated notices/SBOM and their check commands in `docs/drift.json`. Done when: source-pin changes invalidate generated licensing output; ignored upstream source caches remain excluded, and any tracked upstream documentation receives a provenance-only policy before admission.
- [x] Commit: `"converter: pinned bundled codec builds"`

**Test checkpoint:** Run `python3 scripts/build-converter.py --target all`, `python3 scripts/check-converter.py --case bundle`, `python3 -m unittest discover -s tests -p test_toolchain.py -v`, and the bad-download/import fixtures; quote `rfc_bundle_smoke.exe` provider version/operation output from a clean restricted-`PATH` Windows directory and `tools/provision.*` runs under both default and converter scopes.

**Freeze check:** Existing `.fold` v2 and v3 files, `KAT_FOLD` known-answer bytes, and the oracle (`tests/rfi_ref.py`) remain unchanged (`**Corrected 2026-11-15 (plan review):**` extended explicit `.fold` v2 mention to v2 and v3 per `spec/FOLD.md`). Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

> **Verified:** 2026-11-15 | §2 | 2ab7f9349af6678d595ba00b3d228737e4b1c9d2
> **Review:** `docs/reviews/d10-t01-s2.json`
## 3. Bounded asynchronous jobs

> **Started:** 2026-11-15T05:40:55Z

**Job:** A user can configure resource settings, receive progress notifications, cancel and retry asynchronous conversion without freezing the interface.

**Clarified 2026-11-15:** Public job, scheduler, configuration, resource-budget, and decompression-limit declarations live in `converter/include/rfc_job.h` using C11- and C++17-compatible opaque handles (`rfc_budget`, `rfc_job_slot`, `rfc_job`, `rfc_scheduler`), monotonically unique `rfc_job_id` values (`uint64_t`), and existing `rfc_allocator` from `converter/include/rfc_image.h` without redefinition; private `<stdatomic.h>` and platform synchronization primitives (`SRWLOCK`/`CONDITION_VARIABLE`/`_beginthreadex` on `_WIN32` and `pthread` on POSIX) stay confined to `converter/src/`. `converter/include/rfc_codec.h` (`rfc_status`) and `converter/src/registry.c` (`rfc_status_name`) add `RFC_ERR_CANCELLED` (`14`), `RFC_ERR_LIMIT_EXCEEDED` (`15`), `RFC_ERR_STALE_JOB` (`16`), and `RFC_ERR_BUSY` (`17`). `rfc_query_provider_memory_contract(rfc_format_id, rfc_provider_memory_info *)` distinguishes hook-enforced providers (`libpng`, `libjxl` via `JxlMemoryManager`/zlib adapters) from internal-allocator providers where `rfc_limits` preflight caps and conservative scheduler reservations apply, and feeds `scheduler.c` reservation sizing. `converter/CMakeLists.txt` compiles `src/limits.c`, `src/job.c`, and `src/scheduler.c` into `rfc_engine` and builds `bin/converter/windows-x64/rfc_test_jobs.exe` from `tests/converter/test_jobs.c`; `scripts/check-converter.py` adds `--case jobs` (included in `--case all`), verifying C11/C++17 header compilation, native host execution, and isolated restricted-`PATH` Windows execution with PE import validation under `--all-targets` or Windows hosts.

**Corrected 2026-11-15 (plan review):** Folded the five plan-review findings into explicit contracts and checklist items below: (1) added `rfc_exec_context` and session-scoped `rfc_session_adapter` (preserving ABI v1 `struct rfc_adapter` compatibility) so every invocation receives its admitted CPU threads, `rfc_limits`, and job-owned `rfc_allocator` / hook ctx, while refusing unknown reservations (`RFC_ERR_LIMIT_EXCEEDED`/`RFC_ERR_INVALID_ARGUMENT`); (2) defined deep-copy submission ownership, reference-counted `rfc_job`-owned `rfc_budget`/`rfc_allocator`, per-job same-format sessions, `RFC_ERR_BUSY` unregister refusal while active, and `rfc_scheduler_destroy` drain-and-join teardown guarantees; (3) added `rfc_scheduler_config` defaults/ranges/reconfigure/read-back, serialized non-cancelled progress notifications, structured terminal failure snapshots, and `rfc_scheduler_retry` preserving source/settings under a new `rfc_job_id`; (4) specified the 4-stage `rfc_pipeline_stage_ops` boundary refusing missing required stages loudly with `RFC_ERR_NO_ADAPTER`/`RFC_ERR_PLANNED`; and (5) tightened the checkpoint to `--case jobs --all-targets` with explicit boundary assertions distinguishing host fixtures from isolated Windows execution.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [x] Implement job states, immutable job IDs, and the 4-stage pipeline contract (`rfc_pipeline_stage_ops`) in `converter/src/job.c`. Done when: `IMPORT -> ENCODE -> VERIFY -> EXPORT` transitions reject out-of-order and stale completions (`RFC_ERR_STALE_JOB`), unconfigured required stages fail loudly with `RFC_ERR_NO_ADAPTER`/`RFC_ERR_PLANNED`, and `rfc_job` reference-counts its owned `rfc_budget`/`rfc_allocator` across attached partials until worker teardown finishes.
- [x] Implement bounded worker, CPU, and memory budgets plus adapter execution/ownership in `converter/src/scheduler.c` and `converter/include/rfc_codec.h`. Done when: `rfc_exec_context` delivers admitted `codec_threads`, `rfc_limits`, and job-owned `rfc_allocator`/hook contexts to each invocation (`rfc_session_adapter` isolates concurrent same-format sessions while keeping ABI v1 `rfc_adapter` valid), `scheduler.c` consumes `rfc_query_provider_memory_contract` for reservations, unknown or over-budget reservations are refused before queuing, aggregate `active_codec_threads <= total_cpu_threads` and `active_reserved_bytes <= memory_budget_bytes` hold across concurrent jobs, and `rfc_registry_unregister`/`rfc_registry_unregister_session` return `RFC_ERR_BUSY` while active.
- [x] Add configuration, progress notifications, structured terminal results, and `rfc_scheduler_retry` in `converter/src/scheduler.c` and `converter/src/job.c`. Done when: `rfc_scheduler_config` defaults, range validation, runtime `rfc_scheduler_reconfigure`, and `rfc_scheduler_get_config` effective-value read-back work; monotonic `rfc_progress_fn` callbacks fire outside state locks and stop upon cancel/supersede; terminal `rfc_job_snapshot` reports state, status, failing stage, detail, peak allocations, and reservations; and `rfc_scheduler_retry` preserves source/settings/slot under a new immutable `rfc_job_id` while superseding the prior attempt.
- [x] Add cancellation points and safe scheduler teardown in `converter/src/job.c` and `converter/src/scheduler.c`. Done when: non-blocking `rfc_job_cancel` interrupts queued and active jobs across every stage, frees all partial buffers (`live_partial_bytes == 0`), never invokes publish, and `rfc_scheduler_destroy` cancels queued/active jobs, joins all workers, and suppresses callbacks after teardown begins.
- [x] Implement size and decompression limits in `converter/src/limits.c`. Done when: hostile dimensions, frame counts, metadata sizes, integer overflows, and excessive decompression expansion ratios fail with `RFC_ERR_LIMIT_EXCEEDED` before excessive allocation (`0` allocations recorded).
- [x] Add `tests/converter/test_jobs.c`, `converter/CMakeLists.txt` target `rfc_test_jobs`, and `scripts/check-converter.py --case jobs`. Done when: deterministic barrier tests verify C11/C++17 header compilation, effective config read-back, concurrent CPU/memory budget bounds, unknown/hostile-limit zero-allocation refusals, OOM hook enforcement (`peak_allocated_bytes <= budget_bytes`), concurrent same-format job isolation and `RFC_ERR_BUSY` unregister refusal, missing-stage refusal, progress + retry + stale post-retry rejection, all-stage cancellation cleanup (`published_cancelled=0`), and `rfc_scheduler_destroy` shutdown safety.
- [x] Commit: `"converter: bounded asynchronous jobs"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case jobs --all-targets`; assert and quote effective config read-back, concurrent CPU-thread and aggregate memory-reservation bounds, unknown-reservation and hostile-limit zero-allocation refusals, OOM hook cap (`peak_allocated_bytes <= budget_bytes`), post-retry stale callback rejection, safe teardown with queued callbacks, and zero published cancelled jobs across both native host fixtures and isolated restricted-`PATH` `windows-x64` (`bin/converter/windows-x64/rfc_test_jobs.exe`) runs.

**Freeze check:** Existing `.fold` v2 and v3 files, `KAT_FOLD` known-answer bytes, and the oracle (`tests/rfi_ref.py`) remain unchanged (`**Corrected 2026-11-15:**` extended explicit `.fold` v2 mention to `.fold` v2 and v3 per `spec/FOLD.md`). Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

> **Verified:** 2026-11-15 | §3 | 2a52d2e950cb99a015b0643419d4c6c3f964cb98
> **Review:** `docs/reviews/d10-t01-s3.json`
## 4. Exactness verification and differential fixtures

> **Started:** 2026-11-15T17:32:11Z

**Job:** A user can prove that decoding reverses an encode exactly, independently of a color-managed preview.

**Clarified 2026-11-15:** Verification status codes in `converter/include/rfc_codec.h` (`rfc_status`) and `converter/src/registry.c` (`rfc_status_name`) add `RFC_ERR_VERIFY_MISMATCH` (`18`, `"verify-mismatch"`) and `RFC_ERR_METADATA_POLICY` (`19`, `"metadata-policy"`). Public exactness, metadata-policy, canonical layout-and-frame-boundary SHA-256 (`rfc_bytes_hash_sha256`, `rfc_frames_hash_sha256`), JPEG DCT coefficient-domain (`rfc_verify_jpeg_coefficients`), fresh-session verification (`rfc_verify_encoded_fresh`, `rfc_job_verify_report_get`), and display-only sRGB preview (`rfc_preview_frame`, `rfc_preview_render_srgb8`, `rfc_preview_free`) declarations live in `converter/include/rfc_verify.h` (C11 and C++17 clean), keeping separate non-overlapping enums for the four `docs/converter-exactness.md` operation labels (`pixel-exact`, `byte-identical`, `coefficient-exact`, `original-byte-reconstruction`) and the three metadata outcomes (`preserved`, `stripped-explicitly`, `unsupported-items-named`). `converter/src/verify.c` compares frame counts, dimensions, `rfc_sample_format`, `rfc_alpha_mode`, `bits_per_sample`, `duration_ms`, active row samples ignoring `stride_bytes` row padding, RGB samples beneath `A == 0` for straight/premultiplied alpha (`RFC_MISMATCH_TRANSPARENT_RGB`), zero low padding bits when integer `bits_per_sample < storage_bits`, and exact IEEE-754 `uint16_t`/`uint32_t` bit patterns for half/float (`-0.0` vs `+0.0`, NaN payloads). `converter/src/metadata.c` checks item-by-item preservation (`icc`, `orientation`, `frame-timing`) against per-format capabilities: default `metadata=preserve` refuses unacknowledged unsupported present items (`RFC_ERR_METADATA_POLICY`) naming them, reports `unsupported-items-named` when explicitly acknowledged (`acknowledged_unsupported_metadata` on `rfc_job_spec`, preserved across `rfc_scheduler_retry`), and fails (`RFC_ERR_VERIFY_MISMATCH`) if supported items are lost or altered; `metadata=strip` refuses `byte-identical` and `original-byte-reconstruction` (`RFC_ERR_UNSUPPORTED_COMBINATION`), requires removable `icc` and non-default `orientation` to be absent, and reports `stripped-explicitly`. When `RFC_STAGE_VERIFY` runs without an override `on_verify` callback in `converter/src/job.c`, it verifies `byte-identical` via input/output SHA-256 or for `pixel-exact` opens a fresh `rfc_session_adapter` session on `encoded_output` (resetting per-decode stream counters and returning `RFC_ERR_NO_ADAPTER` when no target session adapter is registered), storing a report with `independent_decoder = 0` and `decoder_provenance = "same-provider-fresh-session"`. `converter/src/preview.c` renders display-only 8-bit sRGB buffers (`not_for_verification = 1`) in pure C11 without importing or supplying any symbol to `verify.c`, `metadata.c`, or `job.c`. `tests/converter/reference.py`, `tests/converter/test_exactness.c` (`rfc_test_exactness` linking `rfc_engine` + `librasterfold.a`), `tests/converter/test_exactness.py`, and `scripts/check-converter.py --case exactness --all-targets` drive host C/Python fixtures plus isolated restricted-`PATH` Windows verification with explicit 19-format provenance tracking (detecting Pillow shared C libraries so shared libraries never claim independence) and explicit `JPEG_COEFFICIENT_CONTRACT`, `OPENEXR_BIT_PATTERN_CONTRACT`, and `LIBRAW_RENDERING_CONTRACT`; reversal cost is confined to converter verification headers, sources, and test gates with zero core codec changes.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement sample/frame comparison in `converter/src/verify.c` (`converter/include/rfc_verify.h`, `converter/include/rfc_codec.h`, `converter/src/registry.c`). Done when: `RFC_ERR_VERIFY_MISMATCH` (`18`) is registered; layout-and-frame-boundary SHA-256 and `rfc_verify_frames_equal` participate every frame count, dimension, native bit depth, low zero-padding bit, alpha mode, transparent-pixel RGB, half/float raw bit pattern (`-0.0` vs `+0.0`, NaN payload), row-stride-independent active sample, and `duration_ms` in equality; and JPEG coefficient-domain verification validates quantized DCT blocks and quant tables cleanly.
- [ ] Implement metadata policy comparison in `converter/src/metadata.c` (and `rfc_job_spec.acknowledged_unsupported_metadata` in `converter/include/rfc_job.h`/`scheduler.c`). Done when: `RFC_ERR_METADATA_POLICY` (`19`) is registered, `preserved`, `stripped-explicitly` and `unsupported-items-named` are distinct outcomes, a default `preserve` conversion refuses silent drops of unsupported metadata while naming missing capabilities, and `metadata=strip` fails on retained removable metadata or byte-oriented exactness combinations.
- [ ] Add independent reference decoding to `tests/converter/reference.py`. Done when: all 19 formats in `converter/codecs.json` carry explicit provenance against `converter/vendor.json`, Pillow shared-C-library decoders (`libwebp`, `libtiff`, `openjp2`) are classified honestly as `shared-upstream-library`, independent pure-Python/`tests/rfi_ref.py` decoders operate where representation-compatible (`rasterfold`, uncompressed `BMP`, `PNM`, `PAM`, `QOI`, PNG chunks, uncompressed `OpenEXR`), and `JPEG_COEFFICIENT_CONTRACT`, `OPENEXR_BIT_PATTERN_CONTRACT` and `LIBRAW_RENDERING_CONTRACT` carry executable contract checks.
- [ ] Keep preview color transforms outside verification in `converter/src/preview.c`. Done when: `rfc_preview_render_srgb8` marks `not_for_verification = 1`, `verify.c`/`metadata.c`/`job.c` link and pass without compiling or linking `preview.c`, and two distinct frames that quantize to identical 8-bit sRGB preview buffers fail `rfc_verify_frames_equal`.
- [ ] Add `tests/converter/test_exactness.c`, `tests/converter/test_exactness.py`, and `scripts/check-converter.py --case exactness`. Done when: one changed sample, hidden transparent RGB, changed frame duration, invalid low padding bits, `-0.0`/`+0.0` float flip, lost ICC, unacknowledged unsupported metadata drop, retained stripped metadata, and preview collision each fail the appropriate claim across native host, Python differential, and isolated `windows-x64` (`rfc_test_exactness.exe`) runs.
- [ ] Freshly decode the produced file in `converter/src/verify.c` and builtin `RFC_STAGE_VERIFY` (`converter/src/job.c`) using a new decoder session instance. Done when: `rfc_verify_encoded_fresh` and builtin `RFC_STAGE_VERIFY` open a distinct session, run limit checks plus agreed sample/metadata comparison, record `independent_decoder = 0` and `"same-provider-fresh-session"`, and preserve `RFC_ERR_NO_ADAPTER` when no target session adapter is registered.
- [ ] Commit: `"converter: exactness verification with explicit decoder provenance"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case exactness --all-targets`; quote seeded mismatch failures (1-LSB flip, transparent RGB zeroing, changed frame duration, `-0.0`/`+0.0` float bit flip, lost ICC, unacknowledged unsupported drop refusal, preview collision failing verification) and same-sample fresh-session passes across native host fixtures, Python differential checks, and isolated restricted-`PATH` `windows-x64` (`bin/converter/windows-x64/rfc_test_exactness.exe`) runs. Cheaper substitute rejected: visual similarity or quality=100.

**Freeze check:** Existing `.fold` v2 and v3 files, `KAT_FOLD` known-answer bytes, and the oracle (`tests/rfi_ref.py`) remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 5. Safe import and export

**Requires:** windows-runtime -- The file checkpoint drives Windows file-provider refusal cases. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can export a document to a chosen destination under a safe permissions policy, and deliver a verified file without damaging an existing one.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement file sniffing and bounded reads in `converter/src/input.c`. Done when: content and extension disagreement is reported; a filename never selects an unsafe decoder blindly.
- [ ] Implement exclusive staged publication in `converter/src/output.c`. Done when: an existing destination, input alias, symlink alias or hard-link alias is refused by default.
- [ ] Implement disk-full and cancellation cleanup in `converter/src/output.c`. Done when: only the job-owned temporary file is removed and no partial output is called successful.
- [ ] Define portable input/output callback ownership in `converter/include/rfc_io.h`. Done when: the engine accepts bounded read, seek and staged-publication callbacks without depending on a platform window or activity.
- [ ] Add `tests/converter/test_file_safety.py`. Done when: existing targets, same-file inputs, truncated streams, permission denial and disk exhaustion preserve before/after hashes.
- [ ] Commit: `"converter: safe import and export"`

**Test checkpoint:** Run the file-safety fixtures and drive native Windows destination refusals; capture before/after hashes.

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

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report driven Windows coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
