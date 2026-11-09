# Documentation Site Navigation, States, and Accessibility Validation

**Status:** Current (`.fold` format version 3 <!-- drift:fact format_version = 3 -->, public C API 1.0 (`RF_API_VERSION_MAJOR = 1` <!-- drift:fact api_major = 1 -->, `RF_API_VERSION_MINOR = 0` <!-- drift:fact api_minor = 0 -->)).

This record documents the browser, responsive-layout, keyboard-navigation, state-transition, and accessibility-tree verification of the RasterFold Jekyll documentation site built from `docs/site-content.json` (`14` published pages plus `/RasterFold/404.html`).

## 1. Environment and Build Provenance

- **Staged source tree:** `python3 scripts/build-docs.py --stage build/site-source` (`37` allowlisted files staged).
- **Jekyll build:** `ghcr.io/actions/jekyll-build-pages:v1.0.13` (`just-the-docs/just-the-docs` remote theme, `baseurl: "/RasterFold"`).
- **Display and browser harness:** `xvfb-run -a` X11 display driving Chromium (`headless: false`) via Playwright and the Chrome DevTools Protocol (`Accessibility.getFullAXTree`) against a local HTTP server mounted at `/RasterFold/`.
- **Viewports tested:** Desktop `1280x800` and Mobile `390x844`.

## 2. Desktop and Mobile Layout and Navigation

- **Initial keyboard tab stop (`Skip to main content`):**
  - `docs/_includes/head_custom.html` overrides `Element.prototype.scrollIntoView` and `Element.prototype.removeAttribute` on `.nav-list-link` so Just the Docs' sidebar initialization does not move the sequential focus starting point into `#site-nav` or strip the `href` attribute from the active navigation item.
  - Pressing `Tab` once on `/RasterFold/` focuses `<a class="skip-to-main" href="#main-content">Skip to main content</a>` with computed `outline-style: solid` and `outline-width: 3px`. Activating `.skip-to-main` moves focus to `<main id="main-content" tabindex="-1">`.
- **Main sidebar navigation (`#site-nav`):**
  - All `15` allowlisted pages (`Overview`, `Getting Started`, `Installation and Build`, `CLI and Library Usage`, `C API Guide`, `Format Specification`, `Examples`, `Benchmarks`, `Research Summary`, `Limitations`, `Troubleshooting`, `Contributing`, `Security Policy`, `Licenses and Provenance`, `API Reference`) are present in `<nav id="site-nav" aria-label="Main">`, retain valid `/RasterFold/*.html` `href` attributes, and mark the active page with `aria-current="page"`.
- **Page outline (`#page-outline`):**
  - On pages with two or more `<h2>` sections (`docs/assets/js/site-states.js`), `<nav id="page-outline" class="rf-page-outline" aria-label="On this page">` is inserted after `<h1>`.
  - On `/RasterFold/format.html`, `#page-outline` exposes `7` section links (`#conventions` through `#decoder-validation-summary`), all resolving to matching `h2[id]` anchors on the page.
- **Mobile viewport (`390x844`) and menu button (`#menu-button`):**
  - On `/RasterFold/benchmarks.html` at `390x844`, `document.documentElement.scrollWidth` equals `document.documentElement.clientWidth` (`375 == 375`, zero horizontal page overflow; tables and wide code blocks scroll within their own containers and SVG charts scale via `max-width: 100%; height: auto`).
  - `#menu-button` starts with `aria-controls="site-nav"` and `aria-expanded="false"`. Activating `#menu-button` toggles `aria-expanded="true"`, adds `.nav-open` to `#site-nav`, and announces `"Navigation menu expanded"` in `#rf-a11y-status`. Activating `#menu-button` again sets `aria-expanded="false"`, removes `.nav-open`, and announces `"Navigation menu collapsed"`.

## 3. Themes, Densities, Persistence, and Storage Fallback

The header toolbar (`docs/_includes/header_custom.html`) provides keyboard-operable `<button>` groups (`#rf-theme-controls` and `#rf-density-controls`) with `aria-pressed="true|false"` state and `localStorage` persistence (`rf_docs_theme`, `rf_docs_density`) restored before paint by `#rf-early-state-init`:

| Theme / Density | HTML Attribute | Computed `body` Background | Computed `body` Text / Font Size | Live Status Announcement (`#rf-a11y-status`) |
| --- | --- | --- | --- | --- |
| Light (`light`) | `data-rf-theme="light"` | `rgb(255, 255, 255)` | `rgb(31, 35, 40)` | `Theme set to Light` |
| Dark (`dark`) | `data-rf-theme="dark"` | `rgb(13, 17, 23)` | `rgb(230, 237, 243)` | `Theme set to Dark` |
| High Contrast Light (`hc-light`) | `data-rf-theme="hc-light"` | `rgb(255, 255, 255)` | `rgb(0, 0, 0)` | `Theme set to High Contrast Light` |
| High Contrast Dark (`hc-dark`) | `data-rf-theme="hc-dark"` | `rgb(0, 0, 0)` | `rgb(255, 255, 255)` | `Theme set to High Contrast Dark` |
| Comfortable (`comfortable`) | `data-rf-density="comfortable"` | `16px` body font, `1.65` line height | `0.55rem 0.75rem` table cell padding | `Density set to Comfortable` |
| Compact (`compact`) | `data-rf-density="compact"` | `14px` body font, `1.42` line height | `0.25rem 0.45rem` table cell padding | `Density set to Compact` |

- **Cross-page persistence (`localStorage`):** Selecting `hc-dark` and `compact` on `/RasterFold/` and reloading or navigating to `/RasterFold/format.html` preserves `data-rf-theme="hc-dark"`, `data-rf-density="compact"`, and `aria-pressed="true"` on both active buttons.
- **Storage-unavailable cross-page session fallback (`window.name`):** When `window.localStorage` throws `DOMException("localStorage disabled", "SecurityError")` on every document load (or when `getItem` returns older persisted values while `setItem` throws `QuotaExceededError`), selecting `hc-dark` and `compact` on `/RasterFold/` writes `rf_docs_session:{"theme":"hc-dark","themeUnpersisted":true,"density":"compact","densityUnpersisted":true}` to `window.name`, announces `"Theme set to High Contrast Dark (session only)"` and `"Density set to Compact (session only)"`, and following the sidebar link to `/RasterFold/usage.html` restores `data-rf-theme="hc-dark"`, `data-rf-density="compact"`, and `aria-pressed="true"` before paint on the new document.

## 4. Code-Copy Controls and Live Feedback

- Every code block inside `<main id="main-content">` (`div.highlighter-rouge`, `figure.highlight`) has a single keyboard-focusable `<button type="button" class="rf-copy-button" aria-label="Copy code to clipboard">`.
- **Success state:** Activating `.rf-copy-button` on `/RasterFold/usage.html` sets `data-copy-state="copied"`, updates visible text to `"Copied"`, and announces `"Copied code to clipboard"` in `#rf-a11y-status`.
- **Refusal / error state:** When clipboard write is refused (`window.rfDocsState.setClipboardForceFail(true)`), activating `.rf-copy-button` sets `data-copy-state="failed"`, updates visible text to `"Copy failed"`, and announces `"Unable to copy code to clipboard; select and copy the code block manually"` in `#rf-a11y-status`.

## 5. Local Search, Empty Search, Offline Failure, and Missing-Page (`404.html`) States

- **Published-only search index (`search-data.json`):** Contains `72` section entries across the `14` allowlisted pages only; `404.html` (`search_exclude: true`, `nav_exclude: true`) and all private repository paths (`todo/`, `AGENTS.md`, `GEMINI.md`, `.agents/`, `.conclave/`, `.intelligence/`, `docs/reviews/`, `docs/phase-runs/`) are excluded.
- **Keyboard shortcuts and result activation:**
  - Pressing `/` (outside editable fields) or `Ctrl+K` on desktop (`1280x800`) or mobile (`390x844`, opening `#main-header.nav-open` when collapsed) focuses `#search-input` (`aria-controls="search-results"`, `aria-expanded="false"`) and announces `"Search input focused"`.
  - Querying `"troubleshooting"` expands `#search-results` (`role="region"`, `aria-label="Search results"`), sets `aria-expanded="true"` on `#search-input`, renders `8` `.search-results-list-item` entries, and announces `"8 search results available"`.
  - Pressing `Escape` clears `#search-input.value` to `""`, removes `html.search-active`, empties `#search-results`, sets `aria-expanded="false"`, returns focus to `#search-input`, and announces `"Search cleared"`.
  - Querying `"CRC-32"` and pressing `Enter` activates the first matching result and navigates to `/RasterFold/format.html#file-layout`.
- **Empty search state (`.search-no-result`):**
  - Querying `"xyznonexistentquery999"` renders `.rf-search-empty-message` (`No matching documentation found for "xyznonexistentquery999".`), a keyboard-focusable `#rf-search-clear` (`Clear search`) button, and recovery links to `Overview`, `CLI Usage`, `C API Guide`, and `Troubleshooting`.
  - Activating `#rf-search-clear` (by click or by pressing `ArrowDown` + `Enter` in `#search-input`) resets `#search-input` to `""`, sets `aria-expanded="false"`, returns focus to `#search-input` without navigating away, and announces `"Search cleared"`.
- **Offline / unavailable `search-data.json` state (`#rf-search-offline`) and functional retry:**
  - When the initial `/RasterFold/assets/js/search-data.json` `XMLHttpRequest` fails (network error, HTTP error status, or HTTP `200` with invalid JSON), `#rf-search-offline` (`role="alert"`) becomes visible, `#search-input` sets `aria-invalid="true"`, and `#rf-a11y-status` announces `"Search index is unavailable offline"`.
  - Restoring the endpoint and activating `#rf-search-retry` (`Retry search index`) re-fetches and parses `/RasterFold/assets/js/search-data.json` into the fallback search index, hides `#rf-search-offline`, clears `aria-invalid`, focuses `#search-input`, and announces `"Search index loaded"`. Typing `"FOLD"` into `#search-input` after retry renders `20` `.search-results-list-item` entries, and pressing `Enter` navigates to `/RasterFold/c-api.html`.
- **Missing-page (`404.html`) state:**
  - Requesting an unknown route under `/RasterFold/` serves `404.html` (`<h1>Page not found (404)</h1>`), preserving the site header, search bar, theme/density controls, and direct `/RasterFold/*.html` recovery links to all published sections.

## 6. Accessibility Tree and Screen-Reader Pass Summary

Full Chromium accessibility trees (`Accessibility.getFullAXTree`) were inspected across Overview, Format Specification, Benchmarks, and Troubleshooting:

| Page | AX Nodes | Headings | Links | Buttons | Tables | Verified Landmarks |
| --- | --- | --- | --- | --- | --- | --- |
| `/RasterFold/` (`index.html`) | `624` | `3` | `38` | `7` | `0` | `banner`, `navigation` (`Main`), `region` (`Display preferences and version status`), `main`, `navigation` (`On this page`), `contentinfo`, `status` |
| `/RasterFold/format.html` | `1523` | `21` | `46` | `23` | `0` | `banner`, `navigation` (`Main`), `region` (`Display preferences and version status`), `main`, `navigation` (`On this page`), `contentinfo`, `status` |
| `/RasterFold/benchmarks.html` | `6709` | `11` | `34` | `7` | `14` | `banner`, `navigation` (`Main`), `region` (`Display preferences and version status`), `main`, `navigation` (`On this page`), `contentinfo`, `status` |
| `/RasterFold/troubleshooting.html` | `1226` | `10` | `35` | `9` | `1` | `banner`, `navigation` (`Main`), `region` (`Display preferences and version status`), `main`, `navigation` (`On this page`), `contentinfo`, `status` |
