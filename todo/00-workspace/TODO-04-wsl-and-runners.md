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

> **Goal:** Develop from WSL with repository-local pinned compilers, build Windows, Linux and Android targets, and run the Antigravity (`agy`) writer with GPT-6 Astra independent review in one checkout.

> [!IMPORTANT]
> **Current state (verified 2026-10-24):** The committed replacement uses `scripts/build.py` and pinned local tools for all four targets, with conventional `bin/<target>/` packages, Antigravity (`agy`) as the sole writer, and GPT-6 Astra (`gpt-6-astra` via `codex`) as the sole reviewer through `.conclave/reviewers.toml` and `scripts/review-dispatch.py`. The pre-migration WSL baseline had failed six gates: build dispatch, correctness, fuzz, review citation paths, Stop handling and provisioning. The migration audit records repaired gates and Windows/Linux runtime evidence; Android execution remains unproven. These sections still need their formal review stamps. The campaign controller is `scripts/campaign_guard.py` wired to `.agents/hooks.json` and `.agents/skills/`.
<!-- claim: exists src/sys.c -->
<!-- claim: exists scripts/campaign_guard.py -->
<!-- claim: exists toolchain.json -->

## Inputs

- [`build.ps1`](../../build.ps1), [`build.ninja`](../../build.ninja), [`toolchain.json`](../../toolchain.json): existing Windows build and pins.
- [`src/rf.h`](../../src/rf.h), [`src/sys.c`](../../src/sys.c), [`src/cm.c`](../../src/cm.c): platform boundary and arithmetic that must remain byte-identical.
- [`.agents/skills/`](../../.agents/skills/), [`.agents/hooks.json`](../../.agents/hooks.json), [`scripts/review-dispatch.py`](../../scripts/review-dispatch.py), [`scripts/campaign_guard.py`](../../scripts/campaign_guard.py): lifecycle contracts, native Stop hook, adaptive review dispatcher, and campaign controller.
- -> XREF: D08 T04 §1 -- repository presentation and platform guides.
- -> XREF: D06 T01 §2 -- the public library targets built with these compilers.

- -> XREF: D10 T01 §1 -- shared implementation or verification boundary.
- -> XREF: D00 T01 §6 -- the reviewer pin whose `agy -> astra` route reviews §1's toolchain candidate.

## Outcome

- A verified local compiler installation under `tools/` builds native Linux, cross Windows and Android artifacts, without changing the host toolchain.
- The Antigravity (`agy`) writer has its skills in `.agents/skills/`, native `Stop` hook in `.agents/hooks.json`, campaign guard in `scripts/campaign_guard.py`, and 3-stage adaptive review dispatcher in `scripts/review-dispatch.py`.
- Evidence-backed intelligence is validated through `scripts/intelligence.py` and processed only at a stopped boundary.

**Adjacency:** list=not-applicable (no record list); document=not-applicable (no printed output); settings=not-applicable (no codec option); reporting=not-applicable (existing CLI reports); notifications=not-applicable (existing exit codes); permissions=applicable @ D00 T04 §2; audit=not-applicable (Git retains changes); exchange=applicable @ D00 T04 §2; reverse=applicable @ D00 T04 §2

**Adjacency rationale:** This file owns host tooling and runner control. It does not add image formats or public codec options.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
| 1 | §1 | Local toolchains and WSL gates | D00 T01 §6 | [ ] |
| 2 | §2 | Portable platform and SIMD boundary | §1 | [ ] |
| 3 | §3 | Isolated Codex lifecycle and review runner | §1 | [ ] |
| 4 | §4 | Family-local intelligence | §3 | [ ] |

---

## 1. Local Toolchains and WSL Gates

> **Started:** 2026-09-30T20:10:51Z

**Operator decision 2026-10-21:** "Claude adopts the chain" -- the operator's answer, in an attended Claude `process-plan` session, to: "Phase 0's first row, D00 T04 §1, is Codex's candidate (8b30db5..73f0143 plus fix 55b66a8), parked because Codex's private Claude reviewer timed out at 900s. Claude's own panel is GPT-6 Astra, the same family as the implementer, so this Claude run can't stamp it. D00 T04 §2-§4 and D08 T04 §1 depend on it. How should this run treat that chain?" Claude is the writer of this file's sections and of `D08 T04 §1` from that date.

**Corrected 2026-10-21:** §1 had no dependency. Its candidate holds Codex-written bytes and Claude now finishes it, so neither the Claude family nor the GPT family may review it; the only third family is Gemini, which Claude's rounds reach only through the reviewer pin `D00 T01 §6` lands. §1 now depends on `D00 T01 §6`, and its rows (with every row that depends on it) moved after that section in Phase 0. A later section of this file whose candidate holds only Claude-written bytes is reviewed by Claude's normal route.

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

**Review blocked (2026-09-30):** Candidate `73f0143949aceb1dd8fa4a695069d1df0952fa45` passed the implementation gates, but the private `independent` slot (`claude-opus-5-5`, high effort) exited 124 after 900 seconds with no verdict. No verification stamp is earned. Evidence: `docs/reviews/00-workspace/D00-T04-s1.md`.

**Correction (2026-10-18):** Fresh gate execution exposed a timing-dependent guard fixture in `scripts/campaign_guard.py`: its three-second sleep did not ensure the hook had read the previous owner before handover. The fixture now signals from a disposable copy of the real hook immediately before acquiring the guard lock and waits for that signal before changing ownership. Production ownership handling is unchanged. Before correction, the suite reported `112 cases, 1 failed`; afterward, `python3 scripts/campaign_guard.py --self-test` exited 0 with `112 cases, 0 failed`. Logs: `build/codex-20261018-d00-t04-s1/initial-gate-logs/Claude-guard.log` and `guard-recheck.log`. The current section remains unstamped.

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

**Corrected 2026-10-24:** With the operator decision to consolidate on Antigravity (`agy`) as the sole writer and GPT-6 Astra (`gpt-6-astra` via `codex`) as the sole reviewer (`D00 T01 §6`), the `.claude/` and `.codex/` writer harnesses are retired. This section's deliverables are satisfied by the unified `.agents/skills/` lifecycle skills, `.agents/hooks.json` native `Stop` hook, `scripts/campaign_guard.py` controller, and `scripts/review-dispatch.py` adaptive reviewer.

- [ ] Maintain all lifecycle skills in `.agents/skills/` for Antigravity (`agy`). Done when: `python3 scripts/todo-graph.py validate` and `python3 scripts/campaign_guard.py --self-test` pass with all skills under `.agents/skills/`.
- [ ] Implement native `Stop` hook handling in `.agents/hooks.json` and `scripts/campaign_guard.py` with session-bound ownership and a bounded heartbeat. Done when: `python3 scripts/campaign_guard.py --self-test` passes all 145 cases.
- [ ] Route all independent reviews (`--kind plan`, `--kind completion`, and adaptive code review rounds) from `--writer agy` to `astra` (`codex`, `gpt-6-astra`) through `.conclave/reviewers.toml` and `scripts/review-dispatch.py`. Done when: `python3 scripts/review-dispatch.py --check-pin --json` and `python3 -m unittest discover -s scripts/tests -v` pass.
- [ ] Document exact invocation, hook configuration, and stopped maintenance in `AGENTS.md`, `GEMINI.md`, and `.agents/skills/`. Done when: `python3 scripts/drift-check.py` exits 0.
- [ ] Commit: `"runner: consolidate lifecycle skills, guard, and review dispatch on Antigravity"`

**Test checkpoint:** `python3 -m unittest discover -s scripts/tests -v` and `python3 scripts/campaign_guard.py --self-test` exit zero.

## 4. Family-Local Intelligence

**Corrected 2026-10-24:** Standalone `scripts/intelligence.py` validates the retained `.intelligence/` registers and archives without `.claude/` or `.codex/` runner directories, while `.agents/skills/process-intelligence/SKILL.md` governs stopped-boundary disposition.

- [ ] Maintain intelligence registers and validation in `scripts/intelligence.py` with atomic writes, evidence and owner fields, and retained archives. Done when: `python3 scripts/intelligence.py validate` exits 0 and malformed records are refused.
- [ ] Provide `.agents/skills/process-intelligence/SKILL.md` and stopped-boundary checks in `scripts/intelligence.py`. Done when: active or unreadable guard state prevents disposition and archiving.
- [ ] Verify intelligence register validation and campaign guard stopped-boundary checks. Done when: `python3 scripts/intelligence.py validate` and `python3 scripts/campaign_guard.py --self-test` exit zero.
- [ ] Commit: `"runner: retain and process runner intelligence"`

**Test checkpoint:** `python3 scripts/campaign_guard.py --self-test` and `python3 scripts/intelligence.py validate` exit zero.

## Verification

- [ ] `python3 scripts/check-all.py` passes every required host gate.
- [ ] `python3 scripts/todo-graph.py validate` and `plan --check` pass.
- [ ] Native Windows and Linux tests pass; Android build and runtime evidence are reported separately.
