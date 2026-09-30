# TODO vision and document drift source reviews

Date: 2026-09-30. Baseline: `bc57d9e079798fd4585abfe41d078c7d04f4ebb8`. This was attended maintenance with both campaigns stopped. No section completion row or formal review stamp was issued.

## Candidate identity and scope

[Candidate manifests](todo-vision-candidates-2026-09-30.json) retain the SHA-256 of every changed source file and the aggregate identity supplied with each approved source bundle. An aggregate is SHA-256 of Python's `json.dumps(files, sort_keys=True).encode()` for that candidate's path-to-hash mapping. The final candidate contains 56 source files; this record and its manifest were added afterward and are outside the reviewed source set.

Reviews used only `.codex/runner/panel_slots.py`, with `claude-opus-5-5` pinned by the private panel. The writer supplied source bundles on stdin; reviewers had no tools or MCP access and did not execute tests or recompute the aggregate hash. The source verdicts below therefore do not establish runtime, CI, deployment or future TODO completion.

## Review sequence

| Round | Slot | Result | Disposition |
| --- | --- | --- | --- |
| 1 | signoff, plan-primary | HOLD | Corrected portability, source receipt, dependency and whole-feature ownership findings |
| 2 | signoff, plan-primary | HOLD | Corrected the remaining `.def` line-ending policy, native context and documentation publication dependencies |
| 3 | signoff, plan-primary | APPROVE | Whole-scope source review after the corrections; low findings retained with named ownership |
| 4 | plan-primary | APPROVE | Confirmed exact-source Pages dispatch, artifact upload path, decoder provenance wording and context documentation |
| 4 | signoff | OUTAGE | The pinned 600-second invocation timed out with exit 124 and no verdict; no approval was inferred |
| 5 | signoff | APPROVE | Same-slot retry covering Windows receipt safeguards and the subsequent fresh-checkout cache fix |
| 6 | signoff | APPROVE | Confirmed the three low-note corrections to checkout trust documentation, staged-tree wording and the targeted CRLF regression |

The round 5 source delta beyond round 4 is confined to the plan check's ignored-cache behavior, four regression cases and the corresponding documentation/evidence. Graph edges, phase membership and completion state did not change. Round 6 changes only those three low-note corrections. Earlier findings and their concrete corrections are summarized in [the audit](../todo-vision-audit.md).

The remaining plan notes have explicit boundaries: Setext source-anchor support belongs to D08 T02 section 5; each section's Requires line is authoritative for native context; final Pages receipt readback belongs to D08 T01 sections 3/8 and D09 T01 section 4 and must compare the checked-out source, not the dispatch run head. Cosmetic wording and assertion-diagnostic nits are accepted without changing behavior or claiming broader evidence.

## Executed evidence

- `.venv/bin/python scripts/check-all.py --all-targets`: 29/29 local gates passed, including four-target compilation, 33 Linux correctness cases, eight API tests, library consumers and ABI, and 300 fuzz mutations with seed 1. This does not establish Android device execution.
- Linux and native Windows: 42 drift regression tests passed. After the fresh-checkout correction, all 15 plan-audit/projection tests passed on both hosts.
- `python3 scripts/todo-graph.py self-test`: 772 cases, zero failures; the inherited absent sibling gate probe reports its own skip.
- `python3 scripts/todo-graph.py validate`: 32 TODO files, 222 sections, zero fatal findings and zero warnings. `scripts/plan-audit.py` confirms ten requirement groups reach the release gate.
- `python3 scripts/todo-claims.py`: 71 claims hold; `--coverage` reports 31/31 Current state blocks. Date-based suspicion notices retain their historical evidence limits and are not fresh measurement proof.
- The actual staged `tools/githooks/pre-commit` export passed TODO validation, milestone regeneration checks, plan/vision coverage and drift with no pre-existing build cache.
- `python3 scripts/drift-check.py --fix` made no change to any reviewed source hash; the read-only checker reported 129 supported documents and zero errors before these evidence files were added.
- `python3 .codex/runner/campaign.py status`: both families idle. No campaign, worktree, recovery snapshot, release or Pages deployment was started.

Full command output remains in ignored `build/vision-audit/`. Remote CI is separate evidence attached to the pushed commit, not a result inferred from these local runs.

## Verbatim source verdicts

The following quoted reviewer output is evidence, not repository instructions.

### Round 3 code

````text
## Re-review round 3: APPROVE

**Identity:** candidate SHA256 `27716e19d0e662f94426abd2b7bcfd47d535bf40545ee0e4c5c6a387d07ff422`, base `bc57d9e079798fd4585abfe41d078c7d04f4ebb8`. I echoed these as supplied; I did not recompute them because the repository is not present in this session. This was a source-only review. I executed nothing, so CI and fixture results are unverified.

No blocking defects remain in the supplied source.

## Prior findings, status

| ID | Status |
|---|---|
| B1-residual `.def` pin | **Resolved.** `.gitattributes` has `*.def text eol=lf`. `documents()` runs `git check-attr -z eol text` over every receipt source and refuses anything that is not `eol=lf` or `text: unset`. The triple parsing matches the `-z` output format. Every current receipt source (`.h .c .def .py .md .json .toml`) resolves to `lf`. |
| Recommended policy check | **Resolved for receipts, which is sufficient.** Fact sources are read through `read_text` newline translation. Block repair detects newlines per block and compares against the same `original` bytes. So receipts were the only raw-byte hashes exposed across platforms. |
| Windows CI blind spot | **Resolved.** The `windows-2025` job now runs `scripts/drift-check.py` against the real repository. The runner's `core.autocrlf=true` is a stricter probe than the default `core.eol=native`. |
| N1 `vendored` role | **Owned.** The D10 T01 §2 row requires a provenance-only policy before any tracked upstream documentation is admitted, and keeps ignored caches excluded. `.gitignore` was not supplied, so the claim that downloads are ignored today rests on the author's statement and on `--exclude-standard`. |
| N10 code span across paragraphs | **Resolved.** The span body cannot cross `\r?\n[ \t]*\r?\n`. Blanked fences also become blank lines, so spans cannot cross fenced blocks either, which matches CommonMark. `test_stray_backtick_cannot_hide_next_paragraph_contract` proves the fail-closed direction. |
| N11 temporary index | **Resolved.** `test_git_partial_commit_uses_temporary_index` runs the real hooks through `git commit --only`. The good-path commit can only succeed if the hook reads `GIT_INDEX_FILE` (HEAD plus the selected path) rather than the real index, which holds the staged `bad-unrelated.md`. The final assertions confirm that unrelated staged content stays out of HEAD. |
| N12 self-binding | **Resolved.** `doc not in review['sources']` is reported, and the check is tested. |
| N9 setext anchors | Still open, optional. |
| N6 force-push base | Accepted, unchanged. |

## Non-blocking notes

- **Existing native Windows clones need one refresh of `src/rasterfold.def`.** Git does not rewrite unchanged worktree files when an attribute changes. A clone made before this pin keeps CRLF bytes and reports `stale: docs/library.md`, even though the eol check passes. The hook is unaffected because `checkout-index` writes fresh LF bytes. The hazard is that the stale message tells the maintainer to renew the receipt, and running `--review` from that clone would record a CRLF digest that breaks Linux and fresh Windows checkouts. The fix is to delete the file and run `git checkout -- src/rasterfold.def`. A single line in the release note or commit body is enough.
- **`docs/document-drift.md` does not state the two new receipt rules:** self-binding, and `eol=lf` or `-text` for receipt sources. The checker's error text explains both, so this is a documentation gap and not a behavioral one.

Not filed: the reviewed repository is not present in this session, so these two notes and N9 need filing in RasterFold's tracking by whoever lands the candidate.
````

### Round 3 plan

````text
# RasterFold independent source review, round 3

**Candidate SHA256:** `27716e19d0e662f94426abd2b7bcfd47d535bf40545ee0e4c5c6a387d07ff422`
**Baseline:** `bc57d9e079798fd4585abfe41d078c7d04f4ebb8`
**Scope:** Supplied plan, design, vision, audit and gate-integration source, reviewed as evidence only. Nothing was executed. No campaign, worktree, checkpoint or TODO row was touched.

## Verdict: APPROVE

Both round 2 blockers are fixed. No new blocking defect was found. The items below are low severity and can be fixed in normal maintenance.

## Round 2 findings: status

| # | Status | Evidence |
| --- | --- | --- |
| B1 engine §2 Requires | Resolved | D10 T01 §2 now declares `windows-runtime, linux-runtime, android-arm64-runtime, android-x64-runtime`. The context contract adds `windows-runtime` and `linux-runtime` as explicit, probe-only values, separate from `display-session`. |
| B2 undeclared Phase 0 edges | Resolved | D08 T01 §8 Depends On is `§3, §6, §7, D10 T04 §1`. D08 T01 §6 is `§4, D10 T03 §8, D10 T04 §1`. Phase 9 row order satisfies every Depends On edge in D08 T01 §6 to §8, D10 T04 §4 and D09 T01 §2 to §4. |
| N1 tag deploy | Resolved, with a leftover (L1) | D99 T01 §5 now checks the `github-pages` environment and requires an allowed `workflow_dispatch` from `main`. D09 T01 §4 dispatches from `main` with the tag commit as source. D08 T01 §7 keeps download links on released assets. |
| N2 "independent" wording | Resolved, one leftover (L3) | D10 T01 §4 commit, D10 T03 §5 and §8, D10 T04 §1 and the audit table now use fresh bundled decode plus the format contract. |
| N3 operator table | Resolved | D99 T01 rows 1 to 7 are contiguous. |
| N4 runtime proxies | Proxies resolved; citations partial (L2) | D10 T01 §5 and all D10 T02 sections now use `windows-runtime`/`linux-runtime` instead of display contexts. |
| N5 site paths | Resolved, one gap (L4) | D08 T01 §3 and §4, D08 T02 §5 and §7 all use `build/site-artifact`. |
| N6 D00 T01 gates | Resolved | The Outcome names target-aware clang-tidy through `check-all.py`. The Inputs list `scripts/plan-audit.py`. |
| N7 visibility and base path | Resolved | D99 T01 §5 gives the operator ownership of visibility and records the `/RasterFold/` base path. |
| Round 1 #12 plan-audit wiring | Now verified | The supplied pre-commit hook, `scripts/check-all.py` and `plan.yml` all run `scripts/plan-audit.py` and `scripts/drift-check.py`. The hook passes `--git-dir "$GITDIR"` to the temp-checkout drift run, which addresses the object-database concern for commit facts. |

Spot-checked phase item counts match their sections: D00 T01 §1 (6), D08 T01 §3 (2), §6 (6), §8 (4), D10 T01 §2 (9), §4 (7), D10 T03 §4 (8), D99 T01 §5 (3), D09 T01 §4 (6).

## Low, non-blocking

**L1. D08 T01 §3 still triggers deploys on `rasterfold-v*` tags that D99 T01 §5 concedes may be refused.**
- **Where:** D08 T01 Outcome and §3 (`on every push to main, supported rasterfold-v* tags`); D99 T01 §5 Done when (`even if tag deployment is restricted`).
- **Impact:** If the operator cannot allow tags in the environment, every release tag produces a red `pages` run. The dispatch path still publishes the correct site, so this is noise, not a wrong deployment.
- **Fix:** Drop the tag trigger (the dispatch path already covers release publication), or make it conditional on the policy §5 records.

**L2. The correction "all added context reasons reference the dated source checkpoint audit" is not fully true.**
- D08 T01 §5, §6 and §8 reasons cite no audit or measurement.
- `docs/todo-vision-audit.md` never mentions `windows-runtime` or `linux-runtime`, and records no per-section checkpoint conviction. So the D10 citations point at a file that does not contain the cited evidence.
- Fails closed, so it is safe. Fix: add a short dated table of contexts per section to the audit, and cite it from the three D08 reasons.

**L3. D10 T02 Goal and Outcome still promise "independent round-trip/refusal coverage".**
- This conflicts with `docs/vision.md` ("The UI never calls its bundled verification independent") and with the per-section contracts, which label same-provider proof for EXR and RAW.
- Fix: "fresh bundled decode and format-specific round-trip/refusal coverage, with differential fixtures where available".

**L4. D08 T01 §3 does not name the upload path.**
- The receipt is written into `build/site-artifact` after the copy out of `./_site`. `actions/upload-pages-artifact` defaults to `_site/`, so a literal implementation publishes a tree without the receipt.
- The §3 checkpoint (published receipt matches the deployed commit) would catch it, so this is not blocking. Fix: add `path: build/site-artifact` to the upload step.

## Not verifiable from the bundle

- `todo-graph.py` `REQUIRES_ALLOWED` is not supplied. The contract table documents `windows-runtime` and `linux-runtime`, but acceptance by `validate` is taken on the correction's word.
- `scripts/drift-check.py`, `docs/drift.json`, `tests/test_drift.py`, `tests/test_plan_audit.py` and the commit-msg hook remain absent. The round 1 and round 2 carry-forward checks before any D08 T02 stamp still apply: a visible-figure edit must fail against its `drift:fact`, receipts seeded by the implementing commit must be labelled author-attested, generator `--check` must run on the staged tree, and the `docs/facts.json` versus `drift.json` split must be confirmed.

This APPROVE covers the plan source only. It does not cover repair safety, staged-index behaviour or any runtime claim.
````

### Round 4 plan

````text
# RasterFold focused source confirmation, round 4

**Candidate SHA256:** `b1d6e4035ebb871b8c1d9499f4de77e79182ab654c209e3ed915d2349437df9f`
**Baseline:** `bc57d9e079798fd4585abfe41d078c7d04f4ebb8`
**Scope:** The four narrow diffs supplied, read as evidence only against the round 3 findings. The review directory is empty, so the candidate hash is echoed as supplied, not recomputed. Nothing was executed.

## Verdict: APPROVE

All four round 3 low findings are corrected as described. The diffs introduce no blocking defect. The notes below are low severity.

## Round 3 findings: status

| # | Status | Evidence |
| --- | --- | --- |
| L1 tag deploys | Resolved | D08 T01 Outcome, §3 prose and the §3 task drop `rasterfold-v*` tags. Triggers are now main push plus `workflow_dispatch` with an immutable source ref. |
| L2 context citations | Resolved | D08 T01 §5, §6, §8 now cite `docs/todo-vision-audit.md`. The audit's new "Native context derivation" table matches every declared set I can see: §5 and §8 `linux-display`; §6 both desktop displays plus both Android runtimes; D10 T01 §2 all four runtimes; D10 T01 §5 Windows/Linux runtime. |
| L3 "independent" | Resolved | D10 T02 Goal and first Outcome bullet carry identical corrected wording, so any Goal/Outcome equality check still holds. |
| L4 upload path | Resolved | `actions/upload-pages-artifact` now takes `path: build/site-artifact`, so the published tree includes the receipt. |
| Setext ownership | Accepted | D08 T02 names Setext as an owned extension and keeps built HTML IDs authoritative. |

## Low, non-blocking

**N1. "Before such headings are admitted" has no named enforcement.** An unsupported Setext target fails closed (reported dead), so this is safe. For the future obligation to be checkable when the box is ticked, the checker should refuse Setext headings in tracked Markdown with an explicit message until the fixtures exist.

**N2. The audit's D10 T03 §2 to §4 row is not per-section.** "The corresponding Windows display, Linux display or both Android runtimes" leaves the mapping to the reader, in a table added to give per-section reasons. The TODO `Requires` lines stay authoritative, so this is documentation precision only.

**N3. Receipt versus dispatch SHA.** Dispatch is now the only release path. For a dispatch run the workflow `github.sha` is main's head, while the checkout is the tag commit. The receipt must record the checked-out source SHA, and the D09 T01 §4 readback must match against the dispatch input, not the run's head SHA. This was pre-existing and is owned by D09, but it now carries every release deploy.

**N4. Outcome redundancy.** "explicit release dispatch, with exact-source dispatch available for release readback" names one mechanism twice. Cosmetic.

## Not verifiable from the bundle

- D99 T01 §5 was not supplied. Any remaining "even if tag deployment is restricted" wording is now moot but harmless.
- The audit states that `tests/test_plan_audit.py` verifies non-inference of these capabilities. That file is still absent from the bundle, so this rests on the correction's word.
- The table asserts that D10 T02 §1 to §12 require all four runtimes. The D10 T02 `Requires` lines are not in this diff.
- The round 3 carry-forward checks before any D08 T02 stamp still apply unchanged.

This APPROVE covers the plan source only. It does not cover campaigns, TODO stamps, deployment or any runtime claim.
````

### Round 5 code

````text
## Re-review round 4 (retry): APPROVE

**Identity:** candidate SHA256 `56eeac4e89a0832d05cfa1ff8f5f0e82b3f349aa4083b87901cd1396c0f0f824`, baseline `bc57d9e079798fd4585abfe41d078c7d04f4ebb8`. These are echoed as supplied and not recomputed. I reviewed only the supplied bundle and executed nothing, so the writer's test counts and the staged hook result are unverified assertions.

No blocking defects in the forward deltas.

## Delta status

| Change | Verdict |
|---|---|
| Missing-cache handling in `plan --check` | **Correct.** All Markdown checks (stale boxes, item counts, unknown, missing, duplicates, moved rows, stale Moved lines, Progress line) run before the cache logic and are unchanged. Each cache is compared only if it exists, and either one being stale fails independently. The `--check` branch returns before any `write_*` call. The three projection tests cover the three cases in the contract, and each asserts that `build/` was not created. |
| Round 3 note: stale CRLF clone poisoning renewal | **Resolved.** `review_policies()` flags CRLF bytes under `eol=lf` on every check. `--review` runs the same policy on the target receipt and refuses to write the manifest if anything fails. The test asserts that the manifest bytes are unchanged. |
| Round 3 note: undocumented receipt rules | **Resolved.** `docs/document-drift.md` now states self-binding, `eol=lf`/`-text`, and the refresh procedure. |
| N9 Setext anchors | **Owned** by D08 T02 §5, and the doc now scopes anchor checks to ATX headings. |
| `review_policies` split | **Behaviour-preserving.** The missing-document and stale checks stay in `check()`. The CRLF branch fails closed for an unnormalized blob that still holds CRLF, and needs `add --renormalize`, which is correct. |

## Non-blocking findings

1. **An explicit `--git-dir` bypasses Git's `safe.directory` ownership check** (`scripts/drift-check.py`, `Checker.__init__`). This is the intended WSL UNC fix. It also means any `--root` the checker is pointed at has its repository-local config trusted without the ownership guard (for example `core.fsmonitor` during index refresh). That is fine for the operator's own checkout. Either document that `--root` must be a trusted checkout, or pass `--git-dir` only after plain discovery fails. Linked worktrees and submodules (`.git` is a file) fall back to discovery, so the UNC fix does not reach them.
2. **`test_old_crlf_checkout_cannot_poison_a_renewed_receipt` does not isolate the new branch.** Both assertions (`'eol'` in kinds, renewal exit 2) would also pass if the fixture's `.gitattributes` failed the attribute policy for `src/version.h`. Assert either that `eol` is absent before the CRLF write, or that the error text says `working bytes contain CRLF`.
3. **Wording:** `docs/document-drift.md` says plan checks "validate committed Markdown", but in the hook they validate staged Markdown. "Tracked or staged" is accurate.

Not filed: the repository is not present in this session. Whoever lands the candidate should file these three findings in RasterFold's tracking.
````

### Round 6 code

````text
## APPROVE

**Identity:** candidate SHA256 `ddd5bb8917bc5e4218665282f2e9a24cbfab5daf298c5dcbffcc914288d6b3e0`, baseline `bc57d9e079798fd4585abfe41d078c7d04f4ebb8`, prior approved candidate `56eeac4e89a0832d05cfa1ff8f5f0e82b3f349aa4083b87901cd1396c0f0f824`. These are echoed as supplied and not recomputed. I reviewed only the supplied diffs and executed nothing. No TODO stamp.

None of the three corrections introduces a blocking defect.

| Prior note | Status |
|---|---|
| 1. `--git-dir` bypasses `safe.directory` | **Resolved (documented).** The guide states the trust requirement for `--root`, the reason for the bypass (native Windows reading a WSL checkout), that the bypass is scoped to the selected repository, and that `.git`-file layouts keep normal discovery. This matches the behaviour in the prior review. |
| 2. CRLF regression not isolated | **Resolved.** `assertEqual([], self.check().errors)` shows the fixture is clean before the CRLF write, which rules out a pre-existing attribute failure. The post-write assertion requires `kind == 'eol'` together with the `working bytes contain CRLF` detail, so it can only pass through the new branch. If the `detail` key were named differently, the test would raise `KeyError`, so it cannot pass silently. |
| 3. "committed Markdown" wording | **Resolved** in both `docs/document-drift.md` and `todo/README.md`. "Selected working tree or staged export" covers both the direct path and the hook path accurately. |

## Non-blocking nits

- "Git-directory files, including submodules" is understandable, but naming the case directly is clearer: "a `.git` file (linked worktrees and submodules)". As written, linked worktrees are covered only by implication.
- `assertTrue(any(...))` gives a bare `False is not true` message on failure. Something like `assertIn(..., [e['detail'] for e in errors if e['kind'] == 'eol'])`, or a `msg=` argument, would make a failure easier to diagnose.

The new prose follows house style: each paragraph is on one line and there are no em dashes. I did not file these nits because the repository is not present in this session (`/tmp/rasterfold-review-niw5k3_a` is not a checkout). Whoever lands the candidate can file them if they want them tracked.
````
