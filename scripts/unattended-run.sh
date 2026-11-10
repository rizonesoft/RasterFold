#!/usr/bin/env bash
# Launch the unattended RasterFold campaign in Muse (docs/unattended-run.md).
# Straight on main, yolo, no user-input tool, Spark/Ember subagents loaded.
set -u
cd "$(dirname "$0")/.." || exit 1

if ! python3 scripts/unattended-preflight.py; then
  echo
  echo "Preflight failed: the unattended run was not started. Fix the FAIL lines above."
  exec bash
fi

PROMPT='Act with full autonomy (operator directive 2026-11-10): decide and act, self-heal every defect and red gate at its owning layer, improve the tooling, runners, and plan as you go filing what you discover through add-todo, and hold the best-in-class bar against anyone; never stop at impossible without measured proof recorded in a section. Unattended RasterFold campaign. First create the session goal: "Finish RasterFold 100% complete and excellent: every open TODO section built, gated, reviewed through the pinned three-pass schedule, stamped, and committed straight on main; then the terminal release acceptance passes and the plan closes. Best lossless RGB codec in its class on the research corpus, with no placeholders." Then load the process-plan skill and run it end to end: audit, start the campaign guard and the in-session heartbeat (muse.cron_create, 3-59/5 * * * *, canonical prompt from the skill), and ship section after section through process-todo-section plus review-todo-section until query ready prints 0 runnable now. Never ask the operator a question: where a section leaves a choice open, take the industry-standard option, record the decision and its reversal cost in the section, and carry on. Delegate by tier, cheapest capable first: deterministic work to scripts, verbatim literal work to rf-ember-literal, read-only lookups and exact-spec edits to rf-spark-scout and rf-spark-hands, and most section implementation under your written spec to rf-wonder-hands; honor their ESCALATE-SPARK, ESCALATE-WONDER, and ESCALATE-LEAD replies by re-delegating one tier up, verify every line a subagent produces before it lands, and keep planning, format decisions, gate verdicts, stamps, and commits on yourself as lead. A commit is not a stop. A red gate is repaired at its owning layer, never bypassed. Default every task to the lowest tier that can pass its checkpoint, verify with the cheapest falsifier (script exit codes, then deterministic equivalence, then your reading), allow one repair attempt per failure cause per tier before escalating, promote a work class one tier permanently after two verification failures, and record delegation counts by tier in the run file. Milestone gates are capped (D99 T01 §8): at most 13 follow-up experiment rounds per gate; at the cap close the gate accepted-missed with the figures recorded and the target text unchanged, then move on. Stop only at plan closeout, a recorded park where every leftover row is blocked or runnable-elsewhere, or an escalation the tree cannot fix.'

exec muse --yolo --disable-request-user-input \
  --model muse-wonder-full-1.0 --reasoning-effort high \
  --agents "$(cat .agents/muse-agents.json)" \
  "$PROMPT"
