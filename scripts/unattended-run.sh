#!/usr/bin/env bash
# Launch the unattended RasterFold campaign in Muse (docs/unattended-run.md).
# Straight on main, yolo, no user-input tool, Spark/Ember subagents loaded.
set -u
cd "$(dirname "$0")/.." || exit 1

# The writer and reviewer CLIs (muse, codex, agy) install into ~/.local/bin,
# which only login (.profile) and interactive (.bashrc) shells put on PATH.
# A shortcut or wsl.exe launch runs this script in neither, so build the
# child environment here instead of inheriting whatever PATH arrived.
PATH="${PATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}"
for dir in "$HOME/bin" "$HOME/.local/bin"; do
  if [ -d "$dir" ]; then
    case ":$PATH:" in
      *":$dir:"*) ;;
      *) PATH="$dir:$PATH" ;;
    esac
  fi
done
export PATH

if ! python3 scripts/unattended-preflight.py; then
  echo
  echo "Preflight failed: the unattended run was not started. Fix the FAIL lines above."
  exec bash
fi

PROMPT='Act with full autonomy (operator directive 2026-11-10): decide and act, self-heal every defect and red gate at its owning layer, improve the tooling, runners, and plan as you go filing what you discover through add-todo, and hold the best-in-class bar against anyone; never stop at impossible without measured proof recorded in a section. Unattended RasterFold campaign. First create the session goal: "Finish RasterFold 100% complete and excellent: every open TODO section built, gated, reviewed through the pinned three-pass schedule, stamped, and committed straight on main; then the terminal release acceptance passes and the plan closes. Best lossless RGB codec in its class on the research corpus, with no placeholders." Then load the process-plan skill and run it end to end: audit, start the campaign guard and the in-session heartbeat (muse.cron_create, 3-59/5 * * * *, canonical prompt from the skill), and ship section after section through process-todo-section plus review-todo-section until query ready prints 0 runnable now. Never ask the operator a question: where a section leaves a choice open, take the industry-standard option, record the decision and its reversal cost in the section, and carry on. Delegate by tier, cheapest capable first: deterministic work to scripts, verbatim literal work to rf-spark-hands (tier 1 Ember suspended 2026-11-10 after measured refusal failures, literal work routes to Spark), read-only lookups and exact-spec edits to rf-spark-scout and rf-spark-hands, and most section implementation under your written spec to rf-wonder-hands; honor their ESCALATE-SPARK, ESCALATE-WONDER, and ESCALATE-LEAD replies by re-delegating one tier up, verify every line a subagent produces before it lands, and keep planning, format decisions, gate verdicts, stamps, and commits on yourself as lead. A commit is not a stop. A red gate is repaired at its owning layer, never bypassed. Default every task to the lowest tier that can pass its checkpoint, verify with the cheapest falsifier (script exit codes, then deterministic equivalence, then your reading), allow one repair attempt per failure cause per tier before escalating, promote a work class one tier permanently after two verification failures, and record delegation counts by tier in the run file. Removal outranks both rules: a silent wrong output, an out-of-scope edit, or evidence that does not reproduce removes that agent for the run at once (recorded in the run file, its work redone one tier up, unrelated work preserved), and it is never delegated to again in that run; repair and promotion govern honest failures only. Milestone gates are capped (D99 T01 §8): at most 13 follow-up experiment rounds per gate; at the cap close the gate accepted-missed with the figures recorded and the target text unchanged, file its one dormant backlog revisit entry (D99 T01 §8 triggers: stronger model pinned, incidental re-measurement crossing the bar, new format version, new technique), never park a gate open waiting, then move on. Stop only at plan closeout, a recorded park where every leftover row is blocked or runnable-elsewhere, or an escalation the tree cannot fix.'

# Dry-launch entry point (docs/unattended-run.md): when
# UNATTENDED_PROMPT_OVERRIDE names a readable file, that file's text is the
# seed prompt instead of the campaign prompt above. Unset, the prompt is
# byte-identical to the campaign prompt.
if [ -n "${UNATTENDED_PROMPT_OVERRIDE:-}" ]; then
  if [ ! -r "$UNATTENDED_PROMPT_OVERRIDE" ]; then
    echo
    echo "UNATTENDED_PROMPT_OVERRIDE does not name a readable file: $UNATTENDED_PROMPT_OVERRIDE"
    echo "The unattended run was not started."
    exec bash
  fi
  PROMPT="$(cat "$UNATTENDED_PROMPT_OVERRIDE")"
fi

exec muse --yolo --disable-request-user-input \
  --model muse-wonder-1.0 --reasoning-effort medium \
  --agents "$(cat .agents/muse-agents.json)" \
  "$PROMPT"
