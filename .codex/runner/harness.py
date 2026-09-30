#!/usr/bin/env python3
"""Bounded, read-only native Codex discovery and hook-trust preflight."""
import json
from pathlib import Path
import selectors
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def rpc(method, params, cwd=ROOT):
    process = subprocess.Popen(['codex', 'app-server', '--stdio'], cwd=cwd, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    def send(value):
        process.stdin.write(json.dumps(value) + '\n')
        process.stdin.flush()
    def receive(ident):
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if selector.select(1):
                line = process.stdout.readline()
                if not line:
                    raise RuntimeError('app-server exited')
                value = json.loads(line)
                if value.get('id') == ident:
                    if 'error' in value:
                        raise RuntimeError('app-server request failed: ' + str(value['error']))
                    return value['result']
        raise RuntimeError('app-server deadline exceeded')
    try:
        send({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'clientInfo': {'name': 'rasterfold_codex_preflight', 'version': '1'}, 'capabilities': {'experimentalApi': True}}})
        receive(1)
        send({'jsonrpc': '2.0', 'method': 'initialized'})
        send({'jsonrpc': '2.0', 'id': 2, 'method': method, 'params': params})
        return receive(2)
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        selector.close()
        process.stdin.close()
        process.stdout.close()


def check(data, root=ROOT):
    rows = data.get('data')
    if not isinstance(rows, list) or not rows:
        raise RuntimeError('missing hook inventory')
    hooks = [hook for row in rows for hook in row.get('hooks', []) if str(root / '.codex/hooks.json') in json.dumps(hook)]
    required = {'preToolUse', 'postToolUse', 'sessionStart', 'sessionEnd', 'userPromptSubmit', 'stop', 'interrupt'}
    if required - {h.get('eventName') for h in hooks}:
        raise RuntimeError('required Codex hooks missing or disabled')
    if any(row.get('errors') for row in rows):
        raise RuntimeError('hook configuration has parse errors')
    if any(h.get('trustStatus') not in {'trusted', 'managed'} or h.get('enabled') is False for h in hooks):
        raise RuntimeError('review and trust the current project hooks through Codex /hooks before arming')


if __name__ == '__main__':
    try:
        data = rpc('hooks/list', {'cwds': [str(ROOT)]})
        if len(sys.argv) > 1 and sys.argv[1] == 'trust-check':
            check(data)
            print('all required Codex hooks trusted and enabled')
        else:
            print(json.dumps(data, indent=2))
    except (RuntimeError, OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(2)
