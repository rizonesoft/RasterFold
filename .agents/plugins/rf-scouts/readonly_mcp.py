#!/usr/bin/env python3
"""Read-only stdio MCP server (grep_repo, glob_repo) for Antigravity scouts."""
from __future__ import annotations

import fnmatch
import io
import json
import os
from pathlib import Path
import re
import sys

DEFAULT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SKIP_DIRS = {".git", "build", "bin", "tools", ".venv", "__pycache__"}
BINARY_CHECK_BYTES = 8192

TOOLS = [
    {
        "name": "grep_repo",
        "description": (
            "Read-only regular-expression search across files under the repository root. "
            "Returns matching lines formatted as path:line:text. If output ends with TRUNCATED:, "
            "call again with the reported offset to retrieve remaining matches."
        ),
        "_meta": {"antigravity/alwaysLoad": True},
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "description": "Python regular expression to search for.",
                },
                "path": {
                    "type": "string",
                    "description": "Optional relative file or directory path within the repository root.",
                },
                "glob": {
                    "type": "string",
                    "description": "Optional filename or relative-path glob filter (e.g. '*.py' or 'todo/**/*.md').",
                },
                "max_results": {
                    "type": "integer",
                    "description": "Maximum number of matching lines to return (default 200, max 1000).",
                },
                "offset": {
                    "type": "integer",
                    "description": "Zero-based match offset for pagination when previous call returned TRUNCATED (default 0).",
                },
            },
            "required": ["pattern"],
        },
    },
    {
        "name": "glob_repo",
        "description": (
            "Read-only file path glob search confined to the repository root. "
            "Returns matching relative file paths sorted lexicographically. If output ends with TRUNCATED:, "
            "call again with the reported offset to retrieve remaining matches."
        ),
        "_meta": {"antigravity/alwaysLoad": True},
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "description": "Relative glob pattern to match (e.g. 'todo/**/*.md' or 'src/*.c').",
                },
                "path": {
                    "type": "string",
                    "description": "Optional relative subdirectory within the repository root.",
                },
                "max_results": {
                    "type": "integer",
                    "description": "Maximum number of matching paths to return (default 200, max 1000).",
                },
                "offset": {
                    "type": "integer",
                    "description": "Zero-based match offset for pagination when previous call returned TRUNCATED (default 0).",
                },
            },
            "required": ["pattern"],
        },
    },
]


def repo_root() -> Path:
    env_root = os.environ.get("RF_SCOUT_ROOT", "").strip()
    return Path(env_root).resolve() if env_root else DEFAULT_ROOT


def resolve_confined(root: Path, rel: str) -> Path:
    cleaned = rel.strip() if rel else "."
    if not cleaned:
        cleaned = "."
    raw = Path(cleaned)
    if raw.is_absolute():
        try:
            raw = raw.resolve().relative_to(root)
        except ValueError as exc:
            raise ValueError(f"path escapes repository root: {rel}") from exc
    if ".." in raw.parts or ".git" in raw.parts:
        raise ValueError(f"disallowed path traversal or .git access: {rel}")
    candidate = (root / raw).resolve()
    try:
        rel_resolved = candidate.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"path escapes repository root: {rel}") from exc
    if ".git" in rel_resolved.parts:
        raise ValueError(f"disallowed .git access: {rel}")
    return candidate


def iter_confined_files(root: Path, start: Path):
    explicit_top = start.relative_to(root).parts[0] if start != root else None
    if start.is_file():
        if not start.is_symlink():
            yield start
        return
    if not start.is_dir():
        return
    for dirpath, dirnames, filenames in os.walk(start, followlinks=False):
        cur = Path(dirpath)
        try:
            rel_cur = cur.resolve().relative_to(root)
        except ValueError:
            dirnames[:] = []
            continue
        kept_dirs: list[str] = []
        for d in sorted(dirnames):
            if d == ".git" or (cur / d).is_symlink():
                continue
            if rel_cur == Path(".") and d in DEFAULT_SKIP_DIRS and d != explicit_top:
                continue
            kept_dirs.append(d)
        dirnames[:] = kept_dirs
        for fname in sorted(filenames):
            fpath = cur / fname
            if fpath.is_symlink():
                continue
            try:
                resolved = fpath.resolve()
                rel_file = resolved.relative_to(root)
            except ValueError:
                continue
            if ".git" in rel_file.parts:
                continue
            yield resolved


def parse_pagination(args: dict) -> tuple[int, int, str | None]:
    raw_max = args.get("max_results", 200)
    if raw_max is None:
        raw_max = 200
    if isinstance(raw_max, bool) or not isinstance(raw_max, int) or raw_max < 1:
        return 0, 0, "'max_results' must be a positive integer"
    max_results = min(raw_max, 1000)

    raw_offset = args.get("offset", 0)
    if raw_offset is None:
        raw_offset = 0
    if isinstance(raw_offset, bool) or not isinstance(raw_offset, int) or raw_offset < 0:
        return 0, 0, "'offset' must be a non-negative integer"
    return max_results, raw_offset, None


def run_grep_repo(args: dict, root: Path | None = None) -> tuple[str, bool]:
    base = (root or repo_root()).resolve()
    pattern = args.get("pattern")
    if not isinstance(pattern, str) or not pattern:
        return "grep_repo requires a non-empty string 'pattern'", True
    try:
        regex = re.compile(pattern)
    except re.error as exc:
        return f"invalid regex {pattern!r}: {exc}", True
    rel_path = args.get("path", "")
    if rel_path is None:
        rel_path = ""
    if not isinstance(rel_path, str):
        return "'path' must be a string", True
    glob_pat = args.get("glob", "")
    if glob_pat is None:
        glob_pat = ""
    if not isinstance(glob_pat, str):
        return "'glob' must be a string", True
    max_results, offset, page_err = parse_pagination(args)
    if page_err is not None:
        return page_err, True
    try:
        target = resolve_confined(base, rel_path)
    except ValueError as exc:
        return str(exc), True
    if not target.exists():
        return f"NOT FOUND: {rel_path or '.'}", False

    page: list[str] = []
    matched_count = 0
    truncated = False
    stop_after = offset + max_results

    for fpath in iter_confined_files(base, target):
        rel_str = fpath.relative_to(base).as_posix()
        if glob_pat and not (
            fnmatch.fnmatch(rel_str, glob_pat) or fnmatch.fnmatch(fpath.name, glob_pat)
        ):
            continue
        try:
            with fpath.open("rb") as bf:
                prefix = bf.read(BINARY_CHECK_BYTES)
                if b"\x00" in prefix:
                    continue
                bf.seek(0)
                tf = io.TextIOWrapper(bf, encoding="utf-8", errors="replace")
                for lineno, raw_line in enumerate(tf, 1):
                    line = raw_line.rstrip("\r\n")
                    if regex.search(line):
                        if matched_count >= offset and len(page) < max_results:
                            page.append(f"{rel_str}:{lineno}:{line}")
                        matched_count += 1
                        if matched_count > stop_after:
                            truncated = True
                            break
        except OSError:
            continue
        if truncated:
            break

    if not page:
        return f"NOT FOUND: pattern {pattern!r} in {rel_path or '.'}", False
    if truncated:
        next_offset = offset + len(page)
        page.append(
            f"TRUNCATED: showing {len(page)} match(es) at offset={offset} "
            f"(more matches exist; call grep_repo with offset={next_offset} to retrieve the rest)"
        )
    return "\n".join(page), False


def run_glob_repo(args: dict, root: Path | None = None) -> tuple[str, bool]:
    base = (root or repo_root()).resolve()
    pattern = args.get("pattern")
    if not isinstance(pattern, str) or not pattern.strip():
        return "glob_repo requires a non-empty string 'pattern'", True
    pattern = pattern.strip()
    if pattern.startswith("/") or ".." in Path(pattern).parts or ".git" in Path(pattern).parts:
        return f"disallowed glob pattern: {pattern}", True
    rel_path = args.get("path", "")
    if rel_path is None:
        rel_path = ""
    if not isinstance(rel_path, str):
        return "'path' must be a string", True
    max_results, offset, page_err = parse_pagination(args)
    if page_err is not None:
        return page_err, True
    try:
        target = resolve_confined(base, rel_path)
    except ValueError as exc:
        return str(exc), True
    if not target.exists():
        return f"NOT FOUND: {rel_path or '.'}", False

    page: list[str] = []
    matched_count = 0
    truncated = False
    stop_after = offset + max_results

    for fpath in iter_confined_files(base, target):
        rel_from_root = fpath.relative_to(base).as_posix()
        rel_from_target = fpath.relative_to(target).as_posix() if target.is_dir() else fpath.name
        if (
            fnmatch.fnmatch(rel_from_target, pattern)
            or fnmatch.fnmatch(rel_from_root, pattern)
            or fnmatch.fnmatch(fpath.name, pattern)
        ):
            if matched_count >= offset and len(page) < max_results:
                page.append(rel_from_root)
            matched_count += 1
            if matched_count > stop_after:
                truncated = True
                break
    if not page:
        return f"NOT FOUND: glob {pattern!r} in {rel_path or '.'}", False
    if truncated:
        next_offset = offset + len(page)
        page.append(
            f"TRUNCATED: showing {len(page)} match(es) at offset={offset} "
            f"(more matches exist; call glob_repo with offset={next_offset} to retrieve the rest)"
        )
    return "\n".join(page), False


def error_response(mid, code: int, message: str) -> dict:
    return {
        "jsonrpc": "2.0",
        "id": mid,
        "error": {"code": code, "message": message},
    }


def handle_request(msg: dict, root: Path | None = None) -> dict | None:
    mid = msg.get("id")
    method = msg.get("method", "")
    if not isinstance(method, str):
        return error_response(mid, -32600, "Invalid Request: 'method' must be a string")
    raw_params = msg.get("params")
    if raw_params is None:
        params: dict = {}
    elif isinstance(raw_params, dict):
        params = raw_params
    else:
        return error_response(mid, -32602, "Invalid params: expected JSON object")

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": mid,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "rf-readonly", "version": "1.0.0"},
            },
        }
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}}
    if method == "tools/call":
        name = params.get("name", "")
        raw_args = params.get("arguments")
        if raw_args is None:
            arguments: dict | None = {}
        elif isinstance(raw_args, dict):
            arguments = raw_args
        else:
            arguments = None

        if not isinstance(name, str) or arguments is None:
            text, is_err = "tool 'name' must be a string and 'arguments' must be an object", True
        elif name == "grep_repo":
            text, is_err = run_grep_repo(arguments, root=root)
        elif name == "glob_repo":
            text, is_err = run_glob_repo(arguments, root=root)
        else:
            text, is_err = f"unknown read-only tool: {name}", True
        return {
            "jsonrpc": "2.0",
            "id": mid,
            "result": {
                "content": [{"type": "text", "text": text}],
                "isError": is_err,
            },
        }
    if mid is not None:
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    return None


def main() -> int:
    for line in sys.stdin:
        stripped = line.strip()
        if not stripped:
            continue
        try:
            msg = json.loads(stripped)
        except json.JSONDecodeError:
            continue
        if not isinstance(msg, dict):
            continue
        try:
            resp = handle_request(msg)
        except Exception as exc:  # pragma: no cover - defensive guard
            resp = error_response(msg.get("id"), -32603, f"Internal error: {exc}")
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
