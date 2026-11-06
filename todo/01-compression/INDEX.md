# 01 Compression

> **Phases 2 to 7**

The models, predictors, and coding methods that make `.fold` files smaller. Every change here is measured on the research corpus before it is ported, and every ported change touches the four places a format change owes (the C code, the oracle, the spec, the known-answer file).

## TODOs

| TODO | Title | Status |
| ---- | ----- | :----: |
| [TODO-02](./TODO-02-model-experiments.md) | Model Experiments for Method 1 | draft |
| [TODO-03](./TODO-03-format-version-3.md) | Format Version 3 | active |
| [TODO-05](./TODO-05-every-photo-beats-e9.md) | Every Photograph Beats JPEG XL e9 | draft |
| [TODO-06](./TODO-06-beat-webp-generated.md) | Beat WebP on Generated Images | draft |

## Completed

| TODO | Title | Completed |
| ---- | ----- | :-------: |
| [TODO-01](./TODO-01-palette-method.md) | Palette Method for Images with Few Colours | 2026-10-26 |
| [TODO-04](./TODO-04-exact-repeats.md) | Exact Repeats: a Match Model | 2026-10-30 |

## In scope

- New coding methods (method 2, palette) and their oracle, spec, and rejection rules
- Context models, counters, mixers, predictors, and encoder parameter choice for method 1
- Porting the measured winners into a new format version

## Out of scope

- Speed work that leaves the bytes unchanged (02), buffer lifetimes (03), decoder hardening beyond a new method's own rejection rules (04)

---

Format spec: [../README.md](../README.md) · Root index: [../TODO-00-INDEX.md](../TODO-00-INDEX.md)
