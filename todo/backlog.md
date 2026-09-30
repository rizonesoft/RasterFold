# RasterFold -- Backlog

Ideas worth keeping that are not planned work. Nothing here is a section: an entry has no Implementation Order row, no phase, no checklist, and no owner, so no runner ever picks one up, `plan --check` never counts one, and nothing may defer work to one. The format spec, the admission test, and the budget are in [`README.md`](./README.md) under "The budget and the backlog". There is no cap.

**Entry grammar.** One line per entry: the id in brackets, the title, then ` -- key: value` fields. Required: `source` (one key token: a `docs/research.md` item, a finding ID, or the `-> SOURCE:` key the section would carry), `added` (YYYY-MM-DD), `why deferred`, and `promote when`. Optional: `app` (`rasterfold` or `suite`), `summary`, and `needs` (full `DNN TNN §N` refs to live sections, or other backlog ids). Ids are `B-NNN`, taken in order and never reused.

**Promotion** is `add-todo` on the entry: the promoting commit writes the real section (carrying this entry's source as its `-> SOURCE:` line) and deletes the entry. An entry leaves this file only by that promotion, by a merge that keeps its key as a merge marker on the surviving entry, or with the operator's words in a record under `## Removed with operator approval`.

## Entries


## Removed with operator approval
