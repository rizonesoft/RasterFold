---
schema_version: 1
id: wsl-and-runners
domain: 00-workspace
status: active
title: "TODO-04 -- WSL, Local Toolchains, and Isolated Runners"
depends_on: []
track: W2
frozen: true
---

# TODO-04 -- WSL, Local Toolchains, and Isolated Runners

> **Goal:** Develop from WSL with repository-local pinned compilers, build Windows, Linux and Android targets, and run either Claude or Codex with independently tunable mechanics in one checkout.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** The committed replacement uses `scripts/build.py` and pinned local tools for all four targets, with conventional `bin/<target>/` packages and independent runner mechanics. The pre-migration WSL baseline had failed six gates: build dispatch, correctness, fuzz, review citation paths, Claude Stop handling and provisioning. The migration audit records repaired gates and Windows/Linux runtime evidence; Android execution remains unproven. These sections still need their formal review stamps. Claude's controller is `scripts/campaign_guard.py`; its review pins are `.conclave/panel.toml`. The operator requested Codex parity, independent runner internals, no isolated worktrees and no checkpoint layer, then extended the delivery targets to Windows, Linux and Android executables and libraries.
<!-- claim: exists src/sys.c -->
<!-- claim: exists scripts/campaign_guard.py -->
<!-- claim: exists toolchain.json -->

## Inputs

- [`build.ps1`](../../build.ps1), [`build.ninja`](../../build.ninja), [`toolchain.json`](../../toolchain.json): existing Windows build and pins.
- [`src/rf.h`](../../src/rf.h), [`src/sys.c`](../../src/sys.c), [`src/cm.c`](../../src/cm.c): platform boundary and arithmetic that must remain byte-identical.
- [`.claude/skills/`](../../.claude/skills/): lifecycle contracts to adapt for Codex.
- -> XREF: D08 T04 §1 -- repository presentation and platform guides.
- -> XREF: D06 T01 §2 -- the public library targets built with these compilers.

- -> XREF: D10 T01 §1 -- shared implementation or verification boundary.

## Outcome

- A verified local compiler installation under `tools/` builds native Linux, cross Windows and Android artifacts, without changing the host toolchain.
- Both runner families have their own skills, hooks, settings, state and review tools; a shared admission lock prevents concurrent writers.
- Evidence-backed intelligence stays family-local and is processed only at a stopped boundary.

**Adjacency:** list=not-applicable (no record list); document=not-applicable (no printed output); settings=not-applicable (no codec option); reporting=not-applicable (existing CLI reports); notifications=not-applicable (existing exit codes); permissions=applicable @ D00 T04 §2; audit=not-applicable (Git retains changes); exchange=applicable @ D00 T04 §2; reverse=applicable @ D00 T04 §2

**Adjacency rationale:** This file owns host tooling and runner control. It does not add image formats or public codec options.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Local toolchains and WSL gates | -- | [ ] |
| 2 | §2 | Portable platform and SIMD boundary | §1 | [ ] |
| 3 | §3 | Isolated Codex lifecycle and review runner | §1 | [ ] |
| 4 | §4 | Family-local intelligence | §3 | [ ] |

---

## 1. Local Toolchains and WSL Gates

> **Started:** 2026-09-30T20:10:51Z

Use pinned, checksum-verified LLVM/MinGW and Android NDK installations beneath `tools/`; keep downloaded SDKs and compiler binaries out of Git. Windows remains a supported build target. Document the Linux host SDK requirement separately from the local compiler installation. Repair the measured WSL path, shell and locking defects without weakening gates.

- [x] Add a manifest-driven local bootstrap and build command for Windows, Linux and Android. Done when: every target produces artifacts and reports its compiler and target.
- [x] Organize lowercase `bin/<target>/` packages into conventional `bin/`, `lib/` and `include/` directories, with matching Windows PDBs in `symbols/`. Done when: native consumers and packaged artifacts use the new paths, manifest hashes cover the headers and symbols, and no loose compatibility copies remain.
- [x] Repair the six failed WSL gates and keep Windows validation runnable. Done when: the all-gates command exits zero on WSL and reports target-specific runtime limits honestly.
- [x] Deliver a ZIP artifact containing the built executable, libraries, header and build hashes for each target. Done when: `scripts/package.py` verifies manifests before creating ZIP artifacts.
- [x] Add target build CI and document local provisioning and verification. Done when: clean installation, repeat installation and checksum failure are tested.
- [x] Commit: `"build: provision local cross-platform toolchains from WSL"`

**Implementation evidence:** The bootstrap, target builds, package layout, provisioning guide and target CI shipped in `2c89e2e`; this section verifies those existing surfaces. On 2026-09-30 the four target builds and ZIP packages were reproduced. The four installer fixtures passed clean installation, repeat installation, archive checksum refusal and corrupted-receipt refusal. This candidate adds a required-artifact manifest check to `scripts/package.py`, plus five package tests and their full-gate registration, because a manifest omitting a required header, symbol or binary previously passed packaging.

**Freeze check:** No codec, oracle, format specification or known-answer bytes change in this section; native correctness and oracle comparisons must remain green.

**Checkpoint output (2026-09-30):** `python3 scripts/check-all.py --all-targets` exited 0 with `check-all: 30/30 passed`; Linux and Windows correctness each report `33 passed, 0 failed`; package tests each report five passing tests on Linux and Windows. Linux API: 8 tests; artifact/consumer tests: 4; seeded fuzz: `300 mutated files: 293 rejected, 7 still valid, no crash, hang or mismatch` (seed 1). Toolchain verification reports all three installed components OK. Android ARM64/x64 were built and inspected, not executed. The graph self-test explicitly skipped its optional absent sibling stamp-gate probe. Logs and the initial stale-cache correction are recorded in `docs/phase-runs/2026-09-30-codex-phase-0.md`.

**Test checkpoint:** `python3 scripts/check-all.py` exits zero; `python3 tools/bootstrap.py --verify` verifies every installed component against its manifest and receipt; a corrupted receipt refuses verification.

## 2. Portable Platform and SIMD Boundary

**Job:** A caller can export format files between targets. Decoding can undo encoding. The codec can enforce an output policy that protects existing destination files.

Keep the format frozen at version 2. Add Linux and Android platform support and an integer-equivalent portable SIMD path. Retain Windows file safety, and on POSIX publish an exclusive temporary file without replacing an existing destination. Runtime support is distinct from cross-compilation.

- [ ] Add native POSIX file, timing and suspended-worker support. Done when: correctness and file refusal tests pass on Linux.
- [ ] Make ARM builds preserve all arithmetic and thread-count output. Done when: portable arithmetic passes the frozen oracle cases and both Android ABIs build.
- [ ] Run Windows and Linux correctness, malformed-input fuzz and artifact inspection. Done when: the frozen known-answer bytes are unchanged; missing Android runtime proof is named explicitly.
- [ ] Commit: `"codec: support Linux and Android without changing fold bytes"`

**Test checkpoint:** `python3 tests/test_correctness.py` and `python3 tests/fuzz_decode.py 300 1` exit zero on Linux; Windows equivalents run through `scripts/windows.py`; `python3 scripts/build.py --target all` builds all configured targets.

**Freeze check:** no new method, version, predictor, constant or acceptance rule; every emitted byte must match the independent oracle and existing known-answer file.

## 3. Isolated Codex Lifecycle and Review Runner

Keep Claude's runner installed. Codex owns its implementation, pins, native hooks, heartbeat/recovery mechanism and run records. Share canonical source and TODO contracts, plus only the admission mutex needed for one writer. Do not create worktrees, snapshots or checkpoint gates. Native hook trust is a runtime admission requirement, never synthesized by the runner.

- [ ] Adapt every Claude lifecycle skill for Codex and register supported skill discovery. Done when: every lifecycle has a Codex counterpart without Claude-only tools or paths.
- [ ] Implement native Stop, resume, prompt-stop and interrupt handling with session-bound ownership and a bounded heartbeat. Done when: lifecycle, malformed state, sibling contention and explicit-stop tests pass.
- [ ] Give Codex a private independent review panel and review tooling. Done when: same-family review is refused and a concrete private reviewer invocation is exercised.
- [ ] Document exact invocation, hook trust and stopped maintenance. Done when: a fresh session can discover and audit without arming a campaign.
- [ ] Commit: `"codex: isolate the plan runner and lifecycle skills"`

**Test checkpoint:** `python3 -m unittest discover -s .codex/tests -v` exits zero, and native hook discovery lists the project event definitions without mutating trust.

## 4. Family-Local Intelligence

Carry the JMR pattern at RasterFold's scale: append evidence and proposed corrections during a run, then explicitly apply, reject or defer at a stopped boundary. No automatic rewrite of runner policy and no sharing of family ids.

- [ ] Add separate intelligence registers and commands with atomic writes, evidence and owner fields, and retained archives. Done when: malformed records and cross-family writes are refused.
- [ ] Add process-intelligence skills and stopped-boundary checks for both families. Done when: active, paused or unreadable ownership prevents disposition and archiving.
- [ ] Test capture, de-duplication, apply/reject/defer, archive, concurrency and stopped checks. Done when: every case passes and no live campaign was started by tests.
- [ ] Commit: `"runner: retain and process family-local intelligence"`

**Test checkpoint:** `python3 -m unittest discover -s .codex/tests -v` and `python3 scripts/intelligence.py validate` exit zero.

## Verification

- [ ] `python3 scripts/check-all.py` passes every required host gate.
- [ ] `python3 scripts/todo-graph.py validate` and `plan --check` pass.
- [ ] Native Windows and Linux tests pass; Android build and runtime evidence are reported separately.
