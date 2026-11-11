---
schema_version: 1
id: operator-setup
domain: 99-manual
status: draft
title: "TODO-01 -- Operator Setup and Approvals (Manual)"
depends_on: []
track: OP1
---

# TODO-01 -- Operator Setup and Approvals (Manual)

> **Goal:** Every step only the operator can take is written down with the exact command or decision, so no agent run stalls wondering whether it may act: the retired run-guard folders are gone, the GitHub CLI is logged in for the CI read-back, GitHub Pages is enabled for the docs, and the two standing decisions that let a campaign run through the roadmap (format-version approval and the missed-milestone policy) are recorded in the operator's words. The concrete HEVC distribution decision is a release-only prerequisite.

> [!IMPORTANT]
> **Current state (verified 2026-11-07):** The operator deleted the folders `.grok/`, `guard/`, and `build/review-20260928/` from the previous developer's tooling on 2026-09-29 (the agent session's safety rules had refused the deletion), and `.gitignore` no longer lists `.grok/` or `guard/`. The WSL checkout now has an authenticated GitHub CLI, verified by successful CI readback and pushes on 2026-09-30; a new clone must verify its own identity. Format version 3 exists since `D01 T03 §1` (2026-10-27); on 2026-09-29 the operator gave the standing approval and the missed-milestone policy recorded in §3 and §4, and on 2026-10-29 under §4 (after the five remaining `docs/research.md` section 4 model experiments `D01 T02 §2` to `§6` missed their bars) the operator authorized updating Milestone 4 (`D05 T01 §4`) in `research/milestones.py` to match the shipped Format Version 3 results (`photo` `0.9813 <= 0.982` of JPEG XL e9 and `0.9695 <= 0.970` of e7, `tp` `0.9853 <= 0.986` of e9, `31 of 39` photos below e9 and `39 of 39 <= 1.0000` of e7, three-thread decode `<= 0.6` of JPEG XL's, and the 7-model `alloc_peak <= 10.0 MB` and `<= 1.27x` 5-model Milestone 3 timing envelope). On 2026-10-29 the completion review of `D05 T01 §4` found two of those figures unmet (`kodim17` is 12 bytes over JPEG XL e7, `384,835` vs `384,823`, so 38 of 39 photos are at or below e7, and the 4,095-pixel `syn_odd` encodes at `1.333x` at `-t1` from a fixed counter-table clearing cost), and the operator selected this option, quoted from the session: "Change the target to the measured results and stamp now: 38 of 39 photos at or below e7 (kodim17 +12 bytes, 1.00003); timing at most 1.27x on every set total and every photo, with images under 16K pixels allowed a fixed +1 ms instead of the 1.27x ratio. I'd also file a speed section for the small-image cost (only clear the tables the image uses, or clear them in place)." On 2026-10-30 under §4, after the match model and all three follow-ups for Milestone 5 (`D05 T01 §5`, `D01 T04 §2` to `§9`) missed, the operator chose, quoted from the session: "Relax the timing or memory rule for this milestone; v2 is the nearest candidate. And revise all other milestones to avoid more blockers like this." Milestone 5 now carries an envelope for `D01 T04 §8`'s v2 (time at most 1.15x Milestone 4 on every set total and 1.6x per image of at least 16,384 pixels, covering `syn_ui`'s `-t1` encode ratio where v2's better gradient prediction keeps its CM stream under `syn_ui`'s 1,378-byte Method 3 cap at `run_pipelined`'s `y = 64` 32-row check and aborts at `y = 96` (`97` vs `65` rows, `1.535..1.566x` on an idle machine vs `1.434x` under load in `D01 T04 §8`), with codec working memory unchanged and the input file's growth not counted) and v2 is ported by `D01 T04 §10`; from Milestone 6 the generic no-regression rule in `research/milestones.py` became a cost envelope (set totals within 0.1% and each photograph within 0.5% in size, time within 1.10x, 1.20x on `synth`, from pooled alternating runs, codec working memory within 5%), the all-or-nothing targets of Milestones 7 to 10 gained tolerances, the equal-cost wording of Milestones 14, 16, 22 and 25 became the envelope, and each planning section from Milestone 11 states its bars with tolerances. On 2026-11-07 under §4, during the completion review of `D01 T05 §4` for Milestone 7 (`D05 T01 §7`), the operator approved the Milestone 7 encode timing comparison as `python research/vsjxl.py -n5` at default threads (`<= 0.3x` JPEG XL e7, with `--threads 1` recorded alongside), quoted from the session: "(Recommended) Approve Milestone 7 encode timing as `python research/vsjxl.py -n5` at default threads (`0.147x` / `0.144x <= 0.3x` JPEG XL e7, with `--threads 1` recorded alongside at `0.422x` / `0.399x`), and record this decision under `D99 T01`". GitHub Pages is enabled (`build_type: workflow`, public project-site visibility at `/RasterFold/`), recorded in `docs/site-content.json`.
<!-- claim: count "^\.grok/$" .gitignore = 0 -->
<!-- claim: count "^guard/$" .gitignore = 0 -->
<!-- claim: count "^#define RF_VERSION 3u$" src/rf.h = 1 -->
<!-- claim: exists docs/site-content.json -->
<!-- claim: count "\"base_path\": \"/RasterFold/\"" docs/site-content.json = 1 -->

## Inputs

- [`.gitignore`](../../.gitignore) -- the two lines §1 removes after the folders are gone
- [`AGENTS.md`](../../AGENTS.md) -- "The format is frozen per version", which §3 approves an exception to
- -> XREF: D00 T01 §2 -- the CI read-back that waits on §2's login
- -> XREF: D01 T03 §1 -- the format version 3 port that §3's standing approval covers
- -> XREF: D08 T01 §3 -- the Pages publishing that waits on §5

## Outcome

- `.grok/`, `guard/`, and `build/review-20260928/` no longer exist, and `.gitignore` no longer lists `.grok/` or `guard/`.
- `gh auth status` reports a logged-in account on the development machine.
- This file records the operator's standing approval of format versions (§3) and the missed-milestone policy (§4) in the operator's own words.
- GitHub Pages serves the documentation site from the `pages` workflow.

**Adjacency:** all=not-applicable (operator-only housekeeping, a login, and a decision record; no codec behavior)

**Adjacency rationale:** Each section is a step a person takes; none changes what the tool reads, writes, or reports.

- -> XREF: D10 T02 §7 -- HEVC distribution decision.
- -> XREF: D02 T01 §4 -- Milestone 2 follow-up hot-loop speed section filed under §4.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Remove the retired run-guard folders          | --         |  [x]   |
|   2   |   §2    | Log in the GitHub CLI                         | --         |  [x]   |
|   3   |   §3    | Approve format version 3                      | --         |  [x]   |
|   4   |   §4    | The missed-milestone policy                   | --         |  [x]   |
|   5   |   §5    | Enable GitHub Pages                           | --         |  [x]   |
| 7 | §7 | HEVC distribution decision | -- | [ ] |
| 8 | §8 | The 13-round milestone cap | §4 | [x] |
| 9 | §9 | The 2026-11-11 scope and autonomy decisions | -- | [ ] |

---

## 1. Remove the Retired Run-Guard Folders

> **Started:** 2026-10-24T21:47:10Z

**Requires:** operator -- the agent session's safety rules refused this deletion on 2026-09-29, so only the operator can perform it.

The previous developer's campaign tooling left a lock folder and a run guard in the tree. The agent session was refused the deletion because the folders read as logs, so the operator deletes them.

- [x] Delete the folders: `rm -rf .grok guard build/review-20260928` in Git Bash (or `Remove-Item -Recurse -Force .grok, guard, build\review-20260928` in PowerShell). Done when: `ls -d .grok guard build/review-20260928` reports all three missing. **Corrected 2026-09-29:** the item gave only the PowerShell form, which fails under the session's `!` prefix (Bash); the operator ran the Bash form.
- [x] Remove the `.grok/` and `guard/` lines from `.gitignore`. Done when: `Select-String -Path .gitignore -Pattern '^(\.grok|guard)/$'` prints nothing, and this file's two `.gitignore` claims are updated to `= 0`.
- [x] Commit: `"workspace: drop the retired run-guard folders and their ignore lines"` (the operator deleted the three untracked/ignored folders from the filesystem on 2026-09-29, the tracked `.gitignore` removal landed in `2c89e2e313d5`, and this commit records the section's completion)

**Test checkpoint:** `git status --short --ignored .grok guard` prints nothing; `python scripts/todo-claims.py` exits 0 with the claims updated.

> **Verified:** 2026-10-24 | §1 | 865c03fdbd24ec5450e8da269603db0f6ea79462
> **Review:** `docs/reviews/d99-t01-s1.json`
## 2. Log In the GitHub CLI

> **Started:** 2026-10-24T21:57:15Z

**Requires:** operator -- the login runs in the owner's browser session; a clone without usable existing authentication needs an interactive account enrollment; reuse authorized stored credentials before requesting any new login.

`python3 scripts/ci_status.py` reads CI back through the GitHub CLI. Without it, every push's CI result is unverified, and the phase runner's CI read-back cannot complete.

- [x] Verify the existing CLI with `gh auth status`; only a missing clone setup installs the platform CLI (Windows: `winget install GitHub.cli`; WSL/Linux: the official GitHub CLI package instructions) and uses `gh auth login` when no authorized stored identity is available. Done when: `gh auth status` prints `Logged in to github.com`.
- [x] Commit: `"workspace: record the GitHub CLI login for the CI read-back"`

**Test checkpoint:** `gh run list --repo rizonesoft/RasterFold --limit 1` exits 0.

> **Verified:** 2026-10-25 | §2 | 490b13cba1022d3f448daecb3b2d28c462e5cd37
> **Review:** `docs/reviews/d99-t01-s2.json`
## 3. Approve Format Version 3

> **Started:** 2026-10-24T20:05:45Z

What method 1 writes is frozen per format version (`AGENTS.md`). The operator gave a standing approval for new format versions instead of approving each one, so a campaign does not stop at every version:

**Operator decision 2026-09-29:** "file it like that" -- the operator's reply to this proposal, quoted from the session: "A standing approval for format versions. For example: 'a new format version is approved when its combined measurement clears the milestone bar, the held-out set confirms it, and every older version still decodes.'"

**Reading of the decision (recorded 2026-09-29, the operator may correct it):** version 3 is the development version across milestones 4 to 8 (`D01 T03 §1`, `D01 T04 §3`, `D01 T05 §2`, `D01 T06 §2`, and strips in `D01 T03 §2`); its approval takes effect when milestone 9's held-out check confirms it, and the 1.0 release (`D09 T01 §4`) freezes it. Every later version is approved the same way at the milestone that measures it.

- [x] Record the operator's standing approval under this section. Done when: the decision line quotes the operator.
- [x] Commit: `"todo: record the operator's standing approval of format versions (D99 T01 §3)"` (the decision text first landed inside the port commit `97e5ca5`; this commit is the section's own record)

**Test checkpoint:** the decision line is present and `python3 scripts/todo-graph.py validate` exits 0.

> **Verified:** 2026-10-24 | §3 | dd4d46da9ca3a2dae33ff18480b56c09c5b80b13
> **Review:** `docs/reviews/d99-t01-s3.json`
## 4. The Missed-Milestone Policy

> **Started:** 2026-10-24T20:12:30Z

A milestone whose target is missed would otherwise stop a campaign for a decision each time. The operator set the policy:

**Operator decision 2026-09-29:** "file it like that" -- the operator's reply to this proposal, quoted from the session: "A policy for missed targets. For example: 'when a milestone misses, file up to three follow-up experiments from `docs/research.md`; escalate only if all three miss.'"

Every milestone gate in `todo/05-milestones/` carries this policy: on a miss, up to three follow-up experiments are filed through `add-todo` (with Origin lines), each measured against the gate's bar; the operator is asked only when all three miss.

- [x] Record the operator's missed-milestone policy under this section. Done when: the decision line quotes the operator.
- [x] Commit: `"todo: record the operator's missed-milestone policy (D99 T01 §4)"` (the decision text first landed inside the port commit `97e5ca5`; this commit is the section's own record)

**Test checkpoint:** the decision line is present and every gate in `todo/05-milestones/` names `D99 T01 §4` (`python3 research/milestones.py --check` exits 0).

> **Verified:** 2026-10-24 | §4 | bfe1813e76ac9f3937a770bddb0de0ebb4fedcb4
> **Review:** `docs/reviews/d99-t01-s4.json`
## 5. Enable GitHub Pages

> **Started:** 2026-10-24T22:02:00Z

**Requires:** operator -- the owner decides private-repository Pages eligibility and publication visibility; automation may reuse existing authorized account access for configuration once that decision is recorded.

- [x] In the repository settings of `rizonesoft/RasterFold`, set Pages to deploy from GitHub Actions. Done when: the settings page shows GitHub Actions as the source (configured on 2026-09-29; `gh api repos/rizonesoft/RasterFold/pages` reports `"build_type": "workflow"`, `"public": true`, `"html_url": "https://rizonesoft.github.io/RasterFold/"`, and `"https_enforced": true`).
- [x] Record the public project-site visibility and `/RasterFold/` base path in `docs/site-content.json`; check the `github-pages` environment allows `main` and the intended release triggers. Done when: **Corrected 2026-10-24:** the item originally stated "an exact-release source can deploy through an allowed `workflow_dispatch` from `main` even if tag deployment is restricted", which depends on `.github/workflows/pages.yml` in `D08 T01 §3`; in this section, `docs/site-content.json` records `schema = 1`, `repository = "rizonesoft/RasterFold"`, `visibility = "public"`, `base_path = "/RasterFold/"`, `site_url = "https://rizonesoft.github.io/RasterFold/"`, `build_type = "workflow"`, `https_enforced = true`, and `environment` (`name = "github-pages"`, `custom_branch_policies = true`, `allowed_branches = ["main"]`, and `release_trigger` with `event = "workflow_dispatch"`, `dispatch_branch = "main"`, `supports_exact_release_ref = true`), and a semantic verification check confirms these values match `gh api repos/rizonesoft/RasterFold/pages`, `gh api repos/rizonesoft/RasterFold/environments/github-pages`, and `gh api repos/rizonesoft/RasterFold/environments/github-pages/deployment-branch-policies` and rejects a mutated `base_path` or missing field. Live `workflow_dispatch` execution with a distinct immutable source ref is verified in `D08 T01 §3`.
- [x] Commit: `"workspace: record that GitHub Pages serves the docs"`

**Test checkpoint:** **Corrected 2026-10-24:** the section originally required the first `pages` workflow run to be green and the site's address to load here, before `D08 T01 §3` (which depends on `D99 T01 §5`) creates `.github/workflows/pages.yml`. This section verifies that `gh api repos/rizonesoft/RasterFold/pages`, `gh api repos/rizonesoft/RasterFold/environments/github-pages`, and `gh api repos/rizonesoft/RasterFold/environments/github-pages/deployment-branch-policies` match every required field in `docs/site-content.json` (and that mutating `base_path` or omitting `environment` fails the check), and `python3 scripts/drift-check.py` exits 0; `D08 T01 §3` verifies both the push `pages` workflow run and a `workflow_dispatch` run on `main` with a distinct immutable commit ref, plus the live site response.

> **Verified:** 2026-10-25 | §5 | d64b89ee273fc2f969bd767a38b908423abe0335
> **Review:** `docs/reviews/d99-t01-s5.json`
## 7. HEVC distribution decision

**Origin:** discovered run=61f3a8f4474f 2026-09-30 -- groom-plan codec distribution prerequisite.

**Corrected 2026-11-11 (operator decision):** the release owner decided "OS codec, read only": no bundled HEVC code, HEIC import only through the Windows system codec, no HEIC writing. `docs/codec-distribution.md` records the decision, its reason and its reversal cost. With the decision made, this section no longer carries `Requires: operator` (the operator asked on 2026-11-11 that the rest be left for the runner): what remains is agent work. The distribution channel is the repository's own GitHub Releases page, the default this project already uses for every artifact; the operator's widened autonomy directive of 2026-11-11 (`AGENTS.md`, Autonomy is the default) covers taking that default, and changing it later costs one edit to `docs/codec-distribution.md` because no HEVC code ships on any channel.

- [ ] Name the concrete provider in `docs/codec-distribution.md` from Microsoft's documentation: the Windows Imaging Component HEIF decoder, the system components a user needs for HEVC-coded images, and the Windows versions that carry them, each with its source URL and the date read. Done when: the page names components and versions rather than "the system codec", and states that the application ships no HEVC code on any channel.
- [ ] Add the distribution channel (GitHub Releases) to `docs/codec-distribution.md` beside the recorded decision. Done when: the page states the decision in the operator's words, the channel, and what a user without the system components sees; the `Still owed` paragraph is gone.
- [ ] Commit: `"release: record HEVC provider distribution decision"`

**Test checkpoint:** `docs/codec-distribution.md` names the Windows components and versions with their Microsoft sources and the channel, `python3 scripts/check-converter.py --case contract` passes with HEIF and HEIC import-only, and `python3 scripts/drift-check.py` exits 0; D10 T02 §7 must later prove its adapter matches. No future artifact is required to stamp this decision.

## 8. The 13-Round Milestone Cap

> **Started:** 2026-11-10T22:41:00Z

The missed-milestone policy (§4) let a gate file up to three follow-up experiments and then stopped the campaign for an operator decision. In practice that parked gates (Milestone 8 has sat parked since 2026-11-09 with its three follow-ups shipped and its re-measurement outstanding) and, before the policy gained tolerances on 2026-10-30, twice escalated into operator target changes. The operator now caps the loop instead, so an unattended campaign always terminates a gate one way or the other.

**Operator decision 2026-11-10:** "As for the milestones, let's set a cap for it. for example 'Milestone 8 gate: Beat WebP on generated images', let's give it a 13 round try (or your recommendation) and then accept the outcome, flip it and move on."

The implementing session's recommendation, recorded with the decision: 13 is kept, because the worst gate so far needed 8 experiment sections before its decision (Milestone 5, `D01 T04 §2` to `§9`) and Milestone 4 burned 5 of 6 planned experiments as negative results, so 13 is the worst observed case with over half again headroom, while still bounding one gate's cost in a 60-milestone plan. The cap refines §4; §4 remains the historical record and is not rewritten.

**The rule.** One round is one follow-up experiment section filed under the missed-milestone policy for that gate, built and measured against the gate's bar to a conclusion (shipped or negative result); gate re-measurements between rounds do not count separately, and a repeat of an Already-tried experiment unchanged is not a round. Each gate records its rounds used with refs. When round 13 still misses, the gate closes **accepted-missed**: the missed figures go into `docs/research.md` sections 1 and 9 beside the unmet target, the stamp's evidence states the target was missed and the cap reached (the completion review verifies the figures, the 13 rounds, and the no-regression envelope, not the target), the target text is never rewritten to fit the measurement, the accepted figures become the next milestone's no-regression baseline, and the campaign moves on without asking the operator. Fresh work toward the recorded gap may still be filed later by a planning section as ordinary work; it is not a gate round and does not reopen the gate. Later milestones are expected to keep improving the codec in general (every gate carries the no-regression envelope against the previous milestone's actual figures, accepted ones included, and milestones 11 and later author their work from the previous milestone's measured results), but no later milestone retroactively turns an accepted-missed gate into a reached one.

**Corrected 2026-11-10 (operator asked whether unreachable milestones should instead park open until a more powerful model arrives; implementing session's recommendation, recorded here):** parking a gate open indefinitely is rejected, for three reasons. First, it blocks in proportion to its dependents: an open gate holds its phase open, the plan can never honestly read 100%, and the campaign guard keeps resurfacing it, so "waiting" silently taxes every later phase. Second, the binding constraint on a compression bar is usually the format's algorithmic reach on the measured corpus (a missing model, a coding trick another codec has), not the implementing model's strength; a stronger model widens the experiment search and writes better code, but it does not change what the current format version can express, so most parked gates would wait forever for the wrong thing. Third, parking loses the one thing patience is supposed to buy, because an unrecorded intention to retry "someday" has no trigger that ever fires on its own. The accepted-missed outcome therefore carries a **dormant revisit** instead of a park: the closeout files exactly one backlog entry for the gap (its own `B-NNN`, carrying the gate as its `needs:` ref and the missed figures in its summary) whose `promote when` names all of: a stronger writer or reviewer model is pinned than the one that spent the 13 rounds (named in the entry); an incidental re-measurement at a later gate crosses this gate's bar; a new format version is opened; or a new technique lands in `docs/research.md` that plausibly reaches the bar. To make the second trigger real and not decorative, every later gate's record step re-measures each accepted-missed bar it can measure with the size run it already makes and records the outcome in one line; a bar found crossed is promoted from its backlog entry as fresh ordinary work that claims it properly (measured, reviewed, stamped as its own section), and the original gate's accepted-missed record is never rewritten, so the history stays true about when the bar actually fell. This keeps the operator's intent (do not spend unbounded effort now; do come back when the world changes) while keeping phases closable, the 100% goal meaningful, and the comeback automatic instead of remembered.

- [x] Record the operator's 13-round cap and the accepted-missed closeout under this section, with Milestone 8's count stated (3 rounds used: `D01 T06 §4`, `§5`, `§6`). Done when: the decision line quotes the operator and the rule above names the round, the cap, and the closeout.
- [x] Wire the cap into the generator that owns gate text: `POLICY` and the default checkpoint in `research/milestones.py`, plus Milestone 8's override note and missed-target item, then regenerate. Done when: `python3 research/milestones.py --check` exits 0 and every gate in `todo/05-milestones/` names the 13-round cap and `D99 T01 §8`.
- [x] Carry the cap in the unattended launcher's seed prompt and `docs/unattended-run.md` so a campaign session applies it without rediscovering this section. Done when: both name the 13-round cap and the accepted-missed closeout.
- [x] Evaluate dormant revisits at every phase boundary: evaluated 2026-11-10 at this section's closeout; no gate has closed accepted-missed yet, so no dormant entries exist (explicit empty result, recorded in docs/phase-runs/2026-11-10-phase-7-muse.md); the consumer is wired in process-phase closeout and groom-plan triage
- [x] Prove the lifecycle with acceptance cases (plan review P3): stamp refusal before 13 completed distinct rounds is quoted from a real attempt on the live Milestone 8 gate (at 4 rounds, `TODO refused: unticked checklist items in D05 T01 §8`, 2026-11-10); accepted-missed closure only at 13 with the no-regression envelope still required, and the accepted figures plus dormant backlog entry read back after a closeout, are recorded where the first accepted-missed gate closes (its own record item owns those cases). Done when: the refusal is quoted here.
- [x] Commit: `"todo: fold D99 T01 s8 plan gaps"`

**Test checkpoint:** `python3 research/milestones.py --check` exits 0, `grep -c "13 rounds" todo/05-milestones/TODO-01-beat-jxl-and-webp.md` counts the cap in the generated gates, and this section's decision line quotes the operator.

> **Verified:** 2026-11-11 | §8 | 687e1754a2a9e5446a2fdb42907c4198d315b714
> **Review:** `docs/reviews/d99-t01-s8.json`
## 9. The 2026-11-11 Scope and Autonomy Decisions

> **Started:** 2026-11-11T20:58:32Z

The operator made six decisions in one attended session on 2026-11-11, and a seventh to the campaign session that evening. They changed the product's scope and the way every session works, so they are recorded here in the operator's words, each with where it is implemented and what reversing it costs. The implementing session carried them out in the commits `92885be`, `5a93a14`, `43ee9c9` and their review-fix commits; this section is the decision record and the check that the tree still agrees with it.

**1. Android is removed.** "we will not be doing any android dev in this repo anymore, so please clean the repo. Time is better spend on the algorythms", after "remove Android work. We can always create a fork for that when we are done". RasterFold targets Windows x64 and Linux x64. Reversal cost: restore the NDK pin in `toolchain.json`, the two targets in `scripts/build.py`, `scripts/tidy.py` and `scripts/package.py`, and the Android rows of the plan from the commit before `92885be`.

**2. The converter is decoupled from 1.0 and runs straight after it.** Asked how the plan should treat the converter that blocked every milestone from 11 to 60, the operator chose "Decouple it", then "move it" for running it right after 1.0. RasterFold 1.0 is the codec, command line, library and documentation; the converter is Phase 10 and blocks no codec milestone. Reversal cost: restore the two dependency edges on `D09 T01 §2` and `§4` and move the rows back.

**3. The converter is Windows only.** "As for the linux display, we will never have one on this machine, so shoud lwe drop it like android and do that in a fork at a much later stage?", then the choice "Drop the Linux GUI". The Linux command line and library are unaffected. Reversal cost: re-author a Linux integration section in `todo/10-converter/TODO-03-interface.md`.

**4. The converter uses the Resolute launcher's stack.** "The demo app and converter (GUI) should have the toolstack and interface design as Resolute(launcher)." C++23 with CMake and Ninja on the pinned llvm-mingw, the `resolute-ui` Direct2D library and Lucide glyphs vendored from `rizonesoft/Resolute`, Resolute's `DESIGN.md`; the engine and codec adapters stay C11. Reversal cost: re-author `D10 T03` for another toolkit; nothing shipped depends on it yet.

**5. HEIC is read only through the system codec.** The choice "OS codec, read only"; `§7` and `docs/codec-distribution.md` own it.

**6. Autonomy is widened to attended sessions.** "You need to act autonomously, self-improve, self-heal, and make decisions to shape the and create the best repo, runners, toolset, dev methods, product, end-result, project, etc. you possibly can. You are more intelligent than me, so why stop and ask questions you know the answer to. We are not scared of the competition, no matter if its Adobe, Microsoft, Apple, or Google, we can and will be better. Nothing is impossible and nothing is stopping us from accomplishing the impossible(wire it in)." `AGENTS.md` (Autonomy is the default) carries it. Reversal cost: one paragraph in `AGENTS.md` and one sentence in the launcher prompt.

**7. The desktop-shortcut section is dropped.** "D00 T05 §5 --> let's drop or remove it. Flipping it has become a real pain" (said to the campaign session on 2026-11-11, after its sixth reconciliation returned a new preflight finding). The section and its plan row are removed unstamped; the shortcut on the operator's desktop and `docs/unattended-run.md` stay. Reversal cost: restore the section and row in `todo/00-workspace/TODO-05-lsp-subagents-unattended.md` from the commit before the removal and run one more reconciliation.

<!-- claim: count "android" scripts/build.py = 0 -->
<!-- claim: count "^## 5\. " todo/00-workspace/TODO-05-lsp-subagents-unattended.md = 0 -->
<!-- claim: count "android" toolchain.json = 0 -->
<!-- claim: count "^### Phase 10 -- Native converter$" todo/implementation-plan.md = 1 -->
<!-- claim: exists docs/codec-distribution.md -->
<!-- claim: count "Restated and widened on 2026-11-11" AGENTS.md = 1 -->

- [ ] Check decisions 1 to 5 against the tree and repair any gap at its owning layer. Done when: `python3 scripts/todo-claims.py` holds this section's claims, `git grep -il android -- scripts src tests toolchain.json .github` prints nothing, `python3 scripts/plan-audit.py` exits 0 with `D10 T04 §4` as the later gate, and `python3 scripts/check-converter.py --case contract` passes.
- [ ] Carry decision 6 in the unattended launcher's seed prompt (`scripts/unattended-run.sh`) and `docs/unattended-run.md`, so a campaign session applies it without rediscovering this section. Done when: both state that the session improves the repository, runners, toolset, method and product on its own judgment, and that an unproven impossibility is filed as work with its first experiment named.
- [ ] Record in `docs/vision.md` and `AGENTS.md` any of the seven decisions either file does not yet state, in the operator's words. Done when: each decision is findable in one of the two by its quoted phrase.
- [ ] Commit: `"todo: record the 2026-11-11 scope and autonomy decisions"`

**Corrected 2026-11-11:** decision 7 and its claim were added the evening this section started, so the checkpoint below reads six claims and seven decision lines where it said five and six, and the third item covers all seven decisions.

**Test checkpoint:** `python3 scripts/todo-claims.py` exits 0 with this section's six claims held, `python3 scripts/todo-graph.py validate` and `plan --check` exit 0, and the seven decision lines above quote the operator.

## Verification

- [ ] `python scripts/todo-graph.py validate` clean
- [ ] `python scripts/todo-claims.py` exits 0
