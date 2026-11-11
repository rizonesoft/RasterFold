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

> **Goal:** A modern, accessible C interface previews single images, compares real codec results and demonstrates RasterFold without extra installs.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `include/rasterfold.h` and the four-target build exist. There is no native converter application, GUI or bundled third-party codec source yet; D10 T01 §1 (2026-11-09) added the pixel contract, the capability registry in `converter/`, and its validator, with every format still `planned`. These sections implement the operator-approved native C application with bundled codec source and no additional user installs.
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
- Clean Windows/Linux x64 installations run the shipped application without downloading a runtime or codec. System OS/display services remain prerequisites, documented per target.

**Adjacency:** list=applicable @ D10 T03 §5; document=applicable @ D10 T03 §5; settings=applicable @ D10 T03 §7; reporting=applicable @ D10 T03 §6; notifications=applicable @ D10 T03 §5; permissions=applicable @ D10 T01 §5; audit=applicable @ D10 T01 §6; exchange=applicable @ D10 T03 §5; reverse=applicable @ D10 T01 §4

**Adjacency rationale:** The engine, adapters, native surface and delivery each have one owner. Cross-file dependencies supply the complete import, settings, notification, report, export and reverse-conversion workflow.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | C interface components and SVG icons | D10 T01 §1 | [ ] |
| 2 | §2 | Windows native integration | §1, D10 T01 §3, D10 T01 §5 | [ ] |
| 3 | §3 | Linux native integration | §1, D10 T01 §3, D10 T01 §5 | [ ] |
| 5 | §5 | Single-image conversion and preview | §2, §3, D10 T02 §1, D10 T01 §6 | [ ] |
| 6 | §6 | Synchronized comparisons | §5, D10 T01 §6, D10 T02 §2, D10 T02 §3, D10 T02 §4 | [ ] |
| 7 | §7 | Preferences and accessible error recovery | §5, §6 | [ ] |
| 8 | §8 | Guided RasterFold demonstration | §6, §7 | [ ] |

---

## 1. C interface components and SVG icons

**Job:** A user can recognize, focus and operate a consistent native interface with original scalable icons.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Native, scalable image workspace with restrained color, labeled SVG actions and persistent progress.

**Chrome:** OS window and accessibility integration; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement C component, focus and action interfaces in `converter/ui/ui.h`. Done when: rendering, input, accessibility and application state have separate ownership.
- [ ] Implement theme and density tokens in `converter/ui/theme.c`. Done when: all four themes and both densities use the same semantic roles.
- [ ] Author original icons in `converter/assets/icons/`. Done when: each action has an SVG path and a visible/accessibility label.
- [ ] Add `scripts/build-icons.py` to validate and compile SVG paths into C arrays. Done when: scripts, external resources and unsupported drawing operations fail the build.
- [ ] Implement the bounded path renderer in `converter/ui/svg.c`. Done when: icons render at 100%, 150%, 200% and 300% with no runtime SVG library.
- [ ] Add `tests/converter/test_ui.c` and icon fixtures. Done when: keyboard focus traversal and invalid SVG rejection fail on seeded regressions.
- [ ] Register generated icon arrays and their generator check in `docs/drift.json`. Done when: changing an SVG without rebuilding its C data fails the drift gate.
- [ ] Commit: `"converter: c interface components and svg icons"`

**Test checkpoint:** Run `python3 scripts/check-converter.py --case ui`; inspect rendered theme/scale grids and quote focus/action assertions.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 2. Windows native integration

**Requires:** display-session -- Win32 input, DPI and UI Automation need a driven Windows desktop session. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can operate the converter through the win32 platform facilities.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Native, scalable image workspace with restrained color, labeled SVG actions and persistent progress.

**Chrome:** OS window and accessibility integration; shared application bar, comparison panes and result footer.

**Needs:** Windows host (build/test)

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement `converter/platform/win32.c`. Done when: Win32 lifecycle, per-monitor DPI, native dialogs, clipboard and UI Automation are wired to the C UI and job contracts.
- [ ] Document the platform boundary in `docs/converter-building.md`. Done when: only system facilities and shipped libraries are used; application behavior remains C.
- [ ] Expose names, roles, state, values and actions in `converter/platform/win32_accessibility.c`. Done when: keyboard, touch and screen-reader activation use the same application actions.
- [ ] Add lifecycle/refusal tests in `tests/converter/test_win32.py`. Done when: closing, suspending, resizing, permission denial and display loss cannot leak workers or publish partial files.
- [ ] Record native evidence in `docs/converter-validation.md`. Done when: screenshots and input/accessibility runs name OS, architecture, build revision and tested states.
- [ ] Commit: `"converter: windows native integration"`

**Test checkpoint:** Build the native target and drive win32 interactions on a real desktop/device; quote observed behavior separately from fixture and cross-build results.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 3. Linux native integration

**Requires:** linux-display -- Wayland, X11 and accessibility need driven Linux display sessions. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can operate the converter through the linux platform facilities.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Native, scalable image workspace with restrained color, labeled SVG actions and persistent progress.

**Chrome:** OS window and accessibility integration; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement `converter/platform/linux.c`. Done when: Wayland and X11 backends with packaged non-system support libraries, display negotiation, native file selection and accessibility are wired to the C UI and job contracts.
- [ ] Document the platform boundary in `docs/converter-building.md`. Done when: only system facilities and shipped libraries are used; application behavior remains C.
- [ ] Expose names, roles, state, values and actions in `converter/platform/linux_accessibility.c`. Done when: keyboard, touch and screen-reader activation use the same application actions.
- [ ] Add lifecycle/refusal tests in `tests/converter/test_linux.py`. Done when: closing, suspending, resizing, permission denial and display loss cannot leak workers or publish partial files.
- [ ] Record native evidence in `docs/converter-validation.md`. Done when: screenshots and input/accessibility runs name OS, architecture, build revision and tested states.
- [ ] Commit: `"converter: linux native integration"`

**Test checkpoint:** Build the native target and drive linux interactions on a real desktop/device; quote observed behavior separately from fixture and cross-build results.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 5. Single-image conversion and preview

**Requires:** display-session, linux-display -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can browse a list of supported formats, preview one image, receive progress and error notifications, and save a verified result.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Native, scalable image workspace with restrained color, labeled SVG actions and persistent progress.

**Chrome:** OS window and accessibility integration; shared application bar, comparison panes and result footer.

Build in the listed order. This section adds the named test driver before using its checkpoint; no existing gate is claimed for future files. Keep RasterFold codec artifacts independent from converter dependencies.

- [ ] Implement the open/drop flow in `converter/ui/open.c`. Done when: cancelled dialogs leave state intact and corrupt/unsupported files produce recoverable errors.
- [ ] Implement preview fit, zoom and pan in `converter/ui/preview.c`. Done when: large images use bounded preview memory and source orientation/color metadata are respected.
- [ ] Bind capability-driven settings in `converter/ui/settings.c`. Done when: each visible setting has a real adapter consumer and incompatible output formats explain why.
- [ ] Implement progress, cancel, retry and result views in `converter/ui/convert.c`. Done when: every state in the design spec is visible and a stale job cannot replace the current preview.
- [ ] Implement save/reveal actions in `converter/ui/save.c`. Done when: export reports the actual destination, exactness and size growth/savings without replacing the input.
- [ ] Add `tests/converter/test_single_flow.py`. Done when: open-to-save, cancel-at-each-stage, mismatch, permission denial and growth paths have driven assertions.
- [ ] Commit: `"converter: single-image conversion and preview"`

**Test checkpoint:** Drive `tests/converter/test_single_flow.py` on Windows and Linux; capture each design state and verify a fresh decode under its format-specific contract.

**Freeze check:** Existing `.fold` v2 files and the oracle remain unchanged. Each new output receives a fresh bundled decode and the format-specific verification contract before acceptance; independent fixture coverage is reported separately; input and existing destination bytes survive refusal, cancellation and failure.

## 6. Synchronized comparisons

**Requires:** display-session, linux-display -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can compare RasterFold and other codecs side by side using the same source and preservation policy.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Native, scalable image workspace with restrained color, labeled SVG actions and persistent progress.

**Chrome:** OS window and accessibility integration; shared application bar, comparison panes and result footer.

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

**Requires:** display-session, linux-display -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A user can keep useful settings, reset them, and recover accessibly from every failure.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Native, scalable image workspace with restrained color, labeled SVG actions and persistent progress.

**Chrome:** OS window and accessibility integration; shared application bar, comparison panes and result footer.

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

**Requires:** display-session, linux-display -- Surface checkpoints require real input, preview and accessibility evidence across advertised targets. Source checkpoint audit: docs/todo-vision-audit.md.

**Job:** A new user can try RasterFold and compare codecs immediately using licensed bundled samples.

**Fidelity:** Native converter -- implement the committed surface contract and all failure states.

**Design:** docs/design/converter.md -- states: all in spec -- themes: all four -- density: both

**Treatment:** Native, scalable image workspace with restrained color, labeled SVG actions and persistent progress.

**Chrome:** OS window and accessibility integration; shared application bar, comparison panes and result footer.

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
