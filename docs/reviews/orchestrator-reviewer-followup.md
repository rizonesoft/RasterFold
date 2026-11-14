## Round 1: carbon (`gemini-pro-orchestrator` via `agy`)

- Writer: codex
- Run: 2026-11-14T20:49:33Z
- Lenses: adversarial, integration, source-defect, plan-delivery, consistency, record, quality, optimization
- Lines: 40
- Digest: 599cd5cea99116aa

### adversarial: needs-attention

1. P2 Unrouted writer in a recorded outage round raises `KeyError` instead of `Refused` during receipt validation. At `scripts/todo-graph.py:7088`, `_check_route` returns early when `not recorded.get("reviewer")`, skipping the `require(writer in pin["routes"], ...)` check at `scripts/todo-graph.py:7091`. However, `schedule_ok` calls `_check` for outage rounds (`entry.get("reviewer") is None`) at `scripts/todo-graph.py:7147`, and `_check` now unconditionally indexes `pin["routes"][recorded["writer"]]` at `scripts/todo-graph.py:7105`. If a `## Round N: none` entry carries a retired or unrouted writer key (accepted by `ROUND_LINE` at `scripts/review-dispatch.py:717`), `schedule_ok` raises an uncaught `KeyError` that escapes `validate_section_receipt` (`scripts/todo-graph.py:7472`) and `cmd_stamp` (`scripts/todo-graph.py:7728`) with a traceback instead of a structured `Refused` error. Guard `recorded.get("writer") in pin["routes"]` with `require` before indexing `pin["routes"]` in `_check`.

2. P2 Effort-suffixed Antigravity models bypass schedule agreement when schedule steps omit `effort`. `scripts/review-dispatch.py:86` and `check_agy_effort` at `scripts/review-dispatch.py:227` state that an Antigravity reviewer runs at the effort in its model name and must agree with the schedule so a pass is never recorded at an effort it did not run at, while `scripts/review-dispatch.py:148` forbids setting `effort` on an `agy` reviewer entry. Because `check_step` (`scripts/review-dispatch.py:214`) makes `effort` optional on schedule steps and `check_agy_effort` (`scripts/review-dispatch.py:230`) checks only `{step["effort"] ... if "effort" in step}`, a pin pairing an effort-suffixed `agy` model such as `gemini-4-carbon-medium` with steps that omit `effort` (or set `effort` on only some steps) loads without refusal and records `effort: None` (`scripts/review-dispatch.py:617` and `scripts/review-dispatch.py:922`) despite running at the suffixed effort. Refuse effort-suffixed `agy` models when any scheduled step omits `effort` (and refuse partial step overrides against `agy` models).

### integration: needs-attention

3. P2 Standalone rounds under a schedule-free pin ignore `followup_effort` on subsequent rounds. `load_pin` permits pins without `[schedule]` (`scripts/review-dispatch.py:130` and `scripts/review-dispatch.py:166`), and `review_effort` (`scripts/review-dispatch.py:613`) selects `followup_effort` for `kind == "round"` only when `(plan or {}).get("position", 1) > 1`. In `main` (`scripts/review-dispatch.py:1193`), prior rounds from `args.record` are read only inside `if "schedule" in pin:`, leaving `plan = None` when `[schedule]` is absent so recorded Round 2 and later stay at `reviewer["effort"]` instead of `followup_effort`. Read prior returned rounds whenever `args.record` and `args.kind == "round"` are given so `followup_effort` holds with or without `[schedule]`.

4. P2 End-to-end stamp and receipt validation is untested for the Codex-to-Orchestrator (`effort = None`) route and standalone outage retries. `ThreePassStamp` (`scripts/tests/test_review_dispatch.py:1715`) tests `stamp` and `receipt_data_ok` only for `writer = "claude"` (`astra` with string efforts), `test_codex_work_goes_to_carbon_read_only_through_a_prompt_file` (`scripts/tests/test_review_dispatch.py:2190`) runs one unrecorded pass without `--record` or `stamp`, and `scripts/tests/test_codex_writer_review.py:127` covers only the historical `muse` pin. Add an end-to-end test exercising `--record`, `stamp`, and `receipt_data_ok` across `plan`, `round`, `completion`, and `reconciliation` for `writer = "codex"` (`carbon`, `gemini-pro-orchestrator`, `effort = None`, omitted `- Effort:` metadata line), and extend `test_a_standalone_review_has_no_order` (`scripts/tests/test_review_dispatch.py:2223`) to verify that a recorded outage round (`OVERALL: blocked` or timeout) keeps the next standalone Astra round at `medium`.

### source-defect: approve

No findings. The candidate modifies reviewer configuration, dispatch, receipt validation, and repository documentation rather than `spec/FOLD.md`, `tests/rfi_ref.py`, or codec sources.

### plan-delivery: approve

No additional findings beyond the integration and consistency items. `.conclave/reviewers.toml` routes `codex` to `carbon` (`gemini-pro-orchestrator` via `agy`) without effort overrides, sets Astra to `medium` for `plan`, first adversarial `round`, and `completion` and `low` for `reconciliation` and subsequent standalone rounds, and preserves routes, lenses, `pass_cap = 33`, and `section_pin` / `schedule_sha` historical section policies.

### consistency: needs-attention

5. P2 Prompt templates and docstrings still claim the dispatcher sets `medium` effort on passes that no longer do so. `docs/reconciliation-review-prompt.md:3` still states `The dispatcher sets medium effort`, which is false now that reconciliation runs at `low` for Astra and model-managed (`None`) for Orchestrator; `docs/plan-review-prompt.md:3` states `The dispatcher sets medium effort`, which is inaccurate for Codex (`Orchestrator`); `scripts/review-dispatch.py:50` still states `the plan and completion passes take their scheduled effort`; and `scripts/tests/test_review_dispatch.py:1954` still describes `ThreePassDispatch` as `every pass at medium`. Update those four descriptions to match the current reviewer-level effort policy.

6. P2 Adjacent 2026-11-14 owner-decision comments in `.conclave/reviewers.toml` contradict each other. `.conclave/reviewers.toml:34` states `Astra retains its reviewer-level medium effort` and instructs maintainers to `Restore the prior model and scheduled medium fields to reverse this decision`, while `.conclave/reviewers.toml:38` immediately states `Astra uses medium for plan, first adversarial and completion; low for reconciliation and subsequent standalone rounds` with a different reversal instruction at `.conclave/reviewers.toml:40`. Reconcile those two comment blocks so the active policy and reversal steps are unambiguous.

### record: needs-attention

7. P2 The standalone findings note at `docs/reviews/orchestrator-reviewer-pin.md:7` is stale against the follow-up implementation. `docs/reviews/orchestrator-reviewer-pin.md:7` (committed in `50555bd` after `3b6042c4dd021f31f60d91e27eaf732fd15e9005`) still claims `dispatcher suite ran 133 tests successfully with one skip` and `Astra retains reviewer-level medium effort`, whereas the combined candidate adds `followup_effort = "low"` for reconciliation and subsequent standalone rounds and `scripts/tests/test_review_dispatch.py` now defines 135 tests. Update `docs/reviews/orchestrator-reviewer-pin.md:7` and keep `docs/drift.json` synchronized with the final documentation edits.

### quality: needs-attention

8. P3 `.agents/skills/independent-review/SKILL.md` duplicates the effort rules in one paragraph and embeds transient timeout history. `.agents/skills/independent-review/SKILL.md:8` explains Astra's `medium`/`low` split and Orchestrator's model-managed effort twice in the same paragraph and ends with `the 180-second initial Orchestrator check timed out, so use the normal bound for full candidate reviews`, which is run history belonging in `docs/reviews/` rather than a durable skill specification. Also update the synthetic reconciliation fixtures in `scripts/tests/test_review_dispatch.py:2075` and `scripts/tests/test_review_dispatch.py:2117` from `"effort": "medium"` to `"effort": "low"` so they match `REPO_PIN`.

### optimization: approve

No findings.

HIGHEST: P2
OVERALL: needs-attention

## Writer disposition

- Finding 1 accepted: require a routed writer before resolving effort on outage entries; regression reproduces the prior KeyError and now gets Refused.
- Finding 2 accepted: effort-suffixed Antigravity models require effort on every scheduled pass; omitted and partial schedules are refused. Orchestrator remains unsuffixed and receives no override.
- Finding 3 accepted: read returned rounds independently of the shared schedule and pass the position into reviewer-level effort selection. Schedule-free standalone retries now preserve medium until a returned review, then use low.
- Finding 4 accepted: add a dispatcher-to-record-to-stamp-to-receipt test for the Orchestrator route with null effort across plan, round, completion and reconciliation; test standalone outage retries with and without a schedule.
- Finding 5 accepted: correct plan/reconciliation prompt descriptions and dispatcher/test docstrings.
- Finding 6 accepted: consolidate the current owner decision and separate reversal of Astra follow-up effort from reversal of the Orchestrator model change.
- Finding 7 rejected as a request to rewrite historical measurements: the earlier note describes commit 50555bd and its actual 133-test run. Add an explicit historical label and link here; final validation is recorded here separately. The review's source-defined test count is not a measured executed-test count.
- Finding 8 accepted: remove duplicate policy prose and transient timeout history from the skill; set the reconciliation fixtures to low.

The short live dispatcher health review returned HIGHEST: none and OVERALL: approve. The first full follow-up attempt was interrupted when the operator corrected the effort mapping; it supplied no approval. This recorded adversarial review completed through agy with gemini-pro-orchestrator and no effort override, returning HIGHEST: P2 and OVERALL: needs-attention. The original 180-second timeout remains no approval.

## Operator retirement decision

The operator stopped the Gemini reviewer repair effort and requested only Claude Code and Antigravity as writers, with Codex as the only reviewer. No further live review is run for this authorized policy transition. The current pin removes the Codex writer route and Antigravity reviewer; retained adapters and fixtures exist only to read and verify historical evidence. Codex campaign launching is refused, its native campaign hook disabled, and the desktop installer removes its writer shortcut. Astra uses medium for plan, first adversarial and completion, and low for reconciliation and subsequent standalone rounds. The prior timeout and interrupted attempt remain no approval; the returned Gemini findings and their dispositions above remain historical evidence, not approval of this transition.

## Retirement validation

The current pin validates with exactly claude and gemini routed to Astra, with no Antigravity reviewer. The 218-test combined run had one unrelated fixture failure when an all-digit Git hash was uppercased; replacing that case with a fixed uppercase hexadecimal fixture preserved the intended refusal check. The final focused run passed all 84 tests covering the repaired fixture, current pin, retired Codex route, both launchers, preflight and historical hook compatibility. The earlier repaired dispatcher run passed 140 tests with one skip before retirement. Documentation drift passed with zero errors. Windows shortcut readback confirmed Claude invokes unattended-run.sh, Carbon invokes unattended-run.sh carbon, and the Codex campaign shortcut is absent. No live reviewer was started after the operator retirement instruction.
