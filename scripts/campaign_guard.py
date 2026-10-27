"""The campaign run guard: the Claude Code Stop hook, its lifecycle
commands, and the driven self-test (D00 T04 §32, §34).

`stop-hook` blocks the campaign session's end of turn while its run is
open. It runs in Python so it behaves the same on Windows and WSL,
following the Conclave-SDK todo kit's design (2026-10-21), which itself
ported this repository's former `.claude/hooks/campaign-stop.ps1`. The
guard file it reads is written and deleted only through this module
(D00 T04 §34):

    python3 scripts/campaign_guard.py stop-hook                     (the Stop hook; reads the payload on stdin)
    python3 scripts/campaign_guard.py acquire --session S --phase N --run-file F --cron-id J [--scope plan|phase] [--generation G] [--handover REASON]
    python3 scripts/campaign_guard.py end --session S --reason closeout|park|plan-done|operator-stop|escalation|stall
    python3 scripts/campaign_guard.py hook-error [--session S] [--ack LINE]
    python3 scripts/campaign_guard.py heartbeat-still --session S
    python3 scripts/campaign_guard.py mint-generation
    python3 scripts/campaign_guard.py whoami --session S --generation G
    python3 scripts/campaign_guard.py reset-state --session S | --expect-no-guard yes
    python3 scripts/campaign_guard.py pending-cancel | cancel-confirmed --cron-id J
    python3 scripts/campaign_guard.py reconcile --session S --jobs J1,J2
    python3 scripts/campaign_guard.py health --session S --jobs J1,J2
    python3 scripts/campaign_guard.py expiry --session S [--now ISO]
    python3 scripts/campaign_guard.py repair attempt --red SHA --commit SHA --workflow W --run-file F
    python3 scripts/campaign_guard.py repair close --green SHA --workflow W --run-file F --evidence LINE
    python3 scripts/campaign_guard.py repair status|restore --run-file F [--workflow W] | ceiling --run-id ID
    python3 scripts/campaign_guard.py --self-test

`acquire` creates the guard exclusively; the owning session may re-point
it (a new phase or job) by compare-and-swap on `session_id`, and another
session is refused a live guard unless the operator hands it over. Its
`--scope` says what the run covers: `plan` (process-plan chains every
ready phase) or `phase` (process-phase was asked for one phase), so the
hook and the heartbeat resume only what was asked for.
`end` checks the end path's marker in the run file (a `## Closeout`
heading, a column-0 `PARKED` line, `0 runnable now`, or nothing for an
operator stop), then deletes the guard file and the state file and
names the heartbeat job still to `CronDelete`. `hook-error` prints and
clears an error the hook recorded, for the heartbeat to report.
`heartbeat-still` notes one heartbeat resumption against the run's
progress, so a harness with no Stop hook still trips the stall counter
after repeated resumptions with no tree change. Every
command takes `--root` to act on a fixture workspace instead of this
repository.

The self-test drives the real hook, as a subprocess and through the
command `.agents/hooks.json` wires, with Stop payloads against a
throwaway workspace (a git repository, a guard file, a run file, and a
fake `scripts/todo-graph.py` whose runnable count the test controls) and
asserts every allow and block case, the stall breaker, and the
lifecycle commands.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time

# The guard prints row names (U+00A7) and paths a cp1252 console cannot
# encode, which raises UnicodeEncodeError mid-command. Reconfigure our own
# streams, as scripts/todo-graph.py does, rather than asking every caller
# to set PYTHONIOENCODING.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):  # already wrapped, or not a real stream
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
SETTINGS = os.path.normpath(os.path.join(HERE, "..", ".agents", "hooks.json"))
SESSION = "11111111-2222-3333-4444-555555555555"
RUN_FILE = "docs/phase-runs/2099-01-01-phase-0.md"


REPO = os.path.normpath(os.path.join(HERE, ".."))
END_REASONS = ("closeout", "park", "plan-done", "operator-stop", "escalation", "stall")
SCOPES = ("plan", "phase")
MAX_BLOCKS_WITHOUT_PROGRESS = 3
# Heartbeat resumptions in a row with no tree change that count one stall
# trip on a harness with no Stop hook (owner decision 2026-10-27): at one
# heartbeat per 5 minutes, 6 stills are ~30 minutes of resumptions with
# zero progress, and two trips end the run, so a stuck run terminates
# after ~1 hour instead of resuming forever.
MAX_HEARTBEAT_STILLS = 6
# Claude Code stops a Stop hook at the timeout .claude/settings.json gives
# it (60 seconds) and treats that as a non-blocking error, so the hook
# keeps its own deadline well inside it and still has time to record a
# failure.
HOOK_BUDGET_SECONDS = 40


class GuardError(ValueError):
    """A refusal naming why the guard was not changed."""


def _paths(root: str) -> tuple[str, str]:
    return (os.path.join(root, "build", "claude-campaign-guard.json"),
            os.path.join(root, "build", "claude-campaign-state.json"))


LOCK_WAIT = 10.0
_THREAD_LOCKS: dict[str, threading.Lock] = {}
_THREAD_LOCKS_LOCK = threading.Lock()


class _Lock:
    """An interprocess lock on the guard: every change (acquire,
    re-point, handover, end) holds it, so a read-then-replace can never
    interleave with another session's change (D00 T04 §34 independent
    review). It is an operating-system lock on an open handle
    (`msvcrt.locking` on Windows, `lockf` elsewhere): it never expires
    while its holder lives, and the OS releases it when the holder
    closes the handle or dies, so no age test can break a live holder's
    lock (panel round 1). The lock file itself is never deleted."""

    def __init__(self, root: str, wait: float = LOCK_WAIT):
        self.path = os.path.join(root, "build", "claude-campaign-guard.lock")
        self.wait = wait
        self.fd = None
        with _THREAD_LOCKS_LOCK:
            self.thread_lock = _THREAD_LOCKS.setdefault(os.path.realpath(self.path), threading.Lock())

    def _try(self) -> bool:
        if os.name == "nt":
            import msvcrt
            try:
                os.lseek(self.fd, 0, os.SEEK_SET)
                msvcrt.locking(self.fd, msvcrt.LK_NBLCK, 1)
                return True
            except OSError:
                return False
        import fcntl
        try:
            fcntl.lockf(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError:
            return False

    def __enter__(self):
        import time
        if not self.thread_lock.acquire(timeout=self.wait):
            raise GuardError(f"the guard is locked by another thread ({self.path}); retry")
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o644)
            deadline = time.monotonic() + self.wait
            while not self._try():
                if time.monotonic() >= deadline:
                    raise GuardError(f"the guard is locked by another change ({self.path}); retry")
                time.sleep(0.05)
            return self
        except BaseException:
            if self.fd is not None:
                try:
                    os.close(self.fd)
                except OSError:
                    pass
                self.fd = None
            self.thread_lock.release()
            raise

    def __exit__(self, *exc):
        if self.fd is not None:
            try:
                if os.name == "nt":
                    import msvcrt
                    os.lseek(self.fd, 0, os.SEEK_SET)
                    msvcrt.locking(self.fd, msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.lockf(self.fd, fcntl.LOCK_UN)
            finally:
                os.close(self.fd)
                self.fd = None
                self.thread_lock.release()
        return False


def _read_regular(path: str, limit: int | None = None) -> tuple[os.stat_result, bytes | None]:
    """A file's status and, when it is a regular file no larger than
    `limit`, its bytes. It opens without blocking (a FIFO opened for
    reading with O_NONBLOCK returns at once) and checks the open handle,
    so the check and the read are of the same file and nothing swapped in
    between can block the hook (independent review round 2)."""
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_BINARY", 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or (limit is not None and info.st_size > limit):
            return info, None
        with os.fdopen(fd, "rb", closefd=False) as fh:
            return info, fh.read()
    finally:
        os.close(fd)


def _read_json(path: str) -> object:
    """A JSON file through `_read_regular`, so a FIFO in its place never
    blocks a reader; FileNotFoundError passes through."""
    _, data = _read_regular(path)
    if data is None:
        raise GuardError(f"{path} is not a regular file")
    return json.loads(data.decode("utf-8-sig"))


def read_guard(root: str) -> dict | None:
    guard, _ = _paths(root)
    try:
        doc = _read_json(guard)
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as exc:
        raise GuardError(f"the guard file {guard} does not parse: {exc}")
    if not isinstance(doc, dict):
        raise GuardError(f"the guard file {guard} is not an object")
    return doc


def mint_generation() -> str:
    """A fresh acquisition generation: the heartbeat prompt carries it, so
    a replaced job in the same session knows it is obsolete (D00 T04 §36)."""
    import uuid
    return uuid.uuid4().hex[:12]


def _utc_now() -> str:
    import datetime
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _run_length(root: str, run_file: str) -> int:
    try:
        return os.path.getsize(os.path.join(root, run_file))
    except OSError:
        return 0


def acquire(root: str, session: str, phase: int, run_file: str, cron_id: str,
            handover: str | None = None, generation: str | None = None, scope: str | None = None) -> str:
    """Write the guard for `session`. A new guard is created exclusively
    (O_EXCL), so two sessions racing to start a run cannot both win; the
    owner re-points its own guard by compare-and-swap on `session_id`; a
    live guard owned by another session is refused unless the operator
    hands it over, and the handover is recorded in the guard. A re-point
    or a handover of the same run keeps its `scope` unless one is given;
    a new run defaults to `plan`."""
    if not session or not run_file or not cron_id:
        raise GuardError("acquire needs --session, --run-file, and --cron-id")
    if os.path.isabs(run_file) or ".." in run_file.replace("\\", "/").split("/"):
        raise GuardError(f"run file {run_file!r} must be a repo-relative path inside the workspace")
    if scope is not None and scope not in SCOPES:
        raise GuardError(f"scope {scope!r} is not one of {', '.join(SCOPES)}")
    with _Lock(root):
        return _acquire_locked(root, session, phase, run_file, cron_id, handover, generation, scope)


def _acquire_locked(root: str, session: str, phase: int, run_file: str, cron_id: str,
                    handover: str | None, generation: str | None = None, scope: str | None = None) -> str:
    guard, state_path = _paths(root)
    os.makedirs(os.path.dirname(guard), exist_ok=True)
    try:
        previous = read_guard(root) or {}
    except GuardError:
        previous = {}
    same_run = previous.get("run_file") == run_file and previous.get("session_id") == session
    doc = {"runner": "claude", "workspace": root.replace("\\", "/"), "phase": phase,
           "run_file": run_file, "session_id": session, "cron_id": cron_id,
           "scope": scope or (previous.get("scope") if previous.get("session_id") == session
                              or (handover and previous.get("run_file") == run_file) else None) or "plan",
           # D00 T04 §36: the generation the heartbeat prompt carries, when
           # its job was created (the 7-day expiry counts from it), and the
           # run file's length at acquisition, so a reused run file's old
           # markers never end this run.
           "generation": generation or (previous.get("generation") if same_run and
                                        previous.get("cron_id") == cron_id else None) or mint_generation(),
           "job_created_at": (previous.get("job_created_at") if previous.get("cron_id") == cron_id
                              and previous.get("job_created_at") else _utc_now()),
           # A stable run identity every terminal marker must carry, so a
           # reused run file's old markers never end this run however the
           # file is edited (D00 T04 §36 independent review: a byte offset
           # breaks when a park record is inserted mid-file).
           "run_id": (previous.get("run_id") if (same_run or (handover and previous.get("run_file") == run_file))
                      and previous.get("run_id") else mint_generation())}
    data = json.dumps(doc, indent=2).encode("utf-8")
    try:
        fd = os.open(guard, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    except FileExistsError:
        current = read_guard(root) or {}
        owner = str(current.get("session_id", ""))
        if owner != session:
            if not handover:
                raise GuardError(f"the guard is live for session {owner or '<none>'}; "
                                 f"refusing to take it without an operator handover (--handover REASON)")
            doc["handover_from"] = owner
            doc["handover_reason"] = handover
        # The lock is held from the read above through this replace, so no
        # other change can land in between.
        tmp = guard + f".{os.getpid()}.tmp"
        with open(tmp, "wb") as fh:
            fh.write(json.dumps(doc, indent=2).encode("utf-8"))
        os.replace(tmp, guard)
        what = "handed over" if owner != session else "re-pointed"
        if owner != session or not same_run:
            try:
                os.unlink(state_path)
            except FileNotFoundError:
                pass
        return (f"acquire: guard {what} for session {session} (phase {phase}, scope {doc['scope']}, {run_file}, job {cron_id}, "
                f"generation {doc['generation']}, run {doc['run_id']})")
    with os.fdopen(fd, "wb") as fh:
        fh.write(data)
    # A fresh guard starts a fresh breaker, under the same lock (D00 T04 §36).
    try:
        os.unlink(state_path)
    except FileNotFoundError:
        pass
    return (f"acquire: guard created for session {session} (phase {phase}, scope {doc['scope']}, {run_file}, job {cron_id}, "
            f"generation {doc['generation']}, run {doc['run_id']})")


def ready_rows(root: str, timeout: float = 300.0, phase: int | None = None) -> tuple[int | None, str]:
    """How many rows `query ready` offers and the first of them, or
    (None, "") when the query cannot be read. The `N runnable now`
    summary does not say which phase a row is in, so for a phase-scoped
    run (`phase`) the count is unknown: that run ends on its own closeout
    or park marker, never on another phase's count, and is never pointed
    at another phase's row (Conclave-SDK todo kit)."""
    try:
        # Ask the child for the UTF-8 this decodes: a console code page
        # (cp1252 on Windows) would garble every section mark it prints.
        proc = subprocess.run([sys.executable, os.path.join(root, "scripts", "todo-graph.py"), "query", "ready"],
                              capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=root,
                              env=dict(os.environ, PYTHONIOENCODING="utf-8"), timeout=max(timeout, 1.0))
    except (OSError, subprocess.TimeoutExpired):
        return None, ""
    counts = re.findall(r"(\d+) runnable now", proc.stdout)
    if not counts or phase is not None:
        return None, ""
    lines = [ln.strip() for ln in proc.stdout.splitlines() if ln.strip() and "runnable now" not in ln]
    return int(counts[-1]), (lines[0] if lines else "")


def end(root: str, session: str, reason: str) -> str:
    """End the run on one of its end paths: check the path's marker, then
    delete the guard file and the state file. The heartbeat job lives in
    the session, not on disk, so the caller deletes it (`CronDelete`) and
    the returned line names it."""
    if reason not in END_REASONS:
        raise GuardError(f"reason {reason!r} is not one of {', '.join(END_REASONS)}")
    if not session:
        raise GuardError("end needs --session (the session that owns the guard)")
    # The plan query runs before the lock, so no subprocess is ever held
    # under it (panel round 1).
    ready = ready_rows(root)[0] if reason == "plan-done" else None
    with _Lock(root):
        return _end_locked(root, session, reason, ready)


def _end_locked(root: str, session: str, reason: str, ready: int | None) -> str:
    guard_path, state_path = _paths(root)
    guard = read_guard(root)
    if guard is None:
        raise GuardError("no guard file: the run is already over")
    if str(guard.get("session_id", "")) != session:
        raise GuardError(f"the guard belongs to session {guard.get('session_id')}, not {session}")
    run_rel = str(guard.get("run_file", ""))
    text = _run_markers_text(root, run_rel, guard)
    rid = guard.get("run_id")
    tag = f" run={rid}" if rid else ""
    if reason == "closeout" and not re.search(r"(?m)^## Closeout\b", text):
        raise GuardError(f"closeout needs a '## Closeout{tag}' heading in {run_rel}")
    if reason == "park" and not re.search(r"(?m)^PARKED\b", text):
        raise GuardError(f"park needs a column-0 'PARKED <UTC>{tag} <reason>' line in {run_rel}")
    if reason == "escalation" and not re.search(r"(?m)^PARKED\b.*\bescalation:", text):
        raise GuardError(f"escalation needs a column-0 'PARKED <UTC>{tag} escalation: <cause>' line in {run_rel}")
    if reason == "plan-done" and guard.get("scope") == "phase":
        raise GuardError("a phase-scoped run covers one phase, not the plan: end it with closeout or park")
    if reason == "plan-done" and ready != 0:
        raise GuardError(f"plan-done needs '0 runnable now'; query ready reads {ready}")
    if reason == "stall":
        try:
            with open(state_path, encoding="utf-8-sig") as fh:
                trips = int(json.load(fh).get("trips", 0))
        except (OSError, ValueError, TypeError, AttributeError):
            trips = 0
        if trips < 2:
            raise GuardError(f"stall needs a state file with trips of 2 or more; it reads {trips}")
    # The job id outlives the guard until CronDelete is confirmed: a failed
    # delete stays recoverable (D00 T04 §36).
    pending = _pending_path(root)
    jobs = _pending_jobs(root)
    # Every unconfirmed job is kept: a later end never overwrites an
    # earlier failed cancellation (D00 T04 §36 panel round 1).
    jobs = [j for j in jobs if j.get("cron_id") != guard.get("cron_id")]
    jobs.append({"cron_id": guard.get("cron_id"), "reason": reason, "at": _utc_now()})
    with open(pending + ".tmp", "w", encoding="utf-8") as fh:
        json.dump(jobs, fh)
    os.replace(pending + ".tmp", pending)
    for path in (guard_path, state_path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    left = [p for p in (guard_path, state_path) if os.path.exists(p)]
    if left:
        raise GuardError(f"could not delete {', '.join(left)}")
    return (f"end: {reason}: guard and state deleted; CronDelete {guard.get('cron_id')} now "
            f"(the heartbeat job), confirm it is gone with CronList, then run "
            f"cancel-confirmed --cron-id {guard.get('cron_id')}")


def marker_run(line: str) -> str | None:
    m = re.search(r"\brun=([0-9a-f]{6,})\b", line)
    return m.group(1) if m else None


def _run_markers_text(root: str, run_rel: str, guard: dict) -> str:
    """The run file's lines that count as this run's markers: every line
    when the guard predates run ids, else only `## Closeout` and `PARKED`
    lines carrying `run=<this run's id>`, wherever they sit in the file
    (D00 T04 §36)."""
    try:
        # utf-8-sig, as the hook reads it: a byte-order mark never hides a
        # first-line marker from one and not the other.
        with open(os.path.join(root, run_rel), encoding="utf-8-sig", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return ""
    run_id = guard.get("run_id")
    if not run_id:
        return text
    keep = [ln for ln in text.splitlines()
            if not re.match(r"\A(## Closeout\b|PARKED\b)", ln) or marker_run(ln) == run_id]
    return "\n".join(keep)


def _pending_path(root: str) -> str:
    return os.path.join(root, "build", "claude-campaign-pending-cancel.json")


def _pending_jobs(root: str) -> list[dict]:
    try:
        with open(_pending_path(root), encoding="utf-8") as fh:
            doc = json.load(fh)
    except FileNotFoundError:
        return []
    except (OSError, ValueError):
        raise GuardError("the pending-cancellation record is unreadable: list the jobs and delete any "
                         "RasterFold heartbeat by hand")
    if isinstance(doc, dict):
        doc = [doc]
    return [j for j in doc if isinstance(j, dict)] if isinstance(doc, list) else []


def pending_cancel(root: str) -> str:
    try:
        jobs = _pending_jobs(root)
    except GuardError as exc:
        return f"pending-cancel: {exc}"
    return "\n".join(f"pending-cancel: CronDelete {j.get('cron_id')} (ended by {j.get('reason')} at {j.get('at')})"
                     for j in jobs)


def cancel_confirmed(root: str, cron_id: str) -> str:
    with _Lock(root):
        jobs = _pending_jobs(root)
        if not jobs:
            return "cancel-confirmed: nothing pending"
        if cron_id not in [str(j.get("cron_id")) for j in jobs]:
            raise GuardError(f"no pending cancellation names job {cron_id} "
                             f"(pending: {', '.join(str(j.get('cron_id')) for j in jobs)})")
        rest = [j for j in jobs if str(j.get("cron_id")) != cron_id]
        if rest:
            with open(_pending_path(root) + ".tmp", "w", encoding="utf-8") as fh:
                json.dump(rest, fh)
            os.replace(_pending_path(root) + ".tmp", _pending_path(root))
            return f"cancel-confirmed: job {cron_id} is gone; still pending: {', '.join(str(j.get('cron_id')) for j in rest)}"
        os.unlink(_pending_path(root))
        return f"cancel-confirmed: job {cron_id} is gone; nothing pending"


def whoami(root: str, session: str, generation: str | None = None) -> str:
    """What this heartbeat is to the run: OWNER, NOT THE OWNER, NOT THE
    CURRENT JOB (a replaced job in the owning session), NO GUARD, or
    MALFORMED GUARD (D00 T04 §36)."""
    with _Lock(root):
        try:
            guard = read_guard(root)
        except GuardError:
            return "MALFORMED GUARD"
        if guard is None:
            return "NO GUARD"
        if str(guard.get("session_id", "")) != session:
            return "NOT THE OWNER"
        if generation and str(guard.get("generation", "")) != generation:
            return "NOT THE CURRENT JOB"
        # The run id rides the answer, so the heartbeat reads which markers
        # are this run's without carrying the id in its prompt.
        scope = guard.get("scope") or "plan"
        return f"OWNER run={guard.get('run_id')} scope={scope}" if guard.get("run_id") else f"OWNER scope={scope}"


def reset_state(root: str, session: str | None = None, expect_no_guard: bool = False) -> str:
    """Delete the breaker state under the lock, re-checking ownership or
    absence first: never a direct delete (D00 T04 §36)."""
    _, state_path = _paths(root)
    with _Lock(root):
        try:
            guard = read_guard(root)
        except GuardError:
            raise GuardError("the guard is unreadable; refusing to reset its state")
        if expect_no_guard and guard is not None:
            raise GuardError("a guard exists: a new run started; its state is not this heartbeat's to delete")
        if not expect_no_guard and (guard is None or str(guard.get("session_id", "")) != session):
            raise GuardError("reset-state needs the owning --session of a live guard")
        try:
            os.unlink(state_path)
            return "reset-state: state deleted"
        except FileNotFoundError:
            return "reset-state: no state"


def reconcile(root: str, session: str, jobs: list[str]) -> list[str]:
    """Startup reconciliation between the guard and the live heartbeat jobs
    (D00 T04 §36): what to delete or recreate before a run starts."""
    with _Lock(root):
        try:
            guard = read_guard(root)
        except GuardError:
            return ["reconcile: the guard is unreadable: report it to the operator and start nothing"]
    out: list[str] = []
    try:
        out += [f"reconcile: a pending cancellation is unconfirmed: CronDelete {j.get('cron_id')}, then cancel-confirmed"
                for j in _pending_jobs(root)]
    except GuardError as exc:
        out.append(f"reconcile: {exc}")
    if guard is None:
        out += [f"reconcile: orphan job {j} has no guard: CronDelete it" for j in jobs]
        return out or ["reconcile: consistent (no guard, no job)"]
    owner, job = str(guard.get("session_id", "")), str(guard.get("cron_id", ""))
    if owner != session:
        return [f"reconcile: the guard belongs to session {owner}: start nothing and report the owner"]
    if job not in jobs:
        out.append(f"reconcile: the guard names job {job}, which is not live: CronCreate a heartbeat "
                   f"and re-point with acquire --cron-id")
    out += [f"reconcile: stale job {j} is not the guard's: CronDelete it" for j in jobs if j != job]
    return out or [f"reconcile: consistent (guard and job {job})"]


def health(root: str, session: str, jobs: list[str]) -> tuple[int, str]:
    with _Lock(root):
        try:
            guard = read_guard(root)
        except GuardError:
            return 1, "health: the guard is unreadable"
    if guard is None or str(guard.get("session_id", "")) != session:
        return 1, "health: this session holds no live guard"
    job = str(guard.get("cron_id", ""))
    if job not in jobs:
        return 1, f"health: job {job} is missing: CronCreate a heartbeat and re-point with acquire --cron-id"
    return 0, f"health: ok, job {job} is live"


def expiry(root: str, session: str, now: str | None = None, days: float = 7.0,
           margin_hours: float = 12.0) -> tuple[int, str]:
    """Whether the heartbeat job nears its 7-day expiry (D00 T04 §36):
    exit 1 `replace due` inside the margin, else how long is left."""
    import datetime
    with _Lock(root):
        try:
            guard = read_guard(root)
        except GuardError:
            return 1, "expiry: the guard is unreadable"
    if guard is None or str(guard.get("session_id", "")) != session:
        return 1, "expiry: this session holds no live guard"
    fmt = "%Y-%m-%dT%H:%M:%SZ"
    try:
        created = datetime.datetime.strptime(str(guard.get("job_created_at")), fmt)
    except ValueError:
        return 1, "expiry: the job's creation time is unknown: replace the job"
    current = datetime.datetime.strptime(now, fmt) if now else datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0, tzinfo=None)
    left = datetime.timedelta(days=days) - (current - created)
    if left <= datetime.timedelta(hours=margin_hours):
        return 1, (f"expiry: replace due, job {guard.get('cron_id')} created {guard.get('job_created_at')} "
                   f"expires in {max(left.total_seconds(), 0) / 3600:.1f}h: CronCreate a replacement and re-point")
    return 0, f"expiry: ok, job {guard.get('cron_id')} expires in {left.total_seconds() / 3600:.1f}h"


def hook_error(root: str, session: str | None = None, ack: str | None = None) -> str:
    """Print and clear the error the hook recorded, if any. With a
    session, a guard owned by another session is refused before anything
    is read or cleared (panel round 1: a former owner's heartbeat must
    not consume the current owner's failure record)."""
    with _Lock(root):
        return _hook_error_locked(root, session, ack)


def _hook_error_log(root: str, ack: str | None) -> str:
    """The oldest error the hook recorded without the guard lock (D00 T04
    §36 independent review): one file per error under
    `build/claude-campaign-hook-errors/`, so a writer never races this
    drain (panel round 1); printed like a state error, deleted only on its
    exact --ack."""
    folder = os.path.join(root, "build", "claude-campaign-hook-errors")
    try:
        names = sorted(n for n in os.listdir(folder) if n.endswith(".txt"))
    except OSError:
        return ""
    if not names:
        return ""
    oldest = os.path.join(folder, names[0])
    try:
        with open(oldest, encoding="utf-8", errors="replace") as fh:
            at, _, reason = fh.read().strip().partition(" ")
    except OSError:
        return ""
    line = f"campaign-stop hook failed at {at}: {reason} (recorded without the lock)"
    if ack is None:
        return line
    if ack != line:
        raise GuardError("the acknowledgement does not match the recorded error; nothing cleared")
    os.unlink(oldest)
    return f"hook-error: acknowledged and cleared: {line}"


def _hook_error_locked(root: str, session: str | None, ack: str | None = None) -> str:
    # Under the lock, so no handover lands between the ownership check
    # and the clear (panel round 2). An unreadable guard cannot name an
    # owner, and the error it caused is exactly what must be reported,
    # so it reports rather than refuses (panel round 2).
    unreadable = False
    if session:
        try:
            guard = read_guard(root)
        except GuardError:
            guard, unreadable = None, True
        if guard is not None and str(guard.get("session_id", "")) != session:
            raise GuardError(f"the guard belongs to session {guard.get('session_id')}, not {session}")
    _, state_path = _paths(root)
    try:
        with open(state_path, encoding="utf-8-sig") as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        return _hook_error_log(root, ack)
    err = state.get("hook_error")
    at = state.get("hook_error_at")
    if not err:
        return _hook_error_log(root, ack)
    note = " (the guard file is unreadable: repair it before resuming)" if unreadable else ""
    line = f"campaign-stop hook failed at {at}: {err}{note}"
    if ack is None:
        # Print only: the record clears after the run file holds it and
        # the heartbeat acknowledges that exact line (D00 T04 §36).
        return line
    if ack != line:
        raise GuardError("the acknowledgement does not match the recorded error; nothing cleared")
    state.pop("hook_error", None)
    state.pop("hook_error_at", None)
    tmp = state_path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(state, fh)
    os.replace(tmp, state_path)
    return f"hook-error: acknowledged and cleared: {line}"


# ---- the Stop hook ---------------------------------------------------------

def _env_int(name: str, default: int) -> int:
    value = os.environ.get(name, "")
    return int(value) if value.isdigit() else default


def _remaining(deadline: float) -> float:
    left = deadline - time.monotonic()
    if left <= 0:
        raise GuardError("the hook ran out of its time budget before deciding")
    return left


def _hook_git(root: str, deadline: float, *args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=root, capture_output=True,
                          timeout=max(deadline - time.monotonic(), 1.0)).stdout


def _write_state(path: str, state: dict) -> None:
    tmp = f"{path}.{os.getpid()}.tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(state, fh)
    os.replace(tmp, path)


# The guard's own files: a checkout that does not ignore build/ would
# otherwise see every state write as progress.
OWN_FILES = re.compile(r"^build/claude-campaign-(guard|state|pending-cancel)\.json(\.[0-9]+\.tmp)?$|"
                       r"^build/claude-campaign-guard\.lock$|^build/claude-campaign-hook-errors/")


def fingerprint(root: str, run_rel: str, run_text: str, deadline: float) -> str:
    """Progress: HEAD, the working-tree diff, the untracked files, and the
    run file without its `- bookkeeping:` lines. The run file leaves the
    diff and the untracked set and is hashed on its own, so a heartbeat or
    retry note in its Critical events never passes for progress, while
    any other line still counts (D00 T04 §34, §36). Untracked content is
    hashed for the first 500 files up to 4 MB each, the next 5000 count by
    size and write time, and any past that by name alone, so a huge
    scratch tree cannot stall the hook and no untracked path drops out;
    the bounds read from the environment so fixtures can pin them."""
    max_hashed = _env_int("CAMPAIGN_HASH_FILES", 500)
    max_bytes = _env_int("CAMPAIGN_HASH_BYTES", 4 * 1024 * 1024)
    max_stat = _env_int("CAMPAIGN_STAT_FILES", 5000)
    run_path = run_rel.replace("\\", "/")
    digest = hashlib.sha256()
    digest.update(_hook_git(root, deadline, "rev-parse", "HEAD").strip() + b"\n")
    digest.update(_hook_git(root, deadline, "diff", "HEAD", "--", ".", f":(exclude){run_path}",
                            ":(exclude)build/claude-campaign-*") + b"\n")
    # NUL-delimited, so git never quotes or escapes a name.
    listed = _hook_git(root, deadline, "ls-files", "-z", "--others", "--exclude-standard").decode("utf-8", "replace")
    paths = [p for p in listed.split("\0") if p and p != run_path and not OWN_FILES.match(p)]
    for index, path in enumerate(paths, 1):
        _remaining(deadline)
        if index > max_hashed + max_stat:
            digest.update(f"{path}\n".encode("utf-8"))
            continue
        full = os.path.join(root, path)
        try:
            # Only a regular file's content is read. git never lists a FIFO,
            # but a listed path can become one before the read.
            info, data = _read_regular(full, max_bytes) if index <= max_hashed else (os.stat(full), None)
        except OSError:
            digest.update(f"{path} missing\n".encode("utf-8"))
            continue
        if data is not None:
            digest.update(f"{path} ".encode("utf-8") + hashlib.sha256(data).hexdigest().encode() + b"\n")
        else:
            digest.update(f"{path} {info.st_size} {info.st_mtime_ns}\n".encode("utf-8"))
    digest.update(re.sub(r"(?m)^- bookkeeping:.*(\r?\n)?", "", run_text).encode("utf-8"))
    return digest.hexdigest()


def _same_run(still: dict | None, guard: dict) -> bool:
    """Whether the guard re-read under the lock is still the run the hook
    started deciding for (D00 T04 §36 panel round 1), scope and phase
    included (independent review round 2). A guard without a
    run id predates the rule and compares by session and run file."""
    if still is None or str(still.get("session_id", "")) != str(guard.get("session_id", "")):
        return False
    if str(still.get("run_file", "")) != str(guard.get("run_file", "")):
        return False
    # The scope and the phase decide the block message and the next row.
    if (still.get("scope") or "plan") != (guard.get("scope") or "plan") or still.get("phase") != guard.get("phase"):
        return False
    return "run_id" not in guard or str(still.get("run_id", "")) == str(guard.get("run_id", ""))


def _guard_lock(root: str, wait_ms: int) -> _Lock:
    lock = _Lock(root, wait=wait_ms / 1000)
    try:
        lock.__enter__()
    except GuardError:
        raise GuardError(f"the guard lock stayed held for {wait_ms}ms") from None
    return lock


def heartbeat_still(root: str, session: str) -> str:
    """One heartbeat's progress accounting, for runs on a harness with no
    Stop hook (owner decision 2026-10-27: Muse runs rest on the guard file
    plus the heartbeat, so no hook ever counts their still turns). It
    hashes the same progress fingerprint the hook does: a changed tree
    resets the still count and the stall trips, and
    MAX_HEARTBEAT_STILLS resumptions in a row with no change count one
    trip, so the heartbeat's trips-of-2 stall check and `end --reason
    stall` terminate a stuck run on every harness. Hook runs may call it
    too: progress is progress, whoever observes it."""
    try:
        guard = read_guard(root)
    except GuardError:
        raise GuardError("the guard is unreadable; refusing to note a heartbeat still")
    if guard is None or str(guard.get("session_id", "")) != session:
        raise GuardError("heartbeat-still needs the owning --session of a live guard")
    run_rel = str(guard.get("run_file", ""))
    if not run_rel or os.path.isabs(run_rel):
        raise GuardError(f"run file {run_rel!r} is not a repo-relative path")
    full_root = os.path.normcase(os.path.abspath(root)).rstrip(os.sep)
    full_run = os.path.normcase(os.path.abspath(os.path.join(root, run_rel)))
    if not full_run.startswith(full_root + os.sep):
        raise GuardError(f"run file {run_rel!r} escapes the workspace")
    try:
        _, data = _read_regular(os.path.join(root, run_rel))
    except FileNotFoundError:
        raise GuardError(f"the run file {run_rel} is missing")
    if data is None:
        raise GuardError(f"the run file {run_rel} is not a regular file")
    run_text = data.decode("utf-8-sig", "replace")
    deadline = time.monotonic() + _env_int("CAMPAIGN_HEARTBEAT_BUDGET_SECONDS", 120)
    print_ = fingerprint(root, run_rel, run_text, deadline)
    _, state_path = _paths(root)
    with _Lock(root):
        try:
            still = read_guard(root)
        except GuardError:
            still = None
        if not _same_run(still, guard):
            return "heartbeat-still: the guard changed while noting; nothing written"
        try:
            old = _read_json(state_path)
        except FileNotFoundError:
            old = {}
        if not isinstance(old, dict):
            raise GuardError(f"the state file {state_path} is not an object")
        state = {"fingerprint": str(old.get("fingerprint", "")), "blocks": int(old.get("blocks", 0) or 0),
                 "trips": int(old.get("trips", 0) or 0), "stalled": bool(old.get("stalled", False)),
                 "hb_fingerprint": str(old.get("hb_fingerprint", "")),
                 "hb_still": int(old.get("hb_still", 0) or 0)}
        state.update({k: old[k] for k in ("hook_error", "hook_error_at") if old.get(k)})
        if state["hb_fingerprint"] != print_:
            state.update(hb_fingerprint=print_, hb_still=0, trips=0)
            _write_state(state_path, state)
            return "heartbeat-still: progress (still count and trips reset)"
        state["hb_still"] += 1
        if state["hb_still"] >= MAX_HEARTBEAT_STILLS:
            state.update(hb_still=0, trips=state["trips"] + 1, stalled=True, stalled_at=_utc_now())
            _write_state(state_path, state)
            return (f"heartbeat-still: trip {state['trips']} after {MAX_HEARTBEAT_STILLS} "
                    f"resumptions with no tree change")
        _write_state(state_path, state)
        return f"heartbeat-still: still {state['hb_still']} of {MAX_HEARTBEAT_STILLS}"


def stop_decision(root: str, session: str) -> dict | None:
    """The hook's decision for one Stop event: None allows the stop, a
    dict is the `block` payload. Only the session named in the guard is
    ever blocked; a second session or a subagent ends its turns."""
    guard = read_guard(root)
    if guard is None or not session or str(guard.get("session_id", "")) != session:
        return None
    deadline = time.monotonic() + _env_int("CAMPAIGN_HOOK_BUDGET_SECONDS", HOOK_BUDGET_SECONDS)
    run_rel = str(guard.get("run_file", ""))
    if not run_rel or os.path.isabs(run_rel):
        return None
    full_root = os.path.normcase(os.path.abspath(root)).rstrip(os.sep)
    full_run = os.path.normcase(os.path.abspath(os.path.join(root, run_rel)))
    if not full_run.startswith(full_root + os.sep):
        return None
    try:
        _, data = _read_regular(os.path.join(root, run_rel))
    except FileNotFoundError:
        return None
    # A FIFO or a device would block the read past the budget.
    if data is None:
        raise GuardError(f"the run file {run_rel} is not a regular file")
    # utf-8-sig: a byte-order mark never hides a first-line marker.
    run_text = data.decode("utf-8-sig", "replace")
    # A marker ends the run only when it carries this run's id, wherever it
    # sits in the file, so a reused run file's old closeout or PARKED line
    # never ends a new run (D00 T04 §36); a guard with no run id predates
    # the rule, and any marker counts.
    run_id = str(guard.get("run_id") or "")
    for line in run_text.splitlines():
        if re.match(r"(## Closeout\b|PARKED\b)", line) and (
                not run_id or re.search(r"\brun=" + re.escape(run_id) + r"\b", line)):
            return None
    phase_only = guard.get("scope") == "phase"
    # Genuine halt: nothing runnable here. The runner still owes the PARKED
    # line, but blocking cannot produce work that does not exist.
    ready, next_row = ready_rows(root, timeout=_remaining(deadline) / 2,
                                 phase=guard.get("phase") if phase_only else None)
    if ready == 0:
        return None
    print_ = fingerprint(root, run_rel, run_text, deadline)
    _, state_path = _paths(root)
    lock = _guard_lock(root, min(_env_int("CAMPAIGN_LOCK_WAIT_MS", 5000), int(_remaining(deadline) * 1000)))
    try:
        _remaining(deadline)
        # A handover or an end that landed while this hook worked means the
        # state is no longer this run's, and nothing is written.
        try:
            still = read_guard(root)
        except GuardError:
            still = None
        if not _same_run(still, guard):
            print("campaign-stop: the guard changed while this hook waited; nothing written", file=sys.stderr)
            return None
        try:
            old = _read_json(state_path)
        except FileNotFoundError:
            old = {}
        if not isinstance(old, dict):
            raise GuardError(f"the state file {state_path} is not an object")
        state = {"fingerprint": str(old.get("fingerprint", "")), "blocks": int(old.get("blocks", 0) or 0),
                 "trips": int(old.get("trips", 0) or 0), "stalled": False,
                 "hb_fingerprint": str(old.get("hb_fingerprint", "")),
                 "hb_still": int(old.get("hb_still", 0) or 0)}
        # An error the heartbeat has not acknowledged stays until it is
        # (`hook-error --ack`), however many hook runs come between.
        state.update({k: old[k] for k in ("hook_error", "hook_error_at") if old.get(k)})
        if state["fingerprint"] != print_:
            # The tree moved since the last stop: real progress resets the breaker.
            state.update(fingerprint=print_, blocks=0, trips=0, hb_fingerprint=print_, hb_still=0)
        if state["blocks"] >= MAX_BLOCKS_WITHOUT_PROGRESS:
            # Repeated pushes with no change to the tree mean the session is
            # stuck, not idle. Let it stop and say so.
            state.update(blocks=0, trips=state["trips"] + 1, stalled=True, stalled_at=_utc_now())
            _write_state(state_path, state)
            print(f"campaign-stop: stall breaker tripped ({state['trips']}) after {MAX_BLOCKS_WITHOUT_PROGRESS} "
                  f"blocks with no tree change", file=sys.stderr)
            return None
        state["blocks"] += 1
        _write_state(state_path, state)
    finally:
        lock.__exit__(None, None, None)
    onward = (f" of Phase {guard.get('phase')} only (this run covers that phase)" if phase_only
              else ", then the next phase")
    reason = (f"Campaign run is still open ({run_rel}). Do not end the turn. Finish the open section's checklist, "
              f"run the review panel and stamp it, then the next section{onward}. A commit, a green suite, a red CI "
              f"(repair it through the review skill's bounded repair loop), or a status report is not a stop.")
    if next_row:
        reason += f" Next ready row: {next_row}."
    reason += (f" Finished means a '## Closeout run={run_id}' heading or a column-0 'PARKED <UTC> run={run_id} "
               f"<reason>' line in the run file. Escalating to the operator (an exhausted repair bound, an "
               f"unverifiable CI, a cause the tree cannot fix) ends the run first: write a column-0 'PARKED <UTC> "
               f"run={run_id} escalation: <cause>' line, run 'python3 scripts/campaign_guard.py end --session "
               f"{session} --reason escalation', CronDelete the heartbeat it names, then report. The breaker allows "
               f"the stop after {MAX_BLOCKS_WITHOUT_PROGRESS} pushes with no tree change (this is push "
               f"{state['blocks']}).")
    return {"decision": "block", "reason": reason}


def _record_hook_error(root: str, guard: dict | None, reason: str) -> None:
    """Fail open, but never silently: record the error where the heartbeat
    reads it (D00 T04 §34). Into the state file under the guard lock when
    the lock can be had and the guard is still this run's (or names no
    owner to compare: an unreadable guard is exactly what must be
    reported), else into a file of its own that needs no lock, so a run
    handed over or ended meanwhile never gets a write into its state
    (D00 T04 §36 panel rounds 1 and 2). Best effort only."""
    at = _utc_now()
    try:
        lock = _guard_lock(root, 1000)
    except (GuardError, OSError):
        lock = None
    if lock is not None and guard is not None:
        try:
            still = read_guard(root)
        except GuardError:
            still = None
        if not (still is not None and str(still.get("session_id", "")) == str(guard.get("session_id", ""))
                and ("run_id" not in guard or str(still.get("run_id", "")) == str(guard.get("run_id", "")))):
            lock.__exit__(None, None, None)
            lock = None
    if lock is not None:
        try:
            _, state_path = _paths(root)
            state = {"fingerprint": "", "blocks": 0, "trips": 0, "stalled": False,
                     "hb_fingerprint": "", "hb_still": 0}
            old = {}
            try:
                old = _read_json(state_path)
                state.update(fingerprint=str(old.get("fingerprint", "")), blocks=int(old.get("blocks", 0)),
                             trips=int(old.get("trips", 0)),
                             hb_fingerprint=str(old.get("hb_fingerprint", "")),
                             hb_still=int(old.get("hb_still", 0) or 0))
            except (OSError, ValueError, TypeError, AttributeError):
                old = old if isinstance(old, dict) else {}
            # An unacknowledged error is never replaced: the new one goes to
            # a file of its own, which `hook-error` reports after it.
            if not old.get("hook_error"):
                state.update(hook_error=reason, hook_error_at=at)
                _write_state(state_path, state)
                return
        finally:
            lock.__exit__(None, None, None)
    # One file per error, never an append: a drain can never race a writer.
    folder = os.path.join(root, "build", "claude-campaign-hook-errors")
    os.makedirs(folder, exist_ok=True)
    name = os.path.join(folder, f"{time.time_ns():020d}-{os.getpid()}.txt")
    with open(name + ".tmp", "w", encoding="utf-8") as fh:  # the drain lists only .txt
        fh.write(f"{at} {reason}")
    os.replace(name + ".tmp", name)


def stop_hook(stdin: str, project_dir: str | None) -> tuple[int, str]:
    """Run the hook on one Stop payload; returns (exit, stdout). It always
    exits 0: a bad payload, a missing guard, or any error allows the stop."""
    root = None
    seen: dict = {}
    try:
        if not stdin.strip():
            return 0, ""
        event = json.loads(stdin)
        if event.get("terminationReason") == "USER_CANCELED" or event.get("fullyIdle") is False:
            return 0, ""
        ws_paths = event.get("workspacePaths")
        ws_root = ws_paths[0] if isinstance(ws_paths, list) and ws_paths and isinstance(ws_paths[0], str) else ""
        root = project_dir or ws_root or str(event.get("cwd") or "")
        if not root:
            return 0, ""
        session = str(event.get("conversationId") or event.get("session_id") or "")

        def decide() -> None:
            try:
                seen["guard"] = read_guard(root)
                seen["decision"] = stop_decision(root, session)
            except BaseException as exc:
                seen["error"] = exc

        # The budget is enforced here, not only by the checks inside the
        # decision: whatever blocks in it, the hook stops waiting one second
        # past the budget and fails open (independent review round 3).
        worker = threading.Thread(target=decide, daemon=True)
        worker.start()
        worker.join(_env_int("CAMPAIGN_HOOK_BUDGET_SECONDS", HOOK_BUDGET_SECONDS) + 1)
        if worker.is_alive():
            raise GuardError("the hook ran out of its time budget before deciding")
        if "error" in seen:
            raise seen["error"]
        decision = seen.get("decision")
        if decision and event.get("conversationId"):
            decision = {"decision": "continue", "reason": decision["reason"]}
        return 0, json.dumps(decision) if decision else ""
    except Exception as error:  # fail open; the heartbeat reports what is recorded here
        reason = str(error) or type(error).__name__
        print(f"campaign-stop: {reason}", file=sys.stderr)
        if root:
            try:
                _record_hook_error(root, seen.get("guard"), reason)
            except OSError as exc:
                print(f"campaign-stop: error recording failure: {exc}", file=sys.stderr)
        return 0, ""


REPAIR_BOUND = 3


def _repair_path(root: str) -> str:
    return os.path.join(root, "build", "claude-campaign-repair.json")


def _git(root: str, *args: str) -> tuple[int, str]:
    proc = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    return proc.returncode, proc.stdout.strip()


def _repo_identity(root: str) -> dict:
    """The repository and branch an episode belongs to, read from git."""
    _rc, url = _git(root, "remote", "get-url", "origin")
    _rc, branch = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    return {"repo": url, "branch": branch}


_ATTEMPT_LINE = re.compile(r"repair: episode ([0-9a-f]{12}) attempt (\d+) of \d+ \(([0-9a-f]{12}) repairs ([0-9a-f]{12})\)")
_CLOSED_LINE = re.compile(r"repair: episode ([0-9a-f]{12}) closed green")


def _canon(root: str, rev: str) -> str:
    """A commit's full sha when git resolves it, else the text as given:
    attempts compare by canonical identity, never by printed prefix
    (D00 T04 §37 independent review)."""
    rc, out = _git(root, "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}")
    return out if rc == 0 and out else rev


def _episode_from_run_file(root: str, run_file: str) -> list[tuple[str, str, str]]:
    """(episode, commit, red) for every distinct attempt the run file
    records after its last closed episode: what a lost episode file held.
    An `already counted` line repeats an attempt and is not a new one."""
    try:
        with open(os.path.join(root, run_file), encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return []
    last_close = max((m.end() for m in _CLOSED_LINE.finditer(text)), default=0)
    out: list[tuple[str, str, str]] = []
    seen: set[str] = set()
    for m in _ATTEMPT_LINE.finditer(text, last_close):
        if m.group(3) in seen:
            continue
        seen.add(m.group(3))
        out.append((m.group(1), m.group(3), m.group(4)))
    return out


def repair(root: str, action: str, red: str = "", commit: str = "", green: str = "",
           workflow: str = "", run_file: str = "", evidence: str = "", run_id: str = "") -> tuple[int, str]:
    """The CI repair episode, persisted beside the guard state so a
    resumed or restarted runner cannot recount from zero (D00 T04 §35).
    An episode opens at its first red and closes at the next green; at
    most REPAIR_BOUND repair attempts ride one episode, across however
    many reds it sees. D00 T04 §37 binds it: the episode records its
    repository, branch, workflow, and run file and refuses a mismatched
    call; an attempt is keyed to its repair commit, so a repeat after a
    crash or a failed push counts once; `close` needs a green read-back
    line for that workflow on a descendant of the last attempt; a lost
    episode file is detected from the run file's attempt lines and
    restored; and `ceiling` allows one re-run per GitHub run that stayed
    pending past the ceiling. Returns (exit, line); exit 1 is the
    escalation."""
    path = _repair_path(root)
    with _Lock(root):
        exists = True
        try:
            with open(path, encoding="utf-8") as fh:
                ep = json.load(fh)
        except FileNotFoundError:
            ep, exists = None, False
        except (OSError, ValueError) as exc:
            # An unreadable episode must never reset the bound (panel round 1).
            raise GuardError(f"the repair episode {path} is unreadable ({exc}); the bound cannot be "
                             f"counted, so escalate rather than repair")
        if action == "ceiling":
            cpath = os.path.join(root, "build", "claude-campaign-ceiling.json")
            try:
                with open(cpath, encoding="utf-8") as fh:
                    seen = json.load(fh)
            except FileNotFoundError:
                seen = {}
            except (OSError, ValueError):
                raise GuardError("the ceiling record is unreadable; escalate rather than wait again")
            if not run_id:
                raise GuardError("repair ceiling needs --run-id <GitHub run id>")
            if seen.get(run_id, 0) >= 1:
                return 1, (f"repair: run {run_id} already had its one re-run past the ceiling: escalate "
                           f"(a queue that never drains is outside the tree)")
            seen[run_id] = seen.get(run_id, 0) + 1
            with open(cpath + ".tmp", "w", encoding="utf-8") as fh:
                json.dump(seen, fh)
            os.replace(cpath + ".tmp", cpath)
            return 0, f"repair: run {run_id} may re-run ci-wait once more past the ceiling"
        # Only a missing file means no episode: a file holding anything but
        # a well-formed episode (a JSON null included) refuses (panel round 2).
        if exists and not (isinstance(ep, dict) and isinstance(ep.get("episode"), str)
                           and isinstance(ep.get("attempts"), list)):
            raise GuardError(f"the repair episode {path} is malformed; the bound cannot be counted, "
                             f"so escalate rather than repair")
        if action == "restore":
            if ep:
                return 0, f"repair: episode {ep['episode'][:12]} is present; nothing to restore"
            if not workflow or not run_file:
                raise GuardError("repair restore needs --workflow and --run-file: the restored episode is bound to both")
            found = _episode_from_run_file(root, run_file)
            if not found:
                return 0, "repair: the run file shows no open episode; nothing to restore"
            ep = {"episode": _canon(root, found[0][2]),
                  "attempts": [{"red": _canon(root, r), "commit": _canon(root, c)} for _e, c, r in found],
                  **_repo_identity(root), "workflow": workflow, "run_file": run_file, "restored": True}
            with open(path + ".tmp", "w", encoding="utf-8") as fh:
                json.dump(ep, fh, indent=1)
            os.replace(path + ".tmp", path)
            return 0, (f"repair: episode {ep['episode'][:12]} restored from {run_file} at "
                       f"{len(ep['attempts'])} of {REPAIR_BOUND} attempts")
        if action == "status":
            if not ep:
                lost = _episode_from_run_file(root, run_file) if run_file else []
                if lost:
                    return 1, (f"repair: the episode file is lost but {run_file} shows episode {lost[0][0]} at "
                               f"{len(lost)} of {REPAIR_BOUND} attempts: run repair restore before any repair")
                return 0, "repair: no open episode"
            return 0, (f"repair: episode {ep['episode'][:12]} open, {len(ep['attempts'])} of "
                       f"{REPAIR_BOUND} attempts used")
        identity = {**_repo_identity(root), "workflow": workflow, "run_file": run_file}
        if ep and action in ("attempt", "close"):
            for key in ("repo", "branch", "workflow", "run_file"):
                if ep.get(key, identity[key]) != identity[key]:
                    raise GuardError(f"the open episode belongs to {key} {ep.get(key)!r}, not {identity[key]!r}; "
                                     f"refusing to mix episodes")
        if action == "close":
            if not green:
                raise GuardError("repair close needs --green <sha>")
            if not ep:
                return 0, "repair: no open episode to close"
            want = re.compile(rf"\Aci-wait: {re.escape(green[:12])}[0-9a-f]* {re.escape(workflow or ep.get('workflow', ''))} success\b")
            if not want.match(evidence or ""):
                raise GuardError("repair close needs --evidence with the green ci-wait line for this sha and "
                                 "workflow (an authorized no-run is not green)")
            last = ep["attempts"][-1]["commit"] if ep["attempts"] else ep["episode"]
            rc, _ = _git(root, "merge-base", "--is-ancestor", last, green)
            if rc != 0:
                raise GuardError(f"the green {green[:12]} does not descend from the last attempt {last[:12]}; "
                                 f"it cannot close this episode")
            os.unlink(path)
            return 0, (f"repair: episode {ep['episode'][:12]} closed green at {green[:12]} after "
                       f"{len(ep['attempts'])} attempt(s)")
        if action == "attempt":
            if not red or not commit or not workflow or not run_file:
                raise GuardError("repair attempt needs --red, --commit, --workflow, and --run-file")
            if not ep:
                lost = _episode_from_run_file(root, run_file)
                if lost:
                    raise GuardError(f"the run file shows open episode {lost[0][0]} but its file is lost: "
                                     f"run repair restore first")
            red, commit = _canon(root, red), _canon(root, commit)
            ep = ep or {"episode": red, "attempts": [], **identity}
            done = [a for a in ep["attempts"] if _canon(root, a.get("commit", "")) == commit]
            if done:
                n = ep["attempts"].index(done[0]) + 1
                return 0, (f"repair: episode {ep['episode'][:12]} attempt {n} of {REPAIR_BOUND} "
                           f"({commit[:12]} repairs {done[0]['red'][:12]}) already counted")
            if len(ep["attempts"]) >= REPAIR_BOUND:
                return 1, (f"repair: episode {ep['episode'][:12]} has used all {REPAIR_BOUND} attempts: "
                           f"the bound is exhausted, escalate (PARKED ... escalation:, then end --reason escalation)")
            ep["attempts"].append({"red": red, "commit": commit})
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(ep, fh, indent=1)
            os.replace(tmp, path)
            return 0, (f"repair: episode {ep['episode'][:12]} attempt {len(ep['attempts'])} of "
                       f"{REPAIR_BOUND} ({commit[:12]} repairs {red[:12]})")
        raise GuardError(f"repair action {action!r} is not attempt, close, status, restore, or ceiling")


def _powershell() -> str | None:
    for name in (("powershell.exe", "powershell", "pwsh") if os.name == "nt" else ("pwsh",)):
        found = shutil.which(name)
        if found:
            return found
    return None


def run_hook(root: str, session: str, script: str | None = None,
             env_extra: dict | None = None) -> tuple[int, dict | None, str]:
    """Feed the hook one Stop payload, as the workspace's own copy of this
    module (or `script`), with `env_extra` over the inherited environment;
    return (exit, parsed stdout JSON or None when it allowed silently,
    stderr)."""
    env = dict(os.environ, CLAUDE_PROJECT_DIR=root, **(env_extra or {}))
    payload = json.dumps({"session_id": session, "cwd": root, "hook_event_name": "Stop"})
    proc = subprocess.run([sys.executable, script or os.path.join(root, "scripts", "campaign_guard.py"), "stop-hook"],
                          input=payload, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", env=env, cwd=root, timeout=120)
    out = proc.stdout.strip()
    try:
        parsed = json.loads(out) if out else None
    except ValueError:
        parsed = {"unparsed": out}
    return proc.returncode, parsed, proc.stderr


def configured_command() -> str | None:
    """The Stop hook command exactly as `.agents/hooks.json` wires it."""
    try:
        with open(SETTINGS, encoding="utf-8") as fh:
            doc = json.load(fh)
        return doc["hooks"]["Stop"][0]["hooks"][0]["command"]
    except (OSError, ValueError, KeyError, IndexError, TypeError):
        return None


def run_configured(root: str, session: str, shell: list[str]) -> tuple[int, dict | None, str]:
    """Run the configured command the way Claude Code would, through
    `shell` (sh, Git Bash, or PowerShell), with CLAUDE_PROJECT_DIR pointing at
    the fixture (§32 independent review: a `$VAR` path that one shell
    does not expand never launches the hook)."""
    env = dict(os.environ, CLAUDE_PROJECT_DIR=root)
    payload = json.dumps({"session_id": session, "cwd": root, "hook_event_name": "Stop"})
    proc = subprocess.run([*shell, configured_command()], input=payload, capture_output=True,
                          text=True, encoding="utf-8", errors="replace", env=env, cwd=root, timeout=120)
    out = proc.stdout.strip()
    try:
        parsed = json.loads(out) if out else None
    except ValueError:
        parsed = {"unparsed": out}
    return proc.returncode, parsed, proc.stderr


def _workspace(tmpd: str) -> str:
    root = os.path.join(tmpd, "ws")
    os.makedirs(os.path.join(root, "scripts"))
    os.makedirs(os.path.join(root, "build"))
    os.makedirs(os.path.join(root, "docs", "phase-runs"))
    # The hook runs from the workspace's own copy, as the configured
    # command resolves it through CLAUDE_PROJECT_DIR.
    shutil.copyfile(os.path.join(HERE, "campaign_guard.py"), os.path.join(root, "scripts", "campaign_guard.py"))
    with open(os.path.join(root, ".gitignore"), "w", encoding="utf-8") as fh:
        fh.write("build/\n.runner/\n")
    # The fake graph prints the runnable count the test writes to ready.txt.
    with open(os.path.join(root, "scripts", "todo-graph.py"), "w", encoding="utf-8") as fh:
        fh.write("import os, time\n"
                 "if os.path.exists(os.path.join(os.path.dirname(__file__), 'slow.txt')):\n"
                 "    time.sleep(5)\n"
                 "n = open(os.path.join(os.path.dirname(__file__), 'ready.txt')).read().strip()\n"
                 "if n != '0':\n"
                 "    print('00-workspace/TODO-01-bootstrap.md §3  Red CI repaired, not waited on')\n"
                 "print()\n"
                 "print(f'{n} runnable now, 0 runnable elsewhere')\n")
    _set_ready(root, 3)
    with open(os.path.join(root, RUN_FILE), "w", encoding="utf-8") as fh:
        fh.write("# Phase run: Phase 0\n\n## Sections log\n")
    for args in (["init", "-q"], ["config", "user.email", "t@t"], ["config", "user.name", "t"],
                 ["config", "commit.gpgsign", "false"], ["add", "-A"], ["commit", "-qm", "seed"]):
        subprocess.run(["git", *args], cwd=root, capture_output=True, check=True)
    return root


def _set_ready(root: str, n: int) -> None:
    with open(os.path.join(root, "scripts", "ready.txt"), "w", encoding="utf-8") as fh:
        fh.write(str(n))


def _guard(root: str, session: str = SESSION, run_file: str = RUN_FILE) -> None:
    with open(os.path.join(root, "build", "claude-campaign-guard.json"), "w", encoding="utf-8") as fh:
        json.dump({"runner": "claude", "workspace": root, "phase": 0, "run_file": run_file,
                   "session_id": session, "cron_id": "job-1"}, fh)


def _state(root: str) -> dict:
    path = os.path.join(root, "build", "claude-campaign-state.json")
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8-sig") as fh:
        return json.load(fh)


def _self_test() -> int:
    passed = failed = 0

    def check(name: str, ok: bool, detail: str = "") -> None:
        nonlocal passed, failed
        if ok:
            passed += 1
        else:
            failed += 1
            print(f"FAIL {name} {detail}")

    with tempfile.TemporaryDirectory(prefix="campaign-guard-") as tmpd:
        root = _workspace(tmpd)

        code, out, err = run_hook(root, SESSION)
        check("allow-without-guard", code == 0 and out is None, f"{code} {out} {err}")

        _guard(root)
        code, out, err = run_hook(root, "99999999-0000-0000-0000-000000000000")
        check("allow-another-session", code == 0 and out is None, f"{code} {out} {err}")

        # The configured command, not just the script: through every shell
        # Claude Code may launch it with on this host. On Windows that is
        # PowerShell (a host without Git Bash) and Git Bash; elsewhere sh,
        # and PowerShell when it is installed.
        shells = []
        if os.name == "nt":
            shells.append(("powershell", [_powershell(), "-NoProfile", "-Command"]))
            # Git Bash by path: the `bash` on PATH can be WSL's, which runs a
            # different interpreter against the wrong tree.
            bash = os.path.join(os.environ.get("ProgramFiles", "C:/Program Files"), "Git", "bin", "bash.exe")
            if os.path.isfile(bash):
                shells.append(("bash", [bash, "-c"]))
        else:
            shells.append(("sh", ["/bin/sh", "-c"]))
            if _powershell():
                shells.append(("pwsh", [_powershell(), "-NoProfile", "-Command"]))
        command = configured_command() or ""
        check("settings-wire-the-stop-hook",
              "campaign_guard.py" in command and "'stop-hook'" in command and "CLAUDE_PROJECT_DIR" in command,
              command)
        try:
            with open(SETTINGS, encoding="utf-8") as fh:
                hook_timeout = json.load(fh)["hooks"]["Stop"][0]["hooks"][0].get("timeout")
        except (OSError, ValueError, KeyError, IndexError, TypeError):
            hook_timeout = None
        check("the-hook-budget-fits-inside-the-configured-timeout",
              isinstance(hook_timeout, int) and HOOK_BUDGET_SECONDS + 10 <= hook_timeout, str(hook_timeout))
        for label, shell in shells:
            state_path = os.path.join(root, "build", "claude-campaign-state.json")
            if os.path.exists(state_path):
                os.remove(state_path)
            code, out, err = run_configured(root, SESSION, shell)
            check(f"configured-command-launches-through-{label}",
                  isinstance(out, dict) and out.get("decision") == "block", f"{code} {out} {err[:300]}")
        try:
            with open(os.path.join(REPO, ".claude", "settings.json"), encoding="utf-8") as fh:
                claude_cmd = json.load(fh)["hooks"]["Stop"][0]["hooks"][0]["command"]
        except (OSError, ValueError, KeyError, IndexError, TypeError):
            claude_cmd = None
        check("claude-and-agents-wire-the-same-stop-hook",
              claude_cmd is not None and claude_cmd == configured_command(),
              f"{claude_cmd!r} vs {configured_command()!r}")
        state_path = os.path.join(root, "build", "claude-campaign-state.json")
        if os.path.exists(state_path):
            os.remove(state_path)

        code, out, err = run_hook(root, SESSION)
        check("block-open-run-naming-next-row",
              code == 0 and isinstance(out, dict) and out.get("decision") == "block"
              and "Next ready row: 00-workspace/TODO-01-bootstrap.md §3" in out.get("reason", "")
              and "repair it through the review skill" in out.get("reason", ""), f"{code} {out} {err}")
        check("the-escalation-instruction-names-the-session",
              isinstance(out, dict) and f"end --session {SESSION} --reason escalation" in out.get("reason", ""),
              str(out))

        # Stall breaker: blocks 2 and 3 with no tree change, then the trip.
        run_hook(root, SESSION)
        code, out, _ = run_hook(root, SESSION)
        check("third-block-still-blocks", isinstance(out, dict) and out.get("decision") == "block", str(out))
        code, out, err = run_hook(root, SESSION)
        st = _state(root)
        check("breaker-trips-after-three-unchanged-blocks",
              code == 0 and out is None and st.get("trips") == 1 and st.get("stalled") is True
              and "stall breaker tripped (1)" in err, f"{code} {out} {st} {err}")
        with open(os.path.join(root, "progress.txt"), "w", encoding="utf-8") as fh:
            fh.write("moved\n")
        code, out, _ = run_hook(root, SESSION)
        st = _state(root)
        check("progress-resets-the-breaker",
              isinstance(out, dict) and out.get("decision") == "block"
              and st.get("blocks") == 1 and st.get("trips") == 0, f"{out} {st}")

        _set_ready(root, 0)
        code, out, _ = run_hook(root, SESSION)
        check("allow-when-nothing-is-runnable", code == 0 and out is None, str(out))
        _set_ready(root, 3)

        run_path = os.path.join(root, RUN_FILE)
        with open(run_path, "a", encoding="utf-8") as fh:
            fh.write("\nPARKED 2099-01-01T00:00:00Z every leftover row is blocked\n")
        code, out, _ = run_hook(root, SESSION)
        check("allow-when-parked", code == 0 and out is None, str(out))
        with open(run_path, "w", encoding="utf-8") as fh:
            fh.write("# Phase run: Phase 0\n\n## Closeout\n\nshipped\n")
        code, out, _ = run_hook(root, SESSION)
        check("allow-after-closeout", code == 0 and out is None, str(out))

        with open(run_path, "w", encoding="utf-8") as fh:
            fh.write("# Phase run: Phase 0\n\nThe word PARKED mid-line is not a marker\n")
        code, out, _ = run_hook(root, SESSION)
        check("mid-line-parked-still-blocks", isinstance(out, dict) and out.get("decision") == "block", str(out))

        _guard(root, run_file="../outside.md")
        code, out, _ = run_hook(root, SESSION)
        check("allow-when-run-file-escapes-the-workspace", code == 0 and out is None, str(out))

        with open(os.path.join(root, "build", "claude-campaign-guard.json"), "w", encoding="utf-8") as fh:
            fh.write("{not json")
        code, out, err = run_hook(root, SESSION)
        check("fail-open-on-a-bad-guard", code == 0 and out is None and "campaign-stop:" in err, f"{code} {out} {err}")
        # D00 T04 §34: the failure is recorded for the heartbeat, then
        # reported through the still-malformed guard, then cleared.
        st = _state(root)
        check("a-thrown-error-is-recorded", bool(st.get("hook_error")) and bool(st.get("hook_error_at")), str(st))
        with open(os.path.join(root, "build", "claude-campaign-state.json"), encoding="utf-8-sig") as fh:
            saved_state = fh.read()
        line = hook_error(root, SESSION)
        check("hook-error-reports-through-a-malformed-guard",
              line.startswith("campaign-stop hook failed at ") and "guard file is unreadable" in line
              and bool(_state(root).get("hook_error")), line)
        # D00 T04 §36: printing clears nothing; only the exact acknowledged
        # line clears the record, so an interrupted heartbeat loses nothing.
        try:
            hook_error(root, SESSION, ack="some other line")
            check("hook-error-refuses-a-mismatched-ack", False)
        except GuardError as exc:
            check("hook-error-refuses-a-mismatched-ack",
                  "does not match" in str(exc) and bool(_state(root).get("hook_error")), str(exc))
        cleared = hook_error(root, SESSION, ack=line)
        check("hook-error-clears-on-the-exact-ack",
              cleared.startswith("hook-error: acknowledged and cleared") and "hook_error" not in _state(root),
              cleared)
        with open(os.path.join(root, "build", "claude-campaign-state.json"), "w", encoding="utf-8") as fh:
            fh.write(saved_state)
        _guard(root, session="77777777-0000-0000-0000-000000000000")
        with open(os.path.join(root, "build", "claude-campaign-state.json"), "w", encoding="utf-8") as fh:
            json.dump({"hook_error": "boom", "hook_error_at": "2099-01-01T00:00:00Z"}, fh)
        try:
            hook_error(root, SESSION)
            check("hook-error-refuses-another-sessions-state", False)
        except GuardError as exc:
            check("hook-error-refuses-another-sessions-state",
                  "belongs to session" in str(exc) and _state(root).get("hook_error") == "boom", str(exc))
        line = hook_error(root)
        hook_error(root, ack=line)
        check("hook-error-reports-then-clears-on-ack",
              line.startswith("campaign-stop hook failed at ") and "hook_error" not in _state(root)
              and hook_error(root) == "", f"{line!r} {_state(root)}")

        # D00 T04 §34: the fingerprint sees untracked content and ignores
        # the run file's bookkeeping.
        os.remove(os.path.join(root, "build", "claude-campaign-guard.json"))
        os.remove(os.path.join(root, "build", "claude-campaign-state.json"))
        with open(run_path, "w", encoding="utf-8") as fh:
            fh.write("# Phase run: Phase 0\n\n## Critical events\n\n- started\n\n## Sections\n")
        _guard(root)
        for _ in range(3):
            run_hook(root, SESSION)
        with open(os.path.join(root, "progress.txt"), "a", encoding="utf-8") as fh:
            fh.write("an edit to an untracked file\n")
        code, out, _ = run_hook(root, SESSION)
        st = _state(root)
        check("untracked-content-edit-resets-the-breaker",
              isinstance(out, dict) and out.get("decision") == "block" and st.get("blocks") == 1, f"{out} {st}")
        with open(os.path.join(root, "caf\u00e9.txt"), "w", encoding="utf-8") as fh:
            fh.write("work under a non-ASCII name\n")
        for _ in range(4):
            run_hook(root, SESSION)
        with open(os.path.join(root, "caf\u00e9.txt"), "a", encoding="utf-8") as fh:
            fh.write("an edit only its content hash can see\n")
        code, out, _ = run_hook(root, SESSION)
        check("a-non-ascii-untracked-edit-is-progress",
              isinstance(out, dict) and out.get("decision") == "block" and _state(root).get("blocks") == 1
              and _state(root).get("trips") == 0,
              f"{out} {_state(root)}")
        # Past the content-hash bound every path still counts: a new file
        # beyond the 500th is progress (D00 T04 §34 independent review).
        many = os.path.join(root, "many")
        os.makedirs(many)
        for i in range(505):
            with open(os.path.join(many, f"f{i:04d}.txt"), "w", encoding="utf-8") as fh:
                fh.write("x\n")
        for _ in range(4):
            run_hook(root, SESSION)
        with open(os.path.join(many, "zzzz-new.txt"), "w", encoding="utf-8") as fh:
            fh.write("new work past the bound\n")
        code, out, _ = run_hook(root, SESSION)
        check("a-file-past-the-hash-bound-is-progress",
              isinstance(out, dict) and out.get("decision") == "block" and _state(root).get("trips") == 0,
              f"{out} {_state(root)}")
        shutil.rmtree(many)
        run_hook(root, SESSION)
        for _ in range(2):
            run_hook(root, SESSION)
        text = open(run_path, encoding="utf-8").read().replace(
            "- started\n", "- started\n- bookkeeping: heartbeat fired, resumed\n")
        with open(run_path, "w", encoding="utf-8") as fh:
            fh.write(text)
        code, out, err = run_hook(root, SESSION)
        st = _state(root)
        check("a-bookkeeping-line-alone-still-trips-the-breaker",
              code == 0 and out is None and st.get("trips") == 1 and "stall breaker tripped" in err,
              f"{code} {out} {st} {err}")
        with open(run_path, "a", encoding="utf-8") as fh:
            fh.write("\n### D90 T01 §1 -- shipped\n")
        code, out, _ = run_hook(root, SESSION)
        check("a-sections-entry-is-progress", isinstance(out, dict) and out.get("decision") == "block", str(out))

        # D00 T04 §36: a substantive Critical events line is progress; only
        # a marked bookkeeping line is not.
        for _ in range(3):
            run_hook(root, SESSION)
        text = open(run_path, encoding="utf-8").read().replace(
            "- started\n", "- started\n- 2099-01-01: repaired the red CI by hand, see the run page\n")
        with open(run_path, "w", encoding="utf-8") as fh:
            fh.write(text)
        code, out, _ = run_hook(root, SESSION)
        check("a-substantive-critical-events-line-is-progress",
              isinstance(out, dict) and out.get("decision") == "block" and _state(root).get("trips") == 0,
              f"{out} {_state(root)}")

        # D00 T04 §36: the hook's state write waits for the guard lock.
        holder = subprocess.Popen(
            [sys.executable, "-c",
             "import sys, time; sys.path.insert(0, sys.argv[1]); import campaign_guard as cg\n"
             "with cg._Lock(sys.argv[2]):\n"
             "    print('held', flush=True); time.sleep(2)",
             HERE, root], stdout=subprocess.PIPE, text=True)
        holder.stdout.readline()
        import time as _time
        began = _time.monotonic()
        code, out, err = run_hook(root, SESSION)
        waited = _time.monotonic() - began
        holder.wait()
        check("the-hook-waits-for-the-guard-lock",
              isinstance(out, dict) and out.get("decision") == "block" and waited >= 1.0, f"{waited:.2f}s {out} {err}")
        holder = subprocess.Popen(
            [sys.executable, "-c",
             "import sys, time; sys.path.insert(0, sys.argv[1]); import campaign_guard as cg\n"
             "with cg._Lock(sys.argv[2]):\n"
             "    print('held', flush=True); sys.stdin.readline()",
             HERE, root], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        holder.stdout.readline()
        os.environ["CAMPAIGN_LOCK_WAIT_MS"] = "300"
        try:
            code, out, err = run_hook(root, SESSION)
        finally:
            del os.environ["CAMPAIGN_LOCK_WAIT_MS"]
        holder.communicate("\n")
        folder = os.path.join(root, "build", "claude-campaign-hook-errors")
        files = sorted(os.listdir(folder)) if os.path.isdir(folder) else []
        logged = "".join(open(os.path.join(folder, n), encoding="utf-8").read() for n in files)
        check("a-lock-timeout-fails-open-into-an-error-file",
              code == 0 and out is None and len(files) == 1 and "guard lock stayed held" in logged,
              f"{out} {err} {files} {logged!r}")
        shutil.rmtree(folder)

        # D00 T04 §36 independent review: an error written to the fallback
        # log (the lock was unavailable) reaches the heartbeat too.
        os.makedirs(os.path.join(root, "build", "claude-campaign-hook-errors"), exist_ok=True)
        with open(os.path.join(root, "build", "claude-campaign-hook-errors", "00000000000000000001-1.txt"), "w",
                  encoding="utf-8") as fh:
            fh.write("2099-01-01T00:00:00Z the guard lock stayed held for 300ms")
        st_path = os.path.join(root, "build", "claude-campaign-state.json")
        if os.path.exists(st_path):
            st_now = _state(root)
            st_now.pop("hook_error", None)
            with open(st_path, "w", encoding="utf-8") as fh:
                json.dump(st_now, fh)
        line = hook_error(root)
        check("hook-error-reads-the-fallback-log",
              line == "campaign-stop hook failed at 2099-01-01T00:00:00Z: the guard lock stayed held for 300ms "
                      "(recorded without the lock)", line)
        hook_error(root, ack=line)
        check("hook-error-ack-drains-the-fallback-log", hook_error(root) == "", hook_error(root))

        # D00 T04 §34: an escalation parks and ends the run before its report.
        with open(run_path, "a", encoding="utf-8") as fh:
            fh.write("\nPARKED 2099-01-01T00:00:00Z escalation: repair bound exhausted on D90 T01 §1\n")
        code, out, _ = run_hook(root, SESSION)
        check("the-escalation-report-turn-ends", code == 0 and out is None, str(out))

        # D00 T04 §34: exclusive acquisition and every end path.
        os.remove(os.path.join(root, "build", "claude-campaign-guard.json"))
        msg = acquire(root, SESSION, 0, RUN_FILE, "job-1")
        check("acquire-creates-the-guard", msg.startswith("acquire: guard created"), msg)
        try:
            acquire(root, "22222222-0000-0000-0000-000000000000", 0, RUN_FILE, "job-2")
            check("acquire-refuses-a-second-session", False, "took a live guard")
        except GuardError as exc:
            check("acquire-refuses-a-second-session", "without an operator handover" in str(exc)
                  and read_guard(root)["session_id"] == SESSION, str(exc))
        msg = acquire(root, SESSION, 1, "docs/phase-runs/2099-01-01-phase-1.md", "job-3")
        check("acquire-re-points-its-own-guard", "re-pointed" in msg and read_guard(root)["cron_id"] == "job-3", msg)
        msg = acquire(root, "22222222-0000-0000-0000-000000000000", 1, RUN_FILE, "job-4",
                      handover="operator resumed the run in a new session")
        g = read_guard(root)
        check("acquire-hands-over-with-a-reason", "handed over" in msg and g["handover_from"] == SESSION
              and g["session_id"].startswith("22222222"), f"{msg} {g}")
        # A live holder in another process: the change waits, then refuses.
        holder = subprocess.Popen(
            [sys.executable, "-c",
             "import sys, time; sys.path.insert(0, sys.argv[1]); import campaign_guard as cg\n"
             "with cg._Lock(sys.argv[2]):\n"
             "    print('held', flush=True); time.sleep(float(sys.argv[3]))",
             HERE, root, "3"], stdout=subprocess.PIPE, text=True)
        holder.stdout.readline()
        try:
            with _Lock(root, wait=0.3):
                pass
            check("a-held-lock-refuses-a-change", False, "entered a held lock")
        except GuardError as exc:
            check("a-held-lock-refuses-a-change", "locked by another change" in str(exc), str(exc))
        holder.wait()
        entered = False
        with _Lock(root, wait=2):
            entered = True
        check("the-lock-is-released-after-a-change", entered)
        # A holder that dies without releasing: the OS frees the lock, and
        # no age test was needed to break it.
        dead = subprocess.Popen(
            [sys.executable, "-c",
             "import os, sys; sys.path.insert(0, sys.argv[1]); import campaign_guard as cg\n"
             "lock = cg._Lock(sys.argv[2]); lock.__enter__(); print('held', flush=True); os._exit(0)",
             HERE, root], stdout=subprocess.PIPE, text=True)
        dead.stdout.readline()
        dead.wait()
        entered = False
        with _Lock(root, wait=2):
            entered = True
        check("a-dead-holder-releases-the-lock", entered)
        # Two sessions changing the guard at once: the lock serializes them,
        # and the guard ends owned by exactly the session that changed it last.
        import threading
        results: list = []

        def _take(sess: str) -> None:
            try:
                results.append(acquire(root, sess, 2, RUN_FILE, "job-7", handover="race drill"))
            except GuardError as exc:
                results.append(str(exc))
        threads = [threading.Thread(target=_take, args=(s,)) for s in
                   ("22222222-0000-0000-0000-000000000000", "55555555-0000-0000-0000-000000000000")]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        final = read_guard(root)
        check("concurrent-changes-serialize", len(results) == 2 and final is not None
              and final["session_id"] in ("22222222-0000-0000-0000-000000000000",
                                          "55555555-0000-0000-0000-000000000000")
              and all(r.startswith("acquire: guard") for r in results), f"{results} {final}")
        try:
            end(root, "", "operator-stop")
            check("end-refuses-without-a-session", False)
        except GuardError as exc:
            check("end-refuses-without-a-session", "needs --session" in str(exc), str(exc))
        try:
            acquire(root, SESSION, 0, "../outside.md", "job-5", handover="x")
            check("acquire-refuses-an-escaping-run-file", False)
        except GuardError as exc:
            check("acquire-refuses-an-escaping-run-file", "inside the workspace" in str(exc), str(exc))

        def _fresh(body: str, ready: int = 3) -> None:
            for f in _paths(root):
                if os.path.exists(f):
                    os.remove(f)
            # D00 T04 §36: markers count only after acquisition, so the run
            # file gains its body after the guard is acquired.
            with open(run_path, "w", encoding="utf-8") as fh:
                fh.write("# run\n")
            _set_ready(root, ready)
            acquire(root, SESSION, 0, RUN_FILE, "job-9")
            run_hook(root, SESSION)  # leaves a state file behind
            rid = read_guard(root)["run_id"]
            body = body.replace("## Closeout", f"## Closeout run={rid}").replace(
                "PARKED 2099-01-01T00:00:00Z", f"PARKED 2099-01-01T00:00:00Z run={rid}")
            with open(run_path, "a", encoding="utf-8") as fh:
                fh.write(body)

        for reason, body, ready in (("closeout", "# r\n\n## Closeout\n\nshipped\n", 3),
                                    ("park", "# r\n\nPARKED 2099-01-01T00:00:00Z leftovers blocked\n", 3),
                                    ("plan-done", "# r\n", 0),
                                    ("operator-stop", "# r\n", 3),
                                    ("escalation", "# r\n\nPARKED 2099-01-01T00:00:00Z escalation: gh down\n", 3)):
            _fresh(body, ready)
            msg = end(root, SESSION, reason)
            gone = not any(os.path.exists(f) for f in _paths(root))
            code, out, _ = run_hook(root, SESSION)
            check(f"end-{reason}-leaves-nothing-behind",
                  gone and "CronDelete job-9" in msg and code == 0 and out is None, f"{msg} {gone} {out}")
        _fresh("# r\n", 3)
        try:
            end(root, SESSION, "stall")
            check("end-refuses-a-stall-without-two-trips", False)
        except GuardError as exc:
            check("end-refuses-a-stall-without-two-trips", "trips of 2 or more" in str(exc), str(exc))
        with open(_paths(root)[1], "w", encoding="utf-8") as fh:
            json.dump({"trips": 2, "stalled": True}, fh)
        msg = end(root, SESSION, "stall")
        check("end-stall-leaves-nothing-behind",
              not any(os.path.exists(f) for f in _paths(root)) and "CronDelete job-9" in msg, msg)
        _fresh("# r\n", 3)
        try:
            end(root, SESSION, "closeout")
            check("end-refuses-a-closeout-without-its-heading", False)
        except GuardError as exc:
            check("end-refuses-a-closeout-without-its-heading",
                  "needs a '## Closeout run=" in str(exc) and read_guard(root) is not None, str(exc))
        try:
            end(root, "33333333-0000-0000-0000-000000000000", "operator-stop")
            check("end-refuses-another-session", False)
        except GuardError as exc:
            check("end-refuses-another-session", "belongs to session" in str(exc), str(exc))

    # D00 T04 §36: the lifecycle commands, in a fixture of their own.
    with tempfile.TemporaryDirectory(prefix="campaign-life-") as ltmp:
        lroot = _workspace(ltmp)
        run = os.path.join(lroot, RUN_FILE)
        with open(run, "w", encoding="utf-8") as fh:
            fh.write("# old run\n\nPARKED 2098-01-01T00:00:00Z an earlier run parked here\n")
        g1 = mint_generation()
        acquire(lroot, SESSION, 0, RUN_FILE, "job-a", generation=g1)
        code, out, _ = run_hook(lroot, SESSION)
        check("a-reused-run-files-old-marker-keeps-blocking",
              isinstance(out, dict) and out.get("decision") == "block", str(out))
        rid = read_guard(lroot)["run_id"]
        # D00 T04 §36 independent review: the park record may sit mid-file
        # (process-phase puts it under Gap audit); the run id, not its
        # position, makes it this run's.
        with open(run, "w", encoding="utf-8") as fh:
            fh.write(f"# run\n\n## Gap audit\n\nPARKED 2099-01-01T00:00:00Z run={rid} leftovers blocked\n\n"
                     "## Sections\n\nPARKED 2098-01-01T00:00:00Z an earlier run parked here\n")
        code, out, _ = run_hook(lroot, SESSION)
        check("a-mid-file-marker-carrying-the-run-id-ends-the-run", code == 0 and out is None, str(out))
        with open(run, "w", encoding="utf-8") as fh:
            fh.write("# run\n\n## Critical events\n\n- a new line above the old marker\n\n"
                     "PARKED 2098-01-01T00:00:00Z run=0000deadbeef an earlier run parked here\n")
        code, out, _ = run_hook(lroot, SESSION)
        check("an-old-marker-pushed-down-the-file-keeps-blocking",
              isinstance(out, dict) and out.get("decision") == "block", str(out))
        with open(run, "a", encoding="utf-8") as fh:
            fh.write(f"\nPARKED 2099-01-01T00:00:00Z run={rid} this run parked\n")
        check("whoami-owner-names-the-run-and-its-scope",
              whoami(lroot, SESSION, g1) == f"OWNER run={read_guard(lroot)['run_id']} scope=plan",
              whoami(lroot, SESSION, g1))
        check("whoami-not-the-current-job", whoami(lroot, SESSION, "stale0000000") == "NOT THE CURRENT JOB")
        check("whoami-not-the-owner", whoami(lroot, "99999999-0000-0000-0000-000000000000", g1) == "NOT THE OWNER")
        g2 = mint_generation()
        acquire(lroot, SESSION, 0, RUN_FILE, "job-b", generation=g2)
        check("a-replaced-job-is-not-current", whoami(lroot, SESSION, g1) == "NOT THE CURRENT JOB"
              and whoami(lroot, SESSION, g2).startswith("OWNER"))
        check("health-ok", health(lroot, SESSION, ["job-b"])[0] == 0)
        code_h, line_h = health(lroot, SESSION, ["job-a"])
        check("health-names-a-missing-job", code_h == 1 and "job-b is missing" in line_h, line_h)
        check("reconcile-consistent", reconcile(lroot, SESSION, ["job-b"]) == ["reconcile: consistent (guard and job job-b)"])
        rc = reconcile(lroot, SESSION, ["job-a"])
        check("reconcile-guard-without-job-and-stale-job",
              any("job-b, which is not live" in r for r in rc) and any("stale job job-a" in r for r in rc), str(rc))
        g = read_guard(lroot)
        code_e, line_e = expiry(lroot, SESSION, now=g["job_created_at"])
        check("expiry-ok-when-fresh", code_e == 0 and "expires in 168.0h" in line_e, line_e)
        import datetime as _dt
        later = (_dt.datetime.strptime(g["job_created_at"], "%Y-%m-%dT%H:%M:%SZ")
                 + _dt.timedelta(days=6, hours=13)).strftime("%Y-%m-%dT%H:%M:%SZ")
        code_e, line_e = expiry(lroot, SESSION, now=later)
        check("expiry-replace-due-near-seven-days", code_e == 1 and "replace due" in line_e, line_e)
        try:
            reset_state(lroot, "99999999-0000-0000-0000-000000000000")
            check("reset-state-refuses-another-session", False)
        except GuardError as exc:
            check("reset-state-refuses-another-session", "owning --session" in str(exc), str(exc))
        try:
            reset_state(lroot, expect_no_guard=True)
            check("reset-state-refuses-while-a-guard-lives", False)
        except GuardError as exc:
            check("reset-state-refuses-while-a-guard-lives", "a guard exists" in str(exc), str(exc))
        msg = end(lroot, SESSION, "park")
        check("end-leaves-a-pending-cancellation",
              pending_cancel(lroot).startswith("pending-cancel: CronDelete job-b") and "cancel-confirmed" in msg,
              pending_cancel(lroot))
        try:
            cancel_confirmed(lroot, "job-zzz")
            check("cancel-confirmed-refuses-another-job", False)
        except GuardError as exc:
            check("cancel-confirmed-refuses-another-job", "no pending cancellation names job job-zzz" in str(exc), str(exc))
        check("cancel-confirmed-clears-the-pending-record",
              cancel_confirmed(lroot, "job-b").startswith("cancel-confirmed: job job-b is gone")
              and pending_cancel(lroot) == "")
        check("reconcile-orphan-job", reconcile(lroot, SESSION, ["job-x"])
              == ["reconcile: orphan job job-x has no guard: CronDelete it"])
        # Two unconfirmed cancellations are both kept (panel round 1).
        with open(run, "w", encoding="utf-8") as fh:
            fh.write("# run\n")
        acquire(lroot, SESSION, 0, RUN_FILE, "job-p1")
        end(lroot, SESSION, "operator-stop")
        acquire(lroot, SESSION, 0, RUN_FILE, "job-p2")
        end(lroot, SESSION, "operator-stop")
        pend = pending_cancel(lroot)
        check("pending-cancellations-accumulate", "CronDelete job-p1" in pend and "CronDelete job-p2" in pend, pend)
        check("reconcile-surfaces-a-pending-cancellation",
              any("pending cancellation is unconfirmed: CronDelete job-p1" in r for r in reconcile(lroot, SESSION, [])))
        cancel_confirmed(lroot, "job-p1")
        cancel_confirmed(lroot, "job-p2")
        check("pending-cancellations-drain-one-by-one", pending_cancel(lroot) == "", pending_cancel(lroot))

        def _interleave(change: str) -> tuple:
            """Hold the guard lock in another process, start the hook (it
            reads the guard, then waits on the lock), apply `change` under
            the held lock, release, and return what the hook did."""
            for f in _paths(lroot):
                if os.path.exists(f):
                    os.remove(f)
            with open(run, "w", encoding="utf-8") as fh:
                fh.write("# run\n")
            acquire(lroot, SESSION, 0, RUN_FILE, "job-i")
            # The fixture copy signals just before the hook takes the lock. It
            # is written before the holder starts, so a moved anchor fails
            # here without orphaning a lock holder.
            with open(os.path.join(HERE, "campaign_guard.py"), encoding="utf-8") as fh:
                hook_text = fh.read()
            lock_line = '    lock = _guard_lock(root, min(_env_int("CAMPAIGN_LOCK_WAIT_MS", 5000), int(_remaining(deadline) * 1000)))\n'
            assert hook_text.count(lock_line) == 1, "hook lock boundary changed"
            fixture_hook = os.path.join(ltmp, "interleave_hook.py")
            with open(fixture_hook, "w", encoding="utf-8") as fh:
                fh.write(hook_text.replace(lock_line,
                    "    open(os.environ['RF_TEST_HOOK_READY'], 'w').write('ready')\n" + lock_line))
            go = os.path.join(ltmp, "go.flag")
            if os.path.exists(go):
                os.remove(go)
            holder = subprocess.Popen(
                [sys.executable, "-c",
                 "import os, sys, time; sys.path.insert(0, sys.argv[1]); import campaign_guard as cg\n"
                 "root, change, go = sys.argv[2], sys.argv[3], sys.argv[4]\n"
                 "with cg._Lock(root):\n"
                 "    print('held', flush=True)\n"
                 "    while not os.path.exists(go): time.sleep(0.05)\n"
                 "    if change == 'handover':\n"
                 "        cg._acquire_locked(root, 'cccccccc-0000-0000-0000-000000000000', 0, cg.RUN_FILE, 'job-h', 'drill')\n"
                 "    elif change == 'rescope':\n"
                 "        g = cg.read_guard(root)\n"
                 "        cg._acquire_locked(root, g['session_id'], 0, cg.RUN_FILE, 'job-i', None, None, 'phase')\n"
                 "    else:\n"
                 "        g = cg.read_guard(root)\n"
                 "        cg._end_locked(root, g['session_id'], 'operator-stop', None)\n"
                 "    time.sleep(0.5)",
                 HERE, lroot, change, go], stdout=subprocess.PIPE, text=True)
            holder.stdout.readline()
            # The payload rides a file, so communicate() never touches a pipe
            # this test closed (panel round 2).
            payload_file = os.path.join(ltmp, "payload.json")
            with open(payload_file, "w", encoding="utf-8") as fh:
                json.dump({"session_id": SESSION, "cwd": lroot, "hook_event_name": "Stop"}, fh)
            payload_in = open(payload_file, "rb")
            # Signal from a fixture copy after reading the old guard, immediately
            # before taking its lock. Startup speed cannot establish this order.
            ready = os.path.join(ltmp, "hook-ready.flag")
            if os.path.exists(ready):
                os.remove(ready)
            hook = subprocess.Popen([sys.executable, fixture_hook, "stop-hook"],
                                    stdin=payload_in, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, encoding="utf-8", errors="replace",
                                    env=dict(os.environ, CLAUDE_PROJECT_DIR=lroot, RF_TEST_HOOK_READY=ready))
            import time as _t
            deadline = _t.monotonic() + 60
            while not os.path.exists(ready) and hook.poll() is None and _t.monotonic() < deadline:
                _t.sleep(0.05)
            reached_lock = os.path.exists(ready)
            open(go, "w").close()
            out_h, err_h = hook.communicate(timeout=60)
            payload_in.close()
            holder.wait()
            assert reached_lock, f"hook did not reach lock boundary: {out_h!r} {err_h!r}"
            return out_h.strip(), err_h, _state(lroot), read_guard(lroot)

        out_h, err_h, st_h, g_h = _interleave("handover")
        check("a-handover-while-the-hook-waits-writes-no-state",
              out_h == "" and "guard changed while this hook waited" in err_h and st_h == {}
              and g_h["session_id"].startswith("cccccccc"), f"{out_h!r} {err_h!r} {st_h} {g_h}")
        out_h, err_h, st_h, g_h = _interleave("rescope")
        check("a-scope-change-while-the-hook-waits-writes-no-state",
              out_h == "" and "guard changed while this hook waited" in err_h and st_h == {}
              and g_h["scope"] == "phase", f"{out_h!r} {err_h!r} {st_h} {g_h}")
        out_h, err_h, st_h, g_h = _interleave("end")
        check("an-end-while-the-hook-waits-writes-no-state",
              out_h == "" and "guard changed while this hook waited" in err_h and st_h == {} and g_h is None,
              f"{out_h!r} {err_h!r} {st_h} {g_h}")
        for j in ("job-i",):
            try:
                cancel_confirmed(lroot, j)
            except GuardError:
                pass
        check("reset-state-with-no-guard", reset_state(lroot, expect_no_guard=True).startswith("reset-state:"))
        check("whoami-no-guard", whoami(lroot, SESSION, g2) == "NO GUARD")

        # D00 T04 §36: the fingerprint's degraded coverage, pinned at small
        # bounds: content past the hash bound counts only through size and
        # write time, and past the stat bound only through the name.
        with open(run, "w", encoding="utf-8") as fh:
            fh.write("# run\n")
        acquire(lroot, SESSION, 0, RUN_FILE, "job-c")
        cov = os.path.join(lroot, "cov")
        os.makedirs(cov)
        for n in range(6):
            with open(os.path.join(cov, f"f{n}.txt"), "w", encoding="utf-8") as fh:
                fh.write("aaaa\n")
        env_bounds = {"CAMPAIGN_HASH_FILES": "2", "CAMPAIGN_STAT_FILES": "2"}
        os.environ.update(env_bounds)
        try:
            def _tripped_then(path: str, keep_time: bool) -> dict:
                for _ in range(4):
                    run_hook(lroot, SESSION)
                st_before = os.stat(path)
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write("bbbb\n")
                if keep_time:
                    os.utime(path, ns=(st_before.st_atime_ns, st_before.st_mtime_ns))
                run_hook(lroot, SESSION)
                return _state(lroot)
            st = _tripped_then(os.path.join(cov, "f0.txt"), keep_time=True)
            check("coverage-a-hashed-file-edit-counts-even-with-its-time-kept", st.get("trips") == 0, str(st))
            st = _tripped_then(os.path.join(cov, "f2.txt"), keep_time=True)
            check("coverage-past-the-hash-bound-a-same-size-same-time-edit-is-invisible", st.get("trips") == 1, str(st))
            st = _tripped_then(os.path.join(cov, "f3.txt"), keep_time=False)
            check("coverage-past-the-hash-bound-a-time-change-counts", st.get("trips") == 0, str(st))
            st = _tripped_then(os.path.join(cov, "f5.txt"), keep_time=False)
            check("coverage-past-the-stat-bound-only-the-name-counts", st.get("trips") == 1, str(st))
            os.environ["CAMPAIGN_HASH_BYTES"] = "3"
            try:
                st = _tripped_then(os.path.join(cov, "f1.txt"), keep_time=True)
                check("coverage-a-file-past-the-size-bound-counts-by-size-and-time", st.get("trips") == 1, str(st))
            finally:
                del os.environ["CAMPAIGN_HASH_BYTES"]
        finally:
            for k in env_bounds:
                del os.environ[k]

    # Conclave-SDK todo kit (2026-10-21): a phase-scoped run covers its one
    # phase. The hook never ends it on the plan's count or points it at
    # another phase's row, a re-point or handover keeps the scope, and
    # whoami tells the heartbeat which skill to resume.
    with tempfile.TemporaryDirectory(prefix="campaign-scope-") as stmp:
        sroot = _workspace(stmp)
        try:
            acquire(sroot, SESSION, 2, RUN_FILE, "job-s", scope="everything")
            check("acquire-refuses-an-unknown-scope", False)
        except GuardError as exc:
            check("acquire-refuses-an-unknown-scope", "is not one of plan, phase" in str(exc), str(exc))
        msg = acquire(sroot, SESSION, 2, RUN_FILE, "job-s", scope="phase")
        sg = read_guard(sroot)
        check("acquire-records-a-phase-scope", sg.get("scope") == "phase" and "scope phase" in msg, msg)
        check("whoami-names-a-phase-scope", whoami(sroot, SESSION, sg["generation"])
              == f"OWNER run={sg['run_id']} scope=phase", whoami(sroot, SESSION, sg["generation"]))
        _set_ready(sroot, 0)
        code, out, err = run_hook(sroot, SESSION)
        check("a-phase-run-is-not-ended-by-the-plan-count",
              isinstance(out, dict) and out.get("decision") == "block", f"{code} {out} {err}")
        _set_ready(sroot, 3)
        code, out, err = run_hook(sroot, SESSION)
        reason = out.get("reason", "") if isinstance(out, dict) else ""
        check("a-phase-run-blocks-for-its-own-phase-only",
              "then the next section of Phase 2 only (this run covers that phase)" in reason
              and "then the next phase" not in reason and "Next ready row" not in reason, f"{out} {err}")
        try:
            end(sroot, SESSION, "plan-done")
            check("plan-done-is-refused-for-a-phase-run", False)
        except GuardError as exc:
            check("plan-done-is-refused-for-a-phase-run",
                  "covers one phase" in str(exc) and read_guard(sroot) is not None, str(exc))
        srun = os.path.join(sroot, RUN_FILE)
        with open(srun, encoding="utf-8") as fh:
            sbefore = fh.read()
        with open(srun, "a", encoding="utf-8") as fh:
            fh.write(f"\nPARKED 2099-01-01T00:00:00Z run={sg['run_id']} every leftover row is blocked\n")
        code, out, err = run_hook(sroot, SESSION)
        check("a-parked-marker-carrying-the-run-id-ends-a-phase-run", code == 0 and out is None, f"{out} {err}")
        with open(srun, "w", encoding="utf-8") as fh:
            fh.write(sbefore)
        acquire(sroot, SESSION, 2, RUN_FILE, "job-s2")
        check("a-re-point-keeps-the-scope", read_guard(sroot).get("scope") == "phase", str(read_guard(sroot)))
        acquire(sroot, "22222222-0000-0000-0000-000000000000", 2, RUN_FILE, "job-s3", handover="drill")
        check("a-handover-of-the-same-run-keeps-the-scope", read_guard(sroot).get("scope") == "phase",
              str(read_guard(sroot)))
        acquire(sroot, SESSION, 2, RUN_FILE, "job-s4", handover="back", scope="plan")
        check("a-given-scope-replaces-the-old-one", read_guard(sroot).get("scope") == "plan", str(read_guard(sroot)))
        code, out, err = run_hook(sroot, SESSION)
        check("a-plan-run-blocks-naming-the-next-row",
              isinstance(out, dict) and "then the next phase" in out.get("reason", "")
              and "Next ready row: 00-workspace/TODO-01-bootstrap.md §3" in out.get("reason", ""), f"{out} {err}")
        rid = read_guard(sroot)["run_id"]
        with open(os.path.join(sroot, "build", "claude-campaign-state.json"), "w", encoding="utf-8") as fh:
            json.dump({"fingerprint": "", "blocks": 0, "trips": 0, "stalled": False,
                       "hook_error": "an earlier failure", "hook_error_at": "2099-01-01T00:00:00Z"}, fh)
        run_hook(sroot, SESSION)
        check("an-unacknowledged-hook-error-survives-later-hook-runs",
              _state(sroot).get("hook_error") == "an earlier failure" and _state(sroot).get("blocks") == 1,
              str(_state(sroot)))
        hook_error(sroot, SESSION, ack=hook_error(sroot, SESSION))
        with open(os.path.join(sroot, RUN_FILE), "a", encoding="utf-8") as fh:
            fh.write(f"\n## Closeout run={rid}\n\nshipped\n")
        code, out, err = run_hook(sroot, SESSION)
        check("a-closeout-carrying-the-run-id-ends-a-scoped-run", code == 0 and out is None, f"{out} {err}")
        with open(os.path.join(sroot, RUN_FILE), "w", encoding="utf-8-sig") as fh:
            fh.write(f"## Closeout run={rid}\n\nshipped\n")
        code, out, err = run_hook(sroot, SESSION)
        check("a-byte-order-mark-never-hides-a-first-line-marker", code == 0 and out is None, f"{out} {err}")
        with open(os.path.join(sroot, RUN_FILE), "w", encoding="utf-8") as fh:
            fh.write("# run\n")
        # The lock wait counts against the budget: a lock held past it fails
        # open instead of blocking late.
        holder = subprocess.Popen(
            [sys.executable, "-c",
             "import sys, time; sys.path.insert(0, sys.argv[1]); import campaign_guard as cg\n"
             "with cg._Lock(sys.argv[2]):\n"
             "    print('held', flush=True); time.sleep(4)",
             HERE, sroot], stdout=subprocess.PIPE, text=True)
        holder.stdout.readline()
        os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"] = "2"
        try:
            began = time.monotonic()
            code, out, err = run_hook(sroot, SESSION)
            took = time.monotonic() - began
        finally:
            del os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"]
            holder.wait()
        folder = os.path.join(sroot, "build", "claude-campaign-hook-errors")
        logged = "".join(open(os.path.join(folder, n), encoding="utf-8").read()
                         for n in sorted(os.listdir(folder))) if os.path.isdir(folder) else ""
        check("the-lock-wait-counts-against-the-budget",
              code == 0 and out is None and took < 3.8
              and ("guard lock stayed held" in logged or "time budget" in logged),
              f"{took:.1f}s {out} {err} {logged!r}")
        if os.path.isdir(folder):
            shutil.rmtree(folder)
        os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"] = "0"
        try:
            code, out, err = run_hook(sroot, SESSION)
        finally:
            del os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"]
        check("an-exhausted-budget-fails-open-and-is-recorded",
              code == 0 and out is None and "time budget" in err
              and "time budget" in str(_state(sroot).get("hook_error", "")), f"{out} {err} {_state(sroot)}")
        # A second failure never replaces an unacknowledged first: it lands
        # in a file of its own, reported after the first is acknowledged.
        first = hook_error(sroot, SESSION)
        os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"] = "1"
        open(os.path.join(sroot, "scripts", "slow.txt"), "w").close()
        try:
            began = time.monotonic()
            code, out, err = run_hook(sroot, SESSION)
            took = time.monotonic() - began
        finally:
            del os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"]
            os.remove(os.path.join(sroot, "scripts", "slow.txt"))
        check("the-budget-bounds-a-slow-plan-query",
              code == 0 and out is None and "time budget" in err and took < 4.5, f"{took:.1f}s {out} {err}")
        check("a-second-failure-keeps-the-first-error", hook_error(sroot, SESSION) == first, hook_error(sroot, SESSION))
        hook_error(sroot, SESSION, ack=first)
        second = hook_error(sroot, SESSION)
        check("the-second-failure-is-reported-after-the-first",
              second.startswith("campaign-stop hook failed at ") and "time budget" in second
              and "recorded without the lock" in second, second)
        hook_error(sroot, SESSION, ack=second)
        if hasattr(os, "mkfifo"):
            os.remove(srun)
            os.mkfifo(srun)
            began = time.monotonic()
            code, out, err = run_hook(sroot, SESSION)
            took = time.monotonic() - began
            check("a-fifo-run-file-fails-open-at-once",
                  code == 0 and out is None and "not a regular file" in err and took < 10
                  and "not a regular file" in str(_state(sroot).get("hook_error", "")), f"{took:.1f}s {out} {err}")
            hook_error(sroot, SESSION, ack=hook_error(sroot, SESSION))
            os.remove(srun)
            with open(srun, "w", encoding="utf-8") as fh:
                fh.write("# run\n")
            # git does not list a FIFO as untracked, so this pins end to end
            # that one in the tree never reaches a blocking read.
            os.mkfifo(os.path.join(sroot, "untracked.fifo"))
            began = time.monotonic()
            code, out, err = run_hook(sroot, SESSION)
            took = time.monotonic() - began
            check("an-untracked-fifo-never-blocks-the-fingerprint",
                  isinstance(out, dict) and out.get("decision") == "block" and took < 10, f"{took:.1f}s {out} {err}")
            os.remove(os.path.join(sroot, "untracked.fifo"))
            guard_path, state_path = _paths(sroot)
            os.rename(state_path, state_path + ".saved")
            os.mkfifo(state_path)
            began = time.monotonic()
            code, out, err = run_hook(sroot, SESSION)
            took = time.monotonic() - began
            os.remove(state_path)
            os.rename(state_path + ".saved", state_path)
            check("a-fifo-state-file-fails-open-at-once",
                  code == 0 and out is None and "not a regular file" in err and took < 10, f"{took:.1f}s {out} {err}")
            shutil.rmtree(os.path.join(sroot, "build", "claude-campaign-hook-errors"), ignore_errors=True)
        # Whatever blocks inside the decision, the joined worker bounds the
        # hook: here the fingerprint itself hangs past the budget.
        real_fingerprint = globals()["fingerprint"]
        globals()["fingerprint"] = lambda *args, **kwargs: time.sleep(10)
        os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"] = "1"
        try:
            began = time.monotonic()
            code, out = stop_hook(json.dumps({"session_id": SESSION, "cwd": sroot}), sroot)
            took = time.monotonic() - began
        finally:
            globals()["fingerprint"] = real_fingerprint
            del os.environ["CAMPAIGN_HOOK_BUDGET_SECONDS"]
        check("the-budget-bounds-a-decision-that-hangs",
              code == 0 and out == "" and took < 4 and "time budget" in hook_error(sroot, SESSION),
              f"{took:.1f}s {out!r} {hook_error(sroot, SESSION)}")
        hook_error(sroot, SESSION, ack=hook_error(sroot, SESSION))
        rid = read_guard(sroot)["run_id"]
        with open(os.path.join(sroot, RUN_FILE), "w", encoding="utf-8-sig") as fh:
            fh.write(f"## Closeout run={rid}\n\nshipped\n")
        code, out, err = run_hook(sroot, SESSION)
        try:
            ended = end(sroot, SESSION, "closeout")
        except GuardError as exc:
            ended = str(exc)
        check("a-bom-closeout-that-ends-the-hook-also-ends-the-run",
              out is None and ended.startswith("end:") and read_guard(sroot) is None, f"{out} {ended}")

    # Owner decision 2026-10-27: a harness with no Stop hook (Muse) still
    # terminates a stuck run: consecutive unchanged heartbeat resumptions
    # count trips on the same counter the hook feeds.
    with tempfile.TemporaryDirectory(prefix="campaign-still-") as htmp:
        hroot = _workspace(htmp)
        acquire(hroot, SESSION, 3, RUN_FILE, "job-h")
        msg = heartbeat_still(hroot, SESSION)
        check("heartbeat-still-baselines-first",
              "progress" in msg and _state(hroot).get("hb_still") == 0, f"{msg} {_state(hroot)}")
        for _ in range(MAX_HEARTBEAT_STILLS - 1):
            heartbeat_still(hroot, SESSION)
        st = _state(hroot)
        check("heartbeat-still-counts-unchanged-resumptions",
              st.get("hb_still") == MAX_HEARTBEAT_STILLS - 1 and st.get("trips") == 0, str(st))
        msg = heartbeat_still(hroot, SESSION)
        st = _state(hroot)
        check("heartbeat-still-trips-at-the-bound",
              "trip 1" in msg and st.get("trips") == 1 and st.get("hb_still") == 0, f"{msg} {st}")
        with open(os.path.join(hroot, "progress.txt"), "w", encoding="utf-8") as fh:
            fh.write("moved\n")
        msg = heartbeat_still(hroot, SESSION)
        st = _state(hroot)
        check("heartbeat-still-progress-resets-stills-and-trips",
              "progress" in msg and st.get("hb_still") == 0 and st.get("trips") == 0, f"{msg} {st}")
        code, out, _ = run_hook(hroot, SESSION)
        st = _state(hroot)
        check("hook-blocks-preserve-heartbeat-counters",
              isinstance(out, dict) and st.get("hb_fingerprint") and st.get("hb_still") == 0,
              f"{out} {st}")
        try:
            heartbeat_still(hroot, "99999999-0000-0000-0000-000000000000")
            check("heartbeat-still-refuses-another-session", False)
        except GuardError as exc:
            check("heartbeat-still-refuses-another-session", "owning --session" in str(exc), str(exc))

    # D00 T04 §35, §37: the repair episode survives a restart, refuses a
    # fourth attempt, binds its identity, counts a repeat once, closes only
    # on proven green, and is restored from the run file when lost.
    with tempfile.TemporaryDirectory(prefix="campaign-repair-") as rtmp:
        os.makedirs(os.path.join(rtmp, "build"))
        os.makedirs(os.path.join(rtmp, "docs"))
        for args in (["init", "-q"], ["config", "user.email", "t@t"], ["config", "user.name", "t"],
                     ["config", "commit.gpgsign", "false"]):
            subprocess.run(["git", *args], cwd=rtmp, capture_output=True, check=True)
        with open(os.path.join(rtmp, ".gitignore"), "w", encoding="utf-8") as fh:
            fh.write("build/\n.runner/\n")
        shas = []
        for n in range(6):
            with open(os.path.join(rtmp, "f.txt"), "w", encoding="utf-8") as fh:
                fh.write(f"{n}\n")
            subprocess.run(["git", "add", "-A"], cwd=rtmp, capture_output=True, check=True)
            subprocess.run(["git", "commit", "-qm", f"c{n}"], cwd=rtmp, capture_output=True, check=True)
            shas.append(subprocess.run(["git", "rev-parse", "HEAD"], cwd=rtmp, capture_output=True,
                                       text=True).stdout.strip())
        runf = "docs/run.md"
        with open(os.path.join(rtmp, runf), "w", encoding="utf-8") as fh:
            fh.write("# run\n")

        def _repair_cli(*args: str) -> subprocess.CompletedProcess:
            return subprocess.run([sys.executable, os.path.join(HERE, "campaign_guard.py"), "repair", *args,
                                   "--root", rtmp], capture_output=True, text=True, encoding="utf-8")
        W = ("--workflow", "plan-gates", "--run-file", runf)
        outs = [_repair_cli("attempt", "--red", shas[n - 1], "--commit", shas[n], *W) for n in (1, 2, 3)]
        check("repair-attempts-count-across-processes",
              [o.returncode for o in outs] == [0, 0, 0] and "attempt 3 of 3" in outs[2].stdout
              and f"episode {shas[0][:12]}" in outs[2].stdout, str([o.stdout + o.stderr for o in outs]))
        again = _repair_cli("attempt", "--red", shas[1], "--commit", shas[2], *W)
        check("repair-counts-a-repeated-attempt-once",
              again.returncode == 0 and "already counted" in again.stdout, again.stdout + again.stderr)
        st = _repair_cli("status", "--run-file", runf)
        check("repair-status-reads-the-persisted-episode", "3 of 3 attempts used" in st.stdout, st.stdout)
        fourth = _repair_cli("attempt", "--red", shas[3], "--commit", shas[4], *W)
        check("repair-refuses-a-fourth-attempt-after-a-restart",
              fourth.returncode == 1 and "bound is exhausted, escalate" in fourth.stderr, fourth.stderr)
        other = _repair_cli("attempt", "--red", shas[3], "--commit", shas[4], "--workflow", "release",
                            "--run-file", runf)
        check("repair-refuses-a-mismatched-episode",
              other.returncode == 1 and "belongs to workflow 'plan-gates'" in other.stderr, other.stderr)
        with open(os.path.join(rtmp, "build", "claude-campaign-repair.json"), encoding="utf-8") as fh:
            saved = fh.read()
        for label, body in (("corrupt", "{not json"), ("malformed", '{"episode": 3}'), ("null", "null")):
            with open(os.path.join(rtmp, "build", "claude-campaign-repair.json"), "w", encoding="utf-8") as fh:
                fh.write(body)
            bad = _repair_cli("attempt", "--red", shas[3], "--commit", shas[5], *W)
            check(f"repair-refuses-{label}-state-rather-than-resetting",
                  bad.returncode == 1 and "escalate rather than repair" in bad.stderr, bad.stderr)
        with open(os.path.join(rtmp, "build", "claude-campaign-repair.json"), "w", encoding="utf-8") as fh:
            fh.write(saved)
        no_ev = _repair_cli("close", "--green", shas[5], *W, "--evidence", f"ci-wait: {shas[5][:12]} plan-gates started no run")
        check("repair-close-refuses-without-green-evidence",
              no_ev.returncode == 1 and "needs --evidence with the green ci-wait line" in no_ev.stderr, no_ev.stderr)
        not_desc = _repair_cli("close", "--green", shas[1], *W, "--evidence", f"ci-wait: {shas[1][:12]} plan-gates success x")
        check("repair-close-refuses-a-green-that-is-not-a-descendant",
              not_desc.returncode == 1 and "does not descend from the last attempt" in not_desc.stderr, not_desc.stderr)
        closed = _repair_cli("close", "--green", shas[5], *W, "--evidence", f"ci-wait: {shas[5][:12]} plan-gates success https://x")
        again2 = _repair_cli("attempt", "--red", shas[4], "--commit", shas[5], *W)
        check("repair-close-opens-a-fresh-episode",
              "closed green" in closed.stdout and again2.returncode == 0 and "attempt 1 of 3" in again2.stdout,
              f"{closed.stdout}{closed.stderr} {again2.stdout}{again2.stderr}")
        # A lost episode file is detected from the run file and restored.
        with open(os.path.join(rtmp, runf), "a", encoding="utf-8") as fh:
            fh.write(again2.stdout)
        os.remove(os.path.join(rtmp, "build", "claude-campaign-repair.json"))
        lost = _repair_cli("status", "--run-file", runf)
        check("repair-status-detects-a-lost-episode-file",
              lost.returncode == 1 and "the episode file is lost" in lost.stderr, lost.stdout + lost.stderr)
        blocked = _repair_cli("attempt", "--red", shas[5], "--commit", shas[3], *W)
        check("repair-attempt-refuses-until-restored",
              blocked.returncode == 1 and "run repair restore first" in blocked.stderr, blocked.stderr)
        no_wf = _repair_cli("restore", "--run-file", runf)
        check("repair-restore-refuses-without-a-workflow",
              no_wf.returncode == 1 and "needs --workflow and --run-file" in no_wf.stderr, no_wf.stderr)
        # An `already counted` line in the run file is not a second attempt.
        with open(os.path.join(rtmp, runf), "a", encoding="utf-8") as fh:
            fh.write(again2.stdout.strip() + " already counted\n")
        restored = _repair_cli("restore", *W)
        check("repair-restore-rebuilds-from-the-run-file",
              restored.returncode == 0 and "restored from docs/run.md at 1 of 3 attempts" in restored.stdout,
              restored.stdout + restored.stderr)
        full_retry = _repair_cli("attempt", "--red", shas[4], "--commit", shas[5], *W)
        check("repair-restored-attempts-compare-by-full-sha",
              full_retry.returncode == 0 and "already counted" in full_retry.stdout,
              full_retry.stdout + full_retry.stderr)
        c1 = _repair_cli("ceiling", "--run-id", "777")
        c2 = _repair_cli("ceiling", "--run-id", "777")
        check("repair-ceiling-allows-one-re-run-per-run",
              c1.returncode == 0 and c2.returncode == 1 and "already had its one re-run" in c2.stderr,
              c1.stdout + c2.stderr)

    # D00 T04 §34: the runner's contract routes through these commands.
    plan_skill = os.path.normpath(os.path.join(HERE, "..", ".agents", "skills", "process-plan", "SKILL.md"))
    try:
        with open(plan_skill, encoding="utf-8") as fh:
            skill = fh.read()
    except OSError:
        skill = ""
    for pin, needle in (("skill-acquires-the-guard", "python3 scripts/campaign_guard.py acquire --session"),
                        ("skill-ends-through-end", "python3 scripts/campaign_guard.py end --session"),
                        ("skill-heartbeat-reports-hook-errors", "python3 scripts/campaign_guard.py hook-error"),
                        ("skill-refused-acquire-cancels-its-job", "A refused `acquire` means another session owns the run"),
                        ("skill-heartbeat-checks-ownership", "NOT THE OWNER or NOT THE CURRENT JOB"),
                        ("skill-heartbeat-ends-through-end", "--reason plan-done`, CronDelete this job"),
                        ("skill-heartbeat-owner-first",
                         "1. Run `python3 scripts/campaign_guard.py whoami --session"),
                        ("skill-reconciles-before-starting", "python3 scripts/campaign_guard.py reconcile --session"),
                        ("skill-checks-health-at-each-boundary", "python3 scripts/campaign_guard.py health --session"),
                        ("skill-confirms-the-cancel", "python3 scripts/campaign_guard.py cancel-confirmed --cron-id"),
                        ("skill-marks-bookkeeping", "- bookkeeping: heartbeat resumed"),
                        ("skill-heartbeat-confirms-before-clearing",
                         "CronDelete this job, confirm it gone with CronList, then run `cancel-confirmed`"),
                        ("skill-markers-carry-the-run-id", "a closeout is a line `## Closeout run=<run id>`"),
                        ("skill-acquires-a-plan-scope", "--generation <generation> --scope plan"),
                        ("skill-heartbeat-ends-a-phase-run-on-its-marker",
                         "and step 1 printed scope=phase, the one phase asked for is over"),
                        ("skill-heartbeat-resumes-by-scope",
                         "under process-phase for Phase <N> only when step 1 printed scope=phase")):
        check(pin, needle in skill, plan_skill)
    phase_skill = os.path.normpath(os.path.join(HERE, "..", ".agents", "skills", "process-phase", "SKILL.md"))
    try:
        with open(phase_skill, encoding="utf-8") as fh:
            check("skill-a-standalone-phase-acquires-a-phase-scope", "`acquire --scope phase`" in fh.read(), phase_skill)
    except OSError as exc:
        check("skill-a-standalone-phase-acquires-a-phase-scope", False, str(exc))

    # A stock Windows console writes cp1252. The hook asks its child for
    # the UTF-8 it decodes, so a producer that keeps the console code page
    # (the fake graph does not reconfigure its streams) still names the row.
    with tempfile.TemporaryDirectory(prefix="campaign-guard-enc-") as etmp:
        eroot = _workspace(etmp)
        _guard(eroot)
        code, out, err = run_hook(eroot, SESSION, env_extra={"PYTHONIOENCODING": "cp1252"})
        check("the-next-row-survives-a-legacy-console-encoding",
              code == 0 and isinstance(out, dict)
              and "TODO-01-bootstrap.md §3  Red CI repaired" in out.get("reason", ""), f"{code} {out} {err}")
        # The guard's own streams print what cp1252 cannot encode: a
        # successful command names a run file outside that code page.
        wide = "docs/phase-runs/2099-01-01-\u0100.md"
        with open(os.path.join(eroot, wide), "w", encoding="utf-8") as fh:
            fh.write("# Phase run\n")
        ok = subprocess.run([sys.executable, os.path.join(HERE, "campaign_guard.py"), "acquire", "--root", eroot,
                             "--session", SESSION, "--phase", "0", "--run-file", wide, "--cron-id", "job-1"],
                            capture_output=True, env=dict(os.environ, PYTHONIOENCODING="cp1252"))
        oout = ok.stdout.decode("utf-8", "replace")
        check("the-guard-prints-on-a-legacy-console-encoding",
              ok.returncode == 0 and wide in oout and "acquire:" in oout,
              f"{ok.returncode} {oout[-300:]} {ok.stderr.decode('utf-8', 'replace')[-400:]}")

    print(f"campaign_guard self-test: {passed + failed} cases, {failed} failed")
    return 1 if failed else 0


def _opts(argv: list[str]) -> dict:
    out: dict = {}
    i = 0
    while i < len(argv):
        if not argv[i].startswith("--") or i + 1 >= len(argv):
            raise GuardError(f"bad argument {argv[i]!r}")
        out[argv[i][2:].replace("-", "_")] = argv[i + 1]
        i += 2
    return out


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        return _self_test()
    if argv == ["stop-hook"]:
        code, out = stop_hook(sys.stdin.read(), os.environ.get("CLAUDE_PROJECT_DIR"))
        if out:
            print(out)
        # A decision worker past its budget may still be blocked: exit
        # without waiting on it.
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(code)
    if not argv:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    try:
        if argv[0] == "repair" and len(argv) >= 2:
            ropts = _opts(argv[2:])
            rroot = ropts.pop("root", REPO)
            code, line = repair(rroot, argv[1], ropts.get("red", ""), ropts.get("commit", ""),
                                ropts.get("green", ""), ropts.get("workflow", ""), ropts.get("run_file", ""),
                                ropts.get("evidence", ""), ropts.get("run_id", ""))
            print(line, file=sys.stdout if code == 0 else sys.stderr)
            return code
        opts = _opts(argv[1:])
        root = opts.pop("root", REPO)
        if argv[0] == "acquire":
            print(acquire(root, opts.get("session", ""), int(opts.get("phase", "0")),
                          opts.get("run_file", ""), opts.get("cron_id", ""), opts.get("handover"),
                          opts.get("generation"), opts.get("scope")))
            return 0
        if argv[0] == "end":
            print(end(root, opts.get("session", ""), opts.get("reason", "")))
            return 0
        if argv[0] == "hook-error" and set(opts) <= {"session", "ack"}:
            line = hook_error(root, opts.get("session"), opts.get("ack"))
            if line:
                print(line)
            return 0
        if argv[0] == "heartbeat-still":
            print(heartbeat_still(root, opts.get("session", "")))
            return 0
        if argv[0] == "mint-generation" and not opts:
            print(mint_generation())
            return 0
        if argv[0] == "whoami":
            print(whoami(root, opts.get("session", ""), opts.get("generation")))
            return 0
        if argv[0] == "reset-state":
            print(reset_state(root, opts.get("session"), opts.get("expect_no_guard") == "yes"))
            return 0
        if argv[0] == "pending-cancel" and not opts:
            line = pending_cancel(root)
            if line:
                print(line)
            return 0
        if argv[0] == "cancel-confirmed":
            print(cancel_confirmed(root, opts.get("cron_id", "")))
            return 0
        if argv[0] in ("reconcile", "health"):
            jobs = [j for j in opts.get("jobs", "").split(",") if j]
            if argv[0] == "reconcile":
                print("\n".join(reconcile(root, opts.get("session", ""), jobs)))
                return 0
            code, line = health(root, opts.get("session", ""), jobs)
            print(line, file=sys.stdout if code == 0 else sys.stderr)
            return code
        if argv[0] == "expiry":
            code, line = expiry(root, opts.get("session", ""), opts.get("now"))
            print(line, file=sys.stdout if code == 0 else sys.stderr)
            return code
    except (GuardError, ValueError) as exc:
        print(f"campaign_guard: {exc}", file=sys.stderr)
        return 1
    print(f"campaign_guard: unknown command {' '.join(argv)!r}; see the module docstring", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
