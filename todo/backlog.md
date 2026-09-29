# RasterFold -- Backlog

Ideas worth keeping that are not planned work. Nothing here is a section: an entry has no Implementation Order row, no phase, no checklist, and no owner, so no runner ever picks one up, `plan --check` never counts one, and nothing may defer work to one. The format spec, the admission test, and the budget are in [`README.md`](./README.md) under "The budget and the backlog". There is no cap.

**Entry grammar.** One line per entry: the id in brackets, the title, then ` -- key: value` fields. Required: `source` (one key token: a `docs/research.md` item, a finding ID, or the `-> SOURCE:` key the section would carry), `added` (YYYY-MM-DD), `why deferred`, and `promote when`. Optional: `app` (`rasterfold` or `suite`), `summary`, and `needs` (full `DNN TNN §N` refs to live sections, or other backlog ids). Ids are `B-NNN`, taken in order and never reused.

**Promotion** is `add-todo` on the entry: the promoting commit writes the real section (carrying this entry's source as its `-> SOURCE:` line) and deletes the entry. An entry leaves this file only by that promotion, by a merge that keeps its key as a merge marker on the surviving entry, or with the operator's words in a record under `## Removed with operator approval`.

## Entries

- [B-001] Alpha channel and 16-bit samples -- source: research-7.3-alpha-16bit -- added: 2026-09-29 -- summary: code alpha as a fourth method 1 channel after B with its own models and stream, and 16-bit samples with a wider residual binarization -- why deferred: the tool reads and writes the 24-bit BMP subset only, so no input can carry alpha or 16-bit samples yet -- promote when: an input format that carries them (PNG, TIFF, or 32-bit BMP) is planned
- [B-002] Streaming encode and decode -- source: readme-limitation-streaming -- added: 2026-09-29 -- summary: code and write rows as they arrive instead of holding the whole image, bounding memory by width rather than by image size -- why deferred: the whole-image buffers are 0.33 of JPEG XL's decode memory today, and the thread schedule needs a redesign for bounded rows -- promote when: an image above the 32-megapixel limit is in scope, or memory becomes the measure RasterFold loses on
- [B-005] Grayscale flag -- source: research-7.2-grayscale -- added: 2026-09-29 -- summary: a method or flag for images with R = G = B everywhere that codes G only and copies it, saving two thirds of the single-thread modelling time on such images -- why deferred: it saves CPU time, not wall time on three threads, and gray RGB images are rare in the corpus -- promote when: a grayscale-heavy corpus or a one-thread speed target is in scope

## Removed with operator approval
