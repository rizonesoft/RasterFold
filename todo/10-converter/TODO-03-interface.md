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
- Resolute's design contract (`DESIGN.md` in `rizonesoft/Resolute`) and its launcher (`src/main.cpp` there): the reference for how a `resolute-ui` application is assembled. Both are vendored for reading with the library by §1.

**Stack facts (read 2026-11-11 from `rizonesoft/Resolute` at `513ab81c`; §1 re-reads them at the commit it pins).** `resolute-ui` is a static C++23 library, namespace `rui::`, include prefix `resolute/`. It provides `Toolbar`, `Sidebar`, `StatusBar`, `ContentView`, `ListView` and `PopupMenu`; `Theme` (dark, light and follow-system modes, the token set, `IsHighContrast`); `Typography`; `Dpi`; `AnimationManager`; `LucideIcons`; and `RenderContext` (the Direct2D and DirectWrite factories, `CreateHwndTarget`, `CreateBitmapFromRGBA`, `DrawSvgIcon`). It does not provide a button, drop-down, slider, check box, progress bar, an image pane, or UI Automation providers. The rule for the gap: a control the library provides is used as is; a generic control it lacks is written once under `converter/app/controls/` on `rui::RenderContext`, `rui::Theme` tokens and `rui::Typography`, with a UI Automation provider, and listed in `docs/converter-building.md` as a candidate to move upstream into Resolute; a later pin that ships the control replaces the local one. Converter-specific surfaces (the image pane, the wipe, the difference view) live under `converter/app/` and follow the same token and accessibility rules. Reversal cost of the rule: deleting `converter/app/controls/` once Resolute ships equivalents.

- -> XREF: D10 T04 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §6 -- shared implementation or verification boundary.

- -> XREF: D10 T02 §4 -- shared implementation or verification boundary.

- -> XREF: D08 T01 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §5 -- safe delivery of verified output documents.

## Outcome

- A modern, accessible Windows interface on `resolute-ui` previews single images, compares real codec results and demonstrates RasterFold without extra installs.
- Clean Windows x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T03 §5; document=applicable @ D10 T03 §5; settings=applicable @ D10 T03 §7; reporting=applicable @ D10 T03 §6; notifications=applicable @ D10 T03 §5; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T01 §6; exchange=applicable @ D10 T03 §5; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Interface shell on resolute-ui | D10 T01 §1, D10 T01 §2, D10 T01 §3 | [ ] |
| 2 | §2 | Windows native integration | §1, D10 T01 §3, D10 T01 §5 | [ ] |
| 5 | §5 | Single-image conversion and preview | §2, D10 T02 §1, D10 T01 §6 | [ ] |
| 6 | §6 | Synchronized comparisons | §5, D10 T01 §6, D10 T02 §2, D10 T02 §3, D10 T02 §4 | [ ] |
| 7 | §7 | Preferences and accessible error recovery | §5, §6 | [ ] |
| 8 | §8 | Guided RasterFold demonstration | §6, §7 | [ ] |

---

## 1. Interface shell on resolute-ui

**Requires:** display-session -- The checkpoint captures the shell in every theme and scale and asserts focus and UI Automation in a Windows desktop session; a headless WSL run cannot produce it.

**Job:** A user can recognize, focus and operate a consistent native interface that looks and behaves like the Resolute launcher.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Vendor `resolute-ui` and its Lucide set from `rizonesoft/Resolute` at a pinned commit into `converter/vendor/resolute-ui/` and `converter/vendor/lucide/`, recorded in `converter/vendor.json` with the commit, the file list, SHA-256 hashes and licence. Done when: a changed or missing vendored file fails `scripts/check-converter.py`, and `AGENTS.md` records the dependency and its reason.
- [ ] Add the `RasterFoldConverter` executable to `converter/CMakeLists.txt` (the CMake build and its driver `scripts/build-converter.py` come from D10 T01 §2). Done when: a clean checkout builds a `RasterFoldConverter` Windows x64 executable as C++23 that statically links `ResoluteUI_static` and `Lucide_static`, with the manifest declaring per-monitor DPI v2, and the codec targets in `scripts/build.py` are unchanged.
- [ ] Implement the application shell in `converter/app/main.cpp` and `converter/app/shell.cpp` on `rui::` windows and controls. Done when: the application bar, workspace, settings column and result footer of `docs/design/converter.md` exist as `resolute-ui` controls, and no file under `converter/app/` draws a control the library provides or names a colour, font size or spacing literal.
- [ ] Implement the generic controls the pinned library lacks (button, drop-down, slider, check box, progress bar) and a shared UI Automation provider base under `converter/app/controls/`. Done when: each draws only through `rui::RenderContext` with `rui::Theme` tokens, shows the 2px accent focus ring, meets the 24x24 hit target at 100%, reports name, role, state and value to UI Automation, and is listed in `docs/converter-building.md` as an upstream candidate.
- [ ] Add UI Automation adapters for every `resolute-ui` control the converter uses (`ListView`, `Toolbar`, `Sidebar`, `StatusBar`, `PopupMenu`, `ContentView`) in `converter/app/controls/uia_adapters.cpp`, wrapping each control without changing how it renders. Done when: a UI Automation client enumerates every item, row, button and menu entry of those controls with a name, role and state, can invoke, select and sort through them, and `tests/converter/test_ui.cpp` fails when an adapter is removed.
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
- [ ] Add `tests/converter/uia_driver.cpp`, built by `converter/CMakeLists.txt`: a UI Automation client that starts the converter, finds controls by AutomationId, invokes them, sets values, reads names, states and announcements, and saves a window capture as PNG. Done when: the Python drivers under `tests/converter/` use it for every driven check, it stops the converter's whole process tree on timeout and teardown, and no third-party Python package is added.
- [ ] Add lifecycle/refusal tests in `tests/converter/test_win32.py`. Done when: closing, suspending, resizing, permission denial and display loss cannot leak workers or publish partial files.
- [ ] Record native evidence in `docs/converter-validation.md`. Done when: screenshots and input/accessibility runs name OS, architecture, build revision and tested states.
- [ ] Commit: `"converter: windows native integration"`

**Test checkpoint:** Build the Windows target and drive it through `tests/converter/uia_driver.cpp` in a Windows desktop session; quote observed behaviour separately from fixture and cross-build results.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 5. Single-image conversion and preview

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence in a Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can browse a list of supported formats, preview one image, receive progress and error notifications, and save a verified result.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Implement the open and drop flow in `converter/app/open.cpp` on the native file dialog and drag and drop of §2. Done when: a cancelled dialog leaves state intact, a corrupt or unsupported file produces a recoverable error state, and the format list is a `rui::ListView` filled from the capability registry.
- [ ] Implement the image pane in `converter/app/image_pane.cpp`: fit, zoom and pan over a Direct2D bitmap made with `rui::RenderContext::CreateBitmapFromRGBA`. Done when: large images use bounded preview memory through tiled or downscaled bitmaps, source orientation and colour metadata are respected, per-monitor DPI changes re-lay out without rescaling, and the pane reports its zoom and position to UI Automation.
- [ ] Bind capability-driven settings in `converter/app/settings.cpp` using the controls of §1. Done when: each visible setting has a real adapter consumer, an incompatible output format is disabled with the reason from the registry, and no setting names a colour, font size or spacing literal.
- [ ] Implement progress, cancel, retry and result views in `converter/app/convert.cpp`, with taskbar progress and `rui::StatusBar` text. Done when: every state in the design spec is visible in light, dark and high contrast, each state change is announced through UI Automation, and a stale job cannot replace the current preview.
- [ ] Implement save and reveal actions in `converter/app/save.cpp`. Done when: export reports the actual destination, exactness and size growth or savings without replacing the input.
- [ ] Add `tests/converter/test_single_flow.py` over the driver of §2. Done when: open-to-save, cancel-at-each-stage, mismatch, permission denial and growth paths have driven assertions and a capture per design state.
- [ ] Commit: `"converter: single-image conversion and preview"`

**Test checkpoint:** Drive `tests/converter/test_single_flow.py` in a Windows desktop session; capture each design state in light, dark and high contrast and verify a fresh decode under its format-specific contract.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 6. Synchronized comparisons

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence in a Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can compare RasterFold and other codecs side by side using the same source and preservation policy.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Implement synchronized image-coordinate navigation in `converter/app/compare.cpp` over two image panes of §5. Done when: zoom, pan, fit and actual-pixel views keep the same source point aligned at every DPI.
- [ ] Implement split, wipe and difference modes in `converter/app/compare.cpp`. Done when: the wipe has keyboard and numeric control and a UI Automation range value, and difference mode has a labelled scale and a changed-sample count carried as text, not colour alone.
- [ ] Render comparison results from the engine's report (`converter/src/report.c`) in a `rui::ListView` in `converter/app/results.cpp`. Done when: sizes, effort, thread count, timing and verification status are visible and sortable; unverified results never rank as successful.
- [ ] Add a source and result switch for alpha and colour preview in `converter/app/compare.cpp`. Done when: background and display transform changes cannot alter the encoded samples.
- [ ] Add `tests/converter/test_compare.py` over the driver of §2. Done when: alignment, mismatch, unsupported codec, larger output and stale-result regressions fail.
- [ ] Commit: `"converter: synchronized comparisons"`

**Test checkpoint:** Drive comparisons on the same sample in a Windows desktop session and quote pixel verification plus keyboard and UI Automation navigation evidence from the driver; the Narrator run is owned by §7.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 7. Preferences and accessible error recovery

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence in a Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can keep useful settings, reset them, and recover accessibly from every failure.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Implement a versioned preferences file in `converter/src/preferences.c` (engine side, C11). Done when: theme mode (follow-system, light, dark), density, last safe directory and memory budget survive restart with named consumers.
- [ ] Implement the preferences surface, reset and corrupt-settings recovery in `converter/app/preferences.cpp`. Done when: invalid values fall back safely, reset requires no manual file deletion, and a theme change crossfades through `rui::AnimationManager` unless reduced motion is set.
- [ ] Complete localization-ready labels in `converter/app/strings.cpp`. Done when: long labels, Unicode filenames and right-to-left layout fixtures do not clip essential actions.
- [ ] Add accessibility, contrast and reduced-motion checks in `tests/converter/test_accessibility.py` over the driver of §2. Done when: a keyboard-only run and a UI Automation run complete conversion and comparison in light, dark and Windows high contrast at both densities, and every control reports a name and role.
- [ ] Run conversion, comparison and a failed-conversion recovery with Windows Narrator and record the run in `docs/converter-validation.md`. Done when: the record lists, step by step, what Narrator spoke for each control and each status announcement, names the Windows build and the converter revision, and any control or announcement Narrator did not speak is fixed and re-run; the UI Automation checks above do not stand in for this run.
- [ ] Verify privacy in `tests/converter/test_privacy.py`. Done when: opening, converting and demo operations perform no network requests and reports omit source paths unless explicitly requested.
- [ ] Commit: `"converter: preferences and accessible error recovery"`

**Test checkpoint:** Quote the keyboard-only and UI Automation runs and the Narrator run in a Windows desktop session, including reset, corrupt preferences and failed conversion recovery with its spoken announcements.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 8. Guided RasterFold demonstration

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence in a Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A new user can try RasterFold and compare codecs immediately using licensed bundled samples.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Add small diverse licensed images under `converter/assets/samples/`. Done when: each has provenance and covers photographs, text, graphics and transparent/high-depth capability refusals.
- [ ] Implement Try sample in `converter/app/demo.cpp`. Done when: it enters the real import, encode, verify and compare flow of §5 and §6 without fabricated statistics.
- [ ] Add contextual explanations in `converter/app/demo.cpp`. Done when: the user can distinguish losslessness from size savings and see current RasterFold RGB8 limitations.
- [ ] Add `tests/converter/test_demo.py` over the driver of §2. Done when: offline first launch, sample conversion and fresh-decode output verification work on a clean Windows install.
- [ ] Commit: `"converter: guided rasterfold demonstration"`

**Test checkpoint:** Drive first-launch Try sample with networking disabled in a Windows desktop session and compare saved output against its bundled source.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## Verification

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report driven Windows desktop coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
