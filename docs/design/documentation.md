# Documentation site design contract

The existing Jekyll/Just the Docs choice supplies navigation and search. Original RasterFold SVG branding, readable technical content, clear current-version labels and a consistent content width establish the product identity without adding client frameworks. This document describes a planned surface.

## Layout and states

Use a concise overview with install and converter entry points, a section navigation, page title and summary, a table of contents for long references, code-copy controls with feedback, a version label and an edit-source link. Document desktop/mobile, light/dark, high-contrast light/high-contrast dark, compact/comfortable, keyboard focus, search results, empty search, missing page and offline asset failure states. Search indexes published product content only.

## Content contract

All commands have a target/host label. Current features, planned features and historical measurements are visibly distinct. Format/ABI/release versions have separate labels. Benchmark tables and SVG charts identify data, codec revision, date, corpus, thread count and limitations; no handmade result number substitutes for measured data. The converter's format matrix is generated from its capability registry when that registry exists. Until then it is explicitly planned.

## Publication boundary

Build from an allowlist into an artifact directory and reject any leaked `todo/`, `AGENTS.md`, `.claude/`, `.codex/`, `.intelligence/`, private review or run record. The project base path is `/RasterFold/`; root-relative links cannot assume a root-hosted site. A source receipt and internal link/anchor check cover the built artifact before deployment. Read back the deployed source revision and selected pages after a successful Pages run. Publishing an artifact is not proof that the public URL serves it.
