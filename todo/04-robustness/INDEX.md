# 04 Robustness

> **Phases 1 and 2**

Decoder hardening: malformed and hostile files are rejected quickly and cleanly, never crash, hang, over-read, or decode to something the encoder never wrote.

## TODOs

| TODO | Title | Status |
| ---- | ----- | :----: |

## Completed

| TODO | Title | Completed |
| ---- | ----- | :-------: |
| [TODO-01](./TODO-01-decoder-hardening.md) | Decoder Hardening | 2026-10-26 |

## In scope

- Early rejection of streams that run out, and bounded work on lying headers
- Fuzzing campaigns and their seeds

## Out of scope

- A new method's own rejection rules, which ship with the method (01)

---

Format spec: [../README.md](../README.md) · Root index: [../TODO-00-INDEX.md](../TODO-00-INDEX.md)
