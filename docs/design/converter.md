# Native converter design contract

This is the planned native surface, owned by `D10 T03`; it does not describe an implemented application. All assets are original SVG source compiled into C path data. The native C renderer accepts only this build-time icon vocabulary; user-supplied SVG is not an image decoder or executable input.

## Layout

At desktop widths, use a compact application bar, an input/preview workspace, a settings column and a persistent result footer. The main action says Convert until work starts, then Cancel. Keep original and decoded-result labels visible over synchronized panes. At narrow window widths stack preview and controls with a persistent progress/cancel area; comparison switches between a split and a swipe view without losing the common zoom center. No action is represented by an unexplained icon alone.

## States

Implement empty, importing, ready, encoding, verifying, completed, cancelled, unsupported, permission-denied, out-of-memory and failed states. Empty offers Open image and Try sample. Busy states report the current stage with accessible announcements. Errors preserve the source and settings, identify the failing operation and offer a relevant retry. A stale preview never appears under a new job's result. Results identify codec, exactness verification, elapsed work, output size, savings or growth, and destination.

## Comparison

Side-by-side views share zoom and pan in image coordinates. Offer fit, actual pixels and reset. The wipe has keyboard controls and a numeric position; difference mode has a visible scale and a text summary of changed samples. Background transparency choices never alter encoded pixels. Distinguish original image size from encoder input size and output size, and distinguish preview color conversion from lossless verification. Disabled output formats explain the unsupported depth, alpha, animation or metadata requirement.

## Visual rules

Use light, dark, high-contrast light and high-contrast dark themes; comfortable and compact density; OS text and display scaling from 100% to 300%; named color tokens; a visible focus indicator; logical tab order; reduced-motion behavior; and labels that survive localization expansion. Measure text and controls using native font facilities or bundled audited text libraries. Use neutral image backgrounds so application colors do not bias visual comparisons. Windows UI Automation and Linux accessibility expose names, roles, values, focus and actions. A custom canvas with no accessibility tree fails acceptance.

## SVG icon pipeline

Author a small family of viewBox-based SVG paths for open, save, compare, zoom, fit, play, cancel, retry, settings and information. A stdlib build tool validates geometry and rejects scripts, external references, filters and unsupported paint operations before compiling deterministic C arrays. The C renderer draws paths at device scale with theme colors; no webview, icon font or runtime SVG library. Raster snapshots at every theme/scale are test evidence, not the authoritative assets.

## Delivery evidence

Drive each state on Windows x64 and Linux x64. Record keyboard-only and screen-reader runs, resize/scaling screenshots, cancellation and no-overwrite checks, decoded comparisons and dependency/import audits from clean target installations. No fixture or cross-build is described as a driven native UI test.
