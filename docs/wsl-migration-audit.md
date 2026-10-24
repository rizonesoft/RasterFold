# WSL migration and platform audit

Recorded 2026-09-30 against the working tree based on `97e5ca5`. This is local build, test, source-review and discovery evidence. It is not a release, a remote CI result or a verified TODO section stamp.

## Baseline and repairs

The original WSL all-gates run failed 6 of 19 gates: Windows build dispatch, correctness execution, fuzz execution, review citation path handling, the Claude Stop guard and provisioning. The Windows compiler still rebuilt the original codec through WSL interop. Existing user edits were retained.

The replacement uses pinned repository-local LLVM, Ninja and Android NDK installations. It builds Windows/Linux x64 and Android ARM64/x64 executables, shared libraries and static libraries. Linux uses the host libc development SDK; the compiler installation is local, while the Linux sysroot is not hermetic. Windows retains the freestanding Win32 executable and DLL. The public C API is version 1.0; the `.fold` format, independent oracle and known-answer bytes remain version 2.

The platform work adds POSIX file publication, threads and resource reporting, integer-equivalent portable SIMD, per-call errors and atomic table initialization. File-safety tests cover destination races, symlinks, existing partial files, FIFOs, publication fallback and post-publication errors. Consumer tests compile and run C/C++ static and shared callers, inspect exports/imports and SONAMEs, and verify artifact hashes.

## Local verification

Full logs are retained in ignored `build/logs/` and `build/wsl-audit/`. Commands below were run from this checkout. `.venv/bin/python scripts/check-all.py --all-targets` finished with **25/25 gates passed**. The native Windows rebuild and runtime rerun also completed successfully. Later skill-instruction corrections and their contract checks passed both review-tool self-tests, skill validation, local-link checks and YAML parsing.

| Check | Observed result |
| --- | --- |
| `python3 scripts/build.py --target all` | Four targets built with warnings treated as errors |
| `python3 scripts/windows.py --powershell build.ps1` | Native Windows LLVM build succeeded from the WSL checkout |
| Windows correctness, API and fuzz through `scripts/windows.py` | 33 correctness and 8 API tests passed; seed 1 fuzz: 293 rejected, 7 still valid, no crash, hang or mismatch |
| Windows artifact tests | 2 passed; 2 POSIX-only checks skipped explicitly |
| Linux `tests/test_correctness.py` | 33 passed, including oracle and known-answer fidelity |
| Linux `tests/test_api.py` | 8 passed, including concurrent initialization and calls |
| Linux `tests/test_artifacts.py` | 4 passed, including all-target ABI inspection and native consumers |
| Linux `tests/fuzz_decode.py 300 1` | 293 rejected, 7 still valid; no crash, hang or mismatch |
| Portable x64 build plus correctness | 33 passed; this exercises portable arithmetic, not ARM execution |
| TODO graph self-test | 772 cases, zero failed; the absent optional sibling stamp-gate probe was explicitly skipped |
| TODO validation and projection | 28 files, 183 sections; zero fatal findings and zero warnings |
| Source claims | 62 claims hold |
| Claude review-tool and guard self-tests | 460 and 112 cases respectively, zero failed |
| Codex review-tool and lifecycle tests | 443 cases and 55 tests respectively, zero failed |
| Local archive installer fixtures | 4 passed, including idempotence and refusal of invalid receipts/archives |

The original Windows measurement tools remain available through `scripts/windows.py`. `research/ab.py bench -r1 syn_odd -t1` rebuilt its Windows harness and round-tripped that fixture successfully. Both benchmark modules imported under Windows Python, executed ffmpeg and read nonzero Windows peak memory; ffmpeg advertised libjxl. These are migration smoke checks. They do not replace the corpus protocol or establish a new performance comparison. Historical measurements remain in [measurements.md](measurements.md).

## Runner and intelligence evidence

All nine original lifecycle skills have independent Codex copies, plus a tenth `process-intelligence` skill. Native Codex discovery found all ten enabled through `.agents/skills/`. Seven project hooks were discovered without parse errors: SessionStart, UserPromptSubmit, PreToolUse, PostToolUse, Stop, Interrupt and SessionEnd. They were initially untrusted. A later `python3 .codex/runner/harness.py trust-check` returned `all required Codex hooks trusted and enabled`, including after the stopped-maintenance fixes. The runner refuses to arm without native trust. No live campaign was started as part of this audit.

Codex owns its skills, hook adapter, campaign state, review dispatcher, review evidence tools and intelligence implementation. Claude retains its own implementations. Their shared `.runner/admission.lock` prevents two campaigns from owning the same checkout. No worktrees, snapshot chains or recovery checkpoint layer were added. Intelligence captures require evidence and an owner; dispositions require both families to be stopped, and applied entries require a reachable implementation commit.

Private headless Claude slots were exercised through the pinned dispatcher with tools and MCP disabled. Source review identified file-publication, API, threading and artifact concerns; fixes and focused regressions followed. The follow-up codec review reported no High or Medium findings in its supplied source. Subsequent low-risk corrections added fallback/error tests, SONAME assertions and static-symbol namespace checks. The first narrow runner confirmation used `signoff`, `claude-opus-5-5`, high effort, with a 600-second deadline. It exited 124 without a verdict. The [review skill](../.agents/skills/review-todo-section/SKILL.md) (then at `.codex/skills/review-todo-section/SKILL.md` in `2c89e2e313d552ffdbcb167a2f9f67f1fb0414ac` <!-- drift:commit 2c89e2e313d552ffdbcb167a2f9f67f1fb0414ac .codex/skills/review-todo-section/SKILL.md -->) says "an outage with no verdicts does not earn the stamp"; no fallback or approval was invented. A later same-slot retry returned `HOLDS` on the unchanged supplied files, with a reported duration of 127455 ms. These reviews describe supplied working-tree source and do not stand in for section stamps or runtime proof.

The timed-out confirmation input is retained at `build/wsl-audit/runner-confirm-input.txt`, SHA-256 `12c590d19cc5163bf887dbd77998d1ab4d1c7914c5270049b202a214e1e1fdee`; its diagnostic is `runner-confirm.log` in the same directory. Both campaign controllers reported idle after verification.

### Stopped runner hardening

The operator explicitly kept the plan stopped while its runners were hardened. Fixture regressions reproduced the twelve-digit Origin mismatch, stop requests overwritten by pause, clock rollback losing interrupts, progress reads holding admission, run-file hard-link reuse and supervisor failures leaving active ownership. Fixes use canonical run ids, immutable request ids, bounded thread/process admission, source reads outside admission with revalidation, retained file identities and explicit supervisor parking. A start interrupted during native trust inspection now remains stopped. `status` reports folded operator requests; resume preserves newer requests.

During this work, importing `AdmissionBusy` before defining it caused the native PreToolUse hook to deny editing tools. The operator supplied the missing class. The hook now proves ownership through its small admission module before loading campaign code, so a broken controller remains editable only when the checkout is provably idle. Active and malformed ownership still fail closed. A regression test covers the observed lockout and the guarded maintenance boundary.

`runner-hardening-before.log` records 43 tests with six failures and three errors before the fixes. The first expanded suite records 51 passing tests in `runner-hardening-final-tests.log`; the repeated Claude guard suite records 112 cases with zero failures in `runner-hardening-claude-guard.log`. All lifecycle tests use temporary repositories and fake queue delivery.

The fresh source review used the private `signoff` slot and actual `claude-opus-5-5` model at high effort. Its first input, `runner-hardening-review-1-input.txt`, has SHA-256 `53a90ebf731f7541e97e9c940caa87f9e5ee2f59734ef6ffb873398845bbd79d`. The response identified one Medium finding: supervisor launch reacquired admission after saving active state, so lock contention could leave an unsupervised active owner. It also noted the related error-cleanup contention limit. The correction puts CLI activation and launch in one locked operation and keeps failed supervisors quiescent while retrying admission to park. A copied closeout in a new phase file is also refused. Regression coverage now includes real thread contention across start, resume and advance; `runner-hardening-round-2-tests.log` records 54 passing tests. The subsequent confirmation uses a fresh six-file snapshot in `runner-hardening-review-2-input.txt`, SHA-256 `660c3236c22227c87f5b830c5400accd9a22382e322d1feb3b081d632887a631`. Per-file hashes and receipt nonces are retained beside each input. No earlier verdict is transferred to changed source.

Round 2 returned `HOLD` with no High or Medium finding. Its remaining malformed-request edge case in launch-failure cleanup was also fixed and given a discriminating regression. The final input, `runner-hardening-review-3-input.txt`, has SHA-256 `4b97afc71946cbcb33a7fed6a33ad2a4b8517263f6942a6636b8a786234be3f1`. Round 3 returned `HOLD`, again with no High or Medium defect or introduced regression. The receipt and all six supplied file hashes were verified after the verdict. `runner-hardening-round-3-tests.log` records **55 passing tests**. Corrupt request files are retained for explicit diagnosis, and automatic parking still requires writable state; no recovery/checkpoint system is claimed.

The complete JSON envelopes retain actual producer usage and timing: round 1 reported 181284 ms and USD 0.5927422; round 2 reported 39704 ms and USD 0.3915902; round 3 reported 24641 ms and USD 0.3639222. These are provider-reported source-review durations and costs. The reviewers explicitly did not execute tests. The local test runner supplies runtime fixture evidence, and the actual Codex and Claude controllers both remain idle.

Codex intelligence retains four open observations owned by D00 T04 §3, including the inherited acceptance text, the reproduced stop ordering defect, the import lockout and the Origin mismatch. Their proposed fixes are implemented in the working tree, but no applied disposition or archive is claimed before an implementation commit and its required review exist.

## Delivery and limits

Four development ZIPs and SHA-256 sidecars are available under `build/packages/`: `rasterfold-windows-x64-dev.zip`, `rasterfold-linux-x64-dev.zip`, `rasterfold-android-arm64-dev.zip` and `rasterfold-android-x64-dev.zip`. Each archive passed CRC and manifest-hash verification and contains its CLI, libraries, header, license and usage guides.

The former `Bin/` output directory was replaced with lowercase `bin/<target>/` packages. Each has conventional `bin/`, `lib/` and `include/` destinations; Windows PDBs live in `symbols/`. Windows DLLs sit beside the executable, and the old loose compatibility copies were removed. Both native and cross builds use this layout. PE/PDB identifiers are checked for both Windows binaries. Packaging verifies the header and symbol hashes as well as libraries and executables, preserves the directory structure, and reproduced identical ZIP bytes from unchanged artifacts. The exact member sets, sidecars and hashes passed for all four packages; C and C++ static/shared consumers also passed using an extracted Linux ZIP. Layout evidence is retained in the `layout-*.log` files under `build/wsl-audit/`.

The repository now includes an original SVG identity, a rewritten landing README, build/API guides, contribution and security policies, issue/PR templates, dependency updates, and pinned build/artifact workflows. The presentation takes structural cues from Isotone and Impossible OS; the intelligence design takes the stopped-maintenance pattern from JMR Online.

**Presentation visual inspection and local-link check (2026-10-24):** `assets/brand/cover.svg` (`viewBox="0 0 1280 560"`, `role="img"`, 31 SVG elements) and `assets/brand/icon.svg` (`viewBox="0 0 256 256"`, `role="img"`, 6 SVG elements) were rasterized with `rsvg-convert` (`build/wsl-audit/cover-render.png` 1280x560 RGBA and `build/wsl-audit/icon-render.png` 256x256 RGBA) and visually inspected alongside `README.md`: the dark-navy grid background, `RIZONESOFT / IMAGE CODECS` header, `RasterFold` title, `Every pixel, preserved.` subtitle, bottom `WINDOWS / LINUX / ANDROID` + `RGB8` / `FORMAT 02` / `CLI + C API` footer bar, and folded lime-cyan-coral mark render cleanly with no clipped glyphs, overlapping text, or broken paths. Separate automated link verification resolved all 39 local Markdown and HTML links across `README.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `docs/building.md`, `docs/library.md`, and `docs/wsl-migration-audit.md`, and `python3 scripts/drift-check.py` passed across all 159 supported documents with 0 errors.

Android artifacts are cross-compiled and inspected, with API 21 minimum and 16 KiB segment alignment. No device or emulator was available for runtime verification. Device tests must cover both ABIs and app-sandbox file publication before release. Linux evidence is from this WSL host, not a distribution compatibility matrix. The Windows `.lib` consumer was linked with LLVM, not Microsoft's linker.

The runner is tested for accidental continuation, stale messages, competing ownership, stop/interrupt races and bounded repair. It is not a security sandbox for an agent that can edit its own source and state. Native hook trust is verified; a live attended campaign smoke test remains a separate runtime step, deliberately unstarted at the operator's request.

No new JPEG XL performance claim, remote CI success, published release, commit or TODO verification stamp is asserted here. See [building.md](building.md), [library.md](library.md), [AGENTS.md](../AGENTS.md) and [GEMINI.md](../GEMINI.md) (replacing the historical `.codex/README.md` guide from `2c89e2e313d552ffdbcb167a2f9f67f1fb0414ac` <!-- drift:commit 2c89e2e313d552ffdbcb167a2f9f67f1fb0414ac .codex/README.md -->) for reproducible commands and operational boundaries.
