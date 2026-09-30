# 00 Workspace

> **Phases 0 and 8**

The toolchain, gates, hooks, CI, and the TODO system every later domain leans on. Nothing here changes a `.fold` file; everything later depends on it being boring and green.

## TODOs

| TODO | Title | Status |
| ---- | ----- | :----: |
| [TODO-01](./TODO-01-dev-automation.md) | Dev-Automation Wiring | draft |
| [TODO-02](./TODO-02-measurement-references.md) | Measurement References: JPEG XL e9 and WebP | draft |
| [TODO-03](./TODO-03-held-out-validation.md) | Held-Out Validation | draft |

| [TODO-04](./TODO-04-wsl-and-runners.md) | WSL, Local Toolchains, and Isolated Runners | active |

## Completed

| TODO | Title | Completed |
| ---- | ----- | :-------: |

## In scope

- The pinned toolchain (`toolchain.json`, `tools/bootstrap.ps1`, `tools/provision.ps1`) and moving its pins
- `build.ps1`, `build.ninja`, and one command that runs every gate (`scripts/check-all.ps1`)
- Static analysis, CI workflows, the commit hooks
- The checks that keep the plan honest about itself: the TODO graph, claims, the findings ledger, the review panel, and the campaign guard

## Out of scope

- The codec's models, methods, and format (01), speed (02), memory (03), and decoder hardening (04)
- Operator-only steps (99)

---

Format spec: [../README.md](../README.md) · Root index: [../TODO-00-INDEX.md](../TODO-00-INDEX.md)
