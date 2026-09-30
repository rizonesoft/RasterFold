#!/usr/bin/env python3
"""Read-only validation of both independent intelligence registers."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
if sys.argv[1:] != ['validate']:
    raise SystemExit('Use scripts/intelligence.py validate. Mutations use the family-owned command.')
codes = [subprocess.call([sys.executable, str(root / f'.{family}/runner/intelligence.py'), 'validate'], cwd=root) for family in ('claude', 'codex')]
raise SystemExit(int(any(codes)))
