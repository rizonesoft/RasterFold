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

> **Goal:** A modern, accessible Windows interface on the Resolute launcher's stack (C++23, CMake and Ninja on llvm-mingw, the `resolute-ui` Direct2D library, Lucide glyphs, Resolute's `DESIGN.md`) converts a queue of images, compares real codec results and demonstrates RasterFold without extra installs, built 1:1 to `docs/design/converter.md`.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the Windows/Linux x64 codec build exist. The converter itself targets Windows x64 only (operator decisions 2026-11-11: Android removed, Linux interface dropped; either can return as a fork). There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native application with bundled codec source and no additional user installs; the interface is C++23 on `resolute-ui` (operator decision 2026-11-11) over the C11 engine of D10 T01.
<!-- claim: exists include/rasterfold.h -->
<!-- claim: exists converter/include/rfc_codec.h -->

## Inputs

- [Product vision](../../docs/vision.md) and [native design](../../docs/design/converter.md): scope, exactness, platform and interaction contracts. The design file is the 1:1 contract (layout chosen by the operator on 2026-11-22: the Images queue first, Compare as the codec scoreboard, Samples); its Geometry table and column widths are the values the fidelity checks parse, and [`converter-reference.html`](../../docs/design/converter-reference.html) draws the same screens as a visual reference that the contract overrides.
- [C API](../../include/rasterfold.h): RasterFold ownership and error boundary.
- Resolute's design contract (`DESIGN.md` in `rizonesoft/Resolute-SDK`, moved there from `rizonesoft/Resolute` on 2026-11-15) and the launcher (`src/main.cpp` in `rizonesoft/Resolute`): the reference for how a `resolute-ui` application is assembled. Both are vendored for reading with the library by §1.

**Stack facts (read 2026-11-11 from `rizonesoft/Resolute` at `513ab81c`; §1 re-reads them at the commit it pins; on 2026-11-22 the library and its Lucide set were found moved to `rizonesoft/Resolute-SDK` under `shared/resolute-ui/` and `shared/lucide/`, at `bddf5860`).** `resolute-ui` is a static C++23 library, namespace `rui::`, include prefix `resolute/`. It provides `Toolbar`, `Sidebar`, `StatusBar`, `ContentView`, `ListView` and `PopupMenu`; `Theme` (dark, light and follow-system modes, the token set, `IsHighContrast`); `Typography`; `Dpi`; `AnimationManager`; `LucideIcons`; and `RenderContext` (the Direct2D and DirectWrite factories, `CreateHwndTarget`, `CreateBitmapFromRGBA`, `DrawSvgIcon`). It does not provide a button, drop-down, slider, check box, progress bar, an image pane, or UI Automation providers. The rule for the gap: a control the library provides is used as is; a generic control it lacks is written once under `converter/app/controls/` on `rui::RenderContext`, `rui::Theme` tokens and `rui::Typography`, with a UI Automation provider, and listed in `docs/converter-building.md` as a candidate to move upstream into Resolute; a later pin that ships the control replaces the local one. Converter-specific surfaces (the image pane, the wipe, the difference view) live under `converter/app/` and follow the same token and accessibility rules. Reversal cost of the rule: deleting `converter/app/controls/` once Resolute ships equivalents.

- -> XREF: D10 T04 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §6 -- shared implementation or verification boundary.

- -> XREF: D10 T02 §4 -- shared implementation or verification boundary.

- -> XREF: D08 T01 §1 -- shared implementation or verification boundary.

- -> XREF: D10 T01 §5 -- safe delivery of verified output documents.

## Outcome

- A modern, accessible Windows interface on `resolute-ui` converts a queue of images, compares real codec results and demonstrates RasterFold without extra installs, built 1:1 to `docs/design/converter.md`.
- Clean Windows x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T03 §5; document=applicable @ D10 T03 §5; settings=applicable @ D10 T03 §7; reporting=applicable @ D10 T03 §6; notifications=applicable @ D10 T03 §5; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T01 §6; exchange=applicable @ D10 T03 §5; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Interface shell on resolute-ui | D10 T01 §1, D10 T01 §2, D10 T01 §3 | [ ] |
| 2 | §2 | Windows native integration | §1, D10 T01 §3, D10 T01 §5 | [ ] |
| 5 | §5 | Images queue conversion and preview | §2, D10 T02 §1, D10 T01 §6 | [ ] |
| 6 | §6 | Synchronized comparisons | §5, D10 T01 §6, D10 T02 §2, D10 T02 §3, D10 T02 §4 | [ ] |
| 7 | §7 | Preferences and accessible error recovery | §5, §6 | [ ] |
| 8 | §8 | Guided RasterFold demonstration | §6, §7 | [ ] |

---

## 1. Interface shell on resolute-ui

**Requires:** display-session -- The checkpoint captures the shell in every theme and scale and asserts focus and UI Automation in a Windows desktop session; a headless WSL run cannot produce it.

**Job:** A user can recognize, focus and operate a consistent native interface that looks and behaves like the Resolute launcher.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md#window, docs/design/converter.md#geometry, docs/design/converter.md#sections, docs/design/converter.md#icons, docs/design/converter.md#fidelity -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; the tool window of Resolute-SDK `DESIGN.md` section 16 with the Images, Compare and Samples sections, content headers and status bar.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Vendor `resolute-ui` and its Lucide set from `rizonesoft/Resolute-SDK` (`shared/resolute-ui/`, `shared/lucide/`) at a pinned commit into `converter/vendor/resolute-ui/` and `converter/vendor/lucide/`, recorded in `converter/vendor.json` with the commit, the file list, SHA-256 hashes and licence. Done when: a changed or missing vendored file fails `scripts/check-converter.py`, and `AGENTS.md` records the dependency and its reason.
- [ ] Add the `RasterFoldConverter` executable to `converter/CMakeLists.txt` (the CMake build and its driver `scripts/build-converter.py` come from D10 T01 §2). Done when: a clean checkout builds a `RasterFoldConverter` Windows x64 executable as C++23 that statically links `ResoluteUI_static` and `Lucide_static`, with the manifest declaring per-monitor DPI v2, and the codec targets in `scripts/build.py` are unchanged.
- [ ] Add `converter/app/layout.hpp` with the `DESIGN.md` spacing tokens (`SpacingXS` to `SpacingXL` and the 32, 40, 48 steps) and every converter-owned value of the Geometry table of `docs/design/converter.md` for both densities, each built from a token. Done when: no other file under `converter/app/` names a dimension, and the spacing tokens are listed in `docs/converter-building.md` as an upstream candidate for `resolute-ui`.
- [ ] Implement the application shell in `converter/app/main.cpp` and `converter/app/shell.cpp` on `rui::` windows and controls. Done when: the window opens at the contract's default client size and refuses to shrink below its minimum, the caption shows the RasterFold mark from `assets/brand/icon.svg` and the title, the sidebar lists Images, Compare, Samples, Activity, Settings and About in that order with Images selected, each section has its content header with the primary action rightmost, the framework's Activity, Settings and About pages are hosted, the status bar is present, all as `resolute-ui` controls, and no file under `converter/app/` draws a control the library provides or names a colour, font size or spacing literal.
- [ ] Implement the generic controls the pinned library lacks (button, primary button, drop-down, segmented control, numeric field, slider, check box, progress bar) to the Geometry table's sizes for both densities and a shared UI Automation provider base under `converter/app/controls/`. Done when: each draws only through `rui::RenderContext` with `rui::Theme` tokens, shows the 2px accent focus ring, meets the 24x24 hit target at 100%, reports name, role, state and value to UI Automation, and is listed in `docs/converter-building.md` as an upstream candidate.
- [ ] Add UI Automation adapters for every `resolute-ui` control the converter uses (`ListView`, `Toolbar`, `Sidebar`, `StatusBar`, `PopupMenu`, `ContentView`) in `converter/app/controls/uia_adapters.cpp`, wrapping each control without changing how it renders. Done when: a UI Automation client enumerates every item, row, button and menu entry of those controls with a name, role and state, can invoke, select and sort through them, and `tests/converter/test_ui.cpp` fails when an adapter is removed.
- [ ] Bind the shell to the C engine through `converter/app/engine_bridge.cpp`. Done when: interface code reaches jobs, capabilities and results only through the C headers under `converter/include/`, and rendering, input, accessibility and application state have separate ownership.
- [ ] Add the glyphs the Icons table of `docs/design/converter.md` marks "add" to the vendored Lucide set with their sources in `converter/vendor/lucide/icons/SOURCES.md`. Done when: every glyph that table names resolves through `rui::LucideIcons::Resolve` without the `square-dashed` fallback, each action has a visible and accessible label, and the added glyphs are listed in `docs/converter-building.md` as upstream candidates.
- [ ] Add `tests/converter/test_ui.cpp`. Done when: keyboard focus traversal in the contract's tab order, token-only colour and spacing (a literal in `converter/app/` outside `layout.hpp`), the sidebar order and UI Automation names fail on seeded regressions.
- [ ] Register the vendored trees and their hash check in `docs/drift.json`. Done when: editing a vendored file without updating `converter/vendor.json` fails the drift gate.
- [ ] Commit: `"converter: interface shell on resolute-ui"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case ui`; inspect captures in light, dark and high contrast at 100%, 150% and 200% and quote focus/action assertions.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 2. Windows native integration

**Requires:** display-session -- Win32 input, DPI and UI Automation need a driven Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can operate the converter through the Windows platform facilities.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md#window, docs/design/converter.md#geometry, docs/design/converter.md#fidelity -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; the tool window of Resolute-SDK `DESIGN.md` section 16 with the Images, Compare and Samples sections, content headers and status bar.

**Needs:** Windows host (build/test)

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement `converter/app/platform_win32.cpp`. Done when: window lifecycle, per-monitor DPI changes, native file dialogs, clipboard, drag and drop and taskbar progress are wired to the `resolute-ui` shell and the C job contracts.
- [ ] Document the platform boundary in `docs/converter-building.md`. Done when: only system facilities and shipped libraries are used; conversion behaviour stays in the C engine and the interface owns no codec logic.
- [ ] Expose names, roles, state, values and actions for the converter's own controls (preview panes, wipe, difference view) in `converter/app/accessibility.cpp` through UI Automation. Done when: keyboard and screen-reader activation use the same application actions, and status changes are announced.
- [ ] Add `tests/converter/uia_driver.cpp`, built by `converter/CMakeLists.txt`: a UI Automation client that starts the converter, finds controls by AutomationId, invokes them, sets values, reads names, states and announcements, and saves a window capture as PNG. Done when: the Python drivers under `tests/converter/` use it for every driven check, it stops the converter's whole process tree on timeout and teardown, and no third-party Python package is added.
- [ ] Add `tests/converter/test_geometry.py` over the driver. Done when: it parses the Geometry table and the column tables of `docs/design/converter.md`, reads each named region's UI Automation bounding rectangle at 100%, 150% and 200% in both densities, and fails on a difference over 1 DIP for the window, caption, caption buttons, sidebar, content header, list header and rows, and status bar; §5, §6 and §8 extend it with their sections' regions.
- [ ] Add lifecycle/refusal tests in `tests/converter/test_win32.py`. Done when: closing, suspending, resizing, permission denial and display loss cannot leak workers or publish partial files.
- [ ] Record native evidence in `docs/converter-validation.md`. Done when: screenshots and input/accessibility runs name OS, architecture, build revision and tested states.
- [ ] Commit: `"converter: windows native integration"`

**Test checkpoint:** Build the Windows target and drive it through `tests/converter/uia_driver.cpp` in a Windows desktop session; quote observed behaviour separately from fixture and cross-build results.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 5. Images queue conversion and preview

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence in a Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can add images or a folder to a queue, choose each image's output from the supported formats, preview it, convert the queue with per-image progress and error notifications, and save verified results.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md#images, docs/design/converter.md#row-states, docs/design/converter.md#status-bar, docs/design/converter.md#states, docs/design/converter.md#geometry, docs/design/converter.md#fidelity -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; the tool window of Resolute-SDK `DESIGN.md` section 16 with the Images, Compare and Samples sections, content headers and status bar.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Implement the Images section in `converter/app/images.cpp`: the content header (Add images, Add folder, then Convert N or Cancel), the `rui::ListView` with the contract's six columns, widths, sorting, multi-select, row context menu and keyboard actions, and the empty state. Done when: rows virtualize past 10,000 images, Remove from list never touches a file, Convert N counts Ready rows and is disabled at 0 with its tooltip, and every string matches the contract.
- [ ] Implement the add and drop flow in `converter/app/open.cpp` on the native file dialog and drag and drop of §2, with folders expanded to their supported images. Done when: a cancelled dialog leaves state intact, a corrupt or unsupported file becomes an Unsupported or Failed row with its reason, a drag over the section shows the contract's drop outline and text, and the Output format choices come from the capability registry.
- [ ] Implement the image pane in `converter/app/image_pane.cpp` (used by the detail panel preview here and by the comparison pane of §6): fit, zoom and pan over a Direct2D bitmap made with `rui::RenderContext::CreateBitmapFromRGBA` on the `surfaceBase` neutral ground. Done when: large images use bounded preview memory through tiled or downscaled bitmaps, source orientation and colour metadata are respected, per-monitor DPI changes re-lay out without rescaling, and the pane reports its zoom and position to UI Automation.
- [ ] Implement the detail panel in `converter/app/detail_panel.cpp` with capability-driven settings bound in `converter/app/settings.cpp` using the controls of §1: preview, Output format, Effort, Threads, Destination and the facts line, at the contract's widths for both densities and moving under the list below 960 DIP of content width. Done when: each visible setting has a real adapter consumer, an incompatible output format is disabled with the reason from the registry, a multi-selection shows Mixed and applies a change to every selected row, the no-selection text matches the contract, and no setting names a colour, font size or spacing literal.
- [ ] Implement queue conversion in `converter/app/convert.cpp` over the bounded scheduler of D10 T01 §3, with per-row progress in the Result column, taskbar progress across the queue, the contract's Images status bar texts and one Activity line per finished, cancelled or refused job. Done when: every row state of the Row states table shows its glyph, token and word in light, dark and high contrast, rotation stops under reduced motion, each announced state is spoken through UI Automation with the contract's wording, Cancel stops every running job, Retry re-queues one row, and a stale job cannot replace the current preview.
- [ ] Implement save and reveal actions in `converter/app/save.cpp`. Done when: export reports the actual destination, exactness and size growth or savings without replacing the input.
- [ ] Add `tests/converter/test_images.py` over the driver of §2 and extend `tests/converter/test_geometry.py` with the Images regions (header buttons, list columns, detail panel and its fields, empty state). Done when: add-to-save for one image and for a folder, cancel-at-each-stage, mismatch, permission denial, out-of-memory and growth paths have driven assertions, the geometry check passes within 1 DIP, and a capture per row state is recorded beside `docs/design/converter-reference.html` in `docs/converter-validation.md`.
- [ ] Commit: `"converter: images queue conversion and preview"`

**Test checkpoint:** Drive `tests/converter/test_images.py` and `tests/converter/test_geometry.py` in a Windows desktop session; quote the geometry result, capture each row state in light, dark and high contrast at both densities, and verify a fresh decode under its format-specific contract.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 6. Synchronized comparisons

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence in a Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can compare RasterFold and other codecs side by side using the same source and preservation policy.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md#compare, docs/design/converter.md#comparison, docs/design/converter.md#row-states, docs/design/converter.md#geometry, docs/design/converter.md#fidelity -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; the tool window of Resolute-SDK `DESIGN.md` section 16 with the Images, Compare and Samples sections, content headers and status bar.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Implement synchronized image-coordinate navigation in `converter/app/compare.cpp` over two image panes of §5. Done when: zoom, pan, fit and actual-pixel views keep the same source point aligned at every DPI.
- [ ] Implement the comparison toolbar and the side by side, wipe and difference modes in `converter/app/compare.cpp` to the contract (segmented control, Fit, Actual pixels, Zoom out, Zoom in, Reset view, the wipe position field, the background choice, the pane labels and the wipe handle). Done when: the wipe moves 1% on Left and Right, 10% with Shift, ends on Home and End, and is a UI Automation range value, difference mode has a labelled scale and a changed-sample count carried as text, not colour alone, and below 600 DIP of window width side by side switches to wipe keeping the zoom centre.
- [ ] Implement the Compare section in `converter/app/results.cpp`: the content header (source name, Open, Run again, then Save selected) and the codec `rui::ListView` from the engine's report (`converter/src/report.c`) with the contract's six columns, widths, relative-size bar and visible-row limit. Done when: sizes, effort, thread count, timing and verification status are visible and sortable, unverified, failed and unsupported results sort after every verified one whatever the column, Save selected is disabled until a verified row is selected, and the Compare status bar text matches the contract.
- [ ] Add a source and result switch for alpha and colour preview in `converter/app/compare.cpp`. Done when: background and display transform changes cannot alter the encoded samples.
- [ ] Add `tests/converter/test_compare.py` over the driver of §2 and extend `tests/converter/test_geometry.py` with the Compare regions (codec list columns, comparison toolbar, pane minimum, wipe handle). Done when: alignment, ranking of unverified results, mismatch, unsupported codec, larger output and stale-result regressions fail, and the geometry check passes within 1 DIP.
- [ ] Commit: `"converter: synchronized comparisons"`

**Test checkpoint:** Drive comparisons on the same sample in a Windows desktop session and quote pixel verification plus keyboard and UI Automation navigation evidence from the driver; the Narrator run is owned by §7.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 7. Preferences and accessible error recovery

**Requires:** display-session -- Surface checkpoints require real input, preview and accessibility evidence in a Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can keep useful settings, reset them, and recover accessibly from every failure.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md#activity-settings-and-about, docs/design/converter.md#strings, docs/design/converter.md#visual-rules, docs/design/converter.md#fidelity -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; the tool window of Resolute-SDK `DESIGN.md` section 16 with the Images, Compare and Samples sections, content headers and status bar.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Implement a versioned preferences file in `converter/src/preferences.c` (engine side, C11). Done when: theme mode (follow-system, light, dark), density, last safe directory and memory budget survive restart with named consumers.
- [ ] Implement the converter's page in the framework's Settings section, with exactly the options the contract lists, plus reset and corrupt-settings recovery, in `converter/app/preferences.cpp`. Done when: invalid values fall back safely, reset requires no manual file deletion, and a theme change crossfades through `rui::AnimationManager` unless reduced motion is set.
- [ ] Complete localization-ready labels in `converter/app/strings.cpp`, with the English pack holding every string the contract quotes, verbatim. Done when: a test compares the English pack with the contract's quoted strings, 40% expanded labels move secondary header actions into the overflow menu and never the primary, long labels, Unicode filenames and right-to-left layout fixtures do not clip essential actions.
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

**Design:** docs/design/converter.md#samples, docs/design/converter.md#states, docs/design/converter.md#geometry, docs/design/converter.md#fidelity -- states: all in spec -- themes: all four -- density: both

**Treatment:** Resolute launcher treatment: Direct2D surfaces, theme tokens, Lucide glyphs with labels, and persistent progress.

**Chrome:** Resolute's custom Direct2D caption and window chrome; the tool window of Resolute-SDK `DESIGN.md` section 16 with the Images, Compare and Samples sections, content headers and status bar.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies. Interface code is C++23 under `converter/app/` on the `resolute-ui` shell of §1 and reaches the engine only through the C headers under `converter/include/`; see the stack facts under Inputs.

- [ ] Add small diverse licensed images under `converter/assets/samples/`. Done when: each has provenance and covers photographs, text, graphics and transparent/high-depth capability refusals.
- [ ] Implement the Samples section and Try sample in `converter/app/demo.cpp`: the `rui::ListView` with the contract's four columns and provenance tooltips, Try sample as its primary action and on the Images empty state. Done when: Try sample opens the selected sample in Compare through the real import, encode, verify and compare flow of §5 and §6 without fabricated statistics, and the Samples status bar text matches the contract.
- [ ] Add contextual explanations in `converter/app/demo.cpp`. Done when: the user can distinguish losslessness from size savings and see current RasterFold RGB8 limitations.
- [ ] Add `tests/converter/test_demo.py` over the driver of §2 and extend `tests/converter/test_geometry.py` with the Samples regions. Done when: the geometry check passes within 1 DIP, offline first launch, sample conversion and fresh-decode output verification work on a clean Windows install.
- [ ] Commit: `"converter: guided rasterfold demonstration"`

**Test checkpoint:** Drive first-launch Try sample with networking disabled in a Windows desktop session and compare saved output against its bundled source.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## Verification

- [ ] Run `python3 scripts/check-converter.py --all-targets` once this file adds its gates; report driven Windows desktop coverage separately from cross-builds.
- [ ] Run `python3 scripts/todo-graph.py validate`, `plan --check` and `python3 scripts/drift-check.py`; each exits 0.
