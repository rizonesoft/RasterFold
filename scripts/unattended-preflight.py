#!/usr/bin/env python3
"""Fail-loud preflight for an unattended RasterFold campaign run.

An unattended run that starts against a broken tree, a stale plan, a missing
reviewer, or a nearly full disk does not fail politely: it stalls, parks, or
fills the disk. This checks every load-bearing precondition the process-plan
run guard assumes, prints one PASS/FAIL line each, and exits 1 on any FAIL.
It starts nothing and writes nothing outside build/. With --repair (the
launcher passes it) a stale compilation database is rebuilt through
gen-compdb.py --rebuild-stale, which writes only ignored build output
(build/, bin/, the root compile_commands.json), and only after the branch,
clean-tree and guard checks passed, so nothing is rebuilt under a live writer.

Two runners launch it (operator directive 2026-11-12): Claude Code
(`--runner claude`, the default) and Antigravity on Gemini 4 Carbon
(`--runner gemini`). For Claude Code the launch preconditions are its CLI and
the reviewer's, the campaign Stop hook in .claude/settings.json, the two
dialogs that would sit unanswered, the session cron tools, and the
language-server plugin. For Antigravity they are the agy CLI and the
reviewer's and the campaign Stop hook in .agents/hooks.json. The read-only
Sonnet scout roster is a repository invariant and is checked for both.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAILURES: list[str] = []
# Each runner and the CLI its launch execs (docs/unattended-run.md).
RUNNERS = {"claude": "claude", "gemini": "agy"}
CANCEL_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)


class CancelFlag:
    """The cancellation flag a signal handler sets. One attribute store and nothing else: no lock, so a second
    signal that arrives while the first handler is still running cannot block on anything the first one holds
    (a threading.Event deadlocks exactly there)."""
    __slots__ = ("value",)

    def __init__(self) -> None:
        self.value = False

    def set(self) -> None:
        self.value = True

    def is_set(self) -> bool:
        return self.value

    def clear(self) -> None:
        self.value = False


CANCELLED = CancelFlag()
CLAUDE_HOME = Path.home() / ".claude"
CLAUDE_STATE = Path.home() / ".claude.json"
SCOUTS = ("rf-fact-check", "rf-log-digest", "rf-scout")
READ_ONLY_TOOLS = frozenset({"Read", "Grep", "Glob"})
# Every other frontmatter key can widen what an agent reaches (mcpServers, skills, hooks, permissionMode, memory,
# isolation, background, disallowedTools standing in for an allowlist), so the roster admits only these.
SCOUT_KEYS = frozenset({"name", "description", "tools", "model", "effort", "color", "maxTurns"})


def check(name: str, ok: bool, detail: str = "") -> None:
    if CANCELLED.is_set():
        raise KeyboardInterrupt
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))
    if not ok:
        FAILURES.append(name)


def run(args: list[str], timeout: int = 300) -> subprocess.CompletedProcess:
    """One subprocess check, never raising for a failed check: a spawn error
    or a timeout becomes a failed result the caller reports as its named
    FAIL line, so one broken executable cannot traceback the whole preflight.

    The child runs in its own process group, and that whole group is killed
    and reaped before this function returns or raises, whatever ended it: a
    check or a repair that started builds must not leave them running after
    the preflight has moved on or released the guard lock.

    Cancellation is cooperative so that it has no unsafe instant. Once
    install_signal_handlers() has run, SIGINT, SIGTERM and SIGHUP only set a
    flag; this loop sees the flag, stops the child, and only then raises
    KeyboardInterrupt. No exception can arrive between the child starting
    and the cleanup that owns it, or in the middle of that cleanup."""
    if CANCELLED.is_set():
        raise KeyboardInterrupt
    try:
        proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                start_new_session=True)
    except OSError as exc:
        return subprocess.CompletedProcess(args, 127, "", f"{args[0]}: could not start: {exc}")
    deadline = time.monotonic() + timeout
    out = err = ""
    timed_out = False
    try:
        while True:
            try:
                out, err = proc.communicate(timeout=0.2)
                break
            except subprocess.TimeoutExpired:
                if CANCELLED.is_set():
                    break
                if time.monotonic() >= deadline:
                    timed_out = True
                    break
    finally:
        # Runs on every path, including an exception nobody planned for. With the handlers installed nothing can
        # interrupt it: a signal arriving now only sets the flag checked below.
        kill_group(proc)
        try:
            # Bounded: a descendant that left the group could still hold the pipes open, and collecting output
            # must never be what keeps the preflight from finishing.
            late_out, late_err = proc.communicate(timeout=5)
            out, err = out or late_out, err or late_err
        except (OSError, ValueError, subprocess.TimeoutExpired):
            for pipe in (proc.stdout, proc.stderr):
                try:
                    if pipe is not None:
                        pipe.close()
                except OSError:
                    pass
            proc.poll()
    if CANCELLED.is_set():
        raise KeyboardInterrupt
    if timed_out:
        return subprocess.CompletedProcess(args, 124, out or "", f"{args[0]}: timed out after {timeout}s")
    return subprocess.CompletedProcess(args, proc.returncode, out, err)


def kill_group(proc: subprocess.Popen) -> None:
    """Kill every process in the group `proc` leads and wait until none is left (bounded). The leader is reaped as
    the loop goes, since a group whose only member is an unreaped leader still accepts the signal."""
    for _ in range(100):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            return
        proc.poll()
        time.sleep(0.05)


def install_signal_handlers() -> None:
    """Make stopping the preflight cooperative: Ctrl+C, a closed window (SIGHUP) and a kill (SIGTERM) set CANCELLED
    and nothing else. run() and check() turn the flag into KeyboardInterrupt at points where no child is running."""
    def cancel(signum, frame):
        CANCELLED.set()
    for signum in CANCEL_SIGNALS:
        signal.signal(signum, cancel)


def pid_alive(pid: int) -> bool:
    return Path(f"/proc/{pid}").is_dir()


def claude_registry_running(session_id: str, sessions_dir: Path | None = None) -> bool | None:
    """Whether Claude Code's own session registry (`~/.claude/sessions/<pid>.json`) shows `session_id` on a live
    process. A launcher-started session carries its id on the command line, but an attended one that owns a guard
    shows it nowhere else. This registry is then the only evidence, so failing to read it is unknown, never dead:
    an entry that cannot be read or parsed while its pid is alive, or a directory that cannot be listed."""
    sessions_dir = sessions_dir if sessions_dir is not None else CLAUDE_HOME / "sessions"
    if not sessions_dir.exists():
        return False
    try:
        # os.scandir, not Path.glob: glob swallows a permission error and returns nothing, which would read as
        # "no owner" exactly when the evidence could not be read.
        with os.scandir(sessions_dir) as listing:
            entries = sorted(Path(item.path) for item in listing if item.name.endswith(".json"))
    except OSError:
        return None
    unknown = False
    for entry in entries:
        if not entry.stem.isdigit() or not pid_alive(int(entry.stem)):
            continue  # a dead process owns nothing, readable or not
        try:
            data = json.loads(entry.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            unknown = True
            continue
        if isinstance(data, dict) and data.get("sessionId") == session_id:
            return True
    return None if unknown else False


def session_running(session_id: str) -> bool | None:
    """Whether some live process still owns `session_id`, or None when this host cannot tell: the process table
    first, then Claude Code's session registry. Running wins, then unknown, and only two clean misses are dead."""
    if not session_id:
        return False
    seen = (process_table_running(session_id), claude_registry_running(session_id))
    if True in seen:
        return True
    return None if None in seen else False


def process_table_running(session_id: str) -> bool | None:
    """Whether some live process still owns `session_id`, or None when
    this host cannot tell. On Linux a session owns its id through its
    command line (every launcher-started Claude Code session: `--session-id`)
    or an open file under a path naming it; anything else is not running.
    A process whose descriptors cannot be inspected is not the owner."""
    proc_dir = Path("/proc")
    if not proc_dir.is_dir():
        return None
    needle = session_id.encode()
    unknown = False
    for entry in proc_dir.iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            same_user = entry.stat().st_uid == os.getuid()
        except OSError:
            same_user = False
        try:
            if needle in (entry / "cmdline").read_bytes():
                return True
            try:
                fds = list((entry / "fd").iterdir())
            except OSError:
                # A process that hides its descriptors is not a writer session: every
                # writer (Claude Code, Codex, Antigravity) runs dumpable as this user.
                # Calling it unknown would refuse every stale-guard handover, since
                # ordinary same-user daemons (sd-pam, a secrets agent) always hide them.
                fds = []
            for fd in fds:
                try:
                    if session_id in os.readlink(fd):
                        return True
                except OSError:
                    continue
        except PermissionError:
            unknown = unknown or same_user
        except OSError:
            continue
    return None if unknown else False


def last_line(proc: subprocess.CompletedProcess) -> str:
    out = (proc.stdout or proc.stderr or "").strip().splitlines()
    return out[-1][:160] if out else f"exit {proc.returncode}"


def guard_verdict() -> tuple[str, str]:
    """(`absent` | `stale` | `live` | `unknown` | `unreadable`, detail) for the campaign guard file, read now."""
    guard = ROOT / "build/claude-campaign-guard.json"
    if not guard.is_file():
        return "absent", ""
    try:
        owner = str(json.loads(guard.read_text(encoding="utf-8")).get("session_id", ""))
    except Exception as exc:  # a guard that cannot be read cannot be handed over
        return "unreadable", f"guard file unreadable: {exc}"
    running = session_running(owner) if owner else None
    if running:
        return "live", f"session {owner} is running and may own the tree"
    if running is False:
        return "stale", owner or "(no session)"
    return "unknown", f"cannot tell whether session {owner} is running"


def launch_claim(self_pid: int | None = None) -> tuple[str, str]:
    """(`absent` | `stale` | `live` | `unreadable`, detail) for build/unattended-launch.json, the claim a launch
    writes when its preflight passes. A launched session acquires its campaign guard only after it has started, so
    between the launch and the guard this claim is the only thing a second launch can see. It names the launcher's
    pid, which `exec claude` keeps, and the session id that pid then carries on its command line."""
    path = ROOT / "build/unattended-launch.json"
    if not path.exists():
        return "absent", ""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        pid, session = int(data["pid"]), str(data["session_id"])
    except Exception as exc:
        return "unreadable", f"launch claim unreadable ({exc}); delete build/unattended-launch.json once no run is live"
    if pid == self_pid or not pid_alive(pid):
        return "stale", session
    try:
        command = Path(f"/proc/{pid}/cmdline").read_bytes()
    except OSError:
        return "live", f"pid {pid} holds the launch claim and its command line cannot be read"
    if session.encode() in command or b"unattended-run.sh" in command:
        return "live", f"launch of session {session} (pid {pid}) is running"
    return "stale", session  # the pid was reused by something that is not this launch


def write_claim(session: str, pid: int) -> None:
    path = ROOT / "build/unattended-launch.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(".json.partial")
    partial.write_text(json.dumps({"session_id": session, "pid": pid}) + "\n", encoding="utf-8")
    os.replace(partial, path)


def guard_lock():
    """The campaign guard's own interprocess lock (scripts/campaign_guard.py `_Lock`): every guard change holds
    it, so holding it across a repair means no session can acquire the guard while ignored output is rebuilt."""
    spec = importlib.util.spec_from_file_location("rf_campaign_guard", Path(__file__).resolve().parent / "campaign_guard.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module._Lock(str(ROOT))


def repair_database(self_pid: int | None) -> subprocess.CompletedProcess:
    """Rebuild the stale compilation database, but only while holding the guard lock and only if, read again under
    that lock, still nobody owns the tree. The first ownership checks are several subprocess checks old by now."""
    argv = [sys.executable, "scripts/gen-compdb.py", "--rebuild-stale"]
    try:
        lock = guard_lock()
        lock.__enter__()
    except Exception as exc:
        return subprocess.CompletedProcess(argv, 1, "", f"nothing rebuilt: the campaign guard lock is busy ({exc})")
    try:
        guard, guard_detail = guard_verdict()
        claim, claim_detail = launch_claim(self_pid)
        if guard not in ("absent", "stale") or claim not in ("absent", "stale"):
            return subprocess.CompletedProcess(argv, 1, "", f"nothing rebuilt: ownership changed during the preflight ({guard_detail or claim_detail})")
        return run(argv, timeout=1800)
    finally:
        lock.__exit__(None, None, None)


def read_json(path: Path) -> dict:
    """A settings file as a dict, or {} when it is absent, unreadable or not an object. Callers read single
    booleans out of it and never print its contents: these files can hold credentials."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def stop_hook_wired(settings: dict) -> bool:
    for group in (settings.get("hooks") or {}).get("Stop") or []:
        for hook in (group or {}).get("hooks") or []:
            command = str((hook or {}).get("command", ""))
            if "campaign_guard.py" in command and "stop-hook" in command:
                return True
    return False


def workspace_trusted(state: dict, root: Path) -> bool:
    projects = state.get("projects") or {}
    return any((projects.get(str(p)) or {}).get("hasTrustDialogAccepted") is True for p in (root, *root.parents))


def cron_enabled(environ) -> bool:
    return str(environ.get("CLAUDE_CODE_DISABLE_CRON", "")).strip().lower() in ("", "0", "false")


def frontmatter(text: str) -> dict[str, str] | None:
    lines = text.split("\n")
    if not lines or lines[0].strip() != "---" or "---" not in [line.strip() for line in lines[1:]]:
        return None
    fields: dict[str, str] = {}
    for line in lines[1:]:
        if line.strip() == "---":
            return fields
        match = re.match(r"^([A-Za-z][A-Za-z0-9_-]*):\s*(.*)$", line)
        if match is None:
            return None  # a nested or multi-line value is not a roster this check can vouch for
        fields[match.group(1)] = match.group(2).strip()
    return None


def roster_problems(agents_dir: Path) -> list[str]:
    """Why the Claude Code agent roster is not exactly the read-only Sonnet scouts, or [] when it is. The rule is
    mechanical so that "read-only" is a property of the tool list, not a sentence in a prompt: Sonnet only, tools
    within Read/Grep/Glob, and no frontmatter key that could add a tool, a server, a hook or a permission mode."""
    problems = []
    found = sorted(p.stem for p in agents_dir.glob("*.md")) if agents_dir.is_dir() else []
    if tuple(found) != SCOUTS:
        problems.append(f"roster is {', '.join(found) or '(empty)'}; expected {', '.join(SCOUTS)}")
    for name in found:
        try:
            fields = frontmatter((agents_dir / f"{name}.md").read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError):
            fields = None
        if fields is None:
            problems.append(f"{name}: frontmatter is missing or not flat `key: value` lines")
            continue
        if fields.get("name") != name:
            problems.append(f"{name}: name is {fields.get('name')!r}")
        if fields.get("model") != "sonnet":
            problems.append(f"{name}: model is {fields.get('model')!r}, not sonnet")
        tools = {tool.strip() for tool in fields.get("tools", "").split(",") if tool.strip()}
        if not tools:
            problems.append(f"{name}: no tools line (an agent without one inherits every tool)")
        elif not tools <= READ_ONLY_TOOLS:
            problems.append(f"{name}: tools beyond Read, Grep, Glob: {', '.join(sorted(tools - READ_ONLY_TOOLS))}")
        extra = sorted(set(fields) - SCOUT_KEYS)
        if extra:
            problems.append(f"{name}: frontmatter keys not allowed on a scout: {', '.join(extra)}")
        if not fields.get("description"):
            problems.append(f"{name}: no description")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repair", action="store_true",
                        help="rebuild a stale compilation database (ignored build output) once the ownership checks pass")
    parser.add_argument("--claim", metavar="SESSION", help="on success, record this launch (its session id) in build/unattended-launch.json")
    parser.add_argument("--claim-pid", type=int, help="the launcher's pid, which `exec claude` or `exec agy` keeps (required with --claim)")
    parser.add_argument("--runner", choices=RUNNERS, default="claude",
                        help="the writer the launch starts: claude (Claude Code) or gemini (Antigravity on Gemini 4 Carbon)")
    options = parser.parse_args(argv)
    if options.claim and not options.claim_pid:
        parser.error("--claim needs --claim-pid")
    install_signal_handlers()
    FAILURES.clear()
    # Straight on main, one writer (operator rule; AGENTS.md trunk-based main).
    branch = run(["git", "branch", "--show-current"]).stdout.strip()
    check("on main", branch == "main", f"branch={branch or '(detached)'}")
    status = run(["git", "status", "--porcelain"])
    dirty = status.stdout.strip().splitlines() if status.returncode == 0 else ["<git status failed>"]
    check("tree clean", status.returncode == 0 and not [d for d in dirty if d.strip()], f"{len([d for d in dirty if d.strip()])} dirty path(s)" if status.returncode == 0 else f"git status exit {status.returncode}")
    hooks = run(["git", "config", "core.hooksPath"]).stdout.strip()
    check("commit hooks wired", hooks == "tools/githooks", f"core.hooksPath={hooks or '(unset)'}")
    verdict, detail = guard_verdict()
    if verdict == "stale":
        # Not a failure: the new session takes this guard over through
        # campaign_guard.py acquire --handover (docs/unattended-run.md).
        print(f"STALE  stale campaign guard: {detail} (handover on start)")
    else:
        check("no live campaign guard", verdict == "absent", detail)
    claim, claim_detail = launch_claim(options.claim_pid)
    check("no other unattended launch", claim in ("absent", "stale"), claim_detail)

    # Plan and graph gates (the same ones the pre-commit hook enforces).
    for name, args in (
        ("todo-graph validate", ["scripts/todo-graph.py", "validate"]),
        ("plan --check", ["scripts/todo-graph.py", "plan", "--check"]),
        ("todo-claims", ["scripts/todo-claims.py"]),
        ("plan-audit", ["scripts/plan-audit.py"]),
        ("drift-check", ["scripts/drift-check.py"]),
        ("campaign guard self-test", ["scripts/campaign_guard.py", "--self-test"]),
        ("reviewer pin", ["scripts/review-dispatch.py", "--check-pin"]),
        ("review records parse", ["scripts/review-dispatch.py", "--list-rounds"]),
    ):
        proc = run([sys.executable, *args])
        check(name, proc.returncode == 0, last_line(proc))

    # Nothing below may write unless every ownership check above passed: branch, clean tree, and a guard that is
    # absent or stale. A live or unknown owner is already a FAIL, so a repair never runs under a running writer.
    owned = not FAILURES

    # The writer and its one reviewer under the three-pass schedule (claude and gemini -> astra via codex).
    claude = options.runner == "claude"
    for cli in (RUNNERS[options.runner], "codex"):
        check(f"{cli} on PATH", shutil.which(cli) is not None, shutil.which(cli) or "missing")

    if claude:
        # What Claude Code needs to run with nobody at the keyboard. Only booleans are read from the user's files.
        check("campaign Stop hook wired", stop_hook_wired(read_json(ROOT / ".claude/settings.json")),
              ".claude/settings.json hooks.Stop runs campaign_guard.py stop-hook")
        check("bypass-permissions prompt pre-accepted",
              read_json(CLAUDE_HOME / "settings.json").get("skipDangerousModePermissionPrompt") is True,
              "skipDangerousModePermissionPrompt in ~/.claude/settings.json (accept it once in an attended session)")
        check("workspace trusted", workspace_trusted(read_json(CLAUDE_STATE), ROOT),
              "hasTrustDialogAccepted for this checkout in ~/.claude.json (open it once in an attended session)")
        check("session cron tools enabled", cron_enabled(os.environ), "CLAUDE_CODE_DISABLE_CRON must be unset: the heartbeat is CronCreate")
    else:
        # Antigravity reads its hooks from .agents/hooks.json; the heartbeat is its own schedule tool.
        check("campaign Stop hook wired", stop_hook_wired(read_json(ROOT / ".agents/hooks.json")),
              ".agents/hooks.json hooks.Stop runs campaign_guard.py stop-hook")
    problems = roster_problems(ROOT / ".claude/agents")
    check("read-only Sonnet scout roster", not problems, "; ".join(problems) if problems else ", ".join(SCOUTS))

    # Pinned toolchain and LSP stack.
    tools = ROOT / "tools/llvm-mingw-linux/bin"
    for name, path in (("clang", tools / "clang"), ("clangd", tools / "clangd"),
                       ("clang-tidy", tools / "clang-tidy"), ("ninja", ROOT / "tools/ninja-linux/ninja")):
        check(f"pinned {name}", path.is_file(), str(path.relative_to(ROOT)) if path.is_file() else "run tools/bootstrap.py")
    proc = run([sys.executable, "scripts/gen-compdb.py", "--check"])
    if proc.returncode != 0 and options.repair and owned:
        stale = last_line(proc)
        repair = repair_database(options.claim_pid)
        if repair.returncode == 0:
            print(f"REPAIRED  compile_commands.json  -- was: {stale}")
            proc = run([sys.executable, "scripts/gen-compdb.py", "--check"])
        else:
            proc = repair
    check("compile_commands.json current", proc.returncode == 0, last_line(proc))
    check("basedpyright in .venv", (ROOT / ".venv/bin/basedpyright").is_file(), "pip install -r requirements-dev.txt into .venv")
    if claude:
        plugin = ROOT / ".claude/lsp/rasterfold-lsp"
        servers = read_json(plugin / ".lsp.json")
        check("Claude Code language-server plugin", (plugin / ".claude-plugin/plugin.json").is_file() and {"clangd", "basedpyright"} <= set(servers)
              and (ROOT / ".venv/bin/basedpyright-langserver").is_file(),
              ".claude/lsp/rasterfold-lsp with clangd and basedpyright, and .venv/bin/basedpyright-langserver")

    # Host headroom: an orphaned writer once filled the disk (2026-11-08).
    disk = shutil.disk_usage(ROOT)
    check("disk headroom > 20 GB", disk.free > 20 * 2**30, f"{disk.free / 2**30:.0f} GB free")
    try:
        mem = dict(kv.split(":") for kv in Path("/proc/meminfo").read_text().splitlines() if ":" in kv)
        avail_kb = int(mem["MemAvailable"].strip().split()[0])
        check("RAM headroom > 2 GB", avail_kb > 2 * 2**20, f"{avail_kb / 2**20:.1f} GB available")
    except Exception as exc:
        check("RAM headroom > 2 GB", False, str(exc))

    if FAILURES:
        print(f"\npreflight: {len(FAILURES)} FAIL -- do not start an unattended run")
        return 1
    if options.claim:
        write_claim(options.claim, options.claim_pid)
    print("\npreflight: all checks pass -- the unattended run may start")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\npreflight: cancelled -- the unattended run was not started")
        sys.exit(130)
