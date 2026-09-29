## Summary

<!-- What does this change do, and why? One or two sentences. -->

## Plan reference

<!-- The TODO section this implements, as `DNN TNN §N` (for example `D01 T01 §2`), or the issue it fixes. -->

- Plan: `DNN TNN §N`
- Fixes: #

## Area

- [ ] Compression (models, methods, format)
- [ ] Speed
- [ ] Memory
- [ ] Decoder hardening
- [ ] Build, CI, or tooling
- [ ] Documentation

## Evidence

<!-- Quote what ran: the correctness count, the corpus sizes (set, total, geometric mean, worst image), the timings with thread counts. -->

## Checklist

- [ ] `pwsh scripts/check-all.ps1` is green locally
- [ ] A change to what the encoder writes touches the C code, `tests/rfi_ref.py`, `spec/FOLD.md`, and the known-answer file together, with a new method number or format version
- [ ] Size and speed claims follow `docs/research.md` section 2 (the `photo` set tunes, `tp` confirms, thread counts stated); nothing is tuned to one image
- [ ] `python scripts/todo-graph.py validate` shows 0 fatal and no new warning, and `plan --check` is current
