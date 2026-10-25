# 03 Memory

> **Phase 1**

Peak memory of encode and decode. Every change here shortens buffer lifetimes or removes copies without changing a byte the codec writes.

## TODOs

| TODO | Title | Status |
| ---- | ----- | :----: |

## Completed

| TODO | Title | Completed |
| ---- | ----- | :-------: |
| [TODO-01](./TODO-01-buffer-lifetimes.md) | Buffer Lifetimes and Copies | 2026-10-25 |

## In scope

- The decoder's stream copy, the RGB-to-BMP copy, and the encoder's stream buffers
- Measuring `alloc_peak` and the process peak working set before and after

## Out of scope

- Streaming encode and decode (backlog, `B-002`), model table sizes (01)

---

Format spec: [../README.md](../README.md) · Root index: [../TODO-00-INDEX.md](../TODO-00-INDEX.md)
