# Native converter design contract

This is the planned native surface, owned by `D10 T03`; it does not describe an implemented application. The converter is a Windows x64 desktop application built on the Resolute launcher's stack (operator decision 2026-11-11): C++23 on llvm-mingw with CMake and Ninja, the statically linked `resolute-ui` library (Direct2D and DirectWrite, namespace `rui::`) and its Lucide glyph set, both now in `rizonesoft/Resolute-SDK` under `shared/` (moved from the launcher repository on 2026-11-15). Resolute-SDK's `DESIGN.md` is the governing design contract for rendering, colour tokens, typography, spacing, motion, iconography, window chrome, accessibility and tone; this document adds only what is specific to an image converter, and where the two disagree `DESIGN.md` wins. A converter surface never draws a control `resolute-ui` provides and never hardcodes a colour, font size or spacing value. User-supplied SVG is not an image decoder or executable input.

This file is the contract for a 1:1 implementation: every region, dimension, token, glyph and string the converter owns is stated here, and an implementation matches it within the tolerance in [Fidelity](#fidelity). [`converter-reference.html`](converter-reference.html) in this folder draws the same screens as a visual reference; where the drawing and this file disagree, this file wins and the drawing is corrected. Metrics were read from `resolute-ui` at Resolute-SDK `bddf5860` (2026-11-22); `D10 T03 §1` re-reads them at the commit it pins and corrects this file in the same commit if they moved.

## Decisions

- **Layout (operator, 2026-11-22).** Chosen from three mockups (a queue, a single-image workbench and a codec scoreboard): the Resolute tool window of `DESIGN.md` section 16 with the queue as the first section and the scoreboard as Compare. Reversal cost: re-authoring this file and the shell, flow, comparison and sample items of `D10 T03`; nothing is built yet.
- **Application icon (operator, 2026-11-22).** The converter keeps RasterFold's own mark, `assets/brand/icon.svg` (the folded pixel on its rounded navy tile), in the caption, taskbar and `.ico`. This is a recorded exception to `DESIGN.md` section 14, which asks for free-standing tool icons with no tile; the mark stays the product's single identity across the codec, the site and the converter. Reversal cost: drawing a free-standing variant through Resolute-SDK's brand kit and regenerating the `.ico`.

## Window

| Element | Owner | Value at 100% |
| ------- | ----- | ------------- |
| Default client size | converter | 1280 x 800 DIP, centred on the primary monitor on first run; afterwards remembered per monitor (`DESIGN.md` section 5) |
| Minimum client size | converter | 720 x 480 DIP |
| Caption | `resolute-ui` | 32 DIP tall; RasterFold mark 16 x 16 DIP at `SpacingMD` from the left edge, vertically centred; title "RasterFold Converter" in Body 13 Regular, `text`, `SpacingSM` after the mark |
| Caption buttons | `resolute-ui` | Minimize, maximize and close, each 46 x 32 DIP with a 10 DIP glyph (the Windows 11 metrics); hover fills `surfaceHover`, close hover uses the system close red with white glyph; snap layout flyout on maximize hover |
| Backdrop and corners | `resolute-ui` | `Theme::ApplyBackdrop` and rounded Windows 11 corners (`DESIGN.md` section 8) |
| Taskbar | converter | Progress across all running jobs of the active section; nothing decorative |

The caption, sidebar, list and status bar are `resolute-ui`'s. If the pinned library misses a metric in this file, the fix goes upstream into Resolute-SDK and the converter never draws its own.

## Geometry

Values are device-independent pixels (DIP) at 100% and scale with `rui::Dpi` per monitor. Converter-owned values live in one header, `converter/app/layout.hpp`, as named constants built from the `DESIGN.md` section 5 spacing tokens (`SpacingXS` 4, `SpacingSM` 8, `SpacingMD` 12, `SpacingLG` 16, `SpacingXL` 24, then 32, 40, 48); the pinned library has no spacing tokens yet, so `layout.hpp` declares them under those names and they are an upstream candidate listed in `docs/converter-building.md`. Rows marked `resolute-ui` record the library's own constant for the fidelity check; the converter never overrides them.

| Region | Owner | Normal density | Compact density |
| ------ | ----- | -------------- | --------------- |
| Sidebar width (expanded) | `resolute-ui` (`Sidebar::BASE_WIDTH`) | 200 | 200 |
| Sidebar item height | `resolute-ui` (`Sidebar::BASE_ITEM_HEIGHT`) | 40 | 40 |
| Sidebar collapse | `resolute-ui` | to icons below 600 DIP window width | same |
| Content header height | converter | 56 | 48 |
| Content header padding | converter | `SpacingLG` left and right | `SpacingMD` |
| Content header title | converter | Subtitle 16 SemiBold, `text`, truncates with ellipsis and tooltip | same |
| Header button | converter control | 32 tall, `SpacingMD` horizontal padding, 16 DIP glyph then `SpacingSM` then label in Body 13; gap `SpacingSM` between buttons | 28 tall |
| Primary button | converter control | as header button, fill `accent`, label and glyph `background`, SemiBold; always the rightmost header action | same |
| List header height | `resolute-ui` (`ListView::BASE_HEADER_HEIGHT`) | 32 | 32 |
| List row height | `resolute-ui` (`ListView::BASE_ROW_HEIGHT`) | 28 | 28 until the library exposes a compact row, then 24 |
| List cell padding | `resolute-ui` (`ListView::BASE_PADDING_X`) | 8 | 8 |
| Detail panel width | converter | 280, `SpacingLG` padding, `SpacingMD` between fields | 256, `SpacingMD` padding, `SpacingSM` between fields |
| Detail panel preview | converter | full panel width minus padding, 160 tall, image fitted, neutral ground | 128 tall |
| Field label | converter | Caption 11 Regular, `textSecondary`, `SpacingXS` above its control | same |
| Field control | converter control | 32 tall, 1 DIP `border`, radius 4, Body 13 | 28 tall |
| Comparison toolbar | converter | 40 tall, `SpacingMD` padding, items `SpacingSM` apart | 36 |
| Codec list visible rows | converter | up to 6 rows, then it scrolls; the comparison pane takes the rest | up to 5 |
| Comparison pane minimum | converter | 240 tall | 200 |
| Pane label | converter | Caption 11 SemiBold on `scrim`, `SpacingSM` inset from the pane corner, white text | same |
| Wipe handle | converter control | 2 DIP line in `accent`; 24 x 24 DIP grab target centred on it with the position in Caption 11 | same |
| Status bar height | `resolute-ui` (`StatusBar::BASE_HEIGHT`) | 26 | 26 |
| Focus ring | all controls | 2 DIP `accent`, 1 DIP offset | same |

Every hit target is at least 24 x 24 DIP. Below 960 DIP of content width the detail panel moves under the list at full width and its preview keeps its height; below 600 DIP of window width the sidebar collapses (library behaviour) and the comparison pane switches from side by side to wipe, keeping the common zoom centre.

## Sections

The sidebar lists Images, Compare and Samples, then the framework's Activity, Settings and About, with those names and in that order. Images opens selected. Each section is one content header and one body; nothing is drawn above the content header or below the status bar.

### Images

The first section: the images the tool acts on, one row each.

- **Header:** title "Images"; actions Add images, Add folder, then the primary action. The primary reads "Convert N" (N = rows in Ready) and is disabled with the tooltip "Add images to convert" when N is 0; while any job runs it reads "Cancel" and cancels every running job of the section.
- **List:** `rui::ListView` in Details mode, virtualized, multi-select, sortable by every column. Columns, left to right:

| Column | Width | Alignment | Content |
| ------ | ----: | --------- | ------- |
| Name | fills, minimum 200 | left | file name; full path in the tooltip and the UI Automation help text |
| Size | 96 | right | source file size, tabular figures, `MB` or `KB` with one decimal |
| Dimensions | 112 | right | width x height in pixels |
| Output | 96 | left | the target format's short name (FOLD, PNG, WebP, JPEG XL, TIFF) |
| Result | 112 | right | output size when finished; "N%" progress while encoding; empty otherwise |
| State | 144 | left | the state glyph (16 DIP) then the state word, from [Row states](#row-states) |

- **Detail panel:** on the right, for the selected row (or the first selected of several). Top to bottom: preview, then the fields Output format (drop-down of formats the capability registry allows for this source; a disabled entry states why), Effort (drop-down: Fast, Balanced, Thorough), Threads (drop-down: Automatic, then 1 to the logical processor count), Destination (drop-down: Beside the original, Choose folder...), and a line of facts in Caption 11 `textSecondary`: source format, bit depth, alpha, colour profile. With several rows selected, a field whose values differ shows "Mixed" and a change applies to all selected rows. With nothing selected the panel shows "Select an image to see its settings" in Body 13 `textSecondary`, centred.
- **Row actions:** context menu (`rui::PopupMenu`) and keyboard: Convert (Enter), Cancel, Retry, Compare (opens the row's source in Compare), Show in folder, Remove from list (Delete). Remove never deletes a file.
- **Drop:** files or folders dropped anywhere on the section add rows; a dragged-over section shows a 2 DIP `accent` inset outline and "Drop to add images" centred in Subtitle 16.

### Row states

Each state is a glyph and a word, never colour alone. Glyph colour comes from the token named; the word is `text`.

| State | Word | Glyph | Glyph token | Announced |
| ----- | ---- | ----- | ----------- | --------- |
| ready | Ready | `circle` | `textSecondary` | no |
| importing | Importing | `loader-2`, rotating | `info` | "Importing name" |
| encoding | Encoding | `loader-2`, rotating | `info` | at start and on finish |
| verifying | Verifying | `loader-2`, rotating | `info` | at start |
| completed | Verified | `check-circle` | `success` | "name verified, N MB, saved M%" |
| completed, larger | Verified, larger | `alert-triangle` | `warning` | "name verified, output N% larger than the source" |
| cancelled | Cancelled | `circle-slash` | `textSecondary` | "name cancelled" |
| unsupported | Unsupported | `ban` | `warning` | with the reason |
| permission-denied | No access | `lock` | `error` | with the path and the operation |
| out-of-memory | Out of memory | `memory-stick` | `error` | with the budget that was exceeded |
| failed | Failed | `x-circle` | `error` | with the failing operation |

Rotation stops under reduced motion; the glyph stays still and the word carries the state. The status bar and the row tooltip carry the full reason for every error state, and Retry is offered on each.

### Compare

One source encoded by every bundled codec under the same preservation policy.

- **Header:** title is the source file name; actions Open, Run again, then the primary "Save selected" (disabled until a verified row is selected).
- **Codec list:** `rui::ListView` in Details mode, single select, one row per bundled codec that accepts the source. Default sort is output size ascending; an unverified, failed or unsupported result sorts after every verified one whatever the sort column, so it can never rank as successful. Columns:

| Column | Width | Alignment | Content |
| ------ | ----: | --------- | ------- |
| Codec | fills, minimum 160 | left | codec name and mode ("JPEG XL lossless") |
| Output | 112 | right | output size |
| Relative size | 160 | left | a bar 6 DIP tall, radius 3, track `surfaceActive`, fill `textSecondary` (`accent` on the selected row), length proportional to output over source size, then the percentage in Caption 11 |
| Time | 80 | right | encode wall time, one decimal, seconds |
| Threads | 72 | right | threads the scheduler admitted |
| Exactness | 144 | left | glyph and word from [Row states](#row-states) |

- **Comparison toolbar:** a segmented control Side by side, Wipe, Difference; then Fit, Actual pixels, Zoom out, Zoom in, Reset view (glyph and label each); and in Wipe a numeric position field (0 to 100, percent). Right-aligned: a background choice for transparent images (Checkerboard, Black, White) that never alters encoded samples.
- **Comparison pane:** the original against the selected codec's freshly decoded result, on a neutral ground (`surfaceBase`) so interface colour never biases the comparison. Labels "Original" and the codec name stay visible on their panes. Rules in [Comparison](#comparison).

### Samples

- **Header:** title "Samples"; primary action "Try sample", which opens the selected sample in Compare through the real import, encode and verify flow, with no precomputed figures.
- **List:** `rui::ListView` in Details mode, columns Name (fills), Kind (128: Photograph, Text, Graphics, Transparency, High depth), Dimensions (112, right), Licence (128). Provenance in the row tooltip.

### Activity, Settings and About

The framework's pages, hosted unchanged. Activity is fed by the engine's job reports, newest first, one line per finished, cancelled or refused job with its time. Settings adds one converter page with exactly these options: Theme (Follow system, Light, Dark), Density (Normal, Compact), Default output format, Default effort, Memory budget, Default destination, and Reset converter settings.

## Status bar

Left segment, in words, from the active section:

| Section | Text |
| ------- | ---- |
| Images, idle | "N images · V verified · R ready" (counts that are zero are left out) |
| Images, running | "Converting K of N · name, stage" |
| Compare | "C codecs on one source · E exact · effort E · T threads" |
| Samples | "N licensed samples" |

The right segment shows the selected item's destination path in Cascadia Mono, truncated in the middle with the full path in the tooltip. The status bar's notification toast announces job completion and errors through `UiaRaiseNotificationEvent`.

## States

Implement empty, importing, ready, encoding, verifying, completed, cancelled, unsupported, permission-denied, out-of-memory and failed states. Empty (no rows in Images) shows, centred in the list area, the `images` glyph at 48 DIP in `textSecondary`, "Add images to convert" in Subtitle 16, "Drop files or folders here, or try a bundled sample." in Body 13 `textSecondary`, and the buttons Add images and Try sample. Busy states report the current stage with accessible announcements. Errors preserve the source and settings, identify the failing operation and offer a relevant retry. A stale preview never appears under a new job's result. Results identify codec, exactness verification, elapsed work, output size, savings or growth, and destination.

## Comparison

Side-by-side views share zoom and pan in image coordinates. Offer fit, actual pixels and reset. The wipe has keyboard controls (Left and Right move 1%, Shift moves 10%, Home and End) and a numeric position exposed as a UI Automation range value; difference mode has a visible scale and a text summary of changed samples ("0 of 72,000,000 samples differ"). Background transparency choices never alter encoded pixels. Distinguish original image size from encoder input size and output size, and distinguish preview color conversion from lossless verification. Disabled output formats explain the unsupported depth, alpha, animation or metadata requirement.

## Strings

Every user-facing string resolves from the language pack (`converter/app/strings.cpp`); the English values are the ones in this file, quoted exactly. Labels are sentence case ("Add images", "Save selected"). Labels survive 40% localization expansion without clipping an action: a header that cannot fit moves its secondary actions into the overflow menu, never the primary.

## Icons

Interface glyphs are Lucide SVGs rendered through Direct2D by `resolute-ui`, coloured from theme tokens. A glyph the vendored Lucide set lacks is added to that set with its source recorded in `converter/vendor/lucide/icons/SOURCES.md`, never drawn as a one-off, and is an upstream candidate for Resolute-SDK. No webview, icon font or bitmap icon set per size.

| Use | Glyph | In the set at `bddf5860` |
| --- | ----- | ------------------------ |
| Images (sidebar), empty state | `images` | add |
| Compare (sidebar) | `columns-2` | add |
| Samples (sidebar) | `library` | add |
| Activity (sidebar) | `activity` | add |
| Settings (sidebar) | `settings` | yes |
| About (sidebar) | `badge-info` | yes |
| Add images | `image-plus` | add |
| Add folder | `folder-plus` | add |
| Convert | `play` | add |
| Cancel | `square` | add |
| Retry, Run again | `refresh` | yes |
| Open | `folder-open` | yes |
| Save selected | `save` | yes |
| Show in folder | `folder-closed` | yes |
| Remove from list | `trash` | yes |
| Fit | `maximize` | add |
| Actual pixels | `scan` | add |
| Zoom in, Zoom out | `zoom-in`, `zoom-out` | add |
| Reset view | `rotate-ccw` | add |
| Side by side, Wipe, Difference | `columns-2`, `square-split-horizontal`, `diff` | add |
| Row states | `circle`, `loader-2`, `check-circle`, `alert-triangle` | yes |
| Row states | `circle-slash`, `ban`, `lock`, `memory-stick`, `x-circle` | add |

The application icon is RasterFold's own (see [Decisions](#decisions)). Captures at every theme and scale are test evidence, not the authoritative assets.

## Visual rules

Follow Resolute-SDK's `DESIGN.md`: follow-system theme with explicit light and dark, Windows high contrast honoured (system colours win over the palette), the accent read from the system, token-only colour, per-monitor DPI v2 with layout in scalable units from 100% upward, a 2px accent focus ring, logical tab order (sidebar, content header left to right, list, detail panel or comparison toolbar, pane, status bar), reduced-motion behaviour, and labels that survive localization expansion. Motion uses the shared clock: selection moves with `EaseOutCubic` over 150ms, hover over 120ms, the theme crossfades over 300ms, and reduced motion stops all of it. Normal and compact density use the same tokens. Use neutral image backgrounds so application colours do not bias visual comparisons. Every custom-drawn control exposes UI Automation names, roles, values, focus and actions, and status changes are announced; a canvas with no accessibility tree fails acceptance.

## Fidelity

An implementation matches this file when:

1. **Tokens are exact.** Every colour, type style and spacing value in converter code is a named token or a `layout.hpp` constant built from one; `tests/converter/test_ui.cpp` fails on a literal.
2. **Geometry is within 1 DIP.** The test driver reads each region's UI Automation bounding rectangle at 100%, 150% and 200%, divides by the scale, and compares it with the [Geometry](#geometry) table and the column widths above, parsed from this file; a difference over 1 DIP fails.
3. **Strings and glyphs are exact.** Labels, state words and glyph names match this file in the English pack.
4. **Captures are recorded.** Each section and state is captured in light, dark and high contrast at both densities and at 100%, 150% and 200%, under `build/converter/captures/`, and quoted in `docs/converter-validation.md` beside [`converter-reference.html`](converter-reference.html). Captures are evidence for review, not a pixel gate; the measured checks above are the gate.

## Delivery evidence

Drive each state on Windows x64. Record keyboard-only and screen-reader runs, resize/scaling screenshots, cancellation and no-overwrite checks, decoded comparisons and dependency/import audits from clean target installations. No fixture or cross-build is described as a driven native UI test.
