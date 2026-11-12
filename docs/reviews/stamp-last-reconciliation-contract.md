## Round 1: astra (`gpt-6-astra` via `codex`)

- Writer: claude
- Run: 2026-11-12T06:43:13Z
- Candidate: b2694f562baf3c6190d2429e5ec96f42c073e132
- Effort: medium
- Lenses: adversarial, integration, source-defect, plan-delivery, consistency, record, quality, optimization
- Lines: 34
- Digest: 2dd3844ace9e3386

### adversarial: approve

No findings.

### integration: needs-attention

1. P2 The documented recovery cannot handle a changed-contract reconciliation that returned `complete`, including the motivating D09 T01 §2 case. `.agents/skills/review-todo-section/SKILL.md:92` instructs the writer to preserve that review, restore the text and run another reconciliation, but `scripts/review-dispatch.py:977` refuses another pass after `complete`. Even if another review were recorded, `scripts/todo-graph.py:6850` rejects any earlier complete reconciliation. The existing review ends `complete` at `docs/reviews/09-release/D09-T01-s2.md:118`. An in-memory probe confirmed the dispatcher rejects this sequence while accepting repetition after `gaps`. Define an explicit recovery for this case, preserving the historical verdict and requiring a fresh review of the restored contract, or qualify the instructions to cover only earlier `gaps` reviews. Add coverage through the dispatcher and stamp checks; the helper-only cases at `scripts/todo-graph.py:12767` miss this obstruction.

### source-defect: approve

No findings.

### plan-delivery: approve

No findings.

### consistency: approve

No findings.

### record: approve

No findings. Git history supports the November 6 single-reconciliation rule and November 9 expansion.

### quality: approve

No additional findings.

### optimization: approve

No findings.

HIGHEST: P2
OVERALL: needs-attention

## Writer note: round 1 fixes

- Implemented: 1 (the documented recovery could not run after a changed-contract reconciliation that returned complete). `scripts/review-dispatch.py` now reads the section's contract through `section_contract` (`section_contract_at`, `contract_changed`, `void_reconciliation`): a reconciliation is refused, before a review is spent, when the candidate's section text differs from the text the failed completion read, and a recorded `RECONCILIATION: complete` of such a changed text is void, so it no longer ends the section's reconciliations. `completion_record` in `scripts/todo-graph.py` accepts an earlier complete reconciliation only when it is void in that sense; an earlier complete of the unchanged contract is still refused. Coverage through both tools in `scripts/tests/test_review_dispatch.py`: `test_a_changed_section_is_refused_and_a_void_complete_does_not_end_the_reconciliations` (dispatcher, by subprocess), `test_a_reconciliation_of_a_changed_section_is_void_and_the_restored_text_stamps` and `test_a_changed_section_on_the_stamped_candidate_is_refused_after_an_unchanged_reconciliation` (stamp). The skill text now says the dispatcher refuses the changed text and how a void complete is recovered.

## Round 2: astra (`gpt-6-astra` via `codex`)

- Writer: claude
- Run: 2026-11-12T06:50:10Z
- Candidate: 7764c9440b9482ba83d113b3aec803154e203e06
- Effort: medium
- Lenses: adversarial, integration, source-defect, plan-delivery, consistency, record, quality, optimization
- Lines: 34
- Digest: 49cd79bba218df9a

### adversarial: needs-attention

1. P2 Unreadable contracts fail open at dispatch. `scripts/review-dispatch.py:965` converts every contract-read or module-load exception to `None`, and `scripts/review-dispatch.py:973` treats that as no change. Consequently, `scripts/review-dispatch.py:1023` permits a reconciliation without establishing contract equality. An in-memory probe using the recorded D09 T01 §2 candidates confirmed that the changed candidate is normally refused but allowed when its contract read raises `OSError`. This can spend a capped review on an unverifiable candidate. The stamp still rejects unreadable contracts, so this is not a demonstrated stamp bypass. Refuse dispatch when either contract cannot be read, preserve the diagnostic, and cover this failure path.

### integration: approve

No additional findings. The round 1 recovery obstruction is fixed for readable contracts.

### source-defect: approve

No findings.

### plan-delivery: approve

No additional findings.

### consistency: approve

No additional findings.

### record: approve

No findings.

### quality: approve

No additional findings.

### optimization: approve

No findings.

HIGHEST: P2
OVERALL: needs-attention

## Writer note: round 2 fixes

- Implemented: 1 (unreadable contracts failed open at dispatch). `section_contract_at` in `scripts/review-dispatch.py` now raises `Refused` with the underlying error's type and message when the contract cannot be read at either commit or todo-graph does not load, so `contract_changed` and `void_reconciliation` only ever act on two contracts that were read, and a reconciliation is refused before a review is spent. `test_an_unreadable_section_contract_refuses_the_reconciliation` covers a section removed from the tree and a missing `scripts/todo-graph.py`, asserting the refusal, the kept diagnostic and that no reviewer was called. The three-pass dispatcher fixture now carries the real todo-graph and a section, since every reconciliation reads its contract.
