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
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the Windows/Linux x64 codec build exist. The converter itself targets Windows x64 only (operator decisions 2026-11-11: Android removed, Linux interface dropped; either can return as a fork). There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native C application with bundled codec source and no additional user installs. **Corrected 2026-11-16:** `todo/10-converter/TODO-01-engine.md` (`D10 T01 §1..§6`, `status: done`) shipped `rfc_engine`, the bounded job scheduler, verification/metadata engine, safe staged I/O, comparison/history reports, and the pinned `windows-x64` static builds of all 19 bundled libraries in `converter/vendor.json` (including `zlib` `1.4` and `libpng + libpng-apng` `1.6.59` with `libpng-1.6.59-apng.patch.gz`); the format adapters (`converter/codecs/*.c`) remain unimplemented until each `TODO-02` section ships.
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
| 1 | §1 | RasterFold, BMP, PNG and PNM | D10 T01 §2, D10 T01 §4, D10 T01 §5, D07 T01 §2, D07 T01 §3 | [x] |
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
| 13 | §13 | Broader PNG import for indexed, grayscale-alpha, sub-8-bit and tRNS assets | §1 | [ ] |

---

## 1. RasterFold, BMP, PNG and PNM

> **Started:** 2026-11-16T14:41:45Z

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can import or compress rasterfold, bmp, png and pnm with accurately advertised preservation guarantees.

**Clarified 2026-11-16:** `converter/include/rfc_codecs.h` (C11 and C++17 clean, zero `<windows.h>`, checked by `_check_headers` in `scripts/check-converter.py`) declares `rfc_codecs_register_basic(void)` and `rfc_codecs_unregister_basic(void)`, keeping `rfc_engine` free of auto-registered codec globals so existing mock-adapter tests stay isolated. `converter/CMakeLists.txt` adds static library `rfc_codecs` (`converter/codecs/basic.c`, compiled C11 `-Wall -Wextra -Werror`, linking `rfc_engine`, `${RF_LIBRASTERFOLD}`, `${RF_LIB_PNG}`, `${RF_LIB_Z}`) and `bin/converter/windows-x64/rfc_test_basic.exe` (`tests/converter/test_basic.c`, statically linked, PE-verified zero non-system DLLs, exposing both C self-test suites and a `cli` subcommand driven from `tests/converter/test_basic.py` via `scripts/windows.py --exe`). In `converter/codecs.json` and `converter/src/registry.c`, `rasterfold`, `bmp`, `png`, `pnm`, and `pam` transition to `"availability": "usable"` (`RFC_AVAILABILITY_USABLE`, `unavailable_reason = null`/`NULL`), while `apng` stays `"availability": "planned"` (`unavailable_reason`: `"APNG multi-frame sequence composition and export owned by D10 T02 §12"`, `extension_owner`: `"D10 T02 §12"`) until `§12` implements `converter/src/frames.c`. `validate_registry()` in `scripts/check-converter.py` permits `"usable"` for a `D10 T02 §N` row when `§N` is `[x]` or carries `> **Started:**` alongside its shipped adapter file (`converter/codecs/basic.c` for `§1`), and `tests/converter/test_contract.c` verifies `RFC_ERR_PLANNED` on `RFC_FORMAT_WEBP` and `RFC_ERR_NO_ADAPTER` on `RFC_FORMAT_PNG` before registration. All core codec sources (`src/`, `include/rasterfold.h`, `spec/FOLD.md`, `tests/rfi_ref.py`) remain untouched; converter-private header `converter/codecs/rf_shared_io.h` declares thin `extern` prototypes matching `src/rf.h` for `librasterfold.a`'s existing D07 CLI readers/writers (`rf_bmp_load`, `rf_bmp_store`, `rf_ppm_load`, `rf_ppm_store`, `rf_png_load`, `rf_png_store`, `rf_crc32`, allocating/freeing temporary `rf_image` buffers via public `rf_free` without copying `src/` code). Each session adapter implements full `probe_session`, `encode_session`, `decode_session`, and atomic `cancel_session` respecting `rfc_limits` and `rfc_exec_alloc_frame` / `rfc_exec_allocator`: (1) `RFC_FORMAT_RASTERFOLD` calls only public `include/rasterfold.h` (`rf_inspect`, `rf_encode_image`, `rf_decode_image`, `rf_free`), decoding `.fold` v2/v3 and encoding `.fold` v3 for `RGB8` (`bits_per_sample == 8`, `RFC_ALPHA_NONE`), clamping `rfc_exec_threads` to `1..9`, and refusing unsupported samples, alpha, or unacknowledged metadata under `metadata=preserve`; (2) `RFC_FORMAT_BMP` delegates 24-bit uncompressed `BITMAPINFOHEADER` (`dib == 40`) `RGB8` load/store to shared `rf_bmp_load`/`rf_bmp_store`, adds 8-bit grayscale (`GRAY8`, canonical 256-entry `R==G==B` palette, refusing colour palettes and `1/4/16`-bpp BMPs with `RFC_ERR_UNSUPPORTED_SAMPLE`), adds 32-bit uncompressed `BGRA` (`RGBA8`, `BI_RGB` or standard RGBA `BI_BITFIELDS`, preserving RGB bytes at `A == 0`, refusing `RFC_ALPHA_PREMULTIPLIED` and RLE/JPEG/PNG compression, and flagging embedded V4/V5 ICC profiles as `RFC_META_ITEM_ICC` so unacknowledged ICC loss fails `RFC_ERR_METADATA_POLICY` under `metadata=preserve`); (3) `RFC_FORMAT_PNG` validates chunk structure and CRCs (`rf_crc32`) in `probe_session` before pixel allocation, refusing `acTL`/`fcTL`/`fdAT` animated APNGs (`PNG_APNG_SUPPORTED` + chunk scan, `RFC_ERR_UNSUPPORTED_COMBINATION` naming `D10 T02 §12`), paletted (`color_type == 3`), sub-8-bit (`1/2/4`), `sBIT`-reduced, and binary `tRNS` PNGs (`RFC_ERR_UNSUPPORTED_SAMPLE`), and `RFC_ALPHA_PREMULTIPLIED` (`RFC_ERR_UNSUPPORTED_ALPHA`), detecting `iCCP` (`RFC_META_ITEM_ICC`) and `eXIf` orientation `> 1` (`RFC_META_ITEM_ORIENTATION`), delegating baseline 8-bit opaque RGB PNGs without ancillary ICC/eXIf/tRNS/sBIT/APNG within `RF_MAX_DIM`/`RF_MAX_PIXELS` to shared `rf_png_load` with `rfc_budget` working-set accounting, and routing `gray8`, `gray16`, `rgba8`, `rgb16`, `rgba16`, any `iCCP`-bearing PNG, any `eXIf`-bearing PNG (including baseline `RGB8` PNG with `eXIf` only), and all PNG encodes through bundled `libpng 1.6.59 + zlib 1.4` (`png_create_read_struct_2`/`png_create_write_struct_2` bound to `exec->budget`, `png_set_swap` on 16-bit, verbatim RGB at `A == 0`, `iCCP` preserved under `metadata=preserve` and omitted under `metadata=strip`, never routing higher-depth/alpha PNGs through RGB8 RasterFold); (4) `RFC_FORMAT_PNM` delegates `P6` `maxval == 255` (`RGB8`, no header comments) to shared `rf_ppm_load`/`rf_ppm_store` and supports `P1`/`P4` (`GRAY8`, `bits_per_sample = 1`, left-aligned `0x00`/`0x80`), `P2`/`P5` (`GRAY8` `2..8` bit, `GRAY16` `9..16` bit), and `P3`/`P6` (`RGB8` `1..8` bit, `RGB16` `9..16` bit), while `RFC_FORMAT_PAM` supports `P7` (`BLACKANDWHITE`, `GRAYSCALE`, `RGB`, `RGB_ALPHA` across `GRAY8/GRAY16/RGB8/RGBA8/RGB16/RGBA16`), strictly rejecting non-`(2^bits - 1)` `maxval`, `GRAYSCALE_ALPHA` (`DEPTH 2`), nonzero low padding bits on encode, truncated/trailing bytes, and unacknowledged metadata under `metadata=preserve`.

**Corrected 2026-11-16 (plan review):** Resolved all three pre-build plan review findings into explicit memory, preflight, routing, and fixture contracts: (1) Reconciled shared PNG calls with `converter/src/limits.c` (`RFC_FORMAT_PNG`, `RFC_MEM_HOOK_ENFORCED`): all PNG encodes and all `libpng` reads (`gray8`, `gray16`, `rgba8`, `rgb16`, `rgba16`, plus any `RGB8` PNG with `iCCP` or `eXIf`) route 100% through `libpng 1.6.59` + `zlib 1.4` `rfc_budget` allocator hooks (`png_create_read_struct_2` / `png_create_write_struct_2`); baseline `RGB8` opaque PNG decode without `iCCP`/`eXIf`/`tRNS`/`sBIT`/`PLTE`/`acTL` shares CLI `rf_png_load` after computing the exact `rf_png_load` peak working set (`idat_cap_bound + raw_filter_bytes + w * h * 3` via checked overflow arithmetic; `rf_zlib_inflate` uses zero heap allocations) and reserving `shared_ws_bytes` via `rfc_budget_alloc(exec->budget, ...)` before calling `rf_png_load` (failing immediately with `RFC_ERR_OUT_OF_MEMORY` when `exec->budget` rejects the reservation, holding it across `rf_png_load` + copy + `rf_free`, and releasing it cleanly), with `converter/src/limits.c` updated to document both `libpng` hook enforcement and shared `rf_png_load` pre-reservation, and low-budget `RFC_ERR_OUT_OF_MEMORY` (`rejected_count > 0`) tests added for **both** PNG routes; (2) Defined bounded `.fold` v2/v3 header-only preflight before `rf_inspect`/`rf_decode_image`: `RFC_FORMAT_RASTERFOLD` `probe_session` and `decode_session` validate the 32-byte `.fold` header (`"RFI"`, version `2|3`, header size `32`, `RF_FORMAT_RGB8`, header CRC-32 via `rf_crc32`, `method <= RF_METHOD_SC2`, `reserved == 0`, `w > 0 && h > 0`, `payload_len == size - 32`, `RAW` length check, and `rfc_limits_check_probe` against `exec->limits`) with zero pixel or scratch allocation before any `rf_inspect` or `rf_decode_image` call runs (`rf_inspect` never runs in `probe_session` and only runs/decodes in `decode_session` after `rfc_limits_check_probe` and budget checks succeed), proved by a valid `.fold` v3 fixture (within core `RF_MAX_DIM`/`RF_MAX_PIXELS`) that exceeds tighter job `limits_override` (`max_width` and `max_decoded_bytes`) and fails with `RFC_ERR_LIMIT_EXCEEDED` at zero core allocation; (3) Completed the `libpng` route and orientation-policy cases for `eXIf`-bearing PNGs: any PNG carrying `eXIf` (including baseline `RGB8` PNG with `eXIf` only) routes through bundled `libpng 1.6.59` (`png_get_eXIf_1` + pre-scan IFD0 `0x0112` Orientation parsing), testing all four explicit outcomes (`RFC_ORIENTATION_NORMAL` (`1`) allowed under `metadata=preserve`; `Orientation 2..8` (e.g. `6`) refused `RFC_ERR_METADATA_POLICY` under `metadata=preserve` without acknowledgment because PNG advertises `ICC` only in `converter/src/registry.c`; `Orientation 6` under `metadata=preserve` + `acknowledged_unsupported_metadata |= RFC_META_ITEM_ORIENTATION` succeeding with `RFC_VERIFY_META_UNSUPPORTED_ITEMS_NAMED`; and `Orientation 6` under `metadata=strip` succeeding with `RFC_VERIFY_META_STRIPPED_EXPLICITLY`).

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [x] Pin the required RasterFold, the shared D07 image modules, libpng/zlib for high-depth/alpha PNG, and a pinned audited APNG-capable provider or maintained patch set with complete provenance and build options in `converter/vendor.json`. Done when: the provider builds for Windows x64 with source/license evidence. (**Verified 2026-11-16 (`D10 T01 §2`):** `converter/vendor.json` pins `zlib` `1.4` (`02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584`, `converter/licenses/zlib.txt`) and `libpng + libpng-apng` `1.6.59` (`86a3e4b501f7f50e392c4e456ea158893e9a595b1c63eb88c7bb9f6cf8772dad`, `libpng-1.6.59-apng.patch.gz` `29693a9c9953ca1e88e25401ee500ed959e73f93dc98163fa967df71d3e7a990`, `PNG_APNG_SUPPORTED`, `PNG_HARDWARE_OPTIMIZATIONS=ON`, `converter/licenses/libpng.txt`); `build/converter/windows-x64/deps-prefix/lib/libpng16.a` and `libz.a` compile cleanly for `windows-x64`, and `librasterfold.a` exports `include/rasterfold.h` plus `rf_bmp_*`, `rf_ppm_*`, `rf_png_*`, and `rf_crc32`.)
- [x] Implement `converter/codecs/basic.c`. Done when: the public RasterFold API is reused with bounded header preflight before `rf_inspect`/`rf_decode_image`; RGB8-only restrictions are explicit; BMP/PNG/PPM readers are shared with the CLI rather than copied; baseline `rf_png_load` accounts exact scratch/pixel working set in `rfc_budget`; PBM P1/P4, PGM P2/P5, PPM P3/P6 and PAM P7 are covered with depth/alpha restrictions explicit; higher-depth/alpha/`iCCP`/`eXIf` PNG uses bundled `libpng 1.6.59` with `rfc_budget` hooks without silently routing through RGB8 RasterFold.
- [x] Register exact input/output capabilities in `converter/codecs.json`. Done when: unsupported sample depth, alpha, metadata and frame modes are refused before work, with a user-visible reason.
- [x] Add positive and malformed fixtures in `tests/converter/test_basic.py`. Done when: a fresh decoder proves the advertised representation and metadata contract (including `eXIf` RGB8 PNG orientation `1` vs `6` preserve/refusal/acknowledged-loss/strip fixtures, low-budget `RFC_ERR_OUT_OF_MEMORY` on both shared `rf_png_load` and `libpng` PNG routes, and a valid `.fold` v3 fixture exceeding job `limits_override` rejected before allocation), differential fixtures identify implementation independence or shared-provider limitations, and mutated input cannot crash or hang.
- [x] Add its fixtures to `scripts/check-converter.py`. Done when: the Windows build and native Windows test matrix include this provider.
- [x] Commit: `"converter: rasterfold, bmp, png and pnm"`

**Test checkpoint:** Executed `bosun run -- python3 scripts/check-converter.py --case all --all-targets` and `bosun run -- python3 scripts/check-all.py --all-targets` (`2026-11-16`). Header hygiene compiled cleanly in C11 and C++17 (`rfc_codecs.h`, `rfc_colorimetry` in `rfc_image.h`, and `rfc_budget_reserve`/`rfc_budget_release` in `rfc_job.h`, zero `<windows.h>`), `validate_registry()` verified all 19 entries against `converter/codecs.json`, `jobs` verified `rfc_budget_reserve`/`rfc_budget_release` accounting on host and isolated `windows-x64`, `exactness` (`host`, isolated `windows-x64`, `8/8` Python tests in `3.134s`) verified `rfc_colorimetry` (`sRGB`/`gAMA`/`cHRM`/`cICP`/`mDCV`/`cLLI`) validation, `PRESERVE` mismatch on dropped `gAMA` (`icc == NULL`), `STRIP` retained mismatch, and `BMP` `RFC_ERR_METADATA_POLICY`, `host` `test_basic` passed `739 checks` (`basic: all checks passed (739 checks)`), and cross-built `-O3` `bin/converter/windows-x64/rfc_test_basic.exe` passed PE import verification (`kernel32.dll` + Universal CRT `api-ms-win-crt-*.dll` only) and isolated restricted-`PATH` execution (`basic: all checks passed (741 checks)`). Both targets proved `.fold` v2/v3 `RGB8` thread `1` vs `4` byte identity, valid `128x64` `.fold` v3 rejection with `RFC_ERR_LIMIT_EXCEEDED` and `rf_internal_alloc_peak == 0` under `max_width=64` and `max_decoded_bytes=4096`, `BMP` `GRAY8`/`RGB8`/`RGBA8` (`A == 0` RGB preserved), V4 `LCS_CALIBRATED_RGB` non-zero endpoint/gamma `RFC_ERR_UNSUPPORTED_COMBINATION` refusal (and all-zero / `LCS_WINDOWS_COLOR_SPACE` acceptance), and V5 embedded ICC `RFC_ERR_METADATA_POLICY`, all 6 `PNG` sample depths with `iCCP` preserve/strip, non-ICC `PNG` colourimetry (`gAMA`, `sRGB`+`gAMA`+`cHRM`, and `cICP`+`mDCV`+`cLLI`) preserve/strip round trips, pre-export `RFC_ERR_METADATA_POLICY` refusal to `BMP`/`RASTERFOLD` (`unsupported-items-named`), `ack` pass, and malformed chunk (`gAMA=0`, `sRGB=4`, `cICP` matrix `1`, duplicate `gAMA`) `RFC_ERR_INVALID_ARGUMENT` refusals, `iCCP` PNG `limits_override.max_width = 4` refusal with `icc == NULL` and `current_allocated_bytes == 0`, low-budget `RFC_ERR_OUT_OF_MEMORY` (`rejected_count > 0`, `current_allocated_bytes == 0`) with accounting-only `rfc_budget_reserve` on shared `rf_png_load` (`20480` B refuse, `1 MiB` peak `>= 28736` B) and `libpng` (`16384` B encode / `28000` B `RGB16` decode), `-O3` post-`longjmp` cleanup on `128x512` `RGBA16` PNG encode OOM (`> 16384` B), mid-encode cancellation (`>= 8192` B), and corrupt-IDAT decode (`0` leaked bytes), all 4 `eXIf` `RGB8` PNG orientation outcomes (`Orientation=1` `preserved`; `Orientation=6` `metadata-policy` with fixed `rep.meta_outcome` propagation in `converter/src/job.c` and `converter/src/verify.c`; `Orientation=6` + `ack` `unsupported-items-named`; `Orientation=6` + `strip` `stripped-explicitly`), `PNM` `P1`..`P6` (`1..16`-bit left-aligned) and `PAM` `P7` (`BLACKANDWHITE`, `GRAYSCALE`, `RGB`, `RGB_ALPHA`), explicit `RFC_ERR_UNSUPPORTED_SAMPLE` refusal across `GRAY16`/`RGB16`/`RGBA16` with `bits_per_sample = 1..8` (`0xFF00` included), plus `APNG`/`PLTE`/sub-8/`GRAYSCALE_ALPHA`/non-`(2^n-1)` maxval/non-zero pad-bit/destination-exists refusals and cancellation. `tests/converter/test_basic.py` passed `4/4 OK` (`Ran 4 tests in 41.392s`, `HostBasicTests` and `WindowsBasicTests` on isolated NTFS) checking `.fold` via independent `tests/rfi_ref.py::decode_image`, `BMP`/`PNG`/`PNM`/`PAM` via pure-Python `tests/converter/reference.py` and Pillow, byte-for-byte `gAMA`+`cHRM` and `cICP`+`mDCV`+`cLLI` preserve/strip/`BMP`+`RASTERFOLD` policy CLI `convert` contracts, and seeded truncations/mutations across all 5 formats; `check-converter --case all --all-targets` (`contract`, `jobs`, `exactness` `8/8` `3.134s`, `file-safety` `6/6` `3.076s`, `reports` `5/5` `99.781s`, `basic` `4/4` `41.392s`, `bundle` `8/8` `4.676s`) and `check-all.py --all-targets` (`32/32 passed`) exited `0`.

**Freeze check:** `git diff --exit-code -- src/ include/rasterfold.h spec/FOLD.md tests/rfi_ref.py converter/vendor.json` exited `0`, leaving `.fold` v2 and v3 codecs, `KAT_FOLD`, `tests/rfi_ref.py`, and pinned codec dependencies unchanged; every encoded output received a fresh bundled decode and `rfc_verify` / independent oracle check (`rfi_ref.py`, `reference.py`, Pillow) reported per target; source inputs and pre-existing destinations remained byte-identical under refusal, cancellation, and limit failure.

> **Verified:** 2026-11-16 | §1 | 4b296f4529b041ab0b528f77196f653e467ad56d
> **Review:** `docs/reviews/d10-t02-s1.json`
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

## 13. Broader PNG import for indexed, grayscale-alpha, sub-8-bit and tRNS assets

**Origin:** discovered run=0c72d521c489 2026-11-16 -- D10 T02 §1 completion review out-of-scope finding #2

**Requires:** windows-runtime -- The checkpoint requires driven results from the advertised Windows native target.

**Job:** A user can import indexed, grayscale-alpha, sub-8-bit and `tRNS` PNG graphics assets losslessly without silent colour, alpha, or bit-depth fabrication.

Support standard graphics and legacy PNG variants omitted from `§1`'s minimal lossless subset (`PLTE` indexed images at 1/2/4/8-bit, `color_type == 4` grayscale-alpha at 8/16-bit, sub-8-bit grayscale at 1/2/4-bit, and `tRNS` transparency) without silent colour, precision, or channel loss (Admission Clause 2: Phase 10 format completeness per `docs/reviews/10-converter/D10-T02-s1.md`).

- [ ] Extend PNG prescan and decode in `converter/codecs/basic.c` to import `PLTE` indexed (1/2/4/8-bit), `color_type == 4` grayscale-alpha (8/16-bit), sub-8-bit grayscale (1/2/4-bit), and `tRNS` streams losslessly into `RFC_SAMPLE_*` frames with exact `bits_per_sample` and non-destructive colour/alpha expansion.
- [ ] Enforce strict PNG spec bounds in prescan and decode (valid `PLTE` byte count, `tRNS` length per `color_type`, pre-allocation budget reservation, and cooperative cancellation) while routing expanded alpha/indexed images away from the bare RGB8 `rf_png_load` fast path.
- [ ] Update capability metadata notes in `converter/codecs.json` and `converter/src/registry.c` so contract and UI provenance describe broader PNG import limits accurately.
- [ ] Add C11 and differential Python reference/Pillow tests in `tests/converter/test_basic.c` and `tests/converter/test_basic.py` covering 1/2/4/8-bit `PLTE` with and without `tRNS`, 8/16-bit grayscale-alpha, 1/2/4-bit grayscale with and without `tRNS`, colourimetric chunk combinations, and malformed chunk refusals across host and isolated `windows-x64`.
- [ ] Commit: `"converter: support indexed, grayscale-alpha, sub-8-bit and tRNS png import"`

-> SOURCE: D10-T02-S1-PNG-IMPORT-SUBSET

**Test checkpoint:** Run `python3 scripts/check-converter.py --case basic --all-targets` and `python3 scripts/check-converter.py --case contract --all-targets`; prove indexed, grayscale-alpha, sub-8-bit, and `tRNS` PNG differential checks pass on host and `windows-x64` and malformed palette/`tRNS` inputs refuse cleanly.

**Freeze check:** Existing `.fold` v2/v3 files, core codec (`src/`), and `spec/FOLD.md` remain unchanged; every decoded pixel and colourimetric/alpha attribute round-trips without silent alteration.

## Verification

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report driven Windows coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
