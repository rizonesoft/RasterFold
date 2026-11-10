#!/usr/bin/env python3
"""Fail-loud preflight for an unattended RasterFold campaign run.

An unattended run that starts against a broken tree, a stale plan, a missing
reviewer, or a nearly full disk does not fail politely: it stalls, parks, or
fills the disk. This checks every load-bearing precondition the process-plan
run guard assumes, prints one PASS/FAIL line each, and exits 1 on any FAIL.
It starts nothing and writes nothing outside build/.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAILURES: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))
    if not ok:
        FAILURES.append(name)


def run(args: list[str], timeout: int = 300) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=timeout)


def last_line(proc: subprocess.CompletedProcess) -> str:
    out = (proc.stdout or proc.stderr or "").strip().splitlines()
    return out[-1][:160] if out else f"exit {proc.returncode}"


def main() -> int:
    # Straight on main, one writer (operator rule; AGENTS.md trunk-based main).
    branch = run(["git", "branch", "--show-current"]).stdout.strip()
    check("on main", branch == "main", f"branch={branch or '(detached)'}")
    dirty = run(["git", "status", "--porcelain"]).stdout.strip().splitlines()
    check("tree clean", not [d for d in dirty if d.strip()], f"{len([d for d in dirty if d.strip()])} dirty path(s)")
    hooks = run(["git", "config", "core.hooksPath"]).stdout.strip()
    check("commit hooks wired", hooks == "tools/githooks", f"core.hooksPath={hooks or '(unset)'}")
    guard = ROOT / "build/claude-campaign-guard.json"
    check("no live campaign guard", not guard.is_file(), "another run may own the tree" if guard.is_file() else "")

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

    # Reviewer and writer CLIs the three-pass schedule needs (muse -> astra via codex).
    for cli in ("muse", "codex", "agy"):
        check(f"{cli} on PATH", shutil.which(cli) is not None, shutil.which(cli) or "missing")

    # Pinned toolchain and LSP stack.
    tools = ROOT / "tools/llvm-mingw-linux/bin"
    for name, path in (("clang", tools / "clang"), ("clangd", tools / "clangd"),
                       ("clang-tidy", tools / "clang-tidy"), ("ninja", ROOT / "tools/ninja-linux/ninja")):
        check(f"pinned {name}", path.is_file(), str(path.relative_to(ROOT)) if path.is_file() else "run tools/bootstrap.py")
    proc = run([sys.executable, "scripts/gen-compdb.py", "--check"])
    check("compile_commands.json current", proc.returncode == 0, last_line(proc))
    check("basedpyright in .venv", (ROOT / ".venv/bin/basedpyright").is_file(), "pip install -r requirements-dev.txt into .venv")

    # Spark/Ember agent definitions load under the exact launcher flags.
    agents = ROOT / ".agents/muse-agents.json"
    ok, detail = False, "missing"
    if agents.is_file() and shutil.which("muse"):
        proc = run(["muse", "exec", "--provider", "echo", "--yolo", "--agents", agents.read_text(encoding="utf-8"), "probe"], timeout=60)
        ok, detail = proc.returncode == 0 and "echo: probe" in proc.stdout, last_line(proc)
    check("muse agents (Spark/Ember) load", ok, detail)
    try:
        names = sorted(json.loads(agents.read_text(encoding="utf-8")))
    except Exception:
        names = []
    check("agent roster", names == ["rf-ember-literal", "rf-spark-hands", "rf-spark-scout", "rf-wonder-hands"], ", ".join(names))

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
    print("\npreflight: all checks pass -- the unattended run may start")
    return 0


if __name__ == "__main__":
    sys.exit(main())
