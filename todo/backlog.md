# RasterFold -- Backlog

Ideas worth keeping that are not planned work. Nothing here is a section: an entry has no Implementation Order row, no phase, no checklist, and no owner, so no runner ever picks one up, `plan --check` never counts one, and nothing may defer work to one. The format spec, the admission test, and the budget are in [`README.md`](./README.md) under "The budget and the backlog". There is no cap.

**Entry grammar.** One line per entry: the id in brackets, the title, then ` -- key: value` fields. Required: `source` (one key token: a `docs/research.md` item, a finding ID, or the `-> SOURCE:` key the section would carry), `added` (YYYY-MM-DD), `why deferred`, and `promote when`. Optional: `app` (`rasterfold` or `suite`), `summary`, and `needs` (full `DNN TNN §N` refs to live sections, or other backlog ids). Ids are `B-NNN`, taken in order and never reused.

**Promotion** is `add-todo` on the entry: the promoting commit writes the real section (carrying this entry's source as its `-> SOURCE:` line) and deletes the entry. An entry leaves this file only by that promotion, by a merge that keeps its key as a merge marker on the surviving entry, or with the operator's words in a record under `## Removed with operator approval`.

## Entries

- [B-001] Separate stderr assertion for the guard's legacy-encoding stream case -- app: rasterfold -- source: D00-T01-S7-PR2 -- added: 2026-10-21 -- summary: the D00 T01 §7 self-test proves the UTF-8 reconfigure through stdout only; a second case would print a character outside cp1252 on stderr under PYTHONIOENCODING=cp1252 and assert it intact -- needs: D00 T01 §7 -- why deferred: fails the admission test; both streams reconfigure in one statement, so this is coverage polish, not a defect or a prerequisite -- promote when: the guard's stream setup splits per stream, or a stderr encoding defect is observed
- [B-002] Isolate the photo gain of the rejected eighth-model match model -- app: rasterfold -- source: D01-T04-S2-photo-gain -- added: 2026-10-30 -- summary: the D01 T04 §2 match model (rejected on its synth bar) still made every photo image smaller (photo -0.149%, tp -0.121%) at roughly 1.6x decode time; measure whether the gain comes from the match context itself or from an eighth mixer input (a constant-context eighth model, and the match context replacing the weakest existing model), and whether an optimized SIMD 9-slot mixer keeps the cost near the 7-model decode time -- needs: D01 T04 §2 -- why deferred: fails the admission test; a compression lead from a measurement, not a defect, a prerequisite, or required by an acceptance-bar row -- promote when: a compression phase looks for photo gains beyond version 3, or the operator asks for it

## Removed with operator approval
