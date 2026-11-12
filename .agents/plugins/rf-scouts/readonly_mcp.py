#!/usr/bin/env python3
"""Read-only stdio MCP server (grep_repo, glob_repo) for Antigravity scouts."""
from __future__ import annotations

import fnmatch
import json
import os
import re
import sys
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SKIP_DIRS = {".git", "build", "bin", "tools", ".venv", "__pycache__"}

TOOLS = [
    {
        "name": "grep_repo",
        "description": (
            "Read-only regular-expression search across files under the repository root. "
            "Returns matching lines formatted as path:line:text."
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
            },
            "required": ["pattern"],
        },
    },
    {
        "name": "glob_repo",
        "description": (
            "Read-only file path glob search confined to the repository root. "
            "Returns matching relative file paths sorted lexicographically."
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


def run_grep_repo(args: dict, root: Path | None = None) -> tuple[str, bool]:
    base = (root or repo_root()).resolve()
    pattern = args.get("pattern")
    if not isinstance(pattern, str) or not pattern:
        return "grep_repo requires a non-empty string 'pattern'", True
    try:
        regex = re.compile(pattern)
    except re.error as exc:
        return f"invalid regex {pattern!r}: {exc}", True
    rel_path = args.get("path", "") or ""
    if not isinstance(rel_path, str):
        return "'path' must be a string", True
    glob_pat = args.get("glob", "") or ""
    if not isinstance(glob_pat, str):
        return "'glob' must be a string", True
    max_results = args.get("max_results", 200)
    if not isinstance(max_results, int) or max_results < 1:
        max_results = 200
    max_results = min(max_results, 1000)
    try:
        target = resolve_confined(base, rel_path)
    except ValueError as exc:
        return str(exc), True
    if not target.exists():
        return f"NOT FOUND: {rel_path or '.'}", False

    hits: list[str] = []
    for fpath in iter_confined_files(base, target):
        rel_str = fpath.relative_to(base).as_posix()
        if glob_pat and not (
            fnmatch.fnmatch(rel_str, glob_pat) or fnmatch.fnmatch(fpath.name, glob_pat)
        ):
            continue
        try:
            data = fpath.read_bytes()
        except OSError:
            continue
        if b"\x00" in data[:4096]:
            continue
        text = data.decode("utf-8", errors="replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            if regex.search(line):
                hits.append(f"{rel_str}:{lineno}:{line}")
                if len(hits) >= max_results:
                    return "\n".join(hits), False
    if not hits:
        return f"NOT FOUND: pattern {pattern!r} in {rel_path or '.'}", False
    return "\n".join(hits), False


def run_glob_repo(args: dict, root: Path | None = None) -> tuple[str, bool]:
    base = (root or repo_root()).resolve()
    pattern = args.get("pattern")
    if not isinstance(pattern, str) or not pattern.strip():
        return "glob_repo requires a non-empty string 'pattern'", True
    pattern = pattern.strip()
    if pattern.startswith("/") or ".." in Path(pattern).parts or ".git" in Path(pattern).parts:
        return f"disallowed glob pattern: {pattern}", True
    rel_path = args.get("path", "") or ""
    if not isinstance(rel_path, str):
        return "'path' must be a string", True
    max_results = args.get("max_results", 200)
    if not isinstance(max_results, int) or max_results < 1:
        max_results = 200
    max_results = min(max_results, 1000)
    try:
        target = resolve_confined(base, rel_path)
    except ValueError as exc:
        return str(exc), True
    if not target.exists():
        return f"NOT FOUND: {rel_path or '.'}", False

    matches: list[str] = []
    for fpath in iter_confined_files(base, target):
        rel_from_root = fpath.relative_to(base).as_posix()
        rel_from_target = fpath.relative_to(target).as_posix() if target.is_dir() else fpath.name
        if (
            fnmatch.fnmatch(rel_from_target, pattern)
            or fnmatch.fnmatch(rel_from_root, pattern)
            or fnmatch.fnmatch(fpath.name, pattern)
        ):
            matches.append(rel_from_root)
            if len(matches) >= max_results:
                break
    if not matches:
        return f"NOT FOUND: glob {pattern!r} in {rel_path or '.'}", False
    return "\n".join(matches), False


def handle_request(msg: dict, root: Path | None = None) -> dict | None:
    mid = msg.get("id")
    method = msg.get("method", "")
    params = msg.get("params") or {}
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
        arguments = params.get("arguments") or {}
        if not isinstance(arguments, dict):
            text, is_err = "tool arguments must be an object", True
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
        resp = handle_request(msg)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
