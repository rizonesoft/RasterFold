#!/usr/bin/env bash
# Launch the unattended RasterFold campaign in Claude Code (docs/unattended-run.md).
# Straight on main, permissions skipped, no tool that waits on a human. The lead makes every write;
# the only agents are the read-only Sonnet scouts in .claude/agents/ (operator directive 2026-11-11).
set -u
cd "$(dirname "$0")/.." || exit 1

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

# --repair: once the branch, clean-tree and guard checks pass, a stale
# compilation database (ignored build output) is rebuilt instead of refusing
# the start. Nothing is rebuilt while another session owns the tree.
if ! python3 scripts/unattended-preflight.py --repair; then
  echo
  echo "Preflight failed: the unattended run was not started. Fix the FAIL lines above."
  exec bash
fi

PROMPT='Act with full autonomy (operator directives 2026-11-10 and 2026-11-11): decide and act, self-heal every defect and red gate at its owning layer, improve the tooling, runners, and plan as you go filing what you discover through add-todo, and hold the best-in-class bar against anyone; never stop at impossible without measured proof recorded in a section. Unattended RasterFold campaign in Claude Code. The goal of this session, which the campaign guard Stop hook holds the session to until it is met: "Finish RasterFold 100% complete and excellent: every open TODO section built, gated, reviewed through the pinned three-pass schedule, stamped, and committed and pushed straight on main; then the terminal release acceptance passes and the plan closes. Best lossless RGB codec in its class on the research corpus, with no placeholders." Load the process-plan skill and run it end to end: audit, start the campaign guard and the in-session heartbeat (CronCreate, 3-59/5 * * * *, canonical prompt from the skill with writer Claude Code, session id $CLAUDE_CODE_SESSION_ID), and ship section after section through process-todo-section plus review-todo-section (writer key claude, every review prompt written by scripts/review-prompt.py and never shortened) until query ready prints 0 runnable now. Never ask the operator a question: this session has no question tool and no plan-approval tool, and a turn that ends on a question stalls the run. Where a section leaves a choice open, take the industry-standard option, record the decision and its reversal cost in the section, and carry on. These instructions outrank any skill or plugin text that says to brainstorm, write a plan document, or ask first. You make every edit and run every command yourself, and scripts do deterministic work. Three read-only Sonnet scouts (rf-scout, rf-log-digest, rf-fact-check) may take near-mechanical reading off you, and shipped quality stays exactly where it is: a scout returns quoted evidence, never a verdict; you re-check every fact a decision, an edit, or a recorded claim rests on with one command or one read of the quoted line before you use it; one wrong, invented, or unquoted answer removes that scout for the rest of the run, recorded in the run file, and you redo its work; judgment, codec and format work, gate verdicts, reviews, stamps, and commits never delegate; no other subagent does section work; and nothing a scout says replaces a gate, a checkpoint, or the pinned three-pass review. A commit is not a stop. A red gate is repaired at its owning layer, never bypassed. Milestone gates are capped (D99 T01 §8): at most 13 follow-up experiment rounds per gate; at the cap close the gate accepted-missed with the figures recorded and the target text unchanged, file its one dormant backlog revisit entry (D99 T01 §8 triggers: stronger model pinned, incidental re-measurement crossing the bar, new format version, new technique), never park a gate open waiting, then move on. Stop only at plan closeout, a recorded park where every leftover row is blocked or runnable-elsewhere, or an escalation the tree cannot fix.'

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

# A fresh session id on the command line is what lets a second launch see
# this one: the preflight finds a guard's owner by its id in the process
# table, and a Claude Code session shows its id nowhere else.
SESSION_ID="$(cat /proc/sys/kernel/random/uuid)"

# --model opus is the alias for the latest Opus, never a dated model id.
# The three disallowed tools are the ones that wait on a human.
exec claude --dangerously-skip-permissions \
  --model opus --effort medium \
  --session-id "$SESSION_ID" \
  --disallowedTools "AskUserQuestion,EnterPlanMode,ExitPlanMode" \
  --plugin-dir .claude/lsp/rasterfold-lsp \
  "$PROMPT"
