#!/usr/bin/env python3
"""Publish a root compile_commands.json for clangd from the per-target databases.

scripts/build.py already writes build/<target>/compile_commands.json on every
build. Editors look at the repository root, so this merges the databases that
exist (linux-x64 wins per file, windows-x64 contributes the freestanding
Win32-only units, android-arm64 contributes anything still missing) and writes
the result to compile_commands.json. On a fresh clone with no build yet it
synthesizes entries with flags derived from build.py so navigation works before
the first build; the next real build's databases then take over. --check is
source-derived: it recomputes the expected per-target entries from build.py
and fails when a stored database differs, and it also fails when the root
database is missing or stale. build.py refreshes the root database after
every build by calling write_root() here.
"""
from __future__ import annotations

import argparse
import importlib.util as _ilu
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "compile_commands.json"
PREFERENCE = ("linux-x64", "windows-x64", "android-arm64", "android-x64",
              "linux-x64-portable", "windows-x64-portable", "posix-x64", "posix-arm64")

_spec = _ilu.spec_from_file_location("rf_build", Path(__file__).resolve().parent / "build.py")
if _spec is None or _spec.loader is None:
    raise ImportError("gen-compdb: cannot load scripts/build.py")
build = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(build)


def load(target: str) -> list[dict]:
    path = ROOT / "build" / target / "compile_commands.json"
    if not path.is_file():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    return [e for e in data if isinstance(e, dict) and "file" in e] if isinstance(data, list) else []


def synthesized() -> list[dict]:
    host = "windows-x64" if os.name == "nt" else "linux-x64"
    other = "linux-x64" if host == "windows-x64" else "windows-x64"
    suffix = ".exe" if os.name == "nt" else ""
    entries = []
    for target, units in (
        (host, build.lib_sources_for(host) + ("main",)),
        (other, tuple(u for u in build.lib_sources_for(other) if u not in build.lib_sources_for(host))),
    ):
        tools, _triple = build.target_toolchain(target)
        cc = tools / ("clang" + suffix)
        flags = build.compile_flags(target)
        for name in units:
            src = ROOT / "src" / (name + ".c")
            if src.is_file():
                obj = ROOT / "build" / target / (name + ".o")
                entries.append({"directory": str(ROOT), "file": str(src), "output": str(obj),
                                "arguments": [str(cc), *flags, "-c", str(src), "-o", str(obj)]})
    return entries


def merged() -> tuple[list[dict], list[str]]:
    by_file: dict[str, dict] = {}
    used = []
    for target in PREFERENCE:
        entries = load(target)
        if entries:
            used.append(target)
        for entry in entries:
            by_file.setdefault(os.path.normpath(str(entry["file"])), entry)
    if not by_file:
        return synthesized(), ["synthesized (no build database yet; run scripts/build.py)"]
    return [by_file[k] for k in sorted(by_file)], used


def expected_entries(target: str) -> list[dict] | None:
    portable = target.endswith("-portable")
    base = target[: -len("-portable")] if portable else target
    if base not in build.TARGETS:
        return None
    suffix = ".exe" if os.name == "nt" else ""
    tools, _triple = build.target_toolchain(base)
    cc = tools / ("clang" + suffix)
    flags = build.compile_flags(base, portable=portable)
    entries = []
    for name in build.lib_sources_for(base) + ("main",):
        src = ROOT / "src" / (name + ".c")
        obj = ROOT / "build" / target / (name + ".o")
        entries.append({"directory": str(ROOT), "file": str(src), "output": str(obj),
                        "arguments": [str(cc), *flags, "-c", str(src), "-o", str(obj)]})
    return entries


def write_root() -> tuple[int, list[str]]:
    entries, used = merged()
    text = json.dumps(entries, indent=2) + "\n"
    partial = OUT.with_suffix(".json.partial")
    partial.write_text(text, encoding="utf-8")
    partial.replace(OUT)
    return len(entries), used


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the root database is missing or stale")
    args = parser.parse_args()
    if args.check:
        for target in PREFERENCE:
            if (ROOT / "build" / target / "compile_commands.json").is_file():
                want = expected_entries(target)
                if want is None:
                    continue
                stored = {(str(e.get("file")), tuple(e.get("arguments") or ())) for e in load(target)}
                expected = {(str(e["file"]), tuple(e["arguments"])) for e in want}
                if stored != expected:
                    base = target[: -len("-portable")] if target.endswith("-portable") else target
                    fix = f"python3 scripts/build.py --target {base}" + (" --portable" if target.endswith("-portable") else "")
                    print(f"gen-compdb: per-target compile_commands.json for {target} does not match build.py; run: {fix}", file=sys.stderr)
                    return 1
        entries, used = merged()
        text = json.dumps(entries, indent=2) + "\n"
        if not OUT.is_file() or OUT.read_text(encoding="utf-8") != text:
            print("gen-compdb: compile_commands.json is missing or stale; run scripts/gen-compdb.py", file=sys.stderr)
            return 1
        print(f"gen-compdb: compile_commands.json current ({len(entries)} entries from {', '.join(used)})")
        return 0
    count, used = write_root()
    print(f"gen-compdb: wrote compile_commands.json ({count} entries from {', '.join(used)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
