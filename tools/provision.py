#!/usr/bin/env python3
"""Provision a Linux/WSL clone without changing the system toolchain."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def check_python_version(version_info=None):
    info = sys.version_info if version_info is None else version_info
    if info < (3, 11):
        raise ValueError('Python 3.11+ is required for repository gates')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('Windows hosts use tools/provision.ps1')
    try:
        check_python_version()
        if not args.verify:
            subprocess.run([sys.executable, str(ROOT / 'tools/bootstrap.py')], check=True)
            if not (ROOT / '.venv/bin/python').is_file():
                subprocess.run([sys.executable, '-m', 'venv', str(ROOT / '.venv')], check=True)
            subprocess.run([str(ROOT / '.venv/bin/python'), '-m', 'pip', 'install', '-r', str(ROOT / 'requirements-dev.txt')], check=True)
        else:
            subprocess.run([sys.executable, str(ROOT / 'tools/bootstrap.py'), '--verify'], check=True)
        hooks = subprocess.run(['git','config','--get','core.hooksPath'],cwd=ROOT,capture_output=True,text=True).stdout.strip()
        if not hooks and not args.verify:
            subprocess.run(['git','config','core.hooksPath','tools/githooks'],cwd=ROOT,check=True);hooks='tools/githooks'
        if hooks != 'tools/githooks':
            raise ValueError('core.hooksPath must be tools/githooks; preserve and inspect any custom value first')
        for name in ('pre-commit','commit-msg'):
            path=ROOT/'tools/githooks'/name
            if b'\r' in path.read_bytes():
                raise ValueError(f'{name} must use LF line endings')
            if not os.access(path, os.X_OK):
                if args.verify: raise ValueError(f'{name} is not executable')
                path.chmod(path.stat().st_mode | 0o111)
        for command in ('git','pwsh'):
            if not shutil.which(command):
                raise ValueError(f'{command} is required for repository gates (pwsh serves the PowerShell gate and automation wrappers)')
        python = ROOT/'.venv/bin/python'
        subprocess.run([str(python) if python.is_file() else sys.executable, '-c', 'import sys, PIL\nif sys.version_info < (3, 11):\n    raise SystemExit("Python 3.11+ is required")\nprint("Pillow", PIL.__version__)'], check=True)
        if not Path('/usr/include/stdio.h').is_file():
            raise ValueError('native Linux builds require the host libc development headers (Ubuntu: build-essential)')
        print('provision: local toolchain, hooks, Python/Pillow, PowerShell and native SDK verified')
        print('ffmpeg with libjxl is optional and required only for comparative benchmarks')
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f'provision: {exc}',file=sys.stderr);return 1


if __name__ == '__main__':
    raise SystemExit(main())
