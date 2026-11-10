---
schema_version: 1
id: lsp-subagents-unattended
domain: 00-workspace
status: draft
title: "TODO-05 -- LSP Servers, Spark/Ember Subagents, Tool-Stack Tuning, and the Unattended Run"
depends_on: []
track: W1
---

# TODO-05 -- LSP Servers, Spark/Ember Subagents, Tool-Stack Tuning, and the Unattended Run

> **Goal:** A developer or agent opening this checkout gets working language servers for the C11 codec and the Python tooling from the pinned toolchain, near-mechanical work runs on Muse Spark and Muse Ember subagents under lead verification, the build and dev-tool stack is measured and tuned for this host class with no cargo-cult additions, and one Windows Desktop shortcut starts an unattended Muse campaign (yolo, no user-input tool, straight on `main`) whose goal is RasterFold 100% complete, guarded by a fail-loud preflight so it either runs to closeout or does not start.

> [!IMPORTANT]
> **Current state (verified 2026-11-10):** The pinned LLVM tree already ships clangd 23.1.2 and clang-tidy (`tools/llvm-mingw-linux/bin/`), Ninja 1.13.2 is pinned in `toolchain.json` and `scripts/build.py` already generates a per-target Ninja graph plus `build/<target>/compile_commands.json` and links with LLD, so the build was never missing Ninja; what was missing is the editor-facing root database and LSP configuration, any Spark/Ember agent definitions, a preflight, and a launcher. Measured on this host (10 cores, 13 GB RAM) on 2026-11-10: a cold linux-x64 build takes 3.6 s at `-j4` (4.1 s at `-j8`), a no-op build 0.00 s, and the largest translation unit (`src/cm.c`) peaks at 97 MB per clang process, while `python3 scripts/todo-graph.py validate` takes 16.2 s (of which about 10 s is 1261 `subprocess.run` git calls, mostly stamped-section receipt checks in `scripts/todo-validate.py`) and `scripts/drift-check.py` 10.6 s, so the dev-loop cost is the plan tooling, not the compiler. The Muse model catalog on this machine offers `muse-spark-1.3` and `muse-ember-1.0`, and `muse --agents <JSON>` accepts `{description, prompt, model}` agent definitions (probed offline with `--provider echo`). Sections 1, 2, 4, and 5 were built in the attended operator session of 2026-11-10 and await their review stamps; section 3 records the measurements and the tools not adopted; section 6 is the one piece of new code the campaign itself still owes.
<!-- claim: exists scripts/gen-compdb.py -->
<!-- claim: exists .clangd -->
<!-- claim: exists pyrightconfig.json -->
<!-- claim: exists .agents/muse-agents.json -->
<!-- claim: exists scripts/unattended-preflight.py -->
<!-- claim: exists scripts/unattended-run.sh -->
<!-- claim: exists docs/unattended-run.md -->
<!-- claim: count "\"name\": \"ninja\"" toolchain.json = 1 -->
<!-- claim: count "compile_commands.json" scripts/build.py = 1 -->

## Inputs

- [`scripts/build.py`](../../scripts/build.py) -- already emits each target's Ninja graph and compilation database; §1 merges them, §3 measures them
- [`toolchain.json`](../../toolchain.json) -- the LLVM, Ninja, and NDK pins §1 and §3 consume; no pin moves in this file
- [`.conclave/reviewers.toml`](../../.conclave/reviewers.toml) -- the reviewer pin (muse routes to Astra via the Codex CLI) the unattended run in §4 relies on never waiting for
- [`scripts/campaign_guard.py`](../../scripts/campaign_guard.py) and the `process-plan` skill -- the guard file and in-session heartbeat §4 starts; Muse has no Stop hook, so those two carry the run
- [`docs/building.md`](../../docs/building.md) -- the host SDK requirements §1 extends with the editor setup
- -> XREF: D00 T01 §3 -- the static analysis gate owns the per-target compilation databases §1 publishes to editors, and the clang-tidy configuration `.clangd` mirrors
- -> XREF: D00 T01 §10 -- the OpenCode campaign hook and heartbeat are the precedent for mapping a harness onto the run guard; §4 does the same for the Muse launcher

## Outcome

- Opening the checkout in an editor gives clangd navigation, diagnostics, and clang-tidy on the C11 sources with the exact build flags, and Basedpyright navigation and undefined-name checks on the Python tooling, both from pinned binaries with memory bounds for a 13 GB host.
- `rf-spark-scout`, `rf-spark-hands`, and `rf-ember-literal` are defined once in `.agents/muse-agents.json`, loaded by the launcher, and bound by AGENTS.md rules that keep judgment, codec work, reviews, stamps, and commits on the lead model.
- The tool stack has a measured verdict per candidate tool (Ninja and LLD already in; compiler caches and alternate linkers rejected with numbers), and the one measured bottleneck, receipt validation's per-section git calls, has an owner and a stop condition in §6.
- `scripts/unattended-preflight.py` proves, before any unattended start, that the tree is clean on `main`, the graph and plan are current, the guard self-test and reviewer pin pass, the reviewer CLIs exist, the compilation database is current, the agent definitions load, and disk and RAM headroom suffice.
- A Windows Desktop shortcut starts the whole thing in one double-click and either enters the campaign or shows the preflight failure.

**Adjacency:** list=not-applicable (no record list is added); document=applicable @ D00 T05 §4; settings=applicable @ D00 T05 §1; reporting=applicable @ D00 T05 §4; notifications=not-applicable (the preflight and the campaign report in their own terminal and run file); permissions=applicable @ D00 T05 §4; audit=not-applicable (git history and the campaign run file already record the run); exchange=not-applicable (no artifact import or export is added); reverse=applicable @ D00 T05 §4

**Adjacency rationale:** This file adds editor configuration (settings), one operator document and one preflight report (document, reporting), a deliberate permission posture for the unattended launcher (permissions), and the stop and resume path that reverses a start (reverse), all owned inside this file. It adds no codec surface: no list to browse, no notification channel, and no import or export path, and the audit trail stays where it already lives, in git and in `docs/phase-runs/`.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | LSP servers for C and Python, wired | -- |  [x]   |
|   2   |   §2    | Spark and Ember subagents under lead verification | -- |  [ ]   |
|   3   |   §3    | The measured tool stack: what stays, what is not adopted | §1 |  [ ]   |
|   4   |   §4    | Unattended run: preflight, launcher, and goal | §2 |  [ ]   |
|   5   |   §5    | The Windows Desktop shortcut | §4 |  [ ]   |
|   6   |   §6    | Batch the git calls in receipt validation | -- |  [ ]   |

---

## 1. LSP Servers for C and Python, Wired

> **Started:** 2026-11-10T16:00:21Z

**Job:** A user can open this checkout in an editor and get clangd and Basedpyright language-server settings that resolve the pinned binaries, the exact build flags, and the project include paths with no hand configuration.

The binaries were never the gap: the pinned LLVM tree ships clangd and clang-tidy, and `scripts/build.py` writes a compilation database per target. Editors look at the repository root, so this section publishes a merged root database and the configuration that points both language servers at the pinned tools with memory bounds for this host class (13 GB RAM, about 12 C translation units, 60 Python files). Basedpyright is a new Python dependency beyond stdlib and Pillow; the reason is recorded here as AGENTS.md requires: it is the editor language server for the Python tooling, pinned like Pillow, installed only into `.venv`, and not a gate (its 2026-11-10 basic-mode baseline on the untyped tooling is 684 findings, mostly Optional-member noise over JSON-shaped dicts; making it a gate is a separate decision nobody has taken).

**Corrected 2026-11-10 (plan review `docs/reviews/00-workspace/D00-T05-s1.md`, run `527a1f06ac43`):** the plan review found three P1 gaps, each verified against source before acting: provisioning never generates the root database, so a fresh clone has no editor navigation until a build happens; `--check` only re-merges the per-target databases, so a flag change in `scripts/build.py` leaves every database stale while the check passes, and the fresh-clone synthesis duplicates build flags instead of deriving them; and the Windows editor path documents only the clangd override while the synthesis always picks Linux entries for common sources. The fixed "20 entries" criterion below becomes source coverage (every `src/*.c` has an entry), and three new items carry the fixes. Windows editor runtime proof cannot execute in this Linux context: the fix is host-correct configuration and documentation, with the Linux clangd and langserver probes as the driven evidence, recorded honestly rather than relabeled.

- [x] Write `scripts/gen-compdb.py`: merge `build/<target>/compile_commands.json` (linux-x64 wins per file, windows-x64 contributes the freestanding `mem.c`/`sys.c`, android fills gaps) into a gitignored root `compile_commands.json` written atomically, synthesizing entries mirroring `scripts/build.py` on a fresh clone, with `--check` failing when the root database is stale. Done when: every `src/*.c` file has an entry in the root database and `--check` exits 0.
- [x] Add `.clangd` (root compilation database, `-Werror` removed for editing, clang-tidy bugprone/analyzer/performance mirroring `.clang-tidy`, strict unused/missing includes, background index) and `.vscode/settings.json` plus `.vscode/extensions.json` pointing at `tools/llvm-mingw-linux/bin/clangd` with `-j=4`, `--malloc-trim`, `--pch-storage=memory`, and `--header-insertion=never`, and recommending clangd and Basedpyright while disabling the C/C++ IntelliSense engine. Done when: `tools/llvm-mingw-linux/bin/clangd --check=src/cm.c --compile-commands-dir=.` resolves definitions from the index with no code diagnostics (the 8 `SwapBinaryOperands` tweak failures it prints are a clangd 23 `--check` quirk, not diagnostics).
- [x] Pin `basedpyright==1.40.2` in `requirements-dev.txt`, install it into `.venv`, and add `pyrightconfig.json` scoped to `scripts`, `tests`, `research`, and the two `tools/*.py` provisioners, explicitly excluding the downloaded tool trees under `tools/` (an unscoped first run checked 2236 files in 480 s; scoped it checks 60 files). Done when: `.venv/bin/basedpyright` completes on the scoped tree and the editor resolves `basedpyright-langserver` from `.venv`.
- [x] Document the editor setup and the Windows `clangd.path` override in `AGENTS.md` (the LSP working rule) so a cold session finds it without this section. Done when: the rule names `scripts/gen-compdb.py`, `.clangd`, `pyrightconfig.json`, and the not-a-gate baseline.
- [x] Derive every editor flag from `scripts/build.py` (plan-review P1): extract the per-target compile-flag construction into pure helpers `build.py` and `scripts/gen-compdb.py` share, make `--check` recompute the expected entries from `build.py` so a changed build flag fails it even when all databases agree with each other, make the fresh-clone synthesis use the host target (windows-x64 on a Windows host) with the other platform's unique units added, refresh the root database at the end of every successful build, and gate it in `scripts/check-all.py`. Done when: the per-target and root databases are byte-identical to the pre-change ones on unchanged flags, and tampering a flag in a per-target database (or in `build.py`) makes `--check` exit 1.
- [x] Generate the root database during provisioning (plan-review P1): `tools/provision.py` runs `scripts/gen-compdb.py` after the venv install and `--verify` runs `--check`; `tools/provision.ps1` mirrors it. Done when: a provision run on a tree with no root database leaves one covering every `src/*.c`, before any build.
- [x] Document the editor setup for both hosts in `docs/building.md` (plan-review P1/P2): the refresh command, the Windows overrides for both `clangd.path` and `python.defaultInterpreterPath`, and the dated not-a-gate Basedpyright baseline where an editor user finds it; `.vscode/settings.json` names both overrides. Done when: the document's provisioning paragraph no longer mentions only Pillow and a Windows reader can wire both servers from it alone.
- [x] Commit: `"workspace: LSP, Spark/Ember subagents, measured tool stack, unattended launcher (D00 T05)"` (shipped in `a24edbf` under the whole-file message; the candidate range for this section starts there)

**Test checkpoint:** Driven run with evidence: `python3 scripts/gen-compdb.py && python3 scripts/gen-compdb.py --check` exit 0 with every `src/*.c` covered, `clangd --check` on `src/cm.c` finds definitions with zero code diagnostics, `.venv/bin/basedpyright --version` prints `basedpyright 1.40.2` and a scoped `.venv/bin/basedpyright` run completes over the `pyrightconfig.json` tree, deleting the root `compile_commands.json` makes `--check` exit 1 (then regenerate), and tampering one flag in `build/linux-x64/compile_commands.json` makes `--check` exit 1 (then rebuild).

> **Verified:** 2026-11-10 | §1 | 494fed1969921270a4de654e28417329088129c5
> **Review:** `docs/reviews/d00-t05-s1.json`
## 2. Spark and Ember Subagents Under Lead Verification

> **Started:** 2026-11-10T20:10:19Z
> **Restarted:** 2026-11-10T20:29:00Z | plan review 7ed9b3855d4feed46ffc8b4085945b70eabcd8b4 | checkpoint corrected after Started was committed, so the plan review read a descendant commit

**Corrected 2026-11-10 (operator directive):** the unattended launcher now starts at `--reasoning-effort medium`, not `high` (was `high` in `scripts/unattended-run.sh`, `docs/unattended-run.md`, and this section). Reversal cost: one flag in the launcher plus the two synced prose copies.

Near-mechanical and dummy work (searches, counts, claim re-measurement, verbatim replacements, single gate runs) should not spend the lead model's effort, and must not spend the project's quality either. The split that keeps both: cheaper models produce, the lead verifies every line that lands, and the classes of work that carry judgment never delegate at all. This mirrors the existing rule for cheaper implementers in review (a passing test does not prove equal quality; read the full diff), applied to Muse Spark 1.3 and Muse Ember 1.0 from this machine's model catalog.

**Corrected 2026-11-10 (operator directive, same day):** the roster became a five-tier ladder to save lead usage: tier 0 scripts (deterministic work is never model work; a recurring mechanical step gets scripted once), tier 1 `rf-ember-literal`, tier 2 the two Spark agents, tier 3 `rf-wonder-hands` (muse-wonder-1.0, most section implementation under a lead-written spec), and the lead (muse-wonder-1.0 in an unattended campaign per the operator directive recorded in §4; attended sessions may run muse-wonder-full-1.0) keeping planning, format decisions, gate verdicts, stamps, and commits. Each tier escalates one step up by reply token (`ESCALATE-SPARK`, `ESCALATE-WONDER`, `ESCALATE-LEAD`, replacing the original `NEEDS-LEAD`) and never improvises past its boundary. The `AGENTS.md` tiered-subagents rule and the table in `docs/unattended-run.md` carry the ladder; this section's items below are read with `rf-wonder-hands` included and the escalation tokens as named here. Proved the same day: a Wonder-tier delegation implemented a lead-specified parser in a scratch file and its self-check printed PASS, re-verified independently by the lead including an edge case the spec's check did not cover.

- [x] Define `rf-spark-scout` (muse-spark-1.3, read-only: lookups, counts, inventories, claim re-measurement, one named gate quoted verbatim, never edits), `rf-spark-hands` (muse-spark-1.3, an exact lead-written edit spec outside codec logic, stops on any mismatch), and `rf-ember-literal` (muse-ember-1.0, verbatim replacement, one exact command, list reshaping, no interpretation) in `.agents/muse-agents.json`, each with its tier escalation token (`ESCALATE-SPARK` / `ESCALATE-WONDER` / `ESCALATE-LEAD`) instead of improvisation. Done when: `muse exec --provider echo --yolo --agents "$(cat .agents/muse-agents.json)" probe` exits 0.
- [x] Bind the roster in `AGENTS.md`: the lead reads every Spark/Ember diff in full and re-runs or spot-checks every fact a decision rests on; judgment, codec work, reviews, stamps, and commits never delegate; a new class of mechanical work is replayed once on a known-answer input before it delegates. Done when: the working rule names all three agents, their models, and the verification duty.
- [x] Prove the roster on known answers: one scout count the lead can check with one command, and one literal replacement in a scratch file, through a live `muse` run started with `--agents`. Done when: both results match the lead's independent check byte for byte and the run is quoted in this section's stamp. (Proved 2026-11-10 in the authoring session: Spark counted `^## ` lines in this file as 10, matching `grep -c` = 10; Ember, delegated as `rf-ember-literal`, replaced `REPLACE_ME` with `DONE_42` in a scratch file and nothing else, verified by reading the file back.)
- [x] Commit: `"workspace: wire Spark and Ember subagents for mechanical work (D00 T05 §2)"` (candidate commits ce85200..HEAD; checkpoint probes quoted in docs/phase-runs/2026-11-10-phase-7-muse.md, Ember removed this run after failing ESCALATE-SPARK, class moved to Spark)

**Test checkpoint:** Driven run with evidence: the echo-provider load probe exits 0, and the two known-answer delegations above match independent lead checks; a live `rf-spark-hands` exact-spec edit in a scratch file matches the lead byte and path-scope check, and a mismatched spec makes it reply `ESCALATE-WONDER` with no edits; a scout asked a judgment question replies `ESCALATE-WONDER`, and a Wonder asked a format decision replies `ESCALATE-LEAD` (the failure modes this section exists to prove). Ember exception, measured 2026-11-10: `rf-ember-literal` interpreted a non-literal task instead of replying `ESCALATE-SPARK` under both the native subagent harness and `muse exec` with the loaded roster (2 of 2 failures, recorded in `docs/phase-runs/2026-11-10-phase-7-muse.md`), so tier 1 is suspended and literal work routes to Spark; Ember re-enters only when a pinned Ember model passes this same refusal probe plus the verbatim replacement proof. Removal precedence: a silent wrong output, an out-of-scope edit, or non-reproducing evidence removes that agent for the run at once (recorded in the run file, work redone one tier up), and that removal outranks the one-repair-attempt and two-failure promotion rules, which govern honest failures only; a deliberately incorrect scratch result is rejected, recorded, and reassigned one tier up with unrelated work preserved.

## 3. The Measured Tool Stack: What Stays, What Is Not Adopted

The operator asked whether tools like Ninja would be a good addition. The honest answer is measured, per tool, on 2026-11-10, and mostly already settled by what is pinned: Ninja 1.13.2 drives every build through `scripts/build.py`, LLD links everything, and clang-tidy is gated. The candidates that remain (a compiler cache such as ccache or sccache, the mold linker, a different build generator) are decided against the numbers below, and this section is the record so the question does not reopen without new evidence. Stop condition: a candidate is adopted only if it cuts a measured dev-loop cost by at least a third on this host; otherwise the negative result ships, which is what happened for each candidate below.

- [x] Measure the build as it stands: cold linux-x64 Ninja build 3.6 s at `-j4` and 4.1 s at `-j8`, no-op 0.00 s, `src/cm.c` peak 97 MB per clang process, so even `-j8` peaks under 1 GB and the default `min(cpu, 8)` in `scripts/build.py` stands. Done when: the numbers are recorded here and in the `AGENTS.md` LSP rule with their date.
- [x] Reject ccache/sccache and mold with reasons a future reader can re-check: a cache cannot beat a 3.6 s cold build by a third after hashing and would sit in the PGO provenance path `scripts/build.py` validates by source hash; mold cannot beat LLD by a third on a 12-object link that is not separately measurable inside 3.6 s. Done when: both rejections are stated here as decisions, not preferences, with the adoption bar that would reopen them (a translation-unit count or link time that makes either win by a third).
- [x] Measure where the dev loop actually spends: `todo-graph.py validate` 16.2 s and 55 MB, `drift-check.py` 10.6 s, `todo-claims.py` 0.47 s, `plan-audit.py` 0.43 s, and a cProfile of validate attributing about 10 s to 1261 `subprocess.run` calls. Done when: the profile lands the fix as §6 of this file instead of a vague "make validate faster" note.
- [ ] Commit: `"workspace: record the measured tool-stack verdicts (D00 T05 §3)"`

**Test checkpoint:** Corpus-style measurement quoted with its commands and date (the builds, the per-gate timings, the profile), each reproducible with the commands in this section; no toolchain pin moved and `python3 scripts/build.py --target linux-x64` still exits 0 warning-free.

## 4. Unattended Run: Preflight, Launcher, and Goal

**Job:** A user can start one unattended campaign whose contract lives in documents (`docs/unattended-run.md`), reports every precondition as a pass or fail line before starting, runs with the permission posture the operator chose (yolo, no user-input tool), and can cancel the run through the campaign guard (which deletes the heartbeat job) and resume it later without deleting files by hand.

An unattended run fails in boring ways long before it fails in interesting ones: a dirty tree, a stale plan, a reviewer CLI that moved, a disk an orphaned writer can fill. The preflight checks each precondition the `process-plan` run guard assumes and refuses the start out loud. The launcher then runs Muse interactively (the in-session heartbeat only exists inside a live session; a headless `muse exec` loop would need a new external supervisor and diverge from `process-plan`, which is why it was rejected), straight on `main`, with `--yolo --disable-request-user-input --model muse-wonder-1.0 --reasoning-effort medium`, the §2 agents loaded, and a seed prompt whose first act sets the session goal to finishing the project 100%.

**Corrected 2026-11-10 (operator directive, mid-run):** the launcher starts on Muse Wonder (`muse-wonder-1.0`), not Muse Wonder Full. The campaign is paving the way for Wonder and then Spark to take it over, so the mechanics are written to be followed cold by the weakest tier that will read them: exact commands, exact acceptance checks, no assumed context. The desktop shortcut itself is unchanged (it runs `scripts/unattended-run.sh`, which carries the model pin); §5's read-back proof is unaffected.

- [x] Write `scripts/unattended-preflight.py`: one PASS/FAIL line each for branch `main`, clean tree, `core.hooksPath`, no live campaign guard, `validate`, `plan --check`, claims, plan audit, drift, campaign-guard self-test, reviewer pin, `muse`/`codex`/`agy` on PATH, pinned clang/clangd/clang-tidy/ninja, compilation database current, Basedpyright present, agent definitions load under the launcher flags with the exact three-agent roster, disk over 20 GB free, RAM over 2 GB available; exit 1 on any FAIL, starting and writing nothing. Done when: it exits 0 on this tree as committed and exits 1 naming the check when one precondition is broken in a scratch probe (a dirty file suffices).
- [x] Write `scripts/unattended-run.sh` (preflight, then `exec muse` with the flags above and the seed prompt) and the operator documents for the run, `docs/unattended-run.md` (the contract: the verbatim seed prompt with the 100% goal, why interactive yolo is the shape that actually runs unattended, the Spark/Ember quality rule, stop and resume). Done when: both exist, the script is executable, and the prompt in the script matches the document byte for byte. (Updated 2026-11-10: both copies gained the operator's autonomy clause verbatim, re-verified byte-identical, with the standing rule in `AGENTS.md`.)
- [ ] Dry-prove the launch without starting a campaign: run the preflight green, then start the launcher against a prompt that only creates the goal, lists the loaded agents, and ends the turn with no guard acquired. Done when: the session shows the goal set, the three agents available, `--disable-request-user-input` in effect, and no `build/claude-campaign-guard.json` left behind.
- [ ] Commit: `"workspace: unattended campaign preflight and launcher (D00 T05 §4)"`

**Test checkpoint:** Driven run with evidence: preflight all-PASS quoted, the dirty-tree probe FAIL quoted, and the dry launch's goal and agent roster quoted; `python3 scripts/campaign_guard.py --self-test` exits 0.

## 5. The Windows Desktop Shortcut

The operator double-clicks; everything else is §4's job. The shortcut lives on the Windows desktop (this checkout is reached through WSL), opens Windows Terminal, and runs the launcher inside the `Ubuntu` distribution at this checkout, so the visible artifact is one `.lnk` whose target line a reader can audit.

- [x] Create `RasterFold Unattended Run.lnk` on the Windows desktop (`/mnt/c/Users/DerickPayne/Desktop/`) through Windows PowerShell's `WScript.Shell`, targeting `C:\Windows\System32\wsl.exe` with arguments `-d Ubuntu --cd /home/derickpayne/projects/RasterFold -- bash scripts/unattended-run.sh` (wsl.exe directly, not `wt.exe`: one less alias that can move, and the launcher itself keeps the window open on a preflight failure). Done when: the `.lnk` exists and reading it back through the same COM object prints that exact target and arguments.
- [ ] Prove the double-click path once from Windows (or `cmd.exe /c start` on the `.lnk`): a terminal opens, the preflight prints, and Muse starts on `main` with the goal prompt; stop it through the operator-stop path and confirm no guard or heartbeat is left. Done when: the terminal output is quoted in this section's stamp.
- [ ] Commit: `"workspace: desktop shortcut for the unattended run (D00 T05 §5)"`

**Test checkpoint:** Driven run with evidence: the read-back target line and the launched terminal's preflight output, quoted; the stop leaves `python3 scripts/campaign_guard.py whoami` printing no live owner for this tree.

## 6. Batch the Git Calls in Receipt Validation

`python3 scripts/todo-graph.py validate` measured 16.2 s on 2026-11-10, and a cProfile run attributes about 10 s of it to 1261 `subprocess.run` invocations, the bulk from stamped-section receipt checks (`scripts/todo-validate.py` `partial_flip_scan` and `classify_unchecked`, `todo-graph.py` `validate_section_receipt`) that shell out to git per section or per file. Validate runs in the pre-commit hook on a staged export and at every section boundary of an unattended campaign, so this is the highest-leverage speed fix in the dev loop. Stop condition: keep the change only if validate's wall time on this tree drops by at least a third with byte-identical verdicts (same FATAL/WARN lines) on the current tree and on the self-test fixtures; otherwise ship the measurement and revert.

- [ ] Profile first and name the exact call sites and their git argv shapes in this section before editing. Done when: the sites, counts, and the batched replacement for each (one `git log`/`git show`/`git cat-file --batch` pass feeding an in-memory map, or equivalent) are written here.
- [ ] Batch the calls at the owning layer (the receipt/validation code, not a cache wrapper around it): one git enumeration per validate run, lookups served from memory, and identical verdict text. Done when: validate on this tree prints the same summary line as before (`0 fatal` with the same advisories) and the sabotage fixtures in the self-test still fail the same way.
- [ ] Measure after: three alternating before/after validate runs on this tree quoted with wall times, plus `python3 scripts/todo-graph.py self-test` green. Done when: the median improvement clears the one-third bar or the change is reverted and the negative result recorded here.
- [ ] Commit: `"workspace: batch git calls in receipt validation (D00 T05 §6)"`

**Test checkpoint:** `python3 scripts/todo-graph.py self-test` exits 0, `python3 scripts/todo-graph.py validate` prints the identical verdict block as the pre-change run quoted beside it, and the before/after timings are recorded with their commands and date.

## Verification

- [ ] `python3 scripts/unattended-preflight.py` exits 0 with every line PASS on the committed tree
- [ ] `python3 scripts/gen-compdb.py --check`, `python3 scripts/todo-graph.py validate`, `python3 scripts/todo-graph.py plan --check`, `python3 scripts/todo-claims.py`, and `python3 scripts/drift-check.py` all exit 0
- [ ] `python3 scripts/build.py --target linux-x64` exits 0 with no warning, and `clangd --check` on `src/cm.c` reports no code diagnostics
- [ ] The desktop shortcut read-back prints its target, and one launched dry run was stopped cleanly with no guard left

