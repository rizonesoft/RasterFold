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

> **Goal:** Every step only the operator can take is written down with the exact command or decision, so no agent run stalls wondering whether it may act: the retired run-guard folders are gone, the GitHub CLI is logged in for the CI read-back, GitHub Pages is enabled for the docs, and the two standing decisions that let a campaign run through the roadmap (format-version approval and the missed-milestone policy) are recorded in the operator's words.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** The folders `.grok/`, `guard/`, and `build/review-20260928/` from the previous developer's tooling are still on disk; the agent session's safety rules refused to delete them (they read as logs), so the operator removes them. `.gitignore` still lists `.grok/` and `guard/`. The GitHub CLI (`gh`) is not installed on the development machine. No format version 3 exists; on 2026-09-29 the operator gave the standing approval and the missed-milestone policy recorded in §3 and §4. GitHub Pages is not enabled for the repository.
<!-- claim: count "^\.grok/$" .gitignore = 1 -->
<!-- claim: count "^guard/$" .gitignore = 1 -->
<!-- claim: count "^#define RF_VERSION 2u$" src/rf.h = 1 -->

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

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Remove the retired run-guard folders          | --         |  [ ]   |
|   2   |   §2    | Log in the GitHub CLI                         | --         |  [ ]   |
|   3   |   §3    | Approve format version 3                      | --         |  [ ]   |
|   4   |   §4    | The missed-milestone policy                   | --         |  [ ]   |
|   5   |   §5    | Enable GitHub Pages                           | --         |  [ ]   |

---

## 1. Remove the Retired Run-Guard Folders

**Requires:** operator -- the agent session's safety rules refused this deletion on 2026-09-29, so only the operator can perform it.

The previous developer's campaign tooling left a lock folder and a run guard in the tree. The agent session was refused the deletion because the folders read as logs, so the operator deletes them.

- [ ] Delete the folders: `Remove-Item -Recurse -Force .grok, guard, build\review-20260928`. Done when: `Test-Path .grok, guard, build\review-20260928` prints `False` three times.
- [ ] Remove the `.grok/` and `guard/` lines from `.gitignore`. Done when: `Select-String -Path .gitignore -Pattern '^(\.grok|guard)/$'` prints nothing, and this file's two `.gitignore` claims are updated to `= 0`.
- [ ] Commit: `"workspace: drop the retired run-guard folders and their ignore lines"`

**Test checkpoint:** `git status --short --ignored .grok guard` prints nothing; `python scripts/todo-claims.py` exits 0 with the claims updated.

## 2. Log In the GitHub CLI

**Requires:** operator -- the login runs in the owner's browser session; agents hold no GitHub credentials.

`python scripts/review_prompt.py ci-wait` reads CI back through the GitHub CLI. Without it, every push's CI result is unverified, and the review skill's push block cannot complete.

- [ ] Install the CLI (`winget install GitHub.cli`) and log in (`gh auth login`, GitHub.com, HTTPS). Done when: `gh auth status` prints `Logged in to github.com`.
- [ ] Commit: `"workspace: record the GitHub CLI login for the CI read-back"`

**Test checkpoint:** `gh run list --repo rizonesoft/RasterFold --limit 1` exits 0.

## 3. Approve Format Version 3

What method 1 writes is frozen per format version (`AGENTS.md`). The operator gave a standing approval for new format versions instead of approving each one, so a campaign does not stop at every version:

**Operator decision 2026-09-29:** "file it like that" -- the operator's reply to this proposal, quoted from the session: "A standing approval for format versions. For example: 'a new format version is approved when its combined measurement clears the milestone bar, the held-out set confirms it, and every older version still decodes.'"

**Reading of the decision (recorded 2026-09-29, the operator may correct it):** version 3 is the development version across milestones 4 to 8 (`D01 T03 §1`, `D01 T04 §3`, `D01 T05 §2`, `D01 T06 §2`, and strips in `D01 T03 §2`); its approval takes effect when milestone 9's held-out check confirms it, and the 1.0 release (`D09 T01 §4`) freezes it. Every later version is approved the same way at the milestone that measures it.

- [x] Record the operator's standing approval under this section. Done when: the decision line quotes the operator.
- [ ] Commit: `"todo: record the operator's standing approval of format versions"`

**Test checkpoint:** the decision line is present and `python scripts/todo-graph.py validate` exits 0.

## 4. The Missed-Milestone Policy

A milestone whose target is missed would otherwise stop a campaign for a decision each time. The operator set the policy:

**Operator decision 2026-09-29:** "file it like that" -- the operator's reply to this proposal, quoted from the session: "A policy for missed targets. For example: 'when a milestone misses, file up to three follow-up experiments from `docs/research.md`; escalate only if all three miss.'"

Every milestone gate in `todo/05-milestones/` carries this policy: on a miss, up to three follow-up experiments are filed through `add-todo` (with Origin lines), each measured against the gate's bar; the operator is asked only when all three miss.

- [x] Record the operator's missed-milestone policy under this section. Done when: the decision line quotes the operator.
- [ ] Commit: `"todo: record the operator's missed-milestone policy"`

**Test checkpoint:** the decision line is present and every gate in `todo/05-milestones/` names `D99 T01 §4` (`python research/milestones.py --check` exits 0).

## 5. Enable GitHub Pages

**Requires:** operator -- repository settings need the owner's logged-in session; agents hold no GitHub credentials.

- [ ] In the repository settings of `rizonesoft/RasterFold`, set Pages to deploy from GitHub Actions. Done when: the settings page shows GitHub Actions as the source.
- [ ] Commit: `"workspace: record that GitHub Pages serves the docs"`

**Test checkpoint:** the first `pages` workflow run after the change is green and the site's address loads.

## Verification

- [ ] `python scripts/todo-graph.py validate` clean
- [ ] `python scripts/todo-claims.py` exits 0
