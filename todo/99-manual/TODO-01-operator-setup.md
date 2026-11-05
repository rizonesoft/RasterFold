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

> **Goal:** Every step only the operator can take is written down with the exact command or decision, so no agent run stalls wondering whether it may act: the retired run-guard folders are gone, the GitHub CLI is logged in for the CI read-back, GitHub Pages is enabled for the docs, and the two standing decisions that let a campaign run through the roadmap (format-version approval and the missed-milestone policy) are recorded in the operator's words. Production Android signing identity and the concrete HEVC distribution decision are release-only prerequisites.

> [!IMPORTANT]
> **Current state (verified 2026-11-05):** The operator deleted the folders `.grok/`, `guard/`, and `build/review-20260928/` from the previous developer's tooling on 2026-09-29 (the agent session's safety rules had refused the deletion), and `.gitignore` no longer lists `.grok/` or `guard/`. The WSL checkout now has an authenticated GitHub CLI, verified by successful CI readback and pushes on 2026-09-30; a new clone must verify its own identity. Format version 3 exists since `D01 T03 §1` (2026-10-27); on 2026-09-29 the operator gave the standing approval and the missed-milestone policy recorded in §3 and §4, and on 2026-10-29 under §4 (after the five remaining `docs/research.md` section 4 model experiments `D01 T02 §2` to `§6` missed their bars) the operator authorized updating Milestone 4 (`D05 T01 §4`) in `research/milestones.py` to match the shipped Format Version 3 results (`photo` `0.9813 <= 0.982` of JPEG XL e9 and `0.9695 <= 0.970` of e7, `tp` `0.9853 <= 0.986` of e9, `31 of 39` photos below e9 and `39 of 39 <= 1.0000` of e7, three-thread decode `<= 0.6` of JPEG XL's, and the 7-model `alloc_peak <= 10.0 MB` and `<= 1.27x` 5-model Milestone 3 timing envelope). On 2026-10-29 the completion review of `D05 T01 §4` found two of those figures unmet (`kodim17` is 12 bytes over JPEG XL e7, `384,835` vs `384,823`, so 38 of 39 photos are at or below e7, and the 4,095-pixel `syn_odd` encodes at `1.333x` at `-t1` from a fixed counter-table clearing cost), and the operator selected this option, quoted from the session: "Change the target to the measured results and stamp now: 38 of 39 photos at or below e7 (kodim17 +12 bytes, 1.00003); timing at most 1.27x on every set total and every photo, with images under 16K pixels allowed a fixed +1 ms instead of the 1.27x ratio. I'd also file a speed section for the small-image cost (only clear the tables the image uses, or clear them in place)." On 2026-10-30 under §4, after the match model and all three follow-ups for Milestone 5 (`D05 T01 §5`, `D01 T04 §2` to `§9`) missed, the operator chose, quoted from the session: "Relax the timing or memory rule for this milestone; v2 is the nearest candidate. And revise all other milestones to avoid more blockers like this." Milestone 5 now carries an envelope for `D01 T04 §8`'s v2 (time at most 1.15x Milestone 4 on every set total and 1.6x per image of at least 16,384 pixels, covering `syn_ui`'s `-t1` encode ratio where v2's better gradient prediction keeps its CM stream under `syn_ui`'s 1,378-byte Method 3 cap at `run_pipelined`'s `y = 64` 32-row check and aborts at `y = 96` (`97` vs `65` rows, `1.535..1.566x` on an idle machine vs `1.434x` under load in `D01 T04 §8`), with codec working memory unchanged and the input file's growth not counted) and v2 is ported by `D01 T04 §10`; from Milestone 6 the generic no-regression rule in `research/milestones.py` became a cost envelope (set totals within 0.1% and each photograph within 0.5% in size, time within 1.10x, 1.20x on `synth`, from pooled alternating runs, codec working memory within 5%), the all-or-nothing targets of Milestones 7 to 10 gained tolerances, the equal-cost wording of Milestones 14, 16, 22 and 25 became the envelope, and each planning section from Milestone 11 states its bars with tolerances. GitHub Pages is enabled (`build_type: workflow`, public project-site visibility at `/RasterFold/`), recorded in `docs/site-content.json`.
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

- -> XREF: D10 T04 §3 -- production Android signing identity.
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
| 6 | §6 | Android release signing identity | -- | [ ] |
| 7 | §7 | HEVC distribution decision | -- | [ ] |

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
## 6. Android release signing identity

**Origin:** discovered run=61f3a8f4474f 2026-09-30 -- groom-plan release prerequisites.

**Requires:** operator -- production application identity and durable signing-key custody must be established before public Android packages.

- [ ] Record the chosen application ID, release owner and signing-certificate fingerprint in `docs/release-signing.md`. Done when: the operator's decision is recorded; no private key or secret value enters Git or logs.
- [ ] Locate the existing authorized signing material in the Conclave vault, or have the owner establish it there, and configure scoped CI secret access. Done when: a test-signed artifact verifies against the recorded public fingerprint and recovery ownership is documented without revealing secret values.
- [ ] Commit: `"release: record Android signing identity and custody"`

**Test checkpoint:** Quote the public certificate fingerprint and successful verification of the test APK; secret values remain concealed.

## 7. HEVC distribution decision

**Origin:** discovered run=61f3a8f4474f 2026-09-30 -- groom-plan codec distribution prerequisite.

**Requires:** operator -- the application's intended distribution and any unresolved HEVC licensing obligations need a recorded release-owner decision, not an invented agent approval.

- [ ] Prepare the proposed HEVC provider, source license, distribution channels and unresolved obligations in `docs/codec-distribution.md` using current primary sources. Done when: the decision concerns a concrete provider/build rather than a generic codec warning.
- [ ] Record the release owner's decision in `docs/codec-distribution.md`. Done when: the required enabled HEIC capability has an approved distribution path; an unresolved decision remains visibly blocking and does not silently remove the promised format.
- [ ] Commit: `"release: record HEVC provider distribution decision"`

**Test checkpoint:** The actual decision names the proposed provider/version and intended channels in `docs/codec-distribution.md`; D10 T02 §7 must later prove its pins match that decision. No future artifact is required to stamp this decision.

## Verification

- [ ] `python scripts/todo-graph.py validate` clean
- [ ] `python scripts/todo-claims.py` exits 0
