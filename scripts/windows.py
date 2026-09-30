#!/usr/bin/env python3
"""Run a Windows codec tool from WSL, keeping paths and exit codes intact.

python3 scripts/windows.py tests/test_correctness.py
python3 scripts/windows.py --powershell build.ps1
python3 scripts/windows.py --exe bin/windows-x64/bin/rasterfold.exe inspect image.fold

The codec and its memory measurements remain native Windows processes. Plan,
review and runner tooling runs directly under the host's Python instead.
"""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def windows_path(value: str) -> str:
    if value.startswith("/"):
        return subprocess.check_output(["wslpath", "-w", value], text=True).strip()
    return value


def command(argv: list[str]) -> list[str]:
    mode = "python"
    if argv and argv[0] in ("--powershell", "--exe"):
        mode, argv = argv[0][2:], argv[1:]
    if not argv:
        raise ValueError("provide a script or executable and its arguments")
    target = Path(argv[0])
    if not target.is_absolute():
        target = ROOT / target
    if not target.is_file():
        raise ValueError(f"missing Windows target: {target}")
    if os.name == "nt":
        prefix = [sys.executable] if mode == "python" else (
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
            if mode == "powershell" else [])
        return [*prefix, str(target), *argv[1:]]
    ps = shutil.which("powershell.exe")
    if not ps or not shutil.which("wslpath"):
        raise ValueError("Windows execution requires WSL interop (powershell.exe and wslpath)")
    payload = base64.b64encode(json.dumps({
        "root": windows_path(str(ROOT)), "target": windows_path(str(target)),
        "args": [windows_path(arg) for arg in argv[1:]], "mode": mode,
    }).encode()).decode()
    # Only base64 is interpolated. Paths and arguments are data, never shell code.
    script = (
        "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; "
        "$env:PYTHONIOENCODING='utf-8'; "
        "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
        "$p=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('" + payload + "'))|ConvertFrom-Json; "
        "Set-Location -LiteralPath $p.root; $a=@($p.args); "
        "if($p.mode -eq 'python'){ & python $p.target @a } "
        "elseif($p.mode -eq 'powershell'){ & powershell -NoProfile -ExecutionPolicy Bypass -File $p.target @a } "
        "else { & $p.target @a }; exit $LASTEXITCODE"
    )
    encoded = base64.b64encode(script.encode("utf-16-le")).decode()
    return [ps, "-NoProfile", "-EncodedCommand", encoded]


def main(argv: list[str]) -> int:
    try:
        return subprocess.call(command(argv), cwd=ROOT)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"windows: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
