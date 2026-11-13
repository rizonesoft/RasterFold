# Changelog

All notable changes to RasterFold are recorded here. The format follows Keep a Changelog, and product releases follow Semantic Versioning from the single `VERSION` file (`scripts/version.py`). The product version is independent of the `.fold` format version (in every file header) and of the C API version (`rf_version()`).

Every milestone gate commit appends its summary line to the Unreleased section; a release moves that section under the new version heading (`D09 T01 §4`).

## Unreleased

Work toward RasterFold 1.0.0 (format version 3, C API 1.0):

- One product version source: `VERSION` feeds the CLI `--version` report, the build manifest, development package names, `docs/facts.json`, and the site, with stale builds and contradictory release tags refused.
- The CLI gained `verify`, `inspect --json`, `help` (including per-command help), and extension-based format dispatch.
- The site gained a generated API Reference page rendered from `include/rasterfold.h`.
- Android was removed from this repository: the `android-arm64` and `android-x64` build targets, the NDK pin, and the planned Android converter work are gone. RasterFold targets Windows x64 and Linux x64; Android can return as a fork.
- Images with at most 256 colours are written with method 4, a compact palette method new in format version 3: the palette is stored sorted and Golomb-Rice coded, decisions use 16-bit probabilities, and the image's top row and left column have their own counters. On the generated set the files total 200,774 bytes against lossless WebP's 201,248 (201,640 before). Files written with methods 2 and 3 still decode.
- Milestone 8 reached (2026-11-12): on generated images RasterFold is smaller than lossless WebP in total (200,774 against 201,248 bytes) and in geometric mean (0.8771), with every image within 1.1 times WebP or 32 bytes of it; photographs are byte-identical to Milestone 7.
- Release packages: `scripts/package.py --release` writes one reproducible, self-contained zip per platform to `dist/` from a clean commit, with offline documentation and SHA-256 checksum files inside and beside the packages.
- Releases are published by a workflow from a pushed `rasterfold-v<VERSION>` tag: it runs every push gate on the tagged commit, builds the packages once, runs them on Linux and Windows with `scripts/verify_release.py` (checksums, reported version, a lossless round trip), and only then creates the GitHub release with the packages, their checksums and this changelog's section as notes. A manual run rehearses all of it without publishing.
- The command-line help states that `encode` and `decode` refuse an existing output unless `--overwrite` is given, and the site gained a Command-Line Reference generated from that help text.
- The C API surface is frozen at five exported functions with thread-local errors, and both a byte-identity check against the independent oracle and a multithreaded byte-identity check guard it.
- Screen-content images whose backward-reference copies cover at least half of the pixels are also tried with method 5 (`sc`), new in format version 3, coding subtract-green literals, a 256-entry colour cache, and overlapping LZ77 copies with RFC 9649 prefix codes and 2D-mapped distances under 16-bit adaptive binary range coding, and kept whenever smaller than the raw, context-mixing, or palette choice (708,217 bytes across the ten development screenshots against method 1's 3,383,489, with photographs, the test pack, and the generated set unchanged).
- Screen-content images with more than 16 colours are now written with method 6 (`sc`), new in format version 3, adding causal spatial and recent copy-distance inheritance from length 2, decomposed 2D `(dy, dx)` offsets with repeated vertical distances, single-pixel `W` and `N` neighbour-repeat flags, a 16-entry move-to-front colour list, a 2048-entry colour cache, and `W_g >> 5` literal conditioning: on the ten development screenshots the files drop from 708,217 to 549,635 bytes against lossless WebP's 670,020 (`0.8203` total, `0.8269` geometric mean, all ten smaller than WebP), while photographs, the test pack, and the generated set stay byte-identical and method 5 files continue to decode.

## Pre-1.0 development (since the version 2 codec)

Unversioned development builds; no tagged releases exist before 1.0.0.

- Format version 3: context-mixing method and palette methods in one container, strip-parallel encoding and decoding with byte-identical output for any thread count, and payload/header CRC-32 checks on every file.
- C API 1.0: `rf_encode_image`, `rf_decode_image`, `rf_inspect`, `rf_free`, and `rf_version`, with the same five symbols on Windows, Linux, and both Android ABIs.
- Lossless round trips are proven against an independent Python oracle on a 48-image correctness corpus, and malformed inputs are refused with one of six documented exit/error codes.
