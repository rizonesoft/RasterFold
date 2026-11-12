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
    r"^(?:[-*]\s+)?`?(?P<path>[A-Za-z0-9_./-]+):(?P<start>\d+)(?:-(?P<end>\d+))?`?(?:(?::\s*|\s+)(?P<rest>.*))?$"
)
FACT_PAIR_SPLIT_RE = re.compile(
    r"(?P<arrow>\s+->\s+)|(?P<semi>;\s+)(?=`?(?:[A-Za-z0-9_./-]+:\d+|NOT FOUND:))"
)
RHS_CITE_RE = re.compile(
    r"^`?(?P<path>[A-Za-z0-9_./-]+):(?P<start>\d+)(?:-(?P<end>\d+))?`?(?:(?::\s*|\s+)(?P<quote>.*))?$"
)
COUNT_RE = re.compile(r"^COUNT:\s*(\d+)\s*$")
HELD_RE = re.compile(r"^HELD:\s*(\d+)\s*$")
VERDICT_WORDS_RE = re.compile(
    r"\b(?:OVERALL\s*:|VERDICT\s*:|COMPLETION\s*:|PLAN\s*:|ready to ship|looks good to me|ship it)\b",
    re.IGNORECASE,
)
BLANK_MARKERS = frozenset({
    '""',
    "''",
    "``",
    "<blank>",
    "(blank)",
    "`<blank>`",
    "`(blank)`",
    "<empty>",
    "(empty)",
    "`<empty>`",
    "`(empty)`",
})


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


def extract_quote_segments(raw_quote: str, multi_line: bool = False) -> list[str]:
    q = raw_quote.strip()
    if q.endswith(".") and len(q) >= 2 and q[-2] in ("`", '"', "'"):
        q = q[:-1].strip()
    if len(q) >= 2 and q[0] == q[-1] and q[0] in ('"', "'"):
        q = q[1:-1].strip()
    if "` / `" in q or (multi_line and " / " in q):
        sep = "` / `" if "` / `" in q else " / "
        parts = [p.strip() for p in q.split(sep)]
        cleaned = []
        for p in parts:
            if p.startswith("`"):
                p = p[1:]
            if p.endswith("`"):
                p = p[:-1]
            if p.strip():
                cleaned.append(p.strip())
        return cleaned
    if len(q) >= 2 and q[0] == "`" and q[-1] == "`":
        q = q[1:-1]
    return [q] if q else []


def is_explicit_blank_quote(raw_quote: str | None) -> bool:
    q = (raw_quote or "").strip()
    if not q:
        return True
    if q.endswith(".") and len(q) >= 2:
        q = q[:-1].strip()
    if q.lower() in BLANK_MARKERS:
        return True
    if len(q) >= 2 and q[0] == q[-1] and q[0] in ('"', "'", "`") and not q[1:-1].strip():
        return True
    return False


def check_citation_on_disk(
    rel_path: str,
    start: int,
    end: int,
    raw_quote: str | None,
    root_resolved: Path,
    file_cache: dict[Path, list[str]],
) -> str | None:
    if start < 1 or end < start:
        return f"invalid line range in citation {rel_path}:{start}-{end}"
    candidate = (root_resolved / rel_path).resolve()
    try:
        candidate.relative_to(root_resolved)
    except ValueError:
        return f"citation path escapes repository root: {rel_path}"
    if not candidate.is_file():
        return f"hallucinated file path in citation: {rel_path}:{start}"
    if candidate not in file_cache:
        try:
            file_cache[candidate] = candidate.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            return f"unreadable file in citation {rel_path}:{start}: {exc}"
    file_lines = file_cache[candidate]
    if end > len(file_lines):
        return f"hallucinated line number in citation: {rel_path}:{end} (file has {len(file_lines)} lines)"
    window = "\n".join(file_lines[start - 1 : end])
    quote_str = (raw_quote or "").strip()
    if not window.strip():
        if is_explicit_blank_quote(quote_str):
            return None
        return f"fabricated or mismatched quote at {rel_path}:{start}: source line is blank"
    if not quote_str:
        return f"empty quotation in citation: {rel_path}:{start}"
    if is_explicit_blank_quote(quote_str):
        return f"fabricated or mismatched quote at {rel_path}:{start}: source line is not blank"
    segments = extract_quote_segments(quote_str, multi_line=end > start)
    if not segments:
        return f"empty quotation in citation: {rel_path}:{start}"
    for seg in segments:
        if seg not in window:
            return f"fabricated or mismatched quote at {rel_path}:{start}: {seg!r} not in source line(s)"
    return None


def norm_heading(line: str) -> str:
    s = line.strip().strip("`").strip("*").strip("#").strip()
    if s.endswith(":"):
        s = s[:-1].strip()
    return s


def is_none_line(line: str, prefix: str = "") -> bool:
    s = line.strip().strip("`")
    if s in ("- None.", "- None", "None.", "None", "- none found", "none found"):
        return True
    if prefix and s.upper() == f"{prefix.upper()}: NONE FOUND":
        return True
    return False


def verify_fact_pair_line(
    ln: str,
    allow_not_found: bool,
    root_resolved: Path,
    file_cache: dict[Path, list[str]],
) -> str | None:
    matches = list(FACT_PAIR_SPLIT_RE.finditer(ln))
    if not matches:
        return f"rf-fact-check entry missing ' -> ' or '; ': {ln!r}"

    def is_not_found_rhs(rhs_text: str) -> bool:
        return rhs_text.strip().strip("`").startswith("NOT FOUND:")

    # Try two-citation splits (' -> ' then '; ') before NOT FOUND splits (' -> ' then '; '),
    # from right to left so separators inside the left-hand quotation are skipped cleanly.
    ordered = (
        [m for m in reversed(matches) if m.group("arrow") and not is_not_found_rhs(ln[m.end() :])]
        + [m for m in reversed(matches) if not m.group("arrow") and not is_not_found_rhs(ln[m.end() :])]
        + [m for m in reversed(matches) if m.group("arrow") and is_not_found_rhs(ln[m.end() :])]
        + [m for m in reversed(matches) if not m.group("arrow") and is_not_found_rhs(ln[m.end() :])]
    )
    first_err: str | None = None
    for m in ordered:
        lhs = ln[: m.start()]
        rhs = ln[m.end() :]
        lm = LINE_CITE_RE.match(lhs)
        if not lm:
            err = f"unquoted lhs in rf-fact-check: {lhs!r}"
            if first_err is None:
                first_err = err
            continue
        start = int(lm.group("start"))
        end = int(lm.group("end") or start)
        err = check_citation_on_disk(lm.group("path"), start, end, lm.group("rest"), root_resolved, file_cache)
        if err is not None:
            if first_err is None:
                first_err = err
            continue
        rhs_clean = rhs.strip().strip("`")
        if allow_not_found and rhs_clean.startswith("NOT FOUND:"):
            if not rhs_clean[len("NOT FOUND:") :].strip():
                err = f"empty NOT FOUND in rf-fact-check: {ln!r}"
                if first_err is None:
                    first_err = err
                continue
            return None
        rm = RHS_CITE_RE.match(rhs.strip())
        if not rm:
            err = f"unquoted rhs in rf-fact-check: {rhs!r}"
            if first_err is None:
                first_err = err
            continue
        r_start = int(rm.group("start"))
        r_end = int(rm.group("end") or r_start)
        err = check_citation_on_disk(
            rm.group("path"), r_start, r_end, rm.group("quote"), root_resolved, file_cache
        )
        if err is None:
            return None
        if first_err is None:
            first_err = err
    return first_err or f"invalid rf-fact-check entry: {ln!r}"


def verify_response_evidence(response: str, agent: str, root: Path = ROOT) -> str | None:
    """Mechanically check that the scout response is either a single-line ESCALATE-LEAD / NOT FOUND response or
    conforms to the scout's block structure with every path:line citation and quotation verified on disk."""
    text = response.strip()
    if not text:
        return "empty scout response"
    if "TRUNCATED:" in text:
        return "response contains TRUNCATED marker; scout must retrieve all pages before reporting"

    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if lines[0].startswith("ESCALATE-LEAD:"):
        reason = lines[0][len("ESCALATE-LEAD:") :].strip()
        if not reason:
            return "ESCALATE-LEAD response missing reason"
        if len(lines) > 1:
            return "ESCALATE-LEAD response must be a single line"
        return None

    stripped_first = lines[0].strip("`")
    if stripped_first.startswith("NOT FOUND:"):
        what = stripped_first[len("NOT FOUND:") :].strip()
        if not what:
            return "NOT FOUND response missing target description"
        if len(lines) > 1:
            return "top-level NOT FOUND response must be a single line"
        return None

    root_resolved = root.resolve()
    file_cache: dict[Path, list[str]] = {}

    if agent == "rf-scout":
        cited_count = 0
        declared_count: int | None = None
        for idx, ln in enumerate(lines):
            cm = COUNT_RE.match(ln)
            if cm:
                if idx != len(lines) - 1:
                    return "COUNT line must be the last line of rf-scout output"
                declared_count = int(cm.group(1))
                continue
            m = LINE_CITE_RE.match(ln)
            if not m:
                return f"unquoted or malformed line in rf-scout response: {ln!r}"
            start = int(m.group("start"))
            end = int(m.group("end") or start)
            err = check_citation_on_disk(m.group("path"), start, end, m.group("rest"), root_resolved, file_cache)
            if err is not None:
                return err
            cited_count += 1
        if cited_count == 0:
            return "unquoted response: rf-scout reported zero path:line citations"
        if declared_count is not None and declared_count != cited_count:
            return f"COUNT: {declared_count} does not match {cited_count} cited line(s)"
        return None

    if agent == "rf-log-digest":
        blocks: dict[str, list[str]] = {}
        order: list[str] = []
        current: str | None = None
        for ln in lines:
            clean = ln.strip("`").strip("*").strip()
            if clean.upper() == "FAILURES: NONE FOUND":
                if current is not None:
                    return "unexpected second FAILURES block in rf-log-digest response"
                current = "FAILURES"
                order.append(current)
                blocks[current] = ["FAILURES: none found"]
                continue
            h = norm_heading(ln).upper()
            if h in ("FAILURES", "COUNTS", "LAST LINES"):
                if h in blocks:
                    return f"duplicate {h} block in rf-log-digest response"
                current = h
                order.append(h)
                blocks[h] = []
                continue
            if current is None:
                return f"rf-log-digest line before FAILURES heading: {ln!r}"
            blocks[current].append(ln)

        for req in ("FAILURES", "COUNTS", "LAST LINES"):
            if req not in blocks:
                return f"rf-log-digest response missing {req} block"
        if order != ["FAILURES", "COUNTS", "LAST LINES"]:
            return f"rf-log-digest blocks out of order: {order}"

        for blk_name in ("FAILURES", "COUNTS", "LAST LINES"):
            blk_lines = blocks[blk_name]
            if not blk_lines:
                return f"rf-log-digest {blk_name} block is empty"
            if blk_name in ("FAILURES", "COUNTS") and len(blk_lines) == 1 and is_none_line(blk_lines[0], blk_name):
                continue
            for ln in blk_lines:
                m = LINE_CITE_RE.match(ln)
                if not m:
                    return f"unquoted or malformed line in rf-log-digest {blk_name}: {ln!r}"
                start = int(m.group("start"))
                end = int(m.group("end") or start)
                err = check_citation_on_disk(m.group("path"), start, end, m.group("rest"), root_resolved, file_cache)
                if err is not None:
                    return err
        return None

    if agent == "rf-fact-check":
        blocks = {}
        order = []
        current = None
        for ln in lines:
            h = norm_heading(ln).upper()
            if h in ("MISMATCHES", "NEEDS A COMMAND", "CHECKED AND HOLDING"):
                if h in blocks:
                    return f"duplicate {h} block in rf-fact-check response"
                current = h
                order.append(h)
                blocks[h] = []
                continue
            if current is None:
                return f"rf-fact-check line before MISMATCHES heading: {ln!r}"
            blocks[current].append(ln)

        for req in ("MISMATCHES", "NEEDS A COMMAND", "CHECKED AND HOLDING"):
            if req not in blocks:
                return f"rf-fact-check response missing {req} block"
        if order != ["MISMATCHES", "NEEDS A COMMAND", "CHECKED AND HOLDING"]:
            return f"rf-fact-check blocks out of order: {order}"

        # MISMATCHES: either None or lhs citation ->/; (rhs citation | NOT FOUND: ...)
        mm_lines = blocks["MISMATCHES"]
        if not mm_lines:
            return "rf-fact-check MISMATCHES block is empty"
        if not (len(mm_lines) == 1 and is_none_line(mm_lines[0], "MISMATCHES")):
            for ln in mm_lines:
                err = verify_fact_pair_line(
                    ln, allow_not_found=True, root_resolved=root_resolved, file_cache=file_cache
                )
                if err is not None:
                    return err

        # NEEDS A COMMAND: either None or bullet lines, never a shipping verdict
        cmd_lines = blocks["NEEDS A COMMAND"]
        if not cmd_lines:
            return "rf-fact-check NEEDS A COMMAND block is empty"
        for ln in cmd_lines:
            if VERDICT_WORDS_RE.search(ln):
                return f"disallowed verdict in rf-fact-check NEEDS A COMMAND: {ln!r}"
            if not is_none_line(ln, "NEEDS A COMMAND") and not ln.startswith(("- ", "* ")):
                return f"malformed line in rf-fact-check NEEDS A COMMAND: {ln!r}"

        # CHECKED AND HOLDING: must end with HELD: <n>, and items before it must be verified lhs ->/; rhs citations
        ch_lines = blocks["CHECKED AND HOLDING"]
        if not ch_lines:
            return "rf-fact-check CHECKED AND HOLDING block is empty"
        hm = HELD_RE.match(ch_lines[-1])
        if not hm:
            return "rf-fact-check CHECKED AND HOLDING block must end with HELD: <n>"
        declared_held = int(hm.group(1))
        item_lines = ch_lines[:-1]
        if declared_held == 0 and len(item_lines) == 1 and is_none_line(item_lines[0]):
            item_lines = []
        if len(item_lines) != declared_held:
            return f"HELD: {declared_held} does not match {len(item_lines)} held item(s)"
        for ln in item_lines:
            err = verify_fact_pair_line(
                ln, allow_not_found=False, root_resolved=root_resolved, file_cache=file_cache
            )
            if err is not None:
                return err
        return None

    return f"unknown scout agent: {agent!r}"


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

        if result_count > 0:
            return False, f"stream event on line {idx} after terminal result"

        kind = event.get("event") or event.get("type") or ""
        if kind == "init":
            if saw_init:
                return False, f"duplicate init event on stream line {idx}"
            init_obj = event.get("init") if isinstance(event.get("init"), dict) else event
            model = init_obj.get("model")
            agent = init_obj.get("agent")
            if model != expected_model:
                return False, f"init model is {model!r}, expected {expected_model!r}"
            if agent != expected_agent:
                return False, f"init agent is {agent!r}, expected {expected_agent!r}"
            saw_init = True
            continue

        if not saw_init:
            return False, "stream missing init event before activity"

        if kind == "step_update":
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
