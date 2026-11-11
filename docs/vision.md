# RasterFold product vision

Recorded 2026-09-30 from the operator's request and two clarifications. This is a product contract for planned work, not a claim that the converter or Pages site already ships.

RasterFold supplies a lossless codec, CLI, and shared/static C libraries for Windows/Linux x64. A native image converter demonstrates RasterFold alongside leading image formats. GitHub Pages provides complete public product documentation. The existing sixty codec research milestones remain experiments with measured acceptance bars; executing a plan cannot guarantee an unmeasured compression win.

## Native converter

The operator selected native Windows/Linux (Android was removed from this repository on 2026-11-11: "we will not be doing any android dev in this repo anymore"; a fork can add it later), and defined no dependencies as no additional installs for users: audited, pinned codec source may be bundled. Application and UI logic use C11, original SVG icons and native platform integration. No Electron, browser shell, Qt, SDL or downloaded runtime is needed. Bundled implementations may contain C++ behind C interfaces, including libjxl. Minimal platform interop generated for Android SDK facilities is permitted as a documented implementation default; the application logic remains in C. Changing that default costs only the Android platform boundary, not the engine or codec adapters.

The initial feature set is single-image conversion with a decoded preview, synchronized side-by-side comparisons, an optional wipe and difference view, cancel/retry, a clear output destination, truthful size/time/memory reporting, keyboard and screen-reader access, scaling, themes, and visible failure states. Images stay on the device; demo samples are bundled and licensed. Presets select real adapter settings and never silently reduce precision, flatten alpha, discard metadata or extract the first animation frame.

The required format matrix names RasterFold, BMP, PNG/APNG, PNM/PAM, WebP, JPEG XL, AVIF, JPEG, GIF, TIFF, HEIF/HEIC, JPEG 2000, QOI and OpenEXR. Camera RAW/DNG has an explicit import-only adapter and original-file preservation policy. Capabilities are negotiated per format, bit depth, alpha, frame count and metadata. A format's presence is not a claim that every source can be encoded losslessly into it. JPEG coefficient optimization, JPEG byte reconstruction through JPEG XL, and exact decoded-pixel conversion are distinct operations. Unsupported combinations remain visible with a precise reason.

## Proof of losslessness

Decode each encoded result afresh with the bundled decoder and compare the agreed native samples, dimensions, depth, alpha including transparent RGB, frame timings and metadata policy before reporting success. Hashes include the sample layout and frame boundaries. Test fixtures additionally use independent implementations where available and representation-compatible. Same-provider OpenEXR checks and pinned LibRaw rendering are labeled as such; JPEG coefficient preservation is tested in the coefficient domain, not by demanding identical IDCT output from unrelated decoders. The UI never calls its bundled verification independent. Color-managed preview pixels are never used as proof. “Smaller” is measured from the produced file; a larger lossless result is shown honestly and never causes the input to be deleted.

RasterFold remains RGB8 until its own format/API milestones add further capabilities. A converter can preserve RGBA or 16-bit data in another codec without pretending that the current RasterFold format supports it. Existing destinations are refused by default and inputs remain intact after cancellation, codec failure, disk exhaustion or a refused permission.

## Documentation and release

Keep the recorded Jekyll/Just the Docs choice for GitHub Pages. Publish an explicit allowlist of product pages, original assets and generated references, never agent policy, TODOs, review records, intelligence, local paths or private working data. Test the `/RasterFold/` project base path, navigation, search, keyboard use, mobile layouts and broken links. Cover installation, both targets, CLI, C API, native converter, capabilities, limitations, format specification, measured benchmarks, examples, troubleshooting, accessibility, licensing and release history.

One release version source drives CLI/product metadata, package names and the site; format and ABI versions remain independent compatibility contracts. CI checks tag/version agreement and assembles executable, shared/static library, header, license and symbol packages for each target. Release publication is an explicit tag action.

## Drift and completion

Every supported document is inventoried with its role. Source facts, generated blocks, measured-data bindings, reviewed-source hashes, commit references and built-site links have mechanical checks. Repair only explicitly generated output; prose review, new measurements and historical evidence cannot be manufactured by a repair command. A source change invalidates its review receipt even when made on the same date. CI and the staged commit gate detect drift before it ships. Completed TODO rows still require independent review and their own stamps.

## Primary references

- [WebP exact mode](https://developers.google.com/speed/webp/docs/cwebp): preserve RGB values under transparent pixels.
- [JPEG XL encoder API](https://libjxl.readthedocs.io/en/latest/api_encoder.html): use the C interface and the documented lossless and JPEG reconstruction paths.
- [libavif color representation](https://github.com/AOMediaCodec/libavif/wiki/CICP): RGB exactness requires an appropriate reversible representation, rather than assuming zero quantization is sufficient.
- [GitHub Pages](https://docs.github.com/en/pages/getting-started-with-github-pages/what-is-github-pages): verify account availability and publication visibility before deployment from this private repository.
