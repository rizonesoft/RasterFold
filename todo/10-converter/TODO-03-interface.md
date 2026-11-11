---
schema_version: 1
id: converter-interface
domain: 10-converter
status: draft
title: "TODO-03 -- Native Interface and Comparisons"
depends_on: []
frozen: true
track: GUI3
---

# TODO-03 -- Native Interface and Comparisons

> **Goal:** A modern, accessible Windows interface on the Resolute launcher's stack (C++23, CMake and Ninja on llvm-mingw, the `resolute-ui` Direct2D library, Lucide glyphs, Resolute's `DESIGN.md`) previews single images, compares real codec results and demonstrates RasterFold without extra installs.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the Windows/Linux x64 codec build exist. The converter itself targets Windows x64 only (operator decisions 2026-11-11: Android removed, Linux interface dropped; either can return as a fork). There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native application with bundled codec source and no additional user installs; the interface is C++23 on `resolute-ui` (operator decision 2026-11-11) over the C11 engine of D10 T01.
<!-- claim: exists include/rasterfold.h -->
<!-- claim: exists converter/include/rfc_codec.h -->

## Inputs

- [Product vision](../../docs/vision.md) and [native design](../../docs/design/converter.md): scope, exactness, platform and interaction contracts.
- [C API](../../include/rasterfold.h): RasterFold ownership and error boundary.

- -> XREF: D10 T04 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §6 -- shared implementation or verification boundary.

- -> XREF: D10 T02 §4 -- shared implementation or verification boundary.

- -> XREF: D08 T01 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §5 -- safe delivery of verified output documents.

## Outcome

- A modern, accessible C interface previews single images, compares real codec results and demonstrates RasterFold without extra installs.
- Clean Windows x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T03 §5; document=applicable @ D10 T03 §5; settings=applicable @ D10 T03 §7; reporting=applicable @ D10 T03 §6; notifications=applicable @ D10 T03 §5; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T01 §6; exchange=applicable @ D10 T03 §5; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Interface shell on resolute-ui | D10 T01 §1 | [ ] |
| 2 | §2 | Windows native integration | §1, D10 T01 §3, D10 T01 §5 | [ ] |
| 5 | §5 | Single-image conversion and preview | §2, D10 T02 §1, D10 T01 §6 | [ ] |
| 6 | §6 | Synchronized comparisons | §5, D10 T01 §6, D10 T02 §2, D10 T02 §3, D10 T02 §4 | [ ] |
| 7 | §7 | Preferences and accessible error recovery | §5, §6 | [ ] |
| 8 | §8 | Guided RasterFold demonstration | §6, §7 | [ ] |

---

## 1. Interface shell on resolute-ui

**Job:** A user can recognize, focus and operate a consistent native interface that looks and behaves like the Resolute launcher.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Vendor `resolute-ui` and its Lucide set from `rizonesoft/Resolute` at a pinned commit into `converter/vendor/resolute-ui/` and `converter/vendor/lucide/`, recorded in `converter/vendor.json` with the commit, the file list, SHA-256 hashes and licence. Done when: a changed or missing vendored file fails `scripts/check-converter.py`, and `AGENTS.md` records the dependency and its reason.
- [ ] Pin CMake in `toolchain.json` beside the existing llvm-mingw and Ninja and add `converter/CMakeLists.txt` with `scripts/build-converter.py` as its driver. Done when: a clean checkout builds a `RasterFoldConverter` Windows x64 executable as C++23 that statically links `ResoluteUI_static` and `Lucide_static`, with the manifest declaring per-monitor DPI v2, and the codec targets in `scripts/build.py` are unchanged.
- [ ] Implement the application shell in `converter/app/main.cpp` and `converter/app/shell.cpp` on `rui::` windows and controls. Done when: the application bar, workspace, settings column and result footer of `docs/design/converter.md` exist as `resolute-ui` controls, and no file under `converter/app/` draws a control the library provides or names a colour, font size or spacing literal.
- [ ] Bind the shell to the C engine through `converter/app/engine_bridge.cpp`. Done when: interface code reaches jobs, capabilities and results only through the C headers under `converter/include/`, and rendering, input, accessibility and application state have separate ownership.
- [ ] Add the converter's glyphs to the vendored Lucide set with their sources in `converter/vendor/lucide/icons/SOURCES.md`. Done when: each action has a Lucide glyph and a visible and accessible label.
- [ ] Add `tests/converter/test_ui.cpp`. Done when: keyboard focus traversal, token-only colour and UI Automation names fail on seeded regressions.
- [ ] Register the vendored trees and their hash check in `docs/drift.json`. Done when: editing a vendored file without updating `converter/vendor.json` fails the drift gate.
- [ ] Commit: `"converter: interface shell on resolute-ui"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case ui`; inspect captures in light, dark and high contrast at 100%, 150% and 200% and quote focus/action assertions.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 2. Windows native integration

**Requires:** display-session -- Win32 input, DPI and UI Automation need a driven Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can operate the converter through the Windows platform facilities.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

**Needs:** Windows host (build/test)

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement `converter/app/platform_win32.cpp`. Done when: window lifecycle, per-monitor DPI changes, native file dialogs, clipboard, drag and drop and taskbar progress are wired to the `resolute-ui` shell and the C job contracts.
- [ ] Document the platform boundary in `docs/converter-building.md`. Done when: only system facilities and shipped libraries are used; conversion behaviour stays in the C engine and the interface owns no codec logic.
- [ ] Expose names, roles, state, values and actions for the converter's own controls (preview panes, wipe, difference view) in `converter/app/accessibility.cpp` through UI Automation. Done when: keyboard and screen-reader activation use the same application actions, and status changes are announced.
- [ ] Add lifecycle/refusal tests in `tests/converter/test_win32.py`. Done when: closing, suspending, resizing, permission denial and display loss cannot leak workers or publish partial files.
- [ ] Record native evidence in `docs/converter-validation.md`. Done when: screenshots and input/accessibility runs name OS, architecture, build revision and tested states.
- [ ] Commit: `"converter: windows native integration"`

**Test checkpoint:** Build the native target and drive win32 interactions on a real desktop/device; quote observed behavior separately from fixture and cross-build results.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 5. Single-image conversion and preview

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can browse a list of supported formats, preview one image, receive progress and error notifications, and save a verified result.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement the open/drop flow in `converter/ui/open.c`. Done when: cancelled dialogs leave state intact and corrupt/unsupported files produce recoverable errors.
- [ ] Implement preview fit, zoom and pan in `converter/ui/preview.c`. Done when: large images use bounded preview memory and source orientation/color metadata are respected.
- [ ] Bind capability-driven settings in `converter/ui/settings.c`. Done when: each visible setting has a real adapter consumer and incompatible output formats explain why.
- [ ] Implement progress, cancel, retry and result views in `converter/ui/convert.c`. Done when: every state in the design spec is visible and a stale job cannot replace the current preview.
- [ ] Implement save/reveal actions in `converter/ui/save.c`. Done when: export reports the actual destination, exactness and size growth/savings without replacing the input.
- [ ] Add `tests/converter/test_single_flow.py`. Done when: open-to-save, cancel-at-each-stage, mismatch, permission denial and growth paths have driven assertions.
- [ ] Commit: `"converter: single-image conversion and preview"`

**Test checkpoint:** Drive `tests/converter/test_single_flow.py` on Windows; capture each design state and verify a fresh decode under its format-specific contract.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 6. Synchronized comparisons

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can compare RasterFold and other codecs side by side using the same source and preservation policy.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement synchronized image-coordinate navigation in `converter/ui/compare.c`. Done when: zoom, pan, fit and actual-pixel views keep the same source point aligned.
- [ ] Implement split, wipe and difference modes in `converter/ui/compare.c`. Done when: wipe has keyboard/numeric control and difference mode has a labeled scale and changed-sample count.
- [ ] Render comparison results from `converter/src/report.c`. Done when: sizes, effort, thread count, timing and verification status are visible; unverified results never rank as successful.
- [ ] Add a source/result switch for alpha and color preview in `converter/ui/compare.c`. Done when: background and display transform changes cannot alter the encoded samples.
- [ ] Add `tests/converter/test_compare.py`. Done when: alignment, mismatch, unsupported codec, larger output and stale-result regressions fail.
- [ ] Commit: `"converter: synchronized comparisons"`

**Test checkpoint:** Drive comparisons on the same sample and quote pixel verification plus keyboard/screen-reader navigation evidence.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 7. Preferences and accessible error recovery

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can keep useful settings, reset them, and recover accessibly from every failure.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement a versioned preferences file in `converter/src/preferences.c`. Done when: theme, density, last safe directory and memory budget survive restart with named consumers.
- [ ] Implement reset and corrupt-settings recovery in `converter/ui/preferences.c`. Done when: invalid values fall back safely and reset requires no manual file deletion.
- [ ] Complete localization-ready labels in `converter/ui/strings.c`. Done when: long labels, Unicode filenames and right-to-left layout fixtures do not clip essential actions.
- [ ] Add accessibility/contrast and reduced-motion checks in `tests/converter/test_accessibility.py`. Done when: keyboard-only and screen-reader users can complete conversion and comparison in every theme/density.
- [ ] Verify privacy in `tests/converter/test_privacy.py`. Done when: opening/converting/demo operations perform no network requests and reports omit source paths unless explicitly requested.
- [ ] Commit: `"converter: preferences and accessible error recovery"`

**Test checkpoint:** Quote keyboard-only and screen-reader runs on all native platforms, including reset, corrupt preferences and failed conversion recovery.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 8. Guided RasterFold demonstration

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A new user can try RasterFold and compare codecs immediately using licensed bundled samples.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Add small diverse licensed images under `converter/assets/samples/`. Done when: each has provenance and covers photographs, text, graphics and transparent/high-depth capability refusals.
- [ ] Implement Try sample in `converter/ui/demo.c`. Done when: it enters the real import/encode/verify/compare flow without fabricated statistics.
- [ ] Add contextual explanations in `converter/ui/demo.c`. Done when: the user can distinguish losslessness from size savings and see current RasterFold RGB8 limitations.
- [ ] Add `tests/converter/test_demo.py`. Done when: offline first launch, sample conversion and fresh-decode output verification work on clean installs.
- [ ] Commit: `"converter: guided rasterfold demonstration"`

**Test checkpoint:** Drive first-launch Try sample with networking disabled and compare saved output against its bundled source.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## Verification

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report native device coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
