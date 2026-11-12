#!/usr/bin/env python3
"""Run a read-only Antigravity scout (rf-scout, rf-log-digest, rf-fact-check) on Gemini 4 Hydrogen."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
SCOUTS = ("rf-fact-check", "rf-log-digest", "rf-scout")
DEFAULT_MODEL = "gemini-4-hydrogen-medium"
ALLOWED_MODELS = ("gemini-4-hydrogen-medium", "gemini-4-hydrogen-high")
ALLOWED_TOOLS = frozenset({
    "read_file",
    "view_file",
    "mcp_rf-scouts_rf-readonly_grep_repo",
    "mcp_rf-scouts_rf-readonly_glob_repo",
    "call_mcp_tool",
})
ALLOWED_MCP_TOOLS = frozenset({"grep_repo", "glob_repo"})
CANCEL_SIGNALS = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
LINE_CITE_RE = re.compile(
    r"^(?:[-*]\s+)?`?(?P<path>[A-Za-z0-9_./-]+):(?P<start>\d+)(?:-(?P<end>\d+))?`?(?=[:\s]|$)"
)
ARROW_CITE_RE = re.compile(
    r"->\s*`?(?P<path>[A-Za-z0-9_./-]+):(?P<start>\d+)(?:-(?P<end>\d+))?`?(?=[:\s]|$)"
)
COUNT_RE = re.compile(r"^COUNT:\s*(\d+)\s*$")


class CancelFlag:
    """Lock-free cancellation flag set by signal handlers."""

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


def install_signal_handlers() -> None:
    def cancel(signum, frame):
        CANCELLED.set()

    for signum in CANCEL_SIGNALS:
        signal.signal(signum, cancel)


def pid_alive(pid: int) -> bool:
    try:
        stat_text = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except OSError:
        return False
    state = stat_text.rsplit(")", 1)[-1].split()[:1]
    return state != ["Z"]


def kill_run_tree(proc: subprocess.Popen, run_id: str) -> None:
    """Kill `proc`'s process group and any same-user process carrying `RF_SCOUT_RUN_ID=run_id` in `/proc/<pid>/environ`,
    waiting boundedly until none remains alive."""
    needle = f"RF_SCOUT_RUN_ID={run_id}".encode("utf-8")
    my_pid = os.getpid()
    my_uid = os.getuid()
    for _ in range(100):
        alive_any = False
        try:
            os.killpg(proc.pid, signal.SIGKILL)
            alive_any = True
        except (ProcessLookupError, PermissionError):
            pass
        proc.poll()
        proc_dir = Path("/proc")
        if proc_dir.is_dir():
            for entry in proc_dir.iterdir():
                if not entry.name.isdigit():
                    continue
                pid = int(entry.name)
                if pid == my_pid:
                    continue
                try:
                    if entry.stat().st_uid != my_uid:
                        continue
                    env_bytes = (entry / "environ").read_bytes()
                except OSError:
                    continue
                if needle in env_bytes and pid_alive(pid):
                    alive_any = True
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except OSError:
                        pass
        if not alive_any and proc.poll() is not None:
            return
        time.sleep(0.05)


def load_preflight():
    spec = importlib.util.spec_from_file_location(
        "rf_unattended_preflight",
        Path(__file__).resolve().parent / "unattended-preflight.py",
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load scripts/unattended-preflight.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_roster(root: Path = ROOT) -> list[str]:
    pf = load_preflight()
    return pf.gemini_roster_problems(root)


def run_agy_stream(
    argv: list[str],
    cwd: Path,
    timeout: int = 180,
    env: dict[str, str] | None = None,
) -> tuple[int, str, str]:
    """Run `argv` in a new session with `RF_SCOUT_RUN_ID` in its environment, ensuring the entire child tree is
    killed and reaped before returning or raising."""
    if CANCELLED.is_set():
        raise KeyboardInterrupt
    run_id = str(uuid.uuid4())
    child_env = dict(os.environ if env is None else env)
    child_env["RF_SCOUT_RUN_ID"] = run_id
    try:
        proc = subprocess.Popen(
            argv,
            cwd=cwd,
            env=child_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
    except OSError as exc:
        return 127, "", f"{argv[0]}: could not start: {exc}"
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
        kill_run_tree(proc, run_id)
        try:
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
        return 124, out or "", f"{argv[0]}: timed out after {timeout}s"
    return proc.returncode if proc.returncode is not None else 1, out, err


def check_tool_call(tool_name: str, params: dict) -> str | None:
    if tool_name not in ALLOWED_TOOLS:
        return f"disallowed tool call {tool_name!r}"
    if tool_name == "call_mcp_tool":
        server = params.get("ServerName") or params.get("server_name") or params.get("server")
        tname = params.get("ToolName") or params.get("tool_name") or params.get("name")
        if server != "rf-scouts_rf-readonly" or tname not in ALLOWED_MCP_TOOLS:
            return f"disallowed MCP tool call server={server!r} tool={tname!r}"
    return None


def verify_response_evidence(response: str, agent: str, root: Path = ROOT) -> str | None:
    """Mechanically check that the scout response is either a single-line ESCALATE-LEAD refusal or carries
    verifiable path:line citations / NOT FOUND markers whose cited files and line numbers exist under `root`."""
    text = response.strip()
    if not text:
        return "empty scout response"
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if lines[0].startswith("ESCALATE-LEAD:"):
        reason = lines[0][len("ESCALATE-LEAD:") :].strip()
        if not reason:
            return "ESCALATE-LEAD response missing reason"
        if len(lines) > 1:
            return "ESCALATE-LEAD response must be a single line"
        return None

    citations: list[tuple[str, int, int]] = []
    for ln in lines:
        m = LINE_CITE_RE.match(ln)
        if m:
            s = int(m.group("start"))
            e = int(m.group("end") or s)
            citations.append((m.group("path"), s, e))
        for am in ARROW_CITE_RE.finditer(ln):
            s = int(am.group("start"))
            e = int(am.group("end") or s)
            citations.append((am.group("path"), s, e))

    has_not_found = "NOT FOUND:" in text or "FAILURES: none found" in text
    if not citations and not has_not_found:
        return "unquoted response: missing path:line citations, NOT FOUND:, or ESCALATE-LEAD:"

    root_resolved = root.resolve()
    file_line_counts: dict[Path, int] = {}
    for rel_path, start, end in citations:
        if start < 1 or end < start:
            return f"invalid line range in citation {rel_path}:{start}-{end}"
        candidate = (root_resolved / rel_path).resolve()
        try:
            candidate.relative_to(root_resolved)
        except ValueError:
            return f"citation path escapes repository root: {rel_path}"
        if not candidate.is_file():
            return f"hallucinated file path in citation: {rel_path}:{start}"
        if candidate not in file_line_counts:
            try:
                file_line_counts[candidate] = len(
                    candidate.read_text(encoding="utf-8", errors="replace").splitlines()
                )
            except OSError as exc:
                return f"unreadable file in citation {rel_path}:{start}: {exc}"
        total_lines = file_line_counts[candidate]
        if end > total_lines:
            return f"hallucinated line number in citation: {rel_path}:{end} (file has {total_lines} lines)"

    if agent == "rf-scout":
        for ln in lines:
            cm = COUNT_RE.match(ln)
            if cm:
                declared = int(cm.group(1))
                line_cites = sum(1 for l in lines if LINE_CITE_RE.match(l))
                if declared != line_cites:
                    return f"COUNT: {declared} does not match {line_cites} cited line(s)"
    elif agent == "rf-log-digest" and not has_not_found:
        for heading in ("FAILURES", "COUNTS", "LAST LINES"):
            if heading not in text:
                return f"rf-log-digest response missing {heading} block"
    elif agent == "rf-fact-check" and not has_not_found:
        for heading in ("MISMATCHES", "CHECKED AND HOLDING"):
            if heading not in text:
                return f"rf-fact-check response missing {heading} block"

    return None


def validate_stream(
    stdout: str,
    returncode: int,
    expected_agent: str,
    expected_model: str,
    root: Path = ROOT,
) -> tuple[bool, str]:
    """Validate `agy --output-format stream-json` output. Returns `(ok, response_or_error)`."""
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        return False, f"empty stream output (exit {returncode})"

    saw_init = False
    result_count = 0
    final_response = ""

    for idx, line in enumerate(lines, 1):
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            return False, f"malformed JSON on stream line {idx}: {exc}"
        if not isinstance(event, dict):
            return False, f"non-object JSON on stream line {idx}"

        kind = event.get("event") or event.get("type") or ""
        if kind == "init":
            init_obj = event.get("init") if isinstance(event.get("init"), dict) else event
            model = init_obj.get("model")
            agent = init_obj.get("agent")
            if model != expected_model:
                return False, f"init model is {model!r}, expected {expected_model!r}"
            if agent != expected_agent:
                return False, f"init agent is {agent!r}, expected {expected_agent!r}"
            saw_init = True
        elif kind == "step_update":
            step = event.get("step_update")
            if not isinstance(step, dict):
                return False, f"malformed step_update on stream line {idx}"
            if step.get("step_type") == "tool":
                tname = str(step.get("tool_name") or "")
                tinfo = step.get("tool_info") if isinstance(step.get("tool_info"), dict) else {}
                params = tinfo.get("parameters") if isinstance(tinfo.get("parameters"), dict) else {}
                err = check_tool_call(tname, params)
                if err is not None:
                    return False, err
        elif kind == "tool_use":
            tname = str(event.get("tool_name") or event.get("name") or "")
            params = event.get("parameters") if isinstance(event.get("parameters"), dict) else {}
            err = check_tool_call(tname, params)
            if err is not None:
                return False, err
        elif kind == "error":
            return False, f"stream error event: {event.get('error') or event.get('message') or line}"
        elif kind == "result":
            res_obj = event.get("result") if isinstance(event.get("result"), dict) else event
            result_count += 1
            status = res_obj.get("status")
            if status != "SUCCESS":
                err_msg = res_obj.get("error") or f"status={status!r}"
                return False, f"terminal result not SUCCESS: {err_msg}"
            resp = res_obj.get("response")
            if not isinstance(resp, str) or not resp.strip():
                return False, "terminal result has empty response"
            final_response = resp

    if not saw_init:
        return False, "stream missing init event"
    if result_count != 1:
        return False, f"stream had {result_count} result event(s), expected 1"
    if returncode != 0:
        return False, f"process exited {returncode} despite SUCCESS result event"
    ev_err = verify_response_evidence(final_response, expected_agent, root=root)
    if ev_err is not None:
        return False, ev_err
    return True, final_response


def main(argv: list[str] | None = None, root: Path = ROOT) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--agent", choices=SCOUTS, required=True, help="which read-only scout to run")
    parser.add_argument("--model", default=DEFAULT_MODEL, help="Gemini 4 Hydrogen model ID (default: gemini-4-hydrogen-medium)")
    parser.add_argument("--prompt", help="task prompt for the scout")
    parser.add_argument("--prompt-file", type=Path, help="file containing the task prompt for the scout")
    parser.add_argument("--timeout", type=int, default=180, help="timeout in seconds (default: 180)")
    args = parser.parse_args(argv)

    if args.model not in ALLOWED_MODELS:
        sys.stderr.write(f"gemini-scout: model {args.model!r} is not allowed; choose from {', '.join(ALLOWED_MODELS)}\n")
        return 2
    if bool(args.prompt) == bool(args.prompt_file):
        sys.stderr.write("gemini-scout: pass exactly one of --prompt or --prompt-file\n")
        return 2
    if args.prompt_file is not None:
        try:
            prompt = args.prompt_file.read_text(encoding="utf-8")
        except OSError as exc:
            sys.stderr.write(f"gemini-scout: cannot read --prompt-file {args.prompt_file}: {exc}\n")
            return 2
    else:
        prompt = args.prompt or ""
    if not prompt.strip():
        sys.stderr.write("gemini-scout: prompt must not be empty\n")
        return 2

    install_signal_handlers()
    problems = validate_roster(root)
    if problems:
        sys.stderr.write(f"gemini-scout: scout roster invalid: {'; '.join(problems)}\n")
        return 2

    cmd = [
        "agy",
        "--model",
        args.model,
        "--agent",
        args.agent,
        "--output-format",
        "stream-json",
        f"-p={prompt}",
    ]
    code, out, err = run_agy_stream(cmd, cwd=root, timeout=args.timeout)
    if code in (124, 127) and not out.strip():
        sys.stderr.write(f"gemini-scout: {err.strip() or f'exit {code}'}\n")
        return 2
    ok, response_or_err = validate_stream(
        out,
        code,
        expected_agent=args.agent,
        expected_model=args.model,
        root=root,
    )
    if not ok:
        detail = err.strip()
        suffix = f" ({detail})" if detail else ""
        sys.stderr.write(f"gemini-scout: {response_or_err}{suffix}\n")
        return 2
    sys.stdout.write(response_or_err if response_or_err.endswith("\n") else response_or_err + "\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.stderr.write("\ngemini-scout: cancelled\n")
        sys.exit(130)
