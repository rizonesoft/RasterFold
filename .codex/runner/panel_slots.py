#!/usr/bin/env python3
"""Private Codex review dispatch. Reviewers receive evidence on stdin, no tools."""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[2]
SLOTS = {'bulk', 'signoff', 'depth', 'plan-primary', 'stamp-check', 'independent', 'arch-primary'}


def load(path=None):
    with open(path or ROOT / '.codex/runner/panel.toml', 'rb') as source:
        table = tomllib.load(source)
    settings=json.loads((ROOT/'.codex/runner/reviewer-settings.json').read_text())
    if settings!={'disableAllHooks':True,'attribution':{'commit':'','pr':''}}:
        raise ValueError('reviewer settings must disable hooks and contain only empty attribution')
    models = table['model']
    writer = table['writer']['model']
    if models[writer]['family'] != 'codex' or models[writer].get('retired'):
        raise ValueError('the Codex panel requires a registered active Codex writer')
    if set(table['slot']) != SLOTS:
        raise ValueError('the private panel must declare every review slot exactly once')
    for name, entry in table['slot'].items():
        model = models[entry['model']]
        if model['family'] != 'claude' or model.get('retired'):
            raise ValueError(f'{name}: the writer family cannot review itself')
        if entry['effort'] not in ('medium', 'high') or not isinstance(entry['timeout'], int) or not 1 <= entry['timeout'] <= 1800:
            raise ValueError(f'{name}: invalid effort or timeout')
    return table


def argv_for_slot(slot, table=None):
    entry = (table or load())['slot'][slot]
    return ['claude', '-p', '--model', entry['model'], '--effort', entry['effort'],
            '--output-format', 'json', '--no-session-persistence', '--strict-mcp-config',
            '--mcp-config', '{"mcpServers":{}}', '--settings',
            str(ROOT / '.codex/runner/reviewer-settings.json'), '--tools', '']


def execute(slot):
    table = load()
    entry = table['slot'][slot]
    print(f"Codex review: {slot}, model={entry['model']}, effort={entry['effort']}, timeout={entry['timeout']}s", file=sys.stderr)
    # No extra arguments can change model, tools, permissions or the deadline.
    with tempfile.TemporaryDirectory(prefix='rasterfold-review-') as cwd:
        process = subprocess.Popen(argv_for_slot(slot, table), cwd=cwd, start_new_session=True)
        def kill():
            try:
                import os
                os.killpg(process.pid, signal.SIGKILL)
            except (AttributeError, ProcessLookupError):
                process.kill()
            process.wait()
        def interrupted(signum, _frame):
            kill();raise SystemExit(128+signum)
        handlers={sig:signal.signal(sig,interrupted) for sig in (signal.SIGTERM,signal.SIGINT)}
        try:
            return process.wait(timeout=entry['timeout'])
        except subprocess.TimeoutExpired:
            kill()
            print('review timed out; no fallback or verdict was recorded', file=sys.stderr)
            return 124
        finally:
            for sig,handler in handlers.items():signal.signal(sig,handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('validate', 'show', 'get', 'exec', 'writer', 'family', 'models'))
    parser.add_argument('slot', nargs='?')
    parser.add_argument('field', nargs='?', choices=('model','effort','timeout','family'))
    args = parser.parse_args()
    try:
        table = load()
        if args.command in ('get', 'exec') and not args.slot:
            parser.error('provide a slot')
        if args.command in ('get','exec') and args.slot not in SLOTS:
            parser.error('unknown slot')
        if args.command == 'exec' and args.field:
            parser.error('review overrides are forbidden')
        if args.command == 'get' and args.field:
            print('claude' if args.field == 'family' else table['slot'][args.slot][args.field]); return 0
        if args.command == 'writer' and args.slot:
            print('codex' if args.slot == 'family' else table['writer']['model']); return 0
        if args.command == 'family':
            print(table['model'][args.slot]['family']); return 0
        if args.command == 'models':
            print('\n'.join(k for k,v in table['model'].items() if v['family'] == args.slot)); return 0
        if args.command == 'exec':
            return execute(args.slot)
        if args.command == 'validate':
            print('Codex panel valid: all seven slots are independent Claude reviews')
        else:
            print(json.dumps(table['slot'][args.slot] if args.command == 'get' else table['writer'] if args.command == 'writer' else table, indent=2))
        return 0
    except (KeyError, ValueError, OSError) as exc:
        print(f'Codex panel: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
