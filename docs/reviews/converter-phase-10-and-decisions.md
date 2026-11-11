## Round 1: astra (`gpt-6-astra` via `codex`)

- Writer: claude
- Run: 2026-11-11T19:06:38Z
- Candidate: 777053a94cde76918fb05cd6c09b54c56f7e019d
- Effort: medium
- Lenses: adversarial, integration, source-defect, plan-delivery, consistency, record, quality, optimization
- Lines: 20
- Digest: caaf41305599bda9

Reviewed `b4f3e88..777053a` on `main`, read-only. Three P2 findings:

1. **P2, adversarial / plan-delivery:** `todo/implementation-plan.md:339` marks the interface shell as needing `none`, although its checkpoint requires desktop captures and focus/action assertions (`todo/10-converter/TODO-03-interface.md:85`). Once its prerequisites are stamped, a headless WSL campaign can select this row but cannot complete it, undermining the newly promised park-and-continue behavior before Phase 11. **Smallest fix:** add `Requires: display-session` to D10 T03 §1 and regenerate the plan.

2. **P2, integration / consistency:** `todo/10-converter/TODO-03-interface.md:27` explicitly says Resolute supplies no UI Automation providers, yet requires supplied controls to be used as is. The new provider work at line 78 covers only locally implemented generic controls; §2 covers converter-specific surfaces. Nobody owns accessibility adapters for the supplied `ListView`, toolbar, sidebar and other interactive controls. In particular, the format selector and sortable results can remain inaccessible while those implementation items pass. **Smallest fix:** assign UI Automation adapters for every used Resolute control to §1, preserving its rendering, and require enumeration and action tests for those controls.

3. **P2, plan-delivery / consistency:** `todo/10-converter/TODO-03-interface.md:193` and line 197 replace screen-reader completion evidence with a UI Automation client run. The client specified at line 110 invokes providers and reads properties; that does not demonstrate usable screen-reader navigation or spoken announcements. `docs/design/converter.md:27` still requires actual screen-reader runs. **Smallest fix:** retain the automated UIA checks and restore an explicit Windows screen-reader conversion/comparison run, including error recovery and announcements.

Static checks passed: `plan-audit.py` (254 sections, zero errors), `todo-graph.py plan --check`, `research/milestones.py --check`, and `git diff --check`. No additional phase-number assumption or dependency-order defect was found. No concrete authorization conflict was found in D99 §7/§9 or the widened autonomy paragraph. Upstream Resolute retrieval failed, so its API facts were checked for internal consistency only.

adversarial: needs-attention  
integration: needs-attention  
source-defect: approve  
plan-delivery: needs-attention  
consistency: needs-attention  
record: approve  
quality: approve  
optimization: approve  
HIGHEST: P2  
OVERALL: needs-attention

## Disposition of round 1

All three findings were verified against the tree and fixed in the commit that records this round.

- P2, the interface shell read as needing nothing: `D10 T03 §1` carries `Requires: display-session`, so a headless run parks it instead of selecting it.
- P2, no owner for accessibility of the supplied controls: `D10 T03 §1` adds UI Automation adapters for every `resolute-ui` control the converter uses, with enumeration and action tests.
- P2, screen-reader evidence replaced by a UI Automation client: `D10 T03 §7` restores a recorded Windows Narrator run for conversion, comparison and failed-conversion recovery, and its checkpoint quotes it; the automated checks stay.

