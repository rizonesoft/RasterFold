---
schema_version: 1
id: generated-references
domain: 08-docs
status: draft
title: "TODO-03 -- Generated Command-Line and API References"
depends_on: []
track: D3
---

# TODO-03 -- Generated Command-Line and API References

> **Goal:** The command-line reference and the API reference are generated from the program and the header themselves, so they cannot describe an option or a function that does not exist, and the drift gate fails when they fall behind.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** The command line is documented by its usage text in `src/main.c` and a hand-written section of `README.md`; there is no API.
<!-- claim: absent docs/src/cli.md -->

## Inputs

- [`src/main.c`](../../src/main.c) -- the help text the command-line reference comes from
- -> XREF: D07 T01 §1 -- the version 1 commands and their `help` output
- -> XREF: D06 T01 §1 -- the header whose comments the API reference comes from

## Outcome

- `docs/site/cli.html` and `docs/site/api.html` are generated from `rasterfold help` and `include/rasterfold.h`, and `scripts/drift-check.py` fails when either is stale.

**Adjacency:** all=not-applicable (generated reference pages; the behavior they describe belongs to the command line and the library)

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | The command-line reference | D07 T01 §1, D08 T01 §1 |  [ ]   |
|   2   |   §2    | The API reference | D06 T01 §1, D08 T01 §1 |  [ ]   |

---

## 1. The Command-Line Reference

- [ ] Generate `docs/src/cli.md` from `Bin/rasterfold.exe help <command>` for every command (usage, options, exit codes, examples), and render it into the site. Done when: `python scripts/build-docs.py` writes `docs/site/cli.html` from it. Cheaper substitute: a hand-written page.
- [ ] Add the reference to the generated-files check of `D08 T02 §3`. Done when: changing a usage line without regenerating makes `python scripts/drift-check.py` exit 1.
- [ ] Commit: `"docs: generate the command-line reference from the program"`

**Test checkpoint:** the seeded stale reference quoted with its failure; `python scripts/drift-check.py` exits 0.

## 2. The API Reference

- [ ] Generate `docs/src/api.md` from the doc comments in `include/rasterfold.h` (every function, type, constant, and error code, with its ownership rule), and render it into the site. Done when: every exported function in `src/rasterfold.def` appears in the page.
- [ ] Add it to the generated-files check, and fail when a `.def` export has no documentation. Done when: an undocumented export makes the check exit 1.
- [ ] Commit: `"docs: generate the API reference from the header"`

**Test checkpoint:** the undocumented-export failure quoted, then fixed; `python scripts/drift-check.py` exits 0.

## Verification

- [ ] `python scripts/drift-check.py` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
