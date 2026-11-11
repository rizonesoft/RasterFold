---
schema_version: 1
id: converter-codecs
domain: 10-converter
status: draft
title: "TODO-02 -- Leading Codec and Format Adapters"
depends_on: []
frozen: true
track: GUI2
---

# TODO-02 -- Leading Codec and Format Adapters

> **Goal:** Every format in the required capability matrix has an audited provider, truthful lossless modes and fresh bundled decode and format-specific round-trip/refusal coverage, with differential fixtures where available.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the Windows/Linux x64 codec build exist. The converter itself targets Windows x64 only (operator decisions 2026-11-11: Android removed, Linux interface dropped; either can return as a fork). There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native C application with bundled codec source and no additional user installs.
<!-- claim: exists include/rasterfold.h -->
<!-- claim: exists converter/include/rfc_codec.h -->

## Inputs

- [Product vision](../../docs/vision.md) and [native design](../../docs/design/converter.md): scope, exactness, platform and interaction contracts.
- [C API](../../include/rasterfold.h): RasterFold ownership and error boundary.

- -> XREF: D10 T01 §4 -- shared implementation or verification boundary.

- -> XREF: D07 T01 §3 -- shared implementation or verification boundary.

- -> XREF: D10 T04 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T03 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §5 -- safe delivery of verified output documents.

- -> XREF: D99 T01 §7 -- implementation prerequisite and acceptance boundary.

## Outcome

- Every format in the required capability matrix has an audited provider, truthful lossless modes and fresh bundled decode and format-specific round-trip/refusal coverage, with differential fixtures where available.
- Clean Windows x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T01 §1; document=applicable @ D10 T03 §5; settings=applicable @ D10 T01 §1; reporting=applicable @ D10 T01 §6; notifications=applicable @ D10 T01 §3; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T01 §6; exchange=applicable @ D10 T02 §1; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | RasterFold, BMP, PNG and PNM | D10 T01 §2, D10 T01 §4, D10 T01 §5, D07 T01 §2, D07 T01 §3 | [ ] |
| 2 | §2 | WebP | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 3 | §3 | JPEG XL | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 4 | §4 | AVIF | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 5 | §5 | JPEG optimization and import | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 6 | §6 | TIFF and GIF | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 7 | §7 | HEIF and HEIC | D10 T01 §2, D10 T01 §4, D10 T01 §5, D99 T01 §7 | [ ] |
| 8 | §8 | QOI | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 9 | §9 | JPEG 2000 | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 10 | §10 | OpenEXR | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 11 | §11 | Camera RAW and DNG import | D10 T01 §2, D10 T01 §4, D10 T01 §5 | [ ] |
| 12 | §12 | Animation and multi-page preservation | §2, §3, §4, §6, D10 T01 §4 | [ ] |

---

## 1. RasterFold, BMP, PNG and PNM

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress rasterfold, bmp, png and pnm with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required RasterFold, the shared D07 image modules, libpng/zlib for high-depth/alpha PNG, and a pinned audited APNG-capable provider or maintained patch set with complete provenance and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/basic.c`. Done when: the public RasterFold API is reused; RGB8-only restrictions are explicit; BMP/PNG/PPM readers are shared with the CLI rather than copied; PBM P1/P4, PGM P2/P5, PPM P3/P6 and PAM P7 are covered with depth/alpha restrictions explicit; higher-depth/alpha PNG uses a bundled audited provider without silently routing it through RGB8 RasterFold.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_basic.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: rasterfold, bmp, png and pnm"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case basic`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 2. WebP

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress webp with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required libwebp source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/webp.c`. Done when: lossless encoding sets exact transparent RGB preservation; ICC/EXIF/XMP muxing and decoded RGBA equality are tested against the upstream cwebp/dwebp contract.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_webp.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: webp"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case webp`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 3. JPEG XL

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress jpeg xl with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required libjxl and its required transitive libraries source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/jxl.c`. Done when: the C interface selects documented lossless encoding, retained invisible samples and metadata boxes; JPEG reconstruction uses original JPEG bytes and proves byte-identical reconstruction separately from pixel round trips.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_jxl.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: jpeg xl"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case jxl`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 4. AVIF

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress avif with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required libavif plus one audited AV1 encoder/decoder source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/avif.c`. Done when: lossless mode requires supported full-range 4:4:4 reversible color representation and zero quantization; decoded native RGB equality is verified rather than inferred from an encoder setting.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_avif.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: avif"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case avif`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 5. JPEG optimization and import

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress jpeg optimization and import with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required libjpeg-turbo source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/jpeg.c`. Done when: existing JPEGs can optimize entropy coding without re-quantizing coefficients; arbitrary RGB-to-baseline-JPEG is refused in lossless mode; original-byte reconstruction is delegated to the JPEG XL adapter.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_jpeg.py`. Done when: coefficient arrays, quantization tables, dimensions and metadata are preserved across entropy optimization; pixel import is compared only under the pinned decoder settings, and original-byte reconstruction uses JPEG XL; mutated inputs must not crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: jpeg optimization and import"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case jpeg`; quote the format-specific coefficient/rendering contract and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 6. TIFF and GIF

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress tiff and gif with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required libtiff with its selected compression providers and giflib source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/tiff_gif.c`. Done when: still TIFF sample depth and metadata remain intact; still GIF preserves indexed pixels and refuses palette reduction when exact representation exceeds its capacity; multi-page and animated composition are owned only by §12.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_tiff_gif.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: tiff and gif"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case tiff_gif`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 7. HEIF and HEIC

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import heif and heic through the Windows system codec with accurately advertised preservation guarantees, and is told plainly when the system cannot.

**Corrected 2026-11-11 (operator decision, `docs/codec-distribution.md`):** "OS codec, read only". No libheif, no HEVC decoder and no HEVC encoder is bundled; HEIC is never written.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Record in `converter/vendor.json` that HEIF/HEIC has no bundled provider and uses the Windows Imaging Component decoder. Done when: the build contains no HEVC source or binary, and a check in `scripts/check-converter.py` fails if one is added.
- [ ] Implement `converter/codecs/heif.c` as an import-only adapter over the Windows Imaging Component. Done when: a system without the codec is refused before work with a reason naming the missing system component, decoded pixels are labelled with the representation the system decoder returned, and RGB conversion cannot be mislabeled lossless.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_heif.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: heif and heic"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case heif`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 8. QOI

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress qoi with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required the audited QOI reference source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/qoi.c`. Done when: RGB/RGBA8 preserves all channel bytes, including fully transparent RGB; larger depths and metadata that QOI cannot carry are rejected under preserve mode.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_qoi.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: qoi"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case qoi`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 9. JPEG 2000

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress jpeg 2000 with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required OpenJPEG source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/jp2.c`. Done when: reversible transform mode and component precision preserve native samples; irreversible transform outputs are never offered as lossless.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_jp2.py`. Done when: a fresh decoder proves the advertised representation and metadata contract, differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: jpeg 2000"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case jp2`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 10. OpenEXR

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress openexr with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required OpenEXR and its required support libraries source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/exr.c`. Done when: lossless compression preserves native half/float sample bits, channel names and windows; a tone-mapped preview does not replace the original samples.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_exr.py`. Done when: native half/float bit patterns, channel names/windows and metadata survive a fresh pinned OpenEXR decode, labeled same-provider proof; differential checks name their actual compatible decoder or explicitly record its absence; mutated inputs must not crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: openexr"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case exr`; quote native sample equality and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 11. Camera RAW and DNG import

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress camera raw and dng import with accurately advertised preservation guarantees.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Pin the required LibRaw source and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence.
- [ ] Implement `converter/codecs/raw.c`. Done when: import exposes the chosen demosaic/color pipeline as a rendering operation; the original file is retained and no sensor-data or original-file reconstruction claim is made from a rendered preview.
- [ ] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [ ] Add positive and malformed fixtures in `tests/converter/test_raw.py`. Done when: the original RAW/DNG hash is unchanged, rendering matches the pinned LibRaw pipeline/settings and metadata policy, and no claim of original sensor reconstruction or independent rendering is made; mutated inputs must not crash or hang.
- [ ] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [ ] Commit: `"converter: camera raw and dng import"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case raw`; quote the format-specific coefficient/rendering contract and refusal evidence for each supported target. Compression improvements require the report protocol, not a claim from upstream marketing.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 12. Animation and multi-page preservation

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can preserve an entire supported animation or multi-page image rather than silently exporting frame one.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement frame-sequence handling in `converter/src/frames.c`. Done when: APNG, WebP, GIF, JPEG XL and AVIF adapters retain canvas size, frame rectangles, disposal/blend, loop count and timing when supported.
- [ ] Extend `converter/codecs.json` for APNG and multi-page TIFF. Done when: single-frame-only destinations explain the limitation before encoding.
- [ ] Add frame navigation data to `converter/src/preview.c`. Done when: previewed frame selection does not change the encode scope.
- [ ] Add `tests/converter/test_frames.py`. Done when: reference decoders prove composited/native frame semantics and duration equality, including zero-delay and partial-frame fixtures.
- [ ] Commit: `"converter: animation and multi-page preservation"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case frames`; prove a second-frame mismatch fails even when the first frame is identical.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## Verification

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report driven Windows coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
