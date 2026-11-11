# Native converter design contract

This is the planned native surface, owned by `D10 T03`; it does not describe an implemented application. The converter is a Windows x64 desktop application built on the Resolute launcher's stack (operator decision 2026-11-11): C++23 on llvm-mingw with CMake and Ninja, Resolute's statically linked `resolute-ui` library (Direct2D and DirectWrite, namespace `rui::`) and its Lucide glyph set. Resolute's `DESIGN.md` is the governing design contract for rendering, colour tokens, typography, spacing, motion, iconography, window chrome, accessibility and tone; this document adds only what is specific to an image converter, and where the two disagree `DESIGN.md` wins. A converter surface never draws a control `resolute-ui` provides and never hardcodes a colour, font size or spacing value. User-supplied SVG is not an image decoder or executable input.

## Layout

At desktop widths, use a compact application bar, an input/preview workspace, a settings column and a persistent result footer. The main action says Convert until work starts, then Cancel. Keep original and decoded-result labels visible over synchronized panes. At narrow window widths stack preview and controls with a persistent progress/cancel area; comparison switches between a split and a swipe view without losing the common zoom center. No action is represented by an unexplained icon alone.

## States

Implement empty, importing, ready, encoding, verifying, completed, cancelled, unsupported, permission-denied, out-of-memory and failed states. Empty offers Open image and Try sample. Busy states report the current stage with accessible announcements. Errors preserve the source and settings, identify the failing operation and offer a relevant retry. A stale preview never appears under a new job's result. Results identify codec, exactness verification, elapsed work, output size, savings or growth, and destination.

## Comparison

Side-by-side views share zoom and pan in image coordinates. Offer fit, actual pixels and reset. The wipe has keyboard controls and a numeric position; difference mode has a visible scale and a text summary of changed samples. Background transparency choices never alter encoded pixels. Distinguish original image size from encoder input size and output size, and distinguish preview color conversion from lossless verification. Disabled output formats explain the unsupported depth, alpha, animation or metadata requirement.

## Visual rules

Follow Resolute's `DESIGN.md`: follow-system theme with explicit light and dark, Windows high contrast honoured (system colours win over the palette), the accent read from the system, token-only colour, per-monitor DPI v2 with layout in scalable units from 100% upward, a 2px accent focus ring, logical tab order, reduced-motion behaviour, and labels that survive localization expansion. Comfortable and compact density use the same tokens. Use neutral image backgrounds so application colours do not bias visual comparisons. Every custom-drawn control exposes UI Automation names, roles, values, focus and actions, and status changes are announced; a canvas with no accessibility tree fails acceptance.

## Icons

Interface glyphs are Lucide SVGs rendered through Direct2D by `resolute-ui`, coloured from theme tokens, for open, save, compare, zoom, fit, play, cancel, retry, settings and information. A glyph the vendored Lucide set lacks is added to that set with its source recorded, never drawn as a one-off. The application icon is RasterFold's own. No webview, icon font or bitmap icon set per size. Captures at every theme and scale are test evidence, not the authoritative assets.

## Delivery evidence

Drive each state on Windows x64. Record keyboard-only and screen-reader runs, resize/scaling screenshots, cancellation and no-overwrite checks, decoded comparisons and dependency/import audits from clean target installations. No fixture or cross-build is described as a driven native UI test.
