#!/usr/bin/env python3
"""Publish a root compile_commands.json for clangd from the per-target databases.

scripts/build.py already writes build/<target>/compile_commands.json on every
build. Editors look at the repository root, so this merges the databases that
exist (linux-x64 wins per file, windows-x64 contributes the freestanding
Win32-only units, android-arm64 contributes anything still missing) and writes
the result to compile_commands.json. On a fresh clone with no build yet it
synthesizes linux-x64 entries mirroring build.py so navigation works before
the first build; the next real build's databases then take over via --check
in the LSP preflight.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "compile_commands.json"
PREFERENCE = ("linux-x64", "windows-x64", "android-arm64", "android-x64",
              "linux-x64-portable", "windows-x64-portable", "posix-x64", "posix-arm64")
LINUX_SOURCES = ("platform", "api", "crc32", "bmp", "zlib", "png", "fold", "cm", "pal", "sys_posix", "main")
WINDOWS_ONLY = ("mem", "sys")


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
    if os.name == "nt":
        cc = ROOT / "tools/llvm-mingw/bin/clang.exe"
    else:
        cc = ROOT / "tools/llvm-mingw-linux/bin/clang"
    base = ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-fno-builtin", "-fvisibility=hidden", "-march=x86-64-v2", "-I" + str(ROOT / "include")]
    entries = []
    for name in LINUX_SOURCES:
        src = ROOT / "src" / (name + ".c")
        if src.is_file():
            entries.append({"directory": str(ROOT), "file": str(src),
                            "arguments": [str(cc), "--target=x86_64-linux-gnu", *base, "-fPIC", "-pthread", "-c", str(src)]})
    for name in WINDOWS_ONLY:
        src = ROOT / "src" / (name + ".c")
        if src.is_file():
            entries.append({"directory": str(ROOT), "file": str(src),
                            "arguments": [str(cc), "--target=x86_64-w64-mingw32", *base, "-ffreestanding", "-fno-stack-protector", "-c", str(src)]})
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the root database is missing or stale")
    args = parser.parse_args()
    entries, used = merged()
    text = json.dumps(entries, indent=2) + "\n"
    if args.check:
        if not OUT.is_file() or OUT.read_text(encoding="utf-8") != text:
            print("gen-compdb: compile_commands.json is missing or stale; run scripts/gen-compdb.py", file=sys.stderr)
            return 1
        print(f"gen-compdb: compile_commands.json current ({len(entries)} entries from {', '.join(used)})")
        return 0
    partial = OUT.with_suffix(".json.partial")
    partial.write_text(text, encoding="utf-8")
    partial.replace(OUT)
    print(f"gen-compdb: wrote compile_commands.json ({len(entries)} entries from {', '.join(used)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
