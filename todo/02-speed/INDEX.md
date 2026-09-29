# 02 Speed

> **Phases 1, 5, and 10**

Encode and decode speed. A change here leaves every `.fold` byte unchanged (the known-answer file and every oracle comparison prove it), or measures its size cost and hands the format change to `01-compression`.

## TODOs

| TODO | Title | Status |
| ---- | ----- | :----: |
| [TODO-01](./TODO-01-compiler-options.md) | Compiler Options: PGO, LTO, AVX2 | draft |
| [TODO-02](./TODO-02-parallel-and-simd.md) | Parallel Strips and the SIMD Mixer: Measurements | draft |

## Completed

| TODO | Title | Completed |
| ---- | ----- | :-------: |

## In scope

- Build options (profile-guided and link-time optimization, instruction-set dispatch)
- Measurements of format-changing speed ideas (strips, int16 mixer) before `01-compression` ports them
- Hot-loop work in `src/cm.c` that keeps the bytes identical

## Out of scope

- Porting a format change (01), buffer lifetimes (03)

---

Format spec: [../README.md](../README.md) · Root index: [../TODO-00-INDEX.md](../TODO-00-INDEX.md)
