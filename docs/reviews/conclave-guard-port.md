## Round 1: astra (`gpt-6-astra` via `codex`)

- Writer: claude
- Run: 2026-10-21T04:55:37Z
- Effort: medium
- Lines: 25
- Digest: a84ba78d69589e74

## adversarial: needs-attention

1. **The hook budget does not bound the hook.** [scripts/campaign_guard.py:740](/home/derickpayne/projects/RasterFold/scripts/campaign_guard.py:740) reads the run file before starting the deadline; untracked-file reads at line 698 also have no timeout, and no deadline check follows fingerprinting or lock acquisition. Reproduce with an owning guard whose `run_file` names a FIFO without a writer: the hook waits until the external timeout, without recording its promised budget error. An in-memory clock probe also returned `block` at 41 seconds against the 40-second budget.

## consistency: needs-attention

2. **A later failing hook overwrites an unacknowledged error.** [scripts/campaign_guard.py:849](/home/derickpayne/projects/RasterFold/scripts/campaign_guard.py:849) replaces `hook_error` and its timestamp unconditionally. Reproduce by recording error A, leaving it unacknowledged, then triggering error B before the heartbeat reads it. Only B remains. The preservation added to the successful decision path does not satisfy the documented “kept there through later hook runs until the heartbeat acknowledges it” contract. An in-memory probe confirmed this replacement.

## integration: needs-attention

3. **`end --reason plan-done` bypasses phase scope.** [scripts/campaign_guard.py:346](/home/derickpayne/projects/RasterFold/scripts/campaign_guard.py:346) checks only the global ready count. Acquire with `--scope phase`, leave its run file without a terminal marker, arrange `0 runnable now`, then call `end --reason plan-done`. It deletes the guard and state despite the phase-only contract. An in-memory probe of `_end_locked` confirmed both deletions.

4. **Heartbeat replacement promotes a standalone phase run to a plan run.** [.claude/skills/process-plan/SKILL.md:69](/home/derickpayne/projects/RasterFold/.claude/skills/process-plan/SKILL.md:69) instructs replacement through steps 2 and 3, but step 3 now explicitly supplies `--scope plan`. Reproduce by starting a standalone phase, then following the missing-job or expiry replacement procedure. Explicit scope overrides the existing `phase` value, so subsequent hooks and heartbeats can continue into other phases. Replacement must retain the current scope.

## record: advisory

The new tests leave material gaps:

- [scripts/campaign_guard.py:1827](/home/derickpayne/projects/RasterFold/scripts/campaign_guard.py:1827) switches the fixture to `scope="plan"` before the closeout test at line 1844. That assertion does not exercise phase-scoped closeout.
- The error-preservation test at line 1838 exercises a successful subsequent hook, not a second failure.
- The budget test at line 1845 sets the budget to zero. It exercises immediate rejection, not expiration during file reads, fingerprinting or lock waiting.

I inspected commit `a9a5af7` and its predecessor hook. No files were modified. The probes used mocked filesystem operations; I did not rerun the self-test because it creates and modifies fixture files.

OVERALL: needs-attention

## Response to round 1

All four findings verified against `a9a5af7` and fixed in the next commit; each fix has a self-test case that a mutation of the fix turns red.

1. Budget: the deadline now starts before the run file is read, a run file that is not a regular file fails open at once (`a-fifo-run-file-fails-open-at-once`), the budget is checked again before the lock, and a slow plan query is bounded (`the-budget-bounds-a-slow-plan-query`). Untracked FIFOs: git never lists them, so the fingerprint cannot block on one; its regular-file check covers a listed path that turns into a FIFO before the read.
2. A second failure no longer replaces an unacknowledged first: it goes to a file of its own and is reported after the first is acknowledged (`a-second-failure-keeps-the-first-error`, `the-second-failure-is-reported-after-the-first`). The PowerShell hook had the same overwrite.
3. `end --reason plan-done` is refused for a phase-scoped run (`plan-done-is-refused-for-a-phase-run`).
4. Job replacement and every re-point leave `--scope` out, so a standalone phase run stays phase-scoped (`.claude/skills/process-plan/SKILL.md`, step 3 and the section-boundary check).

Record advisories: a phase-scoped PARKED marker now ends a phase run in its own case (`a-parked-marker-carrying-the-run-id-ends-a-phase-run`), and the second-failure and mid-decision budget cases above cover the other two. Self-test: 137 cases, 0 failed.
