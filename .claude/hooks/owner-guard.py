#!/usr/bin/env python3
"""Keep a Claude session from writing through a different campaign owner."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / '.claude/runner'))
from admission import admission, busy

try:
    payload = json.load(sys.stdin)
    with admission(ROOT):
        codex, claude = busy(ROOT, 'codex'), busy(ROOT, 'claude')
    other = codex or (claude if claude and claude['session_id'] != payload.get('session_id') else None)
    if other and payload.get('tool_name') not in ('Read', 'Glob', 'Grep'):
        raise ValueError('another campaign session owns this checkout; stop it explicitly before writing')
except Exception as exc:
    print(json.dumps({'hookSpecificOutput': {'hookEventName':'PreToolUse', 'permissionDecision':'deny', 'permissionDecisionReason':str(exc)}}))
