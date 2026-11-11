# Changelog

All notable changes to RasterFold are recorded here. The format follows Keep a Changelog, and product releases follow Semantic Versioning from the single `VERSION` file (`scripts/version.py`). The product version is independent of the `.fold` format version (in every file header) and of the C API version (`rf_version()`).

Every milestone gate commit appends its summary line to the Unreleased section; a release moves that section under the new version heading (`D09 T01 §4`).

## Unreleased

Work toward RasterFold 1.0.0 (format version 3, C API 1.0):

- One product version source: `VERSION` feeds the CLI `--version` report, the build manifest, development package names, `docs/facts.json`, and the site, with stale builds and contradictory release tags refused.
- The CLI gained `verify`, `inspect --json`, `help` (including per-command help), and extension-based format dispatch.
- The site gained a generated API Reference page rendered from `include/rasterfold.h`.
- Android was removed from this repository: the `android-arm64` and `android-x64` build targets, the NDK pin, and the planned Android converter work are gone. RasterFold targets Windows x64 and Linux x64; Android can return as a fork.
- The C API surface is frozen at five exported functions with thread-local errors, and both a byte-identity check against the independent oracle and a multithreaded byte-identity check guard it.

## Pre-1.0 development (since the version 2 codec)

Unversioned development builds; no tagged releases exist before 1.0.0.

- Format version 3: context-mixing method and palette methods in one container, strip-parallel encoding and decoding with byte-identical output for any thread count, and payload/header CRC-32 checks on every file.
- C API 1.0: `rf_encode_image`, `rf_decode_image`, `rf_inspect`, `rf_free`, and `rf_version`, with the same five symbols on Windows, Linux, and both Android ABIs.
- Lossless round trips are proven against an independent Python oracle on a 48-image correctness corpus, and malformed inputs are refused with one of six documented exit/error codes.
