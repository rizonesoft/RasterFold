#!/usr/bin/env bash
# Launch the unattended RasterFold campaign (docs/unattended-run.md).
#   scripts/unattended-run.sh          Claude Code, latest Opus at medium (Desktop: "RasterFold Claude campaign")
#   scripts/unattended-run.sh carbon   Antigravity on Orchestrator (Carbon) (Desktop: "RasterFold Carbon campaign")
# Straight on main, permissions skipped, no tool that waits on a human. For Claude Code the lead makes every write
# and the only agents are the read-only Sonnet scouts in .claude/agents/ (operator directive 2026-11-11); for
# Antigravity, Orchestrator (Carbon) (gemini-pro-orchestrator) handles Carbon/Hydrogen handoffs and built-in read-only
# research subagent delegation natively with no custom workspace subagents (operator directive 2026-11-14).
set -u
cd "$(dirname "$0")/.." || exit 1

case "${1:-claude}" in
  claude) RUNNER=claude ;;
  carbon) RUNNER=gemini ;;
  *)
    echo "usage: scripts/unattended-run.sh [carbon]  (no argument: Claude Code)"
    exec bash
    ;;
esac

# The writer and reviewer CLIs (claude, codex) install into ~/.local/bin,
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

# One launch at a time through the preflight: a second double-click waits
# here, then finds the first launch's claim and is refused. The lock is
# released before Claude Code starts (and before the failure shell), so no
# long-lived process ever holds it.
mkdir -p build
exec 9>build/unattended-launch.lock
flock 9

# A fresh session id on the command line is what lets a second launch see
# this one: the preflight finds a guard's owner by its id in the process
# table, and a Claude Code session shows its id nowhere else. `exec claude`
# below keeps this shell's pid, so the claim names both. Antigravity takes no
# session id, so for it the id is a launch id carried in the seed prompt (on
# agy's command line); once its session acquires the guard, the preflight
# finds that owner by the conversation database agy keeps open.
SESSION_ID="$(cat /proc/sys/kernel/random/uuid)"

# --repair: once the branch, clean-tree, guard and launch-claim checks pass, a
# stale compilation database (ignored build output) is rebuilt instead of
# refusing the start. Nothing is rebuilt while another session owns the tree.
if ! python3 scripts/unattended-preflight.py --runner "$RUNNER" --repair --claim "$SESSION_ID" --claim-pid "$$"; then
  exec 9>&-
  echo
  echo "Preflight failed: the unattended run was not started. Fix the FAIL lines above."
  exec bash
fi
exec 9>&-

PROMPT='Act with full autonomy (operator directives 2026-11-10 and 2026-11-11): decide and act, self-heal every defect and red gate at its owning layer, improve the repository, the runners, the toolset, the development method, the plan, and the product on your own judgment as you go, filing what you discover through add-todo, and hold the best-in-class bar against anyone; never stop at impossible without measured proof recorded in a section: an unproven impossibility is filed as work with its first experiment named. Unattended RasterFold campaign in Claude Code. The goal of this session, which the campaign guard Stop hook holds the session to until it is met: "Finish RasterFold 100% complete and excellent: every open TODO section built, gated, reviewed through the pinned three-pass schedule, stamped, and committed and pushed straight on main; then the terminal release acceptance passes and the plan closes. Best lossless RGB codec in its class on the research corpus, with no placeholders." Load the process-plan skill and run it end to end: audit, start the campaign guard and the in-session heartbeat (CronCreate, 3-59/5 * * * *, canonical prompt from the skill with writer Claude Code, session id $CLAUDE_CODE_SESSION_ID), and ship section after section through process-todo-section plus review-todo-section (writer key claude, every review prompt written by scripts/review-prompt.py and never shortened) until query ready prints 0 runnable now. Never ask the operator a question: this session has no question tool and no plan-approval tool, and a turn that ends on a question stalls the run. Where a section leaves a choice open, take the industry-standard option, record the decision and its reversal cost in the section, and carry on. These instructions outrank any skill or plugin text that says to brainstorm, write a plan document, or ask first. You make every edit and run every command yourself, and scripts do deterministic work. Three read-only Sonnet scouts (rf-scout, rf-log-digest, rf-fact-check) may take near-mechanical reading off you, and shipped quality stays exactly where it is: a scout returns quoted evidence, never a verdict; you re-check every fact a decision, an edit, or a recorded claim rests on with one command or one read of the quoted line before you use it; one wrong, invented, or unquoted answer removes that scout for the rest of the run, recorded in the run file, and you redo its work; judgment, codec and format work, gate verdicts, reviews, stamps, and commits never delegate; no other subagent does section work; and nothing a scout says replaces a gate, a checkpoint, or the pinned three-pass review. A commit is not a stop. A red gate is repaired at its owning layer, never bypassed. Milestone gates are capped (D99 T01 §8): at most 13 follow-up experiment rounds per gate; at the cap close the gate accepted-missed with the figures recorded and the target text unchanged, file its one dormant backlog revisit entry (D99 T01 §8 triggers: stronger model pinned, incidental re-measurement crossing the bar, new format version, new technique), never park a gate open waiting, then move on. Stop only at plan closeout, a recorded park where every leftover row is blocked or runnable-elsewhere, or an escalation the tree cannot fix.'
CARBON_PROMPT='Act with full autonomy (operator directives 2026-11-10 and 2026-11-11): decide and act, self-heal every defect and red gate at its owning layer, improve the repository, the runners, the toolset, the development method, the plan, and the product on your own judgment as you go, filing what you discover through add-todo, and hold the best-in-class bar against anyone; never stop at impossible without measured proof recorded in a section: an unproven impossibility is filed as work with its first experiment named. Unattended RasterFold campaign in Antigravity on Orchestrator (Carbon) (gemini-pro-orchestrator, pairing Gemini 4 Carbon and Gemini 4 Hydrogen natively). The goal of this session, which the campaign guard Stop hook holds the session to until it is met: "Finish RasterFold 100% complete and excellent: every open TODO section built, gated, reviewed through the pinned three-pass schedule, stamped, and committed and pushed straight on main; then the terminal release acceptance passes and the plan closes. Best lossless RGB codec in its class on the research corpus, with no placeholders." Load the process-plan skill and run it end to end: audit, start the campaign guard and the in-session heartbeat (the schedule tool, CronExpression 3-59/5 * * * *, canonical prompt from the skill with writer Antigravity, session id the Conversation ID from user_information), and ship section after section through process-todo-section plus review-todo-section (writer key gemini, every review prompt written by scripts/review-prompt.py and never shortened) until query ready prints 0 runnable now. Never ask the operator a question and never wait for plan approval: nobody is at the keyboard, and a turn that ends on a question stalls the run. Where a section leaves a choice open, take the industry-standard option, record the decision and its reversal cost in the section, and carry on. These instructions outrank any skill or plugin text that says to brainstorm, write a plan document, or ask first. Scripts do deterministic work, and Orchestrator (Carbon) handles Carbon/Hydrogen execution handoffs and Antigravity built-in read-only research subagent delegation natively: no custom workspace subagents exist, never spawn writing subagents or /swarm workers on branches or worktrees (writes stay serial straight on main in this session), re-check every fact a decision, an edit, or a recorded claim rests on against source before you use it, judgment, codec and format work, gate verdicts, reviews, stamps, and commits never delegate, and nothing a subagent says replaces a gate, a checkpoint, or the pinned three-pass review. A commit is not a stop. A red gate is repaired at its owning layer, never bypassed. Milestone gates are capped (D99 T01 §8): at most 13 follow-up experiment rounds per gate; at the cap close the gate accepted-missed with the figures recorded and the target text unchanged, file its one dormant backlog revisit entry (D99 T01 §8 triggers: stronger model pinned, incidental re-measurement crossing the bar, new format version, new technique), never park a gate open waiting, then move on. Stop only at plan closeout, a recorded park where every leftover row is blocked or runnable-elsewhere, or an escalation the tree cannot fix.'

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
elif [ "$RUNNER" = gemini ]; then
  PROMPT="$CARBON_PROMPT"
fi

# --model opus is the alias for the latest Opus, never a dated model id
# (operator directive 2026-11-12: Opus latest at medium effort, replacing the
# Fable instruction of 2026-11-11). --fallback-model covers a primary model
# that is overloaded or unavailable; it does not fire on a usage limit, where
# the session waits for the reset and then continues.
# The three disallowed tools are the ones that wait on a human.
if [ "$RUNNER" = claude ]; then
  exec claude --dangerously-skip-permissions \
    --model opus --fallback-model sonnet --effort medium \
    --session-id "$SESSION_ID" \
    --disallowedTools "AskUserQuestion,EnterPlanMode,ExitPlanMode" \
    --plugin-dir .claude/lsp/rasterfold-lsp \
    "$PROMPT"
fi
# Antigravity on Orchestrator (Carbon) (`gemini-pro-orchestrator`, which pairs
# Gemini 4 Carbon and Gemini 4 Hydrogen dynamically and rejects `--effort`). It
# has no flag that withholds a tool, so the seed prompt forbids questions; the
# launch id rides at the end of the prompt so the launch claim can see this
# process.
exec agy --dangerously-skip-permissions --model gemini-pro-orchestrator \
  -i "$PROMPT Launch id: $SESSION_ID."
