#!/usr/bin/env python3
"""Run one independent review round with the reviewer pinned in .conclave/reviewers.toml.

    python3 scripts/review-dispatch.py --writer <claude|codex|gemini|muse|opencode> --prompt review-prompt.md --record docs/reviews/x.md
    python3 scripts/review-dispatch.py --kind completion --candidate <40-char SHA> --writer <claude|codex|gemini|muse|opencode> \
        --prompt completion-prompt.md --record docs/reviews/x.md     the one-pass completion and excellence review
    python3 scripts/review-dispatch.py --check-pin [--json] [--pin <path>]     the pin's verdict; runs no reviewer
    python3 scripts/review-dispatch.py --list-rounds [--json] [--records <dir>]  recorded rounds; runs no reviewer

`--writer` is the agent whose work is under review: `claude`, `codex`, `gemini`, `muse` or `opencode`. Every writer's route in the
pin names exactly one reviewer of a different model family, with no fallback.
A reviewer that is not installed, exits non-zero, times out,
returns nothing, or ends its reply `OVERALL: blocked` (it could not reach the candidate, for example a sandbox that
cannot read the repository) returned no review, and the reason is recorded: exit 1, which never counts as approval.
A review that finds problems is still a review (exit 0).

`--check-pin` loads the pin with `load_pin`, the only place the rules live, and reports without running anything:
with `--json`, `{"ok": true, "schema_version", "reviewers", "routes", "clis": {cli: on PATH}}` and exit 0, or
`{"ok": false, "error": "<the Refused message>"}` and exit 2; without `--json`, `ok` or `refused: <message>`.

The reviewer runs read-only in the repository root: Codex in its read-only sandbox, Antigravity in plan mode inside its
sandbox. Muse Code is no longer routed, but older sections' pins may name it: its shell and write tools are disabled,
so its prompt must include the exact candidate diff. The script prints a JSON summary;
`--record` also appends a `## Round N` section to the findings file with an atomic write, and writes nothing (exit 2,
the summary still printed) to a findings file it cannot read back. Works the same from PowerShell and from WSL.
Stdlib only.

`--kind completion` runs the one-pass completion and excellence review (`docs/completion-review-prompt.md`) on the
built section before its regular rounds. With `--record` it appends a `## Completion review` section naming the
reviewer, its model and the `--candidate` commit instead of a `## Round N` section, and it is not counted as a round.
A findings file holds one returned completion review: a second is refused (exit 2) and nothing is appended, while a
pass that returned no review (`## Completion review: none`) may be followed by another.

`--kind plan` runs the one-pass plan review (`docs/plan-review-prompt.md`) on the section as written, before the build:
it asks whether building the section exactly as planned produces a 100% complete feature. It is recorded as a
`## Plan review` section with the same one-pass rule, and it is not counted as a round.

The current pin's `policy = "three-pass"` (owner decision 2026-11-06) runs one plan review before the build, one
combined adversarial round after the build and its gates, then one completion review of the final candidate, every pass
at medium. A findings file named for a section (`docs/reviews/<NN-domain>/D<NN>-T<NN>-s<N>.md`) has that order enforced
before a review is spent. `--kind reconciliation` (`docs/reconciliation-review-prompt.md`) is the further pass after
a completion that returned gaps, on a candidate descending from it; it ends `RECONCILIATION: complete` or `gaps`. A
reconciliation that returned gaps may be followed by another on a candidate descending from it, until one returns
complete or the section reaches `REVIEW_PASS_CAP` (33) returned passes in all.

A section stays on the policy of the pin at its plan review's candidate, so a section already in its rounds keeps the
schedule and route it started under, and scripts/todo-graph.py `stamp` holds its receipt to that same pin.

When the pin has a `[schedule]`, the dispatcher owns each pass's effort and lenses (owner decision 2026-10-24): the
plan and completion passes take their scheduled effort, and a round's place in `[[schedule.rounds]]` comes from the
rounds that returned a review already recorded in the `--record` file. Rounds past that list rotate one lens per round
through `[schedule.rotation]` (the design lens only with `--surface`), and a low-effort round whose review reads
`HIGHEST: P1` is followed by the `[schedule.escalation]` step. The scheduled lenses are appended to the prompt and
recorded on an `- Lenses:` line. The JSON summary carries `position`, `escalated` and `severity_stop` (true when a
returned round past `severity_stop_after` reads `HIGHEST: P3`: with `severity_stop_after = 0`, any round with no P1 or P2 finding may stop the loop).
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import tomllib

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / ".conclave" / "reviewers.toml"
CLIS = ("codex", "agy", "muse")
# A reviewer is never of the writer's own model family: its CLI names the family, and so does the writer.
FAMILY = {"codex": "openai", "agy": "google", "muse": "meta"}
# `gemini` is Antigravity running a Gemini model; `opencode` is OpenCode running an Anthropic model. The pin's [routes]
# names who may write today (owner decision 2026-11-06: claude, codex, gemini, muse, opencode). `agy` and `copilot` stay
# here only because recorded reviews and the pins of older sections name them, and those must keep reading back.
WRITER_FAMILY = {"claude": "anthropic", "codex": "openai", "gemini": "google", "muse": "meta",
                 "opencode": "anthropic", "agy": "google", "copilot": "anthropic"}
WRITERS = tuple(WRITER_FAMILY)
# Antigravity carries the effort in its model name, so it takes none; load_pin holds that name to the schedule.
EFFORTS = {"codex": ("low", "medium", "high", "xhigh"),
           "muse": ("none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra")}
MODEL = re.compile(r"^[a-z0-9][a-z0-9.-]*$")
NAME = re.compile(r"^[a-z0-9][a-z0-9-]*$")
# A review is prose. Output past this size is a runaway reviewer, not a review.
MAX_OUTPUT_BYTES = 2_000_000
# The prompt's reply format ends with an OVERALL line; `blocked` means the reviewer never saw the work.
BLOCKED = re.compile(r"^\s*(?:OVERALL|COMPLETION):\s*blocked\b", re.IGNORECASE)
POLL_SECONDS = 0.2
CLEANUP_SECONDS = 10


class Refused(Exception):
    """The pin or the arguments are wrong; nothing ran."""


def load_pin(path=PIN, data=None):
    """The checked pin at `path`, or, given `data`, the pin in those bytes (a committed pin read with `git show`), so
    a read-only caller needs no temporary file."""
    try:
        if data is None:
            with open(path, "rb") as handle:
                data = handle.read()
        pin = tomllib.loads(data.decode("utf-8"))
    except FileNotFoundError:
        raise Refused(f"no reviewer pin at {path}") from None
    except tomllib.TOMLDecodeError as exc:
        raise Refused(f"unreadable reviewer pin: {exc}") from None
    # A directory, a file it may not read, bytes that are not UTF-8, an integer past Python's digit limit, or nesting
    # deep enough to exhaust the parser's recursion: each is a pin that cannot be read, so it is a refusal (exit 2,
    # one JSON object), never a traceback. ValueError comes after its subclasses TOMLDecodeError and UnicodeDecodeError.
    except OSError as exc:
        raise Refused(f"unreadable reviewer pin: {exc.strerror or exc}: {path}") from None
    except UnicodeDecodeError:
        raise Refused("unreadable reviewer pin: not UTF-8 text") from None
    except ValueError as exc:
        raise Refused(f"unreadable reviewer pin: {exc}") from None
    except RecursionError:
        raise Refused("unreadable reviewer pin: nested too deeply to read") from None
    # `type(...) is int`: TOML true is Python True (an int subclass) and 1.0 == 1, so `!= 1` alone let both load.
    if not {"schema_version", "reviewers", "routes"} <= set(pin) <= {"schema_version", "reviewers", "routes",
                                                                     "schedule"} \
            or type(pin["schema_version"]) is not int or pin["schema_version"] != 1:
        raise Refused("the reviewer pin needs exactly schema_version = 1, [reviewers.*] and [routes], "
                      "and may add [schedule]")
    reviewers = pin["reviewers"]
    if not isinstance(reviewers, dict) or not reviewers:
        raise Refused("the reviewer pin defines no reviewers")
    for name, entry in reviewers.items():
        # fullmatch: `$` alone also matches before a trailing newline, which would then break the round heading.
        if not NAME.fullmatch(name) or not isinstance(entry, dict):
            raise Refused(f"invalid reviewer {name!r}")
        unknown = set(entry) - {"cli", "model", "effort"}
        if unknown:
            raise Refused(f"reviewer {name!r} has unknown keys: {', '.join(sorted(unknown))}")
        cli, model, effort = entry.get("cli"), entry.get("model"), entry.get("effort")
        if cli not in CLIS:
            raise Refused(f"reviewer {name!r} needs cli = one of {', '.join(CLIS)}")
        if not isinstance(model, str) or not MODEL.fullmatch(model):
            raise Refused(f"reviewer {name!r} has a malformed model")
        if cli == "agy":
            if effort is not None:
                raise Refused(f"reviewer {name!r}: Antigravity carries the effort in the model name, so set no effort")
        elif effort not in EFFORTS[cli]:
            raise Refused(f"reviewer {name!r} needs effort = one of {', '.join(EFFORTS[cli])}")
    routes = pin["routes"]
    if not isinstance(routes, dict) or set(routes) - set(WRITER_FAMILY):
        raise Refused(f"[routes] may only name the writers {', '.join(WRITER_FAMILY)}")
    for writer, chain in routes.items():
        # One reviewer, no fallback (owner decision 2026-10-24).
        if not isinstance(chain, list) or len(chain) != 1 or not isinstance(chain[0], str):
            raise Refused(f"route {writer!r} must list exactly one reviewer, with no fallback")
        if chain[0] not in reviewers:
            raise Refused(f"route {writer!r} must name a defined reviewer")
        if FAMILY[reviewers[chain[0]]["cli"]] == WRITER_FAMILY[writer]:
            raise Refused(f"route {writer!r} names {chain[0]}, of the writer's own family; "
                          f"a review comes from a different family")
    if "schedule" in pin:
        routed = {chain[0] for chain in routes.values()}
        check_schedule(pin["schedule"], {reviewers[name]["cli"] for name in routed} - {"agy"})
        for name in routed:
            if reviewers[name]["cli"] == "agy":
                check_agy_effort(name, reviewers[name]["model"], pin["schedule"])
    return pin


# The lenses a schedule may name, each with the question the reviewer is given for it. `plan` and `plan-delivery`
# read the section's plan: before the build (is it a complete feature?) and after it (did the build deliver it?).
LENSES = {
    "plan": "Will this section, built exactly as written, produce a 100% complete feature? Name every missing "
            "control, state, setting consumer, error or refusal path, undo or reverse path, Ctrl+K reach, persistence "
            "read-back and test, and every `Done when:` that cannot fail.",
    "plan-delivery": "Did the build deliver every checklist item and `Done when:` of the section, with nothing "
                     "partial, stubbed or quietly narrowed?",
    "adversarial": "How does this fail on hostile input, races, aborts, cancellation, automation or links?",
    "integration": "Do callers and consumers still hold, with every contract (threads, ownership, errors) kept?",
    "source-defect": "Is the upstream specification or spec/FOLD.md read accurately, with deviations noted?",
    "consistency": "Does it match the module rules, naming, shared components, settings and sibling code?",
    "quality": "Is the code clear, minimal and maintainable: dead code, duplication, misleading names or comments, "
               "tests that pass without exercising the behaviour?",
    "design": "Does the rendered UI match the section's **Design:** specification and its tokens: roles, radii, icons, states, accessibility?",
    "optimization": "Where does it waste time, memory, allocations, I/O or redraws on the paths users hit, and what "
                    "measured target does it miss?",
    "record": "Does the evidence match what ran, with owned deferrals and an earned flip of every item?",
}
STEP_KEYS = {"effort", "lenses"}
THREE_PASS = "three-pass"
# Operator decision 2026-11-09: a three-pass section takes at most this many returned review passes (plan, round,
# completion and every reconciliation together), so an unattended run repairs and re-reconciles instead of parking
# after one failed reconciliation. scripts/todo-graph.py `stamp` holds receipts to the same number.
REVIEW_PASS_CAP = 33


def check_step(where, step, efforts, lenses=True):
    if not isinstance(step, dict) or set(step) - STEP_KEYS or "effort" not in step \
            or (lenses and "lenses" not in step) or (not lenses and "lenses" in step):
        need = "effort and lenses" if lenses else "only effort"
        raise Refused(f"[schedule] {where} needs {need}")
    if step["effort"] not in efforts:
        raise Refused(f"[schedule] {where} needs effort = one of {', '.join(efforts)}")
    if lenses:
        names = step["lenses"]
        if not isinstance(names, list) or not names or not all(isinstance(n, str) and n in LENSES for n in names) \
                or len(set(names)) != len(names):
            raise Refused(f"[schedule] {where} needs lenses = a list of distinct lenses from "
                          f"{', '.join(LENSES)}")


def check_agy_effort(name, model, schedule):
    """An Antigravity reviewer runs at its model name's effort whatever the schedule says, so the two must agree, or a
    pass would be recorded at an effort it did not run at."""
    steps = [schedule[k] for k in ("plan", "completion", "reconciliation", "rotation", "escalation") if k in schedule]
    for effort in {step["effort"] for step in [*steps, *schedule["rounds"]]}:
        if not model.endswith(f"-{effort}"):
            raise Refused(f"reviewer {name!r} runs at the effort in its model name {model}, but the schedule has a "
                          f"pass at {effort}")


def check_schedule(schedule, clis):
    """The review schedule, as strictly as the reviewers: every effort valid for every routed CLI, every lens known."""
    efforts = [e for e in EFFORTS["codex"] if all(e in EFFORTS[cli] for cli in clis)]
    if isinstance(schedule, dict) and schedule.get("policy") == THREE_PASS:
        keys = {"policy", "plan", "completion", "rounds", "reconciliation"}
        if set(schedule) != keys:
            raise Refused(f"[schedule] with policy = \"{THREE_PASS}\" needs exactly {', '.join(sorted(keys))}")
        check_step("plan", schedule["plan"], efforts)
        check_step("completion", schedule["completion"], efforts, lenses=False)
        check_step("reconciliation", schedule["reconciliation"], efforts)
        rounds = schedule["rounds"]
        if not isinstance(rounds, list) or len(rounds) != 1:
            raise Refused(f"[schedule] with policy = \"{THREE_PASS}\" needs exactly one round")
        check_step("round 1", rounds[0], efforts)
        return
    keys = {"plan", "completion", "rounds", "rotation", "escalation", "severity_stop_after"}
    if not isinstance(schedule, dict) or set(schedule) != keys:
        raise Refused(f"[schedule] needs exactly {', '.join(sorted(keys))}")
    check_step("plan", schedule["plan"], efforts)
    check_step("completion", schedule["completion"], efforts, lenses=False)
    check_step("escalation", schedule["escalation"], efforts)
    check_step("rotation", schedule["rotation"], efforts)
    rounds = schedule["rounds"]
    if not isinstance(rounds, list) or not rounds:
        raise Refused("[schedule] rounds needs at least one round")
    for n, step in enumerate(rounds, 1):
        check_step(f"round {n}", step, efforts)
    stop = schedule["severity_stop_after"]
    if type(stop) is not int or not 0 <= stop <= 33:
        raise Refused("[schedule] severity_stop_after needs a whole number of rounds from 0 to 33")


HIGHEST = re.compile(r"^\s*HIGHEST:\s*(P[123])\b", re.IGNORECASE | re.MULTILINE)


def highest(review):
    """The `HIGHEST:` severity a review declares (`P1`, `P2`, `P3`), or None."""
    found = HIGHEST.findall(review or "")
    return found[-1].upper() if found else None


def returned_review(entry):
    """A recorded round that is a review: a reviewer of another model family than the writer ran and replied with the
    prompt's final `OVERALL: approve` or `OVERALL: needs-attention` line. An outage, a self-review (a reviewer of the
    writer's own family, or of no known family) and a reply without that line are recorded rounds, not reviews.
    scripts/todo-graph.py `stamp` uses this same predicate, so the schedule and the receipt count the same rounds."""
    lines = [x.strip() for x in (entry.get("review") or "").splitlines() if x.strip()]
    return bool(entry.get("reviewer")) and entry.get("writer") in WRITER_FAMILY \
        and FAMILY.get(entry.get("cli")) not in (None, WRITER_FAMILY[entry["writer"]]) \
        and bool(lines) and lines[-1] in ("OVERALL: approve", "OVERALL: needs-attention")


def valid_one_pass_footer(text, kind):
    """True when a one-pass reply (`completion`, `reconciliation` or `plan`) ends with the footer lines `stamp` requires."""
    lines = [x.strip() for x in (text or "").splitlines() if x.strip()]
    if kind == "completion":
        return bool(lines) and lines[-1] in ("COMPLETION: complete", "COMPLETION: gaps")
    if kind == "reconciliation":
        return bool(lines) and lines[-1] in ("RECONCILIATION: complete", "RECONCILIATION: gaps")
    if kind == "plan":
        return (len(lines) >= 2
                and lines[-2] in ("PLAN: complete", "PLAN: gaps")
                and lines[-1] in ("OVERALL: approve", "OVERALL: needs-attention"))
    return False


def returned_one_pass(entry, kind):
    """A recorded completion, reconciliation or plan pass that is a valid returned review from another model family.
    scripts/todo-graph.py `stamp` uses this same predicate."""
    return bool(entry.get("reviewer")) and entry.get("writer") in WRITER_FAMILY \
        and FAMILY.get(entry.get("cli")) not in (None, WRITER_FAMILY[entry["writer"]]) \
        and valid_one_pass_footer(entry.get("review"), kind)


def scheduled(schedule, kind, rounds, surface):
    """The effort and lenses for the next pass, from the pin's schedule and the rounds already recorded.

    The position counts returned reviews only (`returned_review`): an outage, a self-review and a reply without the
    final OVERALL line do not move it. Rounds past the fixed list rotate through `rotation`'s lenses, one per round,
    skipping `design` when the section has no surface. After a low-effort round that declared `HIGHEST: P1`, the next
    round is the `escalation` step instead. scripts/todo-graph.py `stamp` replays this to check every recorded
    pass, so it is the one copy of the schedule rule. The three-pass policy has one round, whose `design` lens applies
    only to a section with a surface; `check_three_pass_order` and the stamp refuse a second round."""
    if kind in ("plan", "completion", "reconciliation"):
        step = schedule[kind]
        return {"position": None, "effort": step["effort"], "lenses": list(step.get("lenses", [])),
                "escalated": False}
    returned = [r for r in rounds if returned_review(r)]
    position = len(returned) + 1
    if schedule.get("policy") == THREE_PASS:
        step = schedule["rounds"][0]
        return {"position": position, "effort": step["effort"],
                "lenses": [n for n in step["lenses"] if surface or n != "design"], "escalated": False}
    last = returned[-1] if returned else None
    if last is not None and last.get("effort") == "low" and highest(last.get("review")) == "P1":
        step, escalated = schedule["escalation"], True
    elif position <= len(schedule["rounds"]):
        step, escalated = schedule["rounds"][position - 1], False
    else:
        rotation = [n for n in schedule["rotation"]["lenses"] if surface or n != "design"] \
            or schedule["rotation"]["lenses"]
        step = {"effort": schedule["rotation"]["effort"],
                "lenses": [rotation[(position - len(schedule["rounds"]) - 1) % len(rotation)]]}
        escalated = False
    return {"position": position, "effort": step["effort"], "lenses": list(step["lenses"]), "escalated": escalated}


def lens_block(plan):
    """The prompt section naming this pass's scheduled lenses and effort, appended after the writer's prompt."""
    where = f"Round {plan['position']}" if plan["position"] else "This pass"
    lines = ["", "", "## Lenses for this pass (scheduled by scripts/review-dispatch.py)", "",
             f"{where} runs at {plan['effort']} reasoning effort"
             + (", escalated after a low-effort round found a P1" if plan["escalated"] else "")
             + ". Review through these lenses only, each as its own verdict, and ignore any other lens list above:",
             ""]
    lines += [f"- `{name}`: {LENSES[name]}" for name in plan["lenses"]]
    return "\n".join(lines) + "\n"


EXTENSION = re.compile(r"\.[A-Za-z0-9]+")


def find_cli(name):
    """A reviewer CLI's path, from the absolute directories on PATH only, or None.

    Reviews run from the reviewed repository's root, and Windows `shutil.which` looks in the working directory first
    (always on Python 3.11, and on later ones unless NoDefaultCurrentDirectoryInExePath is set), so a `codex.cmd`
    committed to that repository would be reported as installed and run as the reviewer. Relative PATH entries are
    dropped for the same reason. On Windows the search is done here, by PATHEXT, so no Python version adds the directory.
    """
    directories = [d for d in os.environ.get("PATH", "").split(os.pathsep) if fully_qualified(d)]
    if os.name != "nt":
        return shutil.which(name, path=os.pathsep.join(directories))
    # An empty PATHEXT gets the defaults too, as shutil.which does. The bare name is the last candidate in each
    # directory: npm puts an extensionless shell script beside codex.cmd, and Windows can run only the .cmd.
    # Only entries shaped like an extension: one that holds a separator or ".." would lead out of the PATH directory.
    extensions = [e for e in os.environ.get("PATHEXT", "").split(os.pathsep) if EXTENSION.fullmatch(e)] \
        or [".COM", ".EXE", ".BAT", ".CMD"]
    for directory in directories:
        for candidate in [os.path.join(directory, name + e) for e in extensions] + [os.path.join(directory, name)]:
            if os.path.isfile(candidate):
                return candidate
    return None


def fully_qualified(directory):
    """True for a PATH entry that names one place whatever the working directory: on Windows a drive and a root
    (`C:\\x`) or a UNC share, never `\\x` (the current drive's) or `C:x` (that drive's current directory), which
    os.path.isabs accepts on Python 3.11 and 3.12."""
    if os.name != "nt":
        return os.path.isabs(directory)
    drive, rest = os.path.splitdrive(directory)
    return drive.startswith(("\\\\", "//")) or (len(drive) == 2 and rest[:1] in ("\\", "/"))


def command(reviewer, executable, output_path, effort=None, prompt_path=None, timeout=None):
    """The argv for one read-only review. Prompt text never appears on the command line."""
    if reviewer["cli"] == "agy":
        # A file pointer, not the prompt: review prose never reaches the command line, where a Windows .cmd shim would
        # re-parse it. The model name carries the effort.
        return [executable, "--model", reviewer["model"], "--mode", "plan", "--sandbox",
                f"--print-timeout={timeout}s" if timeout else "--print-timeout=0s",
                f"-p=Follow the review instructions in the file {prompt_path} exactly. Do not modify any file."]
    if reviewer["cli"] == "muse":
        return [executable, "exec", "--json", "--provider", "meta", "--model", reviewer["model"],
                "--reasoning-effort", effort or reviewer["effort"],
                "--disable-write", "--disable-shell", "--disable-web-tools", "--disable-reminders",
                "--disable-request-user-input", "--approval-mode", "never", "--no-session-log",
                "--no-foreign-personal-context", "--prompt-file", str(prompt_path)]
    return [executable, "exec", "-m", reviewer["model"], "-c", f"model_reasoning_effort={effort or reviewer['effort']}",
            "--sandbox", "read-only", "--ephemeral", "--skip-git-repo-check", "-o", str(output_path), "-"]


CREATE_NEW_PROCESS_GROUP = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0x00000200)
CREATE_SUSPENDED = 0x00000004
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS = 9
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [("ReadOperationCount", ctypes.c_uint64), ("WriteOperationCount", ctypes.c_uint64),
                ("OtherOperationCount", ctypes.c_uint64), ("ReadTransferCount", ctypes.c_uint64),
                ("WriteTransferCount", ctypes.c_uint64), ("OtherTransferCount", ctypes.c_uint64)]


class _JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", ctypes.c_uint32), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", ctypes.c_uint32),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", ctypes.c_uint32),
                ("SchedulingClass", ctypes.c_uint32)]


class _JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", _JOBOBJECT_BASIC_LIMIT_INFORMATION), ("IoInfo", _IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


def _win32_apis():
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    kernel32.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    kernel32.SetInformationJobObject.restype = ctypes.c_int
    kernel32.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.AssignProcessToJobObject.restype = ctypes.c_int
    kernel32.TerminateJobObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.TerminateJobObject.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    ntdll = ctypes.WinDLL("ntdll")
    ntdll.NtResumeProcess.argtypes = [ctypes.c_void_p]
    ntdll.NtResumeProcess.restype = ctypes.c_int32
    return kernel32, ntdll


def create_windows_job():
    """Create a Win32 Job Object configured with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so every descendant is ended."""
    kernel32, _ = _win32_apis()
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    info = _JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not kernel32.SetInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS, ctypes.byref(info),
                                            ctypes.sizeof(info)):
        err = ctypes.get_last_error()
        kernel32.CloseHandle(job)
        raise ctypes.WinError(err)
    return job


def bind_and_resume_windows_job(job, process):
    """Assign a suspended Windows reviewer process to `job` before it executes any instruction, then resume it."""
    kernel32, ntdll = _win32_apis()
    handle = int(process._handle)
    if not kernel32.AssignProcessToJobObject(job, ctypes.c_void_p(handle)):
        raise ctypes.WinError(ctypes.get_last_error())
    status = ntdll.NtResumeProcess(ctypes.c_void_p(handle))
    if status != 0:
        raise OSError(f"NtResumeProcess failed (0x{status & 0xFFFFFFFF:08x})")


def close_windows_job(job):
    """Terminate every process in `job` and close the job handle."""
    if not job:
        return
    kernel32, _ = _win32_apis()
    kernel32.TerminateJobObject(job, 1)
    kernel32.CloseHandle(job)


def kill_tree(process):
    """End a reviewer and everything it started, whether it is still running or has already exited."""
    if os.name == "nt":
        job = getattr(process, "_job_handle", None)
        if job:
            process._job_handle = None
            close_windows_job(job)
        try:
            process.kill()
        except OSError:
            pass
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass  # nothing in the group is left to kill


def remove_scratch(path):
    """Remove a reviewer's scratch directory. On Windows a file stays locked until every process holding it has
    exited, so a failed removal is retried for a bounded time. A directory that still cannot be removed is left behind
    with a note: the round's outcome is already decided."""
    deadline = time.monotonic() + CLEANUP_SECONDS
    while True:
        try:
            shutil.rmtree(path)
            return
        except FileNotFoundError:
            return
        except OSError as exc:
            if time.monotonic() > deadline:
                print(f"review-dispatch: left {path} behind ({exc.strerror or type(exc).__name__})", file=sys.stderr)
                return
            time.sleep(POLL_SECONDS)


def feed(pipe, text):
    try:
        pipe.write(text.encode("utf-8"))
        pipe.close()
    except (BrokenPipeError, OSError):
        pass  # the reviewer stopped reading or exited; its exit code and output decide the round


def run(argv, stdin_text, timeout, watched):
    """Run one reviewer in its own process group (or Win32 Job Object on Windows), watching the size of every file it
    writes while it runs, so neither a hang nor a flood of output can take the dispatcher or the disk down. The group
    or job is ended afterwards whatever happened. Returns (exit code, None) or (None, the reason it returned no review)."""
    group = ({"creationflags": CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED} if os.name == "nt"
             else {"start_new_session": True})
    stdout_path = watched[0]
    with open(stdout_path, "wb") as output:
        job = None
        process = None
        try:
            try:
                if os.name == "nt":
                    job = create_windows_job()
                process = subprocess.Popen(argv, cwd=ROOT, stdin=subprocess.PIPE, stdout=output,
                                           stderr=subprocess.DEVNULL, **group)
                if os.name == "nt":
                    process._job_handle = job
                    job = None
                    bind_and_resume_windows_job(process._job_handle, process)
            except (OSError, ValueError) as exc:
                return None, f"could not start: {getattr(exc, 'strerror', None) or str(exc) or type(exc).__name__}"
            # Fed from a thread: a reviewer that never reads would otherwise block this write past any timeout.
            threading.Thread(target=feed, args=(process.stdin, stdin_text), daemon=True).start()
            deadline = time.monotonic() + timeout
            while process.poll() is None:
                if any(path.is_file() and path.stat().st_size > MAX_OUTPUT_BYTES for path in watched):
                    return None, f"returned more than {MAX_OUTPUT_BYTES} bytes"
                if time.monotonic() > deadline:
                    return None, "timed out"
                time.sleep(POLL_SECONDS)
        finally:
            if job is not None:
                close_windows_job(job)
            if process is not None:
                kill_tree(process)
                process.wait()
                if process.stdin is not None:
                    try:
                        process.stdin.close()
                    except OSError:
                        pass
    if process.returncode != 0:
        return None, f"exited {process.returncode}"
    if any(path.is_file() and path.stat().st_size > MAX_OUTPUT_BYTES for path in watched):
        return None, f"returned more than {MAX_OUTPUT_BYTES} bytes"
    return process.returncode, None


def muse_response(path, model):
    """Extract the completed answer from Muse's JSONL stream, never its progress or tool output."""
    try:
        events = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
        if not events:
            return None, "returned no review"
        if not all(isinstance(event, dict) and isinstance(event.get("payload"), dict) for event in events):
            return None, "returned malformed Muse events"
        models = [event["payload"] for event in events if event.get("payload_type") == "run.model.configured"]
        terminals = [event for event in events if str(event.get("payload_type", "")).startswith("run.terminal.")]
        if len(models) != 1 or models[0].get("model_id") != model or models[0].get("provider_id") != "meta":
            return None, "did not confirm the pinned Muse model and provider"
        if len(terminals) != 1 or terminals[0].get("payload_type") != "run.terminal.completed":
            return None, "did not complete a Muse run"
        final = terminals[0]["payload"]
        if final.get("terminal") != "completed" or not isinstance(final.get("text"), str):
            return None, "returned malformed Muse completion"
        return final["text"], None
    except (OSError, UnicodeDecodeError, ValueError, RecursionError, MemoryError):
        return None, "returned malformed Muse events"


def review(pin, writer, prompt_path, timeout, plan=None, kind="round"):
    """Run the writer's one reviewer. Returns the summary dict: `reviewer` is None when no review came back, and
    `skipped` then names the reviewer and why (there is no fallback). `plan` is the scheduled step (see `scheduled`):
    its effort replaces the reviewer's, and its lenses are appended to the prompt and recorded."""
    if writer not in pin["routes"]:
        raise Refused(f"the reviewer pin has no route for writer {writer!r}")
    prompt = Path(prompt_path).read_text(encoding="utf-8")
    if not prompt.strip():
        raise Refused("the review prompt is empty")
    if "\0" in prompt:
        raise Refused("the review prompt contains a NUL byte; no reviewer can be given it")
    (name,) = pin["routes"][writer]
    reviewer = pin["reviewers"][name]
    effort = plan["effort"] if plan else reviewer.get("effort")
    lenses = plan["lenses"] if plan else []
    if lenses:
        prompt = prompt.rstrip("\n") + lens_block(plan)
    scheduled_fields = {"effort": effort, "lenses": lenses} if plan else {}

    def none(reason):
        return {"writer": writer, "reviewer": None, "skipped": [{"reviewer": name, "reason": reason}], "review": None,
                **scheduled_fields}

    # The resolved path, not the bare name: on Windows a CLI is often a .cmd shim that Popen cannot find by name.
    executable = find_cli(reviewer["cli"])
    if executable is None:
        return none(f"{reviewer['cli']} is not installed")
    text = None
    tmp = Path(tempfile.mkdtemp())
    try:
        stdout_path, message_path, muse_prompt = tmp / "stdout.txt", tmp / "last-message.md", tmp / "prompt.md"
        if reviewer["cli"] != "codex":
            muse_prompt.write_text(prompt, encoding="utf-8")
        _, problem = run(command(reviewer, executable, message_path, effort, muse_prompt, timeout),
                         prompt if reviewer["cli"] == "codex" else "", timeout,
                         [stdout_path, message_path])
        if problem is None:
            if reviewer["cli"] == "muse":
                text, problem = muse_response(stdout_path, reviewer["model"])
            elif reviewer["cli"] == "agy":
                text = stdout_path.read_text(encoding="utf-8", errors="replace") if stdout_path.is_file() else ""
            else:
                # Codex writes its final message to -o; stdout carries progress.
                text = message_path.read_text(encoding="utf-8", errors="replace") if message_path.is_file() else ""
    finally:
        remove_scratch(tmp)
    if problem is None and not (text or "").strip():
        problem = "returned no review"
    if problem is None:
        problem = blocked(text)
    if problem is None and kind in PASSES and not valid_one_pass_footer(text, kind):
        problem = f"returned no {kind} verdict"
    if problem is None and kind == "round":
        lines = [line.strip() for line in text.splitlines() if line.strip()]
        if not lines or lines[-1] not in ("OVERALL: approve", "OVERALL: needs-attention"):
            problem = "returned no round verdict"
    if problem:
        return none(problem)
    return {"writer": writer, "reviewer": name, "cli": reviewer["cli"], "model": reviewer["model"],
            "effort": effort, "skipped": [], "review": text.strip(), **({"lenses": lenses} if plan else {})}


def blocked(text):
    """Why a reply is not a review, when its last line is `OVERALL: blocked` (or `COMPLETION: blocked`); None for a
    review."""
    lines = [line for line in text.strip().splitlines() if line.strip()]
    if not lines or not BLOCKED.match(lines[-1]):
        return None
    reason = next((line.strip() for line in lines[:-1] if not line.lstrip().startswith("#")), "no reason given")
    return f"could not review: {reason[:300]}"


def atomic_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        # newline="": the text is written as given, so a review's own "\r" bytes are stored and nothing is translated.
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


# A findings file is read back by `list_rounds`, so the envelope `record` writes around each review is unambiguous:
# the metadata states how many lines the review has (`- Lines: N`, lines split on "\n" only) and a digest of them
# (`- Digest:`, the first 16 hex digits of the SHA-256 of the canonical lines joined with "\n"). The reader skips
# exactly N lines and checks the digest, so neither a quoted heading nor an edit of the same length passes for a round.
# The review is stored byte for byte; a line's trailing "\r" is dropped only to compare, so a CRLF checkout reads the
# same. Rounds recorded before the count have no Lines field and run to the next round heading; rounds recorded
# before the digest have Lines without Digest and are read by their count alone.
# Numbers are ASCII digits of bounded length, so the reader's int() can never raise (`\d` also takes "²", and a number
# past Python's digit limit raises). A review is at most MAX_OUTPUT_BYTES, so it has at most MAX_OUTPUT_BYTES + 1 lines,
# and the count's width follows from that: whatever `record` writes, the reader reads.
ROUND_HEADING = re.compile(r"^## Round ([0-9]{1,6}): (none|([a-z0-9][a-z0-9-]*) \(`([^`]+)` via `([a-z]+)`\))$")
ROUND_LINE = re.compile(r"^- (Writer|Run|Candidate|Effort|Lenses|Lines|Digest|Skipped): (\S.*)$")
# The one-pass completion review, recorded beside the rounds and never counted as one. It always carries Lines, Digest
# and the Candidate commit it reviewed; the writer's notes after it (its `- Filed:` lines) run to the next `## ` line.
COMPLETION_HEADING = re.compile(r"^## Completion review: (none|([a-z0-9][a-z0-9-]*) \(`([^`]+)` via `([a-z]+)`\))$")
PLAN_HEADING = re.compile(r"^## Plan review: (none|([a-z0-9][a-z0-9-]*) \(`([^`]+)` via `([a-z]+)`\))$")
RECONCILIATION_HEADING = re.compile(r"^## Reconciliation review: (none|([a-z0-9][a-z0-9-]*) \(`([^`]+)` via `([a-z]+)`\))$")
COMPLETION_LINE = re.compile(r"^- (Writer|Run|Candidate|Effort|Lenses|Lines|Digest|Skipped): (\S.*)$")
# The one-pass reviews, by --kind: the heading each is recorded under and the key read_findings lists them under.
PASSES = {"completion": ("## Completion review", COMPLETION_HEADING, "completions"),
          "plan": ("## Plan review", PLAN_HEADING, "plans"),
          "reconciliation": ("## Reconciliation review", RECONCILIATION_HEADING, "reconciliations")}
RESERVED = ("## Round", *(prefix for prefix, _, _ in PASSES.values()))
RESERVED_NAMES = ("headings starting `## Round`, `## Completion review`, `## Plan review` or "
                  "`## Reconciliation review` are reserved for --record")
CANDIDATE = re.compile(r"[0-9a-f]{40}")
SKIPPED = re.compile(r"^([a-z0-9][a-z0-9-]*) \((.*)\)$")
COUNT = re.compile(r"[0-9]{1,%d}" % len(str(MAX_OUTPUT_BYTES + 1)))
DIGEST = re.compile(r"[0-9a-f]{16}")


def raw_lines(text):
    """Lines split on "\n" alone, the one boundary `record` and the reader share, every other byte kept. `str.splitlines`
    also splits on "\r", U+2028, U+0085 and others, which a review may contain."""
    return text.split("\n")


def canonical(line):
    """A line as compared and digested: a CRLF checkout's trailing "\r" dropped."""
    return line[:-1] if line.endswith("\r") else line


def digest(lines):
    return hashlib.sha256("\n".join(canonical(line) for line in lines).encode("utf-8")).hexdigest()[:16]


def _read_findings(text):
    """The rounds and completion reviews `record` and `record_completion` wrote in one findings file, as
    `{"rounds": [...], "completions": [...]}`, or None when anything they write is not as they write it.

    A round is a `## Round` heading, a blank line, and a metadata block to the next blank line in which every line is
    one of `record`'s fields (Writer and Run exactly once, Candidate (a full sha), Effort, Lines and Digest at most once, any number of Skipped;
    Digest only with Lines). With Lines, the next N lines are the review, skipped unread, and a blank line or the end of
    the file must follow them; with Digest, they must also hash to it, so a hand edit inside a review shows as
    unreadable even when it keeps the count. Without Lines (a round recorded before the count), the round runs to the
    next reserved heading. A completion review, and a plan review (`## Plan review`), is read the same way, with
    Writer, Run, Candidate, Lines and Digest required; the lines after it up to the next `## ` line are its `notes`,
    the writer's own record of what was done with each gap. Lenses, when present, is the comma-separated list the
    schedule gave the pass. Anything else makes the file unreadable rather than losing a field."""
    found = {"rounds": [], "completions": [], "plans": [], "reconciliations": []}
    raw = raw_lines(text)
    lines = [canonical(line) for line in raw]
    i = 0
    while i < len(lines):
        kind = next((k for k, (prefix, _, _) in PASSES.items() if lines[i].startswith(prefix)), None)
        completion = kind is not None
        if not lines[i].startswith("## Round") and not completion:
            i += 1
            continue
        head = i
        heading = (PASSES[kind][1] if completion else ROUND_HEADING).match(lines[i])
        if not heading or i + 1 >= len(lines) or lines[i + 1] != "":
            return None, head
        if completion:
            entry = {"reviewer": heading.group(2), "model": heading.group(3), "cli": heading.group(4), "writer": None,
                     "run": None, "candidate": None, "effort": None, "lenses": None, "skipped": []}
        else:
            entry = {"number": int(heading.group(1)), "reviewer": heading.group(3), "model": heading.group(4),
                     "cli": heading.group(5), "writer": None, "run": None, "candidate": None, "effort": None,
                     "lenses": None, "skipped": []}
        count = sealed = None
        i += 2
        while i < len(lines) and lines[i] != "":
            field = (COMPLETION_LINE if completion else ROUND_LINE).match(lines[i])
            if not field:
                return None, head
            key, value = field.group(1).lower(), field.group(2)
            if key == "skipped":
                skipped = SKIPPED.match(value)
                if not skipped:
                    return None, head
                entry["skipped"].append({"reviewer": skipped.group(1), "reason": skipped.group(2)})
            elif key == "lines":
                # ASCII digits only: "²" passes str.isdigit and int() raises on it.
                if count is not None or not COUNT.fullmatch(value):
                    return None, head
                count = int(value)
            elif key == "digest":
                if sealed is not None or not DIGEST.fullmatch(value):
                    return None, head
                sealed = value
            elif entry[key] is not None or (key == "candidate" and not CANDIDATE.fullmatch(value)):
                return None, head
            elif key == "lenses":
                names = value.split(", ")
                if not all(name in LENSES for name in names) or len(set(names)) != len(names):
                    return None, head
                entry[key] = names
            else:
                entry[key] = value
            i += 1
        if entry["writer"] is None or entry["run"] is None or (sealed is not None and count is None):
            return None, head
        if completion and (entry["candidate"] is None or sealed is None):
            return None, head
        if count is not None:
            start = i + 1  # the blank line after the metadata, then the review, unread
            i = start + count
            if i > len(lines) or (i < len(lines) and lines[i] != ""):
                return None, head
            if sealed is not None and digest(raw[start:i]) != sealed:
                return None, head
            entry["review"] = "\n".join(lines[start:i])
        else:
            end = next((j for j in range(i, len(lines)) if lines[j].startswith(RESERVED)), len(lines))
            entry["review"] = "\n".join(lines[i:end])
            i = end
        if completion:
            end = next((j for j in range(i, len(lines)) if lines[j].startswith("## ")), len(lines))
            entry["notes"] = [line for line in lines[i:end] if line.strip()]
            i = end
            found[PASSES[kind][2]].append(entry)
        else:
            found["rounds"].append(entry)
    return found, None


def read_findings(text):
    """See `_read_findings`: the parsed rounds, completion and plan reviews, or None when the file cannot be read."""
    return _read_findings(text)[0]


def unreadable_heading(text):
    """The line where a findings file stops being readable, as `line N: <text>`, so a refusal can name it. A writer's
    own note under a heading that starts `## Round`, `## Completion review` or `## Plan review` is the usual cause:
    those prefixes are reserved for what `--record` writes."""
    found, head = _read_findings(text)
    if found is not None or head is None:
        return ""
    lines = raw_lines(text)
    return f"; the first unreadable record starts at line {head + 1}: {canonical(lines[head])!r}" if head < len(lines) else ""


def read_rounds(text):
    """The rounds `record` wrote in one findings file (completion reviews are not rounds), or None when the file is not
    as `record` and `record_completion` write it (see read_findings)."""
    found = read_findings(text)
    return None if found is None else found["rounds"]


def read_completions(text):
    """The completion reviews `record_completion` wrote in one findings file, or None when it cannot be read back."""
    found = read_findings(text)
    return None if found is None else found["completions"]


def read_plans(text):
    """The plan reviews `--kind plan` recorded in one findings file, or None when it cannot be read back."""
    found = read_findings(text)
    return None if found is None else found["plans"]


class Unrecorded(Exception):
    """The findings file cannot be read back, so a round is not appended to it."""


def existing_findings(path, what):
    """A findings file's text as stored, or `Unrecorded` when it cannot be read."""
    try:
        # Bytes, not read_text: universal newlines would turn a stored lone "\r" into a line break and move the
        # line count of an earlier round.
        return path.read_bytes().decode("utf-8") if path.exists() else ""
    except (OSError, UnicodeDecodeError) as exc:
        raise Unrecorded(f"{path} cannot be read back ({exc}), so the {what} was not recorded") from None


def append_findings(path, existing, lines):
    # Always one blank line before the heading, even when the file already ends in one: a review whose last line is
    # empty also ends the file in "\n\n", and the reader needs a blank line after the review's own lines.
    separator = "" if not existing else ("\n" if existing.endswith("\n") else "\n\n")
    atomic_write(path, existing + separator + "\n".join(lines))


def record(path, result, when, candidate=None):
    """Append one round and return its number. A findings file the reader cannot read back gets nothing appended:
    numbering it would have to guess (counting headings counts quoted ones), so `Unrecorded` is raised instead."""
    existing = existing_findings(path, "round")
    known = read_rounds(existing)
    if known is None:
        raise Unrecorded(f"{path} cannot be read back (a round heading or line is not in the format --record writes"
                         f"{unreadable_heading(existing)}; {RESERVED_NAMES}), so the round was not recorded")
    number = len(known) + 1
    who = f"{result['reviewer']} (`{result['model']}` via `{result['cli']}`)" if result["reviewer"] else "none"
    review = raw_lines(result["review"] or "No reviewer returned a review. This round is not an approval.")
    if not COUNT.fullmatch(str(len(review))) or not ROUND_HEADING.match(f"## Round {number}: none"):
        raise Unrecorded(f"the review or the round number is past what {path} can be read back with, "
                         f"so the round was not recorded")
    lines = [f"## Round {number}: {who}", "", f"- Writer: {result['writer']}", f"- Run: {when}"]
    lines += [f"- Candidate: {candidate}"] if candidate else []
    lines += metadata(result)
    lines += [f"- Lines: {len(review)}", f"- Digest: {digest(review)}", "", *review, ""]
    append_findings(path, existing, lines)
    return number


def metadata(result):
    lines = [f"- Effort: {result['effort']}"] if result.get("effort") else []
    if result.get("lenses"):
        lines.append(f"- Lenses: {', '.join(result['lenses'])}")
    return lines + [f"- Skipped: {f['reviewer']} ({f['reason']})" for f in result["skipped"]]


def descends(older, newer):
    """True when commit `older` is `newer` or an ancestor of it."""
    return subprocess.run(["git", "-C", str(ROOT), "merge-base", "--is-ancestor", older, newer],
                          capture_output=True, timeout=30).returncode == 0


def section_pin(current, record_path):
    """The pin a section's passes run under: the one at its returned plan review's candidate, so a pin change never
    moves a section already under review. A section with no plan review yet, or a candidate whose pin cannot be read
    or no longer loads, takes `current`; the stamp replays the same rule."""
    if not record_path or not Path(record_path).exists():
        return current
    found = read_findings(existing_findings(Path(record_path), "review"))
    plans = [e for e in (found or {"plans": []})["plans"] if returned_one_pass(e, "plan")]
    if len(plans) != 1:
        return current
    shown = subprocess.run(["git", "-C", str(ROOT), "show", f"{plans[0]['candidate']}:.conclave/reviewers.toml"],
                           capture_output=True, timeout=30)
    if shown.returncode != 0:
        return current
    try:
        return load_pin(data=shown.stdout)
    except Refused:
        return current


SECTION_FINDINGS = re.compile(r"(?:^|/)docs/reviews/[0-9]{2}-[A-Za-z0-9_-]+/D[0-9]{2}-T[0-9]{2}-s[1-9][0-9]*\.md$")


def check_three_pass_order(kind, record_path, candidate):
    """Plan, one round, completion, then reconciliations only after a completion with gaps, each after a reconciliation
    with gaps, up to REVIEW_PASS_CAP returned passes in all. Refused here so a pass
    out of order never spends a review; the stamp holds the receipt to the same order. A findings file not named for a
    section (a standalone independent review) has no order."""
    if not record_path or not SECTION_FINDINGS.search(Path(record_path).as_posix()):
        return
    found = read_findings(existing_findings(Path(record_path), f"{kind} review"))
    if found is None:
        raise Unrecorded(f"{record_path} cannot be read back ({RESERVED_NAMES}), so no {kind} review was run")
    plans = [e for e in found["plans"] if returned_one_pass(e, "plan")]
    rounds = [e for e in found["rounds"] if returned_review(e)]
    completions = [e for e in found["completions"] if returned_one_pass(e, "completion")]
    reconciled = [e for e in found["reconciliations"] if returned_one_pass(e, "reconciliation")]
    if kind == "plan":
        if plans or rounds or completions:
            raise Refused("the plan review runs once, before the build and every other pass")
        return
    if len(plans) != 1:
        raise Refused(f"the {kind} pass needs the section's returned plan review first")
    if kind == "round":
        if rounds or completions:
            raise Refused("the three-pass policy allows one adversarial round, before the completion review")
        if not candidate:
            raise Refused("a section's adversarial round needs --candidate")
        if not descends(plans[0]["candidate"], candidate):
            raise Refused(f"the round's candidate does not descend from the plan review's {plans[0]['candidate']}")
    elif kind == "completion":
        if len(rounds) != 1 or completions:
            raise Refused("the completion review runs once, after the section's returned adversarial round")
        if rounds[0]["candidate"] is None or not descends(rounds[0]["candidate"], candidate):
            raise Refused(f"the completion's candidate does not descend from the round's {rounds[0]['candidate']}")
    elif kind == "reconciliation":
        if len(rounds) != 1 or len(completions) != 1 or not completions[0]["review"].rstrip().endswith("COMPLETION: gaps"):
            raise Refused("a reconciliation follows one round and a completion review that returned gaps")
        if reconciled and reconciled[-1]["review"].rstrip().endswith("RECONCILIATION: complete"):
            raise Refused("the section's reconciliation already returned complete; it needs no further pass")
        if len(plans) + len(rounds) + len(completions) + len(reconciled) >= REVIEW_PASS_CAP:
            raise Refused(f"the section has spent its {REVIEW_PASS_CAP} review passes; it stays blocked")
        previous = (reconciled or completions)[-1]
        if not descends(previous["candidate"], candidate):
            raise Refused(f"the reconciliation's candidate does not descend from the last pass's "
                          f"{previous['candidate']}")


def record_completion(path, result, when, candidate, kind="completion"):
    """Append the completion review (or, with kind "plan", the plan review). Each is one pass: a findings file that
    already holds a returned one gets nothing appended (`Unrecorded`), and neither does one the reader cannot read."""
    prefix, _, key = PASSES[kind]
    what = f"{kind} review"
    existing = existing_findings(path, what)
    found = read_findings(existing)
    if found is None:
        raise Unrecorded(f"{path} cannot be read back (a heading or line is not in the format --record writes"
                         f"{unreadable_heading(existing)}; {RESERVED_NAMES}), so the {what} was not recorded")
    # Reconciliations repeat (check_three_pass_order bounds them); the plan and completion passes run once.
    if kind != "reconciliation" and any(returned_one_pass(entry, kind) for entry in found[key]):
        raise Unrecorded(f"{path} already holds a {what}; it is one pass, so this one was not recorded")
    who = f"{result['reviewer']} (`{result['model']}` via `{result['cli']}`)" if result["reviewer"] else "none"
    review = raw_lines(result["review"] or f"No reviewer returned a {what}. This pass is not an approval.")
    if not COUNT.fullmatch(str(len(review))):
        raise Unrecorded(f"the review is past what {path} can be read back with, so it was not recorded")
    lines = [f"{prefix}: {who}", "", f"- Writer: {result['writer']}", f"- Run: {when}", f"- Candidate: {candidate}"]
    lines += metadata(result)
    lines += [f"- Lines: {len(review)}", f"- Digest: {digest(review)}", "", *review, ""]
    append_findings(path, existing, lines)


def list_rounds(records_dir):
    """Every findings file under records_dir that holds rounds, with each round as `record` wrote it. A file
    whose round headings or lines are not `record`'s is named in `unreadable`, never dropped."""
    records_dir = Path(records_dir)
    records, unreadable = [], []

    def shown(path):
        # Never resolve: a path being reported as unreadable may be a symlink loop, which resolve() raises on.
        path = Path(os.path.abspath(path))
        return path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else path.as_posix()

    try:
        exists = records_dir.exists() or records_dir.is_symlink()
        is_dir = records_dir.is_dir() and not records_dir.is_symlink()
    except OSError:
        return {"records": [], "unreadable": [shown(records_dir)]}
    if exists and not is_dir:
        return {"records": [], "unreadable": [shown(records_dir)]}
    paths = []
    if is_dir:
        for dirpath, dirnames, filenames in os.walk(records_dir, onerror=lambda err: unreadable.append(
                shown(getattr(err, "filename", None) or records_dir))):
            dirnames.sort()
            for name in sorted(dirnames) + sorted(filenames):
                if name.endswith(".md"):
                    paths.append(Path(dirpath) / name)
                    if name in dirnames:
                        dirnames.remove(name)
    for path in sorted(paths):
        # One file that cannot be read, or is not UTF-8, is named in `unreadable`; the others are still listed.
        try:
            found = read_findings(path.read_bytes().decode("utf-8"))
        # RuntimeError: a symlink loop on older Pythons. ValueError: anything read_findings meets that int() or the
        # digest cannot take, so one bad file never aborts the listing.
        except (OSError, UnicodeDecodeError, RuntimeError, ValueError):
            found = None
        if found is None:
            unreadable.append(shown(path))
        elif found["rounds"] or found["completions"] or found["plans"] or found["reconciliations"]:
            records.append({"path": shown(path), "rounds": found["rounds"],
                            **({"completions": found["completions"]} if found["completions"] else {}),
                            **({"plans": found["plans"]} if found["plans"] else {}),
                            **({"reconciliations": found["reconciliations"]} if found["reconciliations"] else {})})
    return {"records": records, **({"unreadable": unreadable} if unreadable else {})}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--writer", choices=WRITERS, help="The agent whose work is under review")
    parser.add_argument("--prompt", help="File holding the review prompt")
    parser.add_argument("--record", help="Findings file to append this round (or completion review) to")
    parser.add_argument("--kind", choices=("round", "completion", "plan", "reconciliation"), default="round",
                        help="round (default): one review round; completion: the one-pass completion and excellence "
                             "review, recorded as a ## Completion review section and not counted as a round; plan: "
                             "the one-pass plan review before the build, recorded as a ## Plan review section; "
                             "reconciliation: the one pass after a completion that returned gaps")
    parser.add_argument("--candidate", help="The full 40-character commit under review: required with --kind "
                                            "completion or plan (for a plan review, the commit holding the section "
                                            "as reviewed); optional for a round, recorded as its Candidate line "
                                            "(a section carrying Restarted needs one round after the restart with it)")
    parser.add_argument("--surface", action="store_true",
                        help="The section has Surface: yes, so the schedule's rotation includes the design lens")
    parser.add_argument("--timeout", type=int, default=1800,
                        help="Seconds before the reviewer is stopped and the round records no review")
    parser.add_argument("--pin", default=str(PIN), help=argparse.SUPPRESS)
    parser.add_argument("--check-pin", action="store_true",
                        help="Report whether the pin loads, and each CLI's presence, instead of running a review; "
                             "no reviewer runs (exit 0 loads, 2 refused)")
    parser.add_argument("--list-rounds", action="store_true",
                        help="List the recorded review rounds instead of running one; no reviewer runs")
    parser.add_argument("--json", action="store_true", help="With --list-rounds, print the listing as JSON")
    parser.add_argument("--records", default=str(ROOT / "docs" / "reviews"),
                        help="With --list-rounds, the folder of findings files (default docs/reviews)")
    args = parser.parse_args(argv)
    if args.check_pin:
        # load_pin stays the only place the rules live: this reports its verdict and runs nothing.
        try:
            pin = load_pin(args.pin)
        except Refused as exc:
            print(json.dumps({"ok": False, "error": str(exc)}) if args.json else f"refused: {exc}")
            return 2
        clis = {cli: find_cli(cli) is not None for cli in CLIS}
        print(json.dumps({"ok": True, "schema_version": pin["schema_version"], "reviewers": pin["reviewers"],
                          "routes": pin["routes"], "schedule": pin.get("schedule"), "clis": clis})
              if args.json else "ok")
        return 0
    if args.list_rounds:
        listing = list_rounds(args.records)
        if args.json:
            print(json.dumps(listing, indent=2))
        else:
            for entry in listing["records"]:
                for p in entry.get("plans", []):
                    who = f"{p['reviewer']} ({p['model']} via {p['cli']})" if p["reviewer"] else "no review"
                    print(f"{entry['path']}  Plan review: {who}, candidate {p['candidate']}, {p['run']}")
                for c in entry.get("completions", []):
                    who = f"{c['reviewer']} ({c['model']} via {c['cli']})" if c["reviewer"] else "no review"
                    print(f"{entry['path']}  Completion review: {who}, candidate {c['candidate']}, {c['run']}")
                for c in entry.get("reconciliations", []):
                    who = f"{c['reviewer']} ({c['model']} via {c['cli']})" if c["reviewer"] else "no review"
                    print(f"{entry['path']}  Reconciliation review: {who}, candidate {c['candidate']}, {c['run']}")
                for r in entry["rounds"]:
                    who = f"{r['reviewer']} ({r['model']} via {r['cli']})" if r["reviewer"] else "no review"
                    print(f"{entry['path']}  Round {r['number']}: {who}, writer {r['writer']}, {r['run']}")
            for path in listing.get("unreadable", []):
                print(f"{path}  unreadable: a round heading or line is not in the record format")
        return 1 if listing.get("unreadable") else 0
    if not args.writer or not args.prompt:
        parser.error("--writer and --prompt are required unless --check-pin or --list-rounds is given")
    one_pass = args.kind in PASSES
    if one_pass and not (args.candidate and CANDIDATE.fullmatch(args.candidate)):
        parser.error(f"--kind {args.kind} needs --candidate, the full 40-character commit under review")
    if args.kind == "round" and args.candidate and not CANDIDATE.fullmatch(args.candidate):
        parser.error("--candidate must be the full 40-character commit under review")
    try:
        pin = section_pin(load_pin(args.pin), args.record)
        three_pass = pin.get("schedule", {}).get("policy") == THREE_PASS
        if three_pass:
            check_three_pass_order(args.kind, args.record, args.candidate)
        elif args.kind == "reconciliation":
            raise Refused("this section's pin has no reconciliation pass; only the three-pass policy takes one")
        plan = None
        if "schedule" in pin:
            rounds = []
            if args.record and args.kind == "round":
                # The position in the schedule comes from the rounds already recorded; a file that cannot be read
                # back is refused here, before a review is spent that could not be recorded.
                existing = existing_findings(Path(args.record), "round")
                rounds = read_rounds(existing)
                if rounds is None:
                    raise Unrecorded(f"{args.record} cannot be read back (a round heading or line is not in the "
                                     f"format --record writes{unreadable_heading(existing)}; {RESERVED_NAMES}), so "
                                     f"no round was run")
            plan = scheduled(pin["schedule"], args.kind, rounds, args.surface)
        result = review(pin, args.writer, args.prompt, args.timeout, plan, args.kind)
    except (Refused, Unrecorded, OSError) as exc:
        print("review-dispatch refused: " + str(exc), file=sys.stderr)
        return 2
    when = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    if one_pass or args.candidate:
        result.update(kind=args.kind, candidate=args.candidate)
    if plan and plan["position"]:
        result.update(position=plan["position"], escalated=plan["escalated"])
        # The severity stop: past `severity_stop_after` (0 in the legacy pin, so from round 1 on), a returned round
        # whose worst finding is P3 ends the loop (todo-graph.py SEVERITY_CUTOFF_ROUND is the same number). The
        # three-pass policy has no loop to stop.
        result["severity_stop"] = not three_pass and returned_review(result) \
            and plan["position"] > pin["schedule"]["severity_stop_after"] and highest(result["review"]) == "P3"
    if args.record:
        try:
            if one_pass:
                record_completion(Path(args.record), result, when, args.candidate, args.kind)
                result["recorded"] = True
            else:
                result["round"] = record(Path(args.record), result, when, args.candidate)
        except Unrecorded as exc:
            # The review still reaches stdout; only the findings file is left as it was.
            unrecorded = {"recorded": False} if one_pass else {"round": None}
            print(json.dumps({**result, **unrecorded, "run": when}, indent=2))
            print("review-dispatch refused: " + str(exc), file=sys.stderr)
            return 2
    print(json.dumps({**result, "run": when}, indent=2))
    return 0 if result["reviewer"] else 1


if __name__ == "__main__":
    sys.exit(main())
