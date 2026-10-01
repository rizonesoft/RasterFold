#!/usr/bin/env python3
"""Route Claude subagents: validate the fleet, guard dispatches, and prove what served them.

`.claude/agents/*.md` is the single source of each delegated agent's model alias
and effort. This tool holds them to the routing policy (a Sonnet alias, effort
high or above, read-only classes without write tools), refuses dispatches that
would bypass the fleet or silently change its model, records the model and
effort each subagent was actually served (from its transcript, at SubagentStop),
and checks the recorded probe in `.claude/agents/routing-probe.json`.

  validate             fleet, settings wiring, routing doc and probe agree
  guard                PreToolUse hook on Agent (JSON on stdin)
  ledger               SubagentStop hook (JSON on stdin); appends build/agent-routing/ledger.jsonl
  verify [--session S] [--record]
                       check served model and effort for a session; --record updates the probe
  grant <agent> <path>...
                       give a writing agent its owned paths and snapshot the tree for weaken-scan
  protect              PreToolUse hook on Edit/Write/NotebookEdit: fleet writes only to granted paths
  weaken-scan          flag every change since the grant that could weaken the implementation
  release              clear the grants once the lead has accepted the slice
  --self-test          exercise every leg in a throwaway tree

Standard library only. Codex has its own mechanics and never reads this.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
AGENTS = '.claude/agents'
PROBE = '.claude/agents/routing-probe.json'
SETTINGS = '.claude/settings.json'
DOC = '.claude/agent-routing.md'
LEDGER = 'build/agent-routing/ledger.jsonl'
GRANTS = 'build/agent-routing/grants.json'
BASELINE = 'build/agent-routing/baseline'

# The contract an implementer is judged by, and the records of judgment. No grant
# reaches these: the lead edits them itself.
NEVER_GRANTED = ('build/agent-routing/', 'AGENTS.md', 'CLAUDE.md', '.gitignore', '.gitattributes', 'todo/', 'docs/reviews/', 'docs/phase-runs/', 'docs/drift.json', '.conclave/', '.claude/',
                 '.codex/', '.agents/', '.intelligence/', 'tools/githooks/')
# Grantable, but every change is flagged for the lead: a weaker gate is a weaker implementation.
GATES = ('.github/', 'scripts/', 'tests/test_correctness.py', 'tests/fuzz_decode.py', 'tests/rfi_ref.py', 'spec/FOLD.md')
WEAKENING = (
    ('removed check', 'removed', re.compile(r'\brf_fail\s*\(|\bRF_ERR_|\brf_write_atomic|\bassert|\braise\b|FormatError|Error\(|\breturn\s+RF_|\bgoto\s+(fail|bad|out)|\bbad\s*=|\breject|\brefuse|>\s*RF_MAX|<\s*RF_MIN|\bif\s*\(.*[<>]=?.*\)\s*(return|goto)', re.I)),
    ('removed test', 'removed', re.compile(r'^\s*def\s+test|\bcase\s*\(|^\s*check\(|self\.assert', re.I)),
    ('skip or stub added', 'added', re.compile(r'\bskip|xfail|#\s*if\s+0|NotImplemented|\bTODO\b|\bFIXME\b|\bXXX\b|\bstub|placeholder|\bHACK\b|\bpass\s*(#.*)?$|noqa|pragma: no cover|NOLINT', re.I)),
    ('limit changed', 'either', re.compile(r'^\s*#\s*define\s+\w+\s+\(?-?\d|^\s*[A-Z][A-Z0-9_]{2,}\s*=\s*[-\d(]')),
    ('known answer changed', 'either', re.compile(r'KAT_|known.answer', re.I)),
)

# Policy. The fleet runs on the Sonnet alias, so it follows the newest Sonnet
# Claude Code serves; judgment work stays with the lead session.
ALLOWED_MODELS = {'sonnet'}
EFFORTS = ('low', 'medium', 'high', 'xhigh', 'max')
MIN_EFFORT = 'high'
WRITE_TOOLS = {'Edit', 'Write', 'NotebookEdit'}
CLASSES = {'explorer': False, 'researcher': False, 'reviewer': False, 'runner': False, 'implementer': True}
# Generic built-in workers run at an unpinned model and effort; repository work
# goes to the fleet instead. An omitted subagent_type means general-purpose.
UNROUTED = {'', 'general-purpose', 'explore', 'plan', 'claude'}
# Environment that silently replaces a routed model.
SUBSTITUTING_ENV = ('CLAUDE_CODE_SUBAGENT_MODEL', 'CLAUDE_CODE_SUBAGENT_MODEL_FORCE', 'ANTHROPIC_DEFAULT_SONNET_MODEL')
FAMILY = re.compile(r'^claude-(sonnet|opus|haiku|fable)-')
CLASS_MARK = re.compile(r'<!-- agent-class: ([a-z]+) -->')


def frontmatter(path: Path) -> tuple[dict, str]:
    text = path.read_text(encoding='utf-8')
    if not text.startswith('---\n'):
        raise ValueError(f'{path.name}: no frontmatter')
    head, _, body = text[4:].partition('\n---\n')
    fields = {}
    for line in head.splitlines():
        key, sep, value = line.partition(':')
        if sep and key.strip() and not key.startswith(' '):
            fields[key.strip()] = value.strip()
    return fields, body


def fleet(root: Path) -> dict[str, dict]:
    agents = {}
    for path in sorted((root / AGENTS).glob('*.md')):
        fields, body = frontmatter(path)
        mark = CLASS_MARK.search(body)
        agents[path.stem] = {
            'name': fields.get('name', ''),
            'model': fields.get('model', ''),
            'effort': fields.get('effort', ''),
            'tools': [t.strip() for t in fields.get('tools', '').split(',') if t.strip()],
            'class': mark.group(1) if mark else '',
            'description': fields.get('description', ''),
        }
    return agents


def family(model: str) -> str:
    match = FAMILY.match(model or '')
    return match.group(1) if match else ''


def hook_wired(settings: dict, event: str, verb: str, matcher: str | None = None) -> bool:
    for entry in settings.get('hooks', {}).get(event, []):
        if matcher is not None and entry.get('matcher') != matcher:
            continue
        for hook in entry.get('hooks', []):
            if 'scripts/agent_routing.py' in hook.get('command', '') and hook.get('command', '').rstrip().endswith(verb):
                return True
    return False


def validate(root: Path) -> list[str]:
    errors = []
    try:
        agents = fleet(root)
    except (OSError, ValueError) as exc:
        return [str(exc)]
    if not agents:
        errors.append(f'{AGENTS}: no agents')
    for stem, a in agents.items():
        where = f'{AGENTS}/{stem}.md'
        if a['name'] != stem:
            errors.append(f'{where}: name {a["name"]!r} must equal the file name')
        if not a['description']:
            errors.append(f'{where}: description is required')
        if a['model'] not in ALLOWED_MODELS:
            errors.append(f'{where}: model {a["model"]!r} is not a routed alias {sorted(ALLOWED_MODELS)} (no pinned ids, haiku, or inherit)')
        if a['effort'] not in EFFORTS or EFFORTS.index(a['effort']) < EFFORTS.index(MIN_EFFORT):
            errors.append(f'{where}: effort {a["effort"]!r} must be set explicitly to {MIN_EFFORT} or above')
        if a['class'] not in CLASSES:
            errors.append(f'{where}: agent-class marker {a["class"]!r} is not one of {sorted(CLASSES)}')
        elif not CLASSES[a['class']] and WRITE_TOOLS & set(a['tools']):
            errors.append(f'{where}: {a["class"]} class may not hold {sorted(WRITE_TOOLS & set(a["tools"]))}')
        if not a['tools']:
            errors.append(f'{where}: tools must be listed explicitly')
    try:
        settings = json.loads((root / SETTINGS).read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        errors.append(f'{SETTINGS}: {exc}')
    else:
        if not hook_wired(settings, 'PreToolUse', 'guard', 'Agent'):
            errors.append(f'{SETTINGS}: PreToolUse matcher "Agent" must run scripts/agent_routing.py guard')
        if not hook_wired(settings, 'PreToolUse', 'protect', 'Edit|Write|NotebookEdit'):
            errors.append(f'{SETTINGS}: PreToolUse matcher "Edit|Write|NotebookEdit" must run scripts/agent_routing.py protect')
        if not hook_wired(settings, 'SubagentStop', 'ledger'):
            errors.append(f'{SETTINGS}: SubagentStop must run scripts/agent_routing.py ledger')
    try:
        doc = (root / DOC).read_text(encoding='utf-8')
    except OSError as exc:
        errors.append(f'{DOC}: {exc}')
    else:
        rows = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'^\| `([a-z0-9-]+)` \| `([a-z0-9-]+)` \| `([a-z]+)` \|', doc, re.M)}
        for stem, a in agents.items():
            if rows.get(stem) != (a['model'], a['effort']):
                errors.append(f'{DOC}: routing table row for `{stem}` must read | `{stem}` | `{a["model"]}` | `{a["effort"]}` |')
        for stem in sorted(set(rows) - set(agents)):
            errors.append(f'{DOC}: routing table names `{stem}`, which has no agent file')
    try:
        probe = json.loads((root / PROBE).read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        errors.append(f'{PROBE}: {exc} (run a delegated probe, then verify --record)')
        probe = {}
    for stem, a in agents.items():
        entry = probe.get('agents', {}).get(stem)
        if not entry:
            errors.append(f'{PROBE}: `{stem}` has never been probed (dispatch it, then verify --record)')
            continue
        if entry.get('alias') != a['model'] or entry.get('effort') != a['effort']:
            errors.append(f'{PROBE}: `{stem}` probe ran {entry.get("alias")}/{entry.get("effort")}, the file says {a["model"]}/{a["effort"]}; re-probe')
        if family(entry.get('served', '')) != a['model']:
            errors.append(f'{PROBE}: `{stem}` was served {entry.get("served")!r}, not the {a["model"]} family')
        if entry.get('served_effort') != a['effort']:
            errors.append(f'{PROBE}: `{stem}` was served effort {entry.get("served_effort")!r}, routed to {a["effort"]}')
        if not isinstance(entry.get('turns'), int) or entry['turns'] < 1:
            errors.append(f'{PROBE}: `{stem}` records no served turns')
        newest = entry.get('catalog_newest')
        if newest not in (None, 'unknown') and entry.get('served') != newest:
            errors.append(f'{PROBE}: `{stem}` was served {entry.get("served")!r}, not the catalog newest {newest!r}')
    return errors


# ---- hooks ---------------------------------------------------------------

def deny(reason: str) -> dict:
    return {'hookSpecificOutput': {'hookEventName': 'PreToolUse', 'permissionDecision': 'deny', 'permissionDecisionReason': reason}}


def guard(root: Path, payload: dict, env: dict) -> dict | None:
    if payload.get('tool_name') != 'Agent':
        return None
    args = payload.get('tool_input') or {}
    kind = str(args.get('subagent_type') or '')
    agents = fleet(root)
    names = ', '.join(sorted(agents))
    if kind.lower() in UNROUTED:
        return deny(f'"{kind or "general-purpose"}" runs at an unpinned model and effort. Route repository work to a fleet agent ({names}; see {DOC}), or use subagent_type "fork" for lead-grade judgment.')
    if kind not in agents:
        return None
    if args.get('isolation'):
        return deny(f'{kind} must work in the shared checkout (AGENTS.md: no isolated worktrees); drop isolation and give it owned paths instead.')
    want = agents[kind]['model']
    asked = str(args.get('model') or '')
    if asked and asked != want:
        return deny(f'{kind} is routed to {want} at effort {agents[kind]["effort"]}; a model override ({asked}) would substitute it. Drop the model argument, or change the agent file and re-probe.')
    for var in SUBSTITUTING_ENV:
        if env.get(var):
            return deny(f'{var} is set and would replace the routed model for {kind}. Unset it before delegating.')
    return None


def served(transcript: Path) -> dict:
    models, efforts, responses, evidenced = set(), set(), set(), set()
    with transcript.open(encoding='utf-8', errors='replace') as handle:
        for line in handle:
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get('type') != 'assistant':
                continue
            message = row.get('message') or {}
            model = message.get('model') if isinstance(message, dict) else None
            if not model or model.startswith('<'):
                continue
            # One response can span several records sharing a message id.
            response = message.get('id') or row.get('requestId') or row.get('uuid') or len(responses)
            responses.add(response)
            models.add(model)
            for key in ('effort', 'perTurnEffort'):
                if row.get(key):
                    efforts.add(row[key])
                    evidenced.add(response)
    return {'models': sorted(models), 'efforts': sorted(efforts), 'turns': len(responses), 'unevidenced': len(responses - evidenced)}


def judge(expected: dict | None, got: dict) -> list[str]:
    if expected is None:
        return []
    problems = []
    if not got['turns']:
        problems.append('no served turns recorded')
    for model in got['models']:
        if family(model) != expected['model']:
            problems.append(f'served {model}, routed to {expected["model"]}')
    if got.get('unevidenced', 1):
        problems.append(f'{got.get("unevidenced", "unknown")} served response(s) recorded no effort')
    if got['turns'] and got['efforts'] != [expected['effort']]:
        problems.append(f'effort {got["efforts"] or "unrecorded"}, routed to {expected["effort"]}')
    return problems


def ledger(root: Path, payload: dict) -> tuple[dict, list[str]]:
    kind = str(payload.get('agent_type') or '')
    transcript = Path(str(payload.get('agent_transcript_path') or ''))
    expected = fleet(root).get(kind)
    got = served(transcript) if transcript.is_file() else {'models': [], 'efforts': [], 'turns': 0, 'unevidenced': 0}
    problems = judge(expected, got)
    entry = {
        'at': dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'session': payload.get('session_id', ''), 'agent_id': payload.get('agent_id', ''), 'agent_type': kind,
        'routed': {'model': expected['model'], 'effort': expected['effort']} if expected else None,
        **got, 'ok': not problems, 'problems': problems,
    }
    path = root / LEDGER
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a', encoding='utf-8') as handle:
        handle.write(json.dumps(entry, sort_keys=True) + '\n')
    return entry, problems


# ---- ownership and weakening ------------------------------------------------

def git(root: Path, *args: str, binary: bool = False, missing_ok: bool = False):
    """Run git; any failure raises, so a broken snapshot or scan fails closed."""
    out = subprocess.run(['git', '--no-replace-objects', *args], cwd=root, capture_output=True, check=False)
    if out.returncode and not missing_ok:
        raise RuntimeError(f'git {" ".join(args[:3])} failed: {out.stderr.decode("utf-8", "replace").strip()[:200]}')
    if out.returncode:
        return b'' if binary else ''
    return out.stdout if binary else out.stdout.decode('utf-8', 'replace')


def digest(path: Path) -> str | None:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def dirty(root: Path) -> list[str]:
    # Staged and unstaged changes against HEAD, plus untracked files.
    listing = git(root, 'diff', '--name-only', '--no-renames', '-z', 'HEAD') + '\0' + git(root, 'ls-files', '-z', '-o', '--exclude-standard')
    return sorted({p for p in listing.split('\0') if p})


def norm(path: str) -> str:
    path = path.replace('\\', '/')
    while path.startswith('./'):
        path = path[2:]
    return path


def covered(rel: str, paths: list[str]) -> bool:
    # Grants are literal files or directories, never patterns.
    return any(rel == p.rstrip('/') or rel.startswith(p.rstrip('/') + '/') for p in paths)


def control_files(root: Path) -> dict[str, str]:
    """Hash every contract, record, and control file, ignored ones included."""
    found = {}
    for top in NEVER_GRANTED:
        if top.startswith('build/'):
            continue
        base = root / top.rstrip('/')
        files = [base] if base.is_file() else (base.rglob('*') if base.is_dir() else [])
        for f in files:
            if f.is_file() and '__pycache__' not in f.parts and f.suffix not in ('.pyc', '.lock'):
                found[f.relative_to(root).as_posix()] = digest(f)
    return found


def never(rel: str) -> bool:
    rel = norm(rel)
    return any(rel == p.rstrip('/') or rel.startswith(p) for p in NEVER_GRANTED)


def locked(root: Path):
    sys.path.insert(0, str(ROOT / '.claude/runner'))
    from admission import admission  # the repository's existing cross-process lock
    return admission(root)


def grant(root: Path, agent: str, paths: list[str]) -> list[str]:
    paths = [norm(p) for p in paths]
    errors = [f'{p}: never granted (the lead edits contract and record files itself)' for p in paths if never(p) or p.startswith('/') or '..' in p.split('/')]
    errors += [f'{p}: grants are literal files or directories, not patterns' for p in paths if any(c in p for c in '*?[')]
    if agent not in fleet(root):
        errors.append(f'{agent}: not a fleet agent')
    if errors:
        return errors
    try:
        with locked(root):
            return claim(root, agent, paths)
    except (RuntimeError, OSError, ValueError) as exc:
        return [f'grant failed, nothing granted: {exc}']


def overlaps(a: str, b: str) -> bool:
    a, b = a.rstrip('/'), b.rstrip('/')
    return a == b or a.startswith(b + '/') or b.startswith(a + '/')


def claim(root: Path, agent: str, paths: list[str]) -> list[str]:
    path = root / GRANTS
    state = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    clashes = [f'{p}: overlaps {q}, granted to {other}; owned paths never overlap'
               for other, held in state.get('agents', {}).items() if other != agent for q in held for p in paths if overlaps(p, q)]
    if clashes:
        return clashes
    if not state.get('agents'):
        # First grant of a batch: snapshot so weaken-scan sees only what the dispatch changed.
        shutil.rmtree(root / BASELINE, ignore_errors=True)
        snap = {}
        for rel in dirty(root):
            snap[rel] = digest(root / rel)
            if snap[rel]:
                copy = root / BASELINE / rel
                copy.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(root / rel, copy)
        state = {'head': git(root, 'rev-parse', 'HEAD').strip(), 'snap': snap, 'control': control_files(root), 'index': git(root, 'write-tree').strip(), 'agents': {}, 'holders': {}}
    state['agents'][agent] = sorted(set(state['agents'].get(agent, [])) | set(paths))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
    return []


def protect(root: Path, payload: dict) -> dict | None:
    kind = str(payload.get('agent_type') or '')
    if not payload.get('agent_id') or payload.get('tool_name') not in WRITE_TOOLS or kind not in fleet(root):
        return None
    args = payload.get('tool_input') or {}
    target = Path(str(args.get('file_path') or args.get('notebook_path') or ''))
    target = (target if target.is_absolute() else Path(str(payload.get('cwd') or root)) / target).resolve()
    try:
        rel = target.relative_to(root.resolve()).as_posix()
    except ValueError:
        return None  # outside the repository: scratch space
    if rel.startswith('build/') and not rel.startswith('build/agent-routing'):
        return None  # ignored scratch and logs
    if never(rel):
        return deny(f'{rel} is a contract or record file; fleet agents never write it. Return BLOCKED to the lead instead.')
    with locked(root):
        return hold(root, kind, rel, str(payload['agent_id']))


def hold(root: Path, kind: str, rel: str, agent_id: str) -> dict | None:
    path = root / GRANTS
    state = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    if not covered(rel, state.get('agents', {}).get(kind, [])):
        return deny(f'{rel} is not among the paths granted to {kind}. Return BLOCKED to the lead naming the path and why.')
    # A grant belongs to one dispatch: the first instance that writes under it holds it.
    holder = state.setdefault('holders', {}).get(kind)
    if holder is None:
        state['holders'][kind] = agent_id
        path.write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
    elif holder != agent_id:
        return deny(f'the {kind} grant is held by another dispatch; one writer per agent type at a time. Return BLOCKED to the lead.')
    return None


def weaken_scan(root: Path) -> list[str]:
    import difflib
    path = root / GRANTS
    if not path.is_file():
        return ['no grant snapshot: run grant before dispatching a writer']
    state = json.loads(path.read_text(encoding='utf-8'))
    snap, owned = state['snap'], sorted({p for ps in state['agents'].values() for p in ps})
    flags = []
    for rel in sorted(set(dirty(root)) | set(snap)):
        if rel.startswith(('build/', '.runner/')):
            continue  # ignored scratch and shared runner locks
        now = digest(root / rel)
        if rel in snap and snap[rel] == now:
            continue  # untouched since the grant
        if rel in snap:
            before = (root / BASELINE / rel).read_bytes() if snap[rel] else b''
        else:
            before = git(root, 'show', f'{state["head"]}:{rel}', binary=True, missing_ok=True)
        after = (root / rel).read_bytes() if now else b''
        if never(rel):
            flags.append(f'{rel}: contract or record file changed during the dispatch')
        elif not covered(rel, owned):
            flags.append(f'{rel}: changed outside every granted path')
        if covered(rel, list(GATES)):
            flags.append(f'{rel}: gate, oracle, or spec touched; the lead reads every changed line')
        if now is None:
            flags.append(f'{rel}: file deleted')
            continue
        flags += pattern_flags(rel, before, after)
    # The index is what the next commit records. Writers never stage, so any index
    # change is flagged, and its content is scanned like the working tree.
    index = git(root, 'write-tree').strip()
    if not state.get('index'):
        flags.append('grant snapshot has no index tree: release and grant again')
    elif index != state['index']:
        for rel in [p for p in git(root, 'diff', '--name-only', '--no-renames', '-z', state['index'], index).split('\0') if p]:
            flags.append(f'{rel}: staged during the dispatch (writers never stage)')
            flags += pattern_flags(f'{rel} (staged)', git(root, 'show', f'{state["index"]}:{rel}', binary=True, missing_ok=True), git(root, 'show', f'{index}:{rel}', binary=True, missing_ok=True))
    # Ignored control files (local settings, runner state) are invisible to git: compare hashes.
    before, now = state.get('control', {}), control_files(root)
    for rel in sorted(set(before) | set(now)):
        if before.get(rel) != now.get(rel):
            flags.append(f'{rel}: contract or record file changed during the dispatch')
    return list(dict.fromkeys(flags))


def pattern_flags(rel: str, before: bytes, after: bytes) -> list[str]:
    import difflib
    flags, line = [], 0
    old = before.decode('utf-8', 'replace').splitlines()
    new = after.decode('utf-8', 'replace').splitlines()
    for op in difflib.unified_diff(old, new, lineterm='', n=0):
        hunk = re.match(r'^@@ -\d+(?:,\d+)? \+(\d+)', op)
        if hunk:
            line = int(hunk.group(1))
            continue
        if op.startswith(('---', '+++')) or not op[:1] in '+-':
            continue
        side = 'added' if op[0] == '+' else 'removed'
        for label, when, pattern in WEAKENING:
            if when in (side, 'either') and pattern.search(op[1:]):
                flags.append(f'{rel}:{line}: {label}: {op[:160]}')
        if side == 'added':
            line += 1
    return flags


def release(root: Path) -> None:
    with locked(root):
        (root / GRANTS).unlink(missing_ok=True)
        shutil.rmtree(root / BASELINE, ignore_errors=True)


# ---- verification ----------------------------------------------------------

def catalog_latest(path: Path | None = None) -> tuple[dict, str]:
    """Read the newest model per family from the installed Claude Code catalog."""
    if path is None:
        exe = shutil.which('claude')
        if not exe:
            return {}, 'claude not on PATH'
        path = Path(exe).resolve()
    try:
        with path.open('rb') as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
            match = re.search(rb'latest_per_family:\{([^}]{0,400})\}', data)
            table = bytes(match.group(1)) if match else None  # copy out before the mmap closes
    except (OSError, ValueError) as exc:
        return {}, f'catalog unreadable: {exc}'
    if table is None:
        return {}, f'no latest_per_family catalog in {path.name}'
    return dict(re.findall(r'(\w+):"([a-z0-9-]+)"', table.decode('ascii', 'replace'))), str(path)


def claude_version() -> str:
    try:
        out = subprocess.run(['claude', '--version'], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return 'unknown'
    return out.strip().split(' ')[0] or 'unknown'


def read_ledger(root: Path) -> list[dict]:
    path = root / LEDGER
    if not path.is_file():
        return []
    rows = []
    for line in path.read_text(encoding='utf-8').splitlines():
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass
    return rows


def verify(root: Path, session: str | None, record: bool, env: dict) -> int:
    rows = read_ledger(root)
    if not rows:
        print(f'verify: no ledger at {LEDGER}; dispatch a fleet agent first')
        return 1
    session = session or rows[-1].get('session', '')
    rows = [r for r in rows if r.get('session') == session and r.get('routed')]
    agents = fleet(root)
    latest, source = catalog_latest()
    failed = 0
    print(f'session {session}; Claude Code {claude_version()}; catalog newest sonnet: {latest.get("sonnet", "unknown")} ({source})')
    for var in SUBSTITUTING_ENV:
        if env.get(var):
            print(f'FAIL environment {var} is set: it substitutes routed models')
            failed += 1
    best = {}
    for r in rows:
        expected = agents.get(r['agent_type'])
        problems = judge(expected, r) if expected else ['agent file removed']
        newest = latest.get(expected['model']) if expected else None
        if newest and any(m != newest for m in r['models']):
            problems.append(f'served {r["models"]}, the catalog newest is {newest}')
        status = 'FAIL' if problems else 'PASS'
        failed += bool(problems)
        print(f'{status} {r["agent_type"]} ({r["agent_id"]}): served {",".join(r["models"]) or "-"} effort {",".join(r["efforts"]) or "-"} over {r["turns"]} turns' + (f' -- {"; ".join(problems)}' if problems else ''))
        if not problems:
            best[r['agent_type']] = r
    if not rows:
        print('FAIL no fleet dispatch recorded in this session')
        failed += 1
    if record and not failed:
        path = root / PROBE
        probe = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {'agents': {}}
        version = claude_version()
        for kind, r in best.items():
            probe['agents'][kind] = {
                'alias': agents[kind]['model'], 'effort': agents[kind]['effort'], 'served': r['models'][0],
                'served_effort': r['efforts'][0], 'catalog_newest': latest.get(agents[kind]['model'], 'unknown'),
                'probed': r['at'][:10], 'claude_code': version, 'session': session, 'turns': r['turns'],
            }
        probe['agents'] = dict(sorted(probe['agents'].items()))
        path.write_text(json.dumps(probe, indent=2) + '\n', encoding='utf-8')
        print(f'recorded {len(best)} agent(s) in {PROBE}')
    elif record:
        print('not recorded: fix the failures first')
    return 1 if failed else 0


# ---- self-test -------------------------------------------------------------

def self_test() -> int:
    checks = failures = 0

    def check(name, got, want):
        nonlocal checks, failures
        checks += 1
        if got != want:
            failures += 1
            print(f'FAIL {name}: got {got!r}, want {want!r}')

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / AGENTS).mkdir(parents=True)
        (root / 'docs').mkdir()

        def agent(name, model='sonnet', effort='high', tools='Read, Grep', cls='explorer'):
            (root / AGENTS / f'{name}.md').write_text(f'---\nname: {name}\ndescription: test agent\nmodel: {model}\neffort: {effort}\ntools: {tools}\n---\n\n<!-- agent-class: {cls} -->\n', encoding='utf-8')

        agent('mapper')
        agent('builder', tools='Read, Edit, Write, Bash', cls='implementer')
        settings = {'hooks': {
            'PreToolUse': [{'matcher': 'Agent', 'hooks': [{'type': 'command', 'command': 'python3 "$CLAUDE_PROJECT_DIR/scripts/agent_routing.py" guard'}]},
                           {'matcher': 'Edit|Write|NotebookEdit', 'hooks': [{'type': 'command', 'command': 'python3 "$CLAUDE_PROJECT_DIR/scripts/agent_routing.py" protect'}]}],
            'SubagentStop': [{'hooks': [{'type': 'command', 'command': 'python3 "$CLAUDE_PROJECT_DIR/scripts/agent_routing.py" ledger'}]}]}}
        (root / SETTINGS).write_text(json.dumps(settings), encoding='utf-8')
        (root / DOC).write_text('| `builder` | `sonnet` | `high` | x |\n| `mapper` | `sonnet` | `high` | x |\n', encoding='utf-8')
        good = {'alias': 'sonnet', 'effort': 'high', 'served': 'claude-sonnet-9-9', 'served_effort': 'high', 'turns': 3, 'catalog_newest': 'claude-sonnet-9-9'}
        probe = {'agents': {n: dict(good) for n in ('builder', 'mapper')}}
        (root / PROBE).write_text(json.dumps(probe), encoding='utf-8')
        check('valid tree', validate(root), [])
        for field, value, words in (('served_effort', 'low', 'served effort'), ('turns', 0, 'no served turns'), ('catalog_newest', 'claude-sonnet-9-10', 'catalog newest')):
            probe['agents']['mapper'] = {**good, field: value}
            (root / PROBE).write_text(json.dumps(probe), encoding='utf-8')
            check(f'probe {field} contradiction refused', any(words in e for e in validate(root)), True)
        probe['agents']['mapper'] = dict(good)
        (root / PROBE).write_text(json.dumps(probe), encoding='utf-8')

        agent('mapper', effort='medium')
        check('low effort refused', any('effort' in e for e in validate(root)), True)
        agent('mapper', model='haiku')
        check('haiku refused', any("'haiku'" in e for e in validate(root)), True)
        agent('mapper', model='claude-sonnet-5-5')
        check('pinned id refused', any('routed alias' in e for e in validate(root)), True)
        agent('mapper', tools='Read, Edit')
        check('read-only class with Edit refused', any('may not hold' in e for e in validate(root)), True)
        agent('mapper')
        agent('stray')
        errors = validate(root)
        check('unprobed agent refused', any('`stray` has never been probed' in e for e in errors), True)
        check('undocumented agent refused', any('row for `stray`' in e for e in errors), True)
        (root / AGENTS / 'stray.md').unlink()
        (root / SETTINGS).write_text('{}', encoding='utf-8')
        check('unwired hooks refused', len([e for e in validate(root) if SETTINGS in e]), 3)
        (root / SETTINGS).write_text(json.dumps(settings), encoding='utf-8')

        def decision(tool_input, env=None):
            out = guard(root, {'tool_name': 'Agent', 'tool_input': tool_input}, env or {})
            return out['hookSpecificOutput']['permissionDecision'] if out else 'allow'

        check('guard: fleet agent', decision({'subagent_type': 'builder'}), 'allow')
        check('guard: omitted type', decision({}), 'deny')
        check('guard: general-purpose', decision({'subagent_type': 'general-purpose'}), 'deny')
        check('guard: Explore', decision({'subagent_type': 'Explore'}), 'deny')
        check('guard: fork', decision({'subagent_type': 'fork'}), 'allow')
        check('guard: plugin agent', decision({'subagent_type': 'codex:codex-rescue'}), 'allow')
        check('guard: same model', decision({'subagent_type': 'builder', 'model': 'sonnet'}), 'allow')
        check('guard: model override', decision({'subagent_type': 'builder', 'model': 'haiku'}), 'deny')
        check('guard: substituting env', decision({'subagent_type': 'builder'}, {'CLAUDE_CODE_SUBAGENT_MODEL': 'haiku'}), 'deny')
        check('guard: isolation', decision({'subagent_type': 'builder', 'isolation': 'worktree'}), 'deny')
        check('guard: other tool', guard(root, {'tool_name': 'Bash'}, {}), None)

        def transcript(name, model, effort):
            path = root / f'{name}.jsonl'
            rows = [{'type': 'user', 'message': {'role': 'user'}},
                    {'type': 'assistant', 'message': {'id': 'm1', 'model': model}, 'effort': effort, 'perTurnEffort': effort},
                    {'type': 'assistant', 'message': {'id': 'm1', 'model': model}, 'effort': effort, 'perTurnEffort': effort},
                    {'type': 'assistant', 'message': {'model': '<synthetic>'}}]
            path.write_text(''.join(json.dumps(r) + '\n' for r in rows), encoding='utf-8')
            return str(path)

        entry, problems = ledger(root, {'session_id': 's1', 'agent_id': 'a1', 'agent_type': 'builder', 'agent_transcript_path': transcript('ok', 'claude-sonnet-9-9', 'high')})
        check('ledger: routed and served', (entry['ok'], entry['models'], entry['efforts'], entry['turns']), (True, ['claude-sonnet-9-9'], ['high'], 1))
        _, problems = ledger(root, {'session_id': 's1', 'agent_id': 'a2', 'agent_type': 'builder', 'agent_transcript_path': transcript('low', 'claude-sonnet-9-9', 'medium')})
        check('ledger: lower effort caught', problems, ["effort ['medium'], routed to high"])
        _, problems = ledger(root, {'session_id': 's1', 'agent_id': 'a3', 'agent_type': 'mapper', 'agent_transcript_path': transcript('sub', 'claude-haiku-4-5', 'high')})
        check('ledger: substituted model caught', problems, ['served claude-haiku-4-5, routed to sonnet'])
        _, problems = ledger(root, {'session_id': 's1', 'agent_id': 'a4', 'agent_type': 'claude-code-guide', 'agent_transcript_path': transcript('x', 'claude-haiku-4-5', 'low')})
        check('ledger: unrouted agent only recorded', problems, [])
        check('ledger: lines appended', len(read_ledger(root)), 4)
        mixed = root / 'mixed.jsonl'
        mixed.write_text(''.join(json.dumps(r) + '\n' for r in (
            {'type': 'assistant', 'message': {'id': 'm1', 'model': 'claude-sonnet-9-9'}, 'effort': 'high'},
            {'type': 'assistant', 'message': {'id': 'm1', 'model': 'claude-sonnet-9-9'}},
            {'type': 'assistant', 'message': {'id': 'm2', 'model': 'claude-sonnet-9-9'}})), encoding='utf-8')
        _, problems = ledger(root, {'session_id': 's1', 'agent_id': 'a5', 'agent_type': 'builder', 'agent_transcript_path': str(mixed)})
        check('ledger: a response without effort caught, a split response is not', problems, ['1 served response(s) recorded no effort'])
        binary = root / 'claude.bin'
        binary.write_bytes(b'\x00junk latest_per_family:{fable:"claude-fable-9-1",sonnet:"claude-sonnet-9-9"} tail')
        check('catalog read after the map closes', catalog_latest(binary)[0], {'fable': 'claude-fable-9-1', 'sonnet': 'claude-sonnet-9-9'})
        binary.write_bytes(b'no catalog here')
        check('catalog missing reported', catalog_latest(binary)[0], {})
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        run = lambda *a: subprocess.run(['git', *a], cwd=root, capture_output=True, check=True)
        run('init', '-q'); run('config', 'user.email', 't@t'); run('config', 'user.name', 't')
        (root / AGENTS).mkdir(parents=True)
        (root / AGENTS / 'builder.md').write_text('---\nname: builder\ndescription: d\nmodel: sonnet\neffort: high\ntools: Read, Edit\n---\n<!-- agent-class: implementer -->\n', encoding='utf-8')
        (root / 'src').mkdir(); (root / 'todo').mkdir(); (root / 'tests').mkdir()
        (root / 'src/dec.c').write_text('#define RF_MAX_DIM 32768\nint dec(int w) {\n    if (w > RF_MAX_DIM) return RF_EBAD;\n    return 0;\n}\n', encoding='utf-8')
        (root / 'tests/test_x.py').write_text('def test_bound():\n    assert dec(40000) == RF_EBAD\n', encoding='utf-8')
        (root / 'todo/T.md').write_text('- [ ] bound the width\n', encoding='utf-8')
        (root / 'src/lead.c').write_text('int lead;\n', encoding='utf-8')
        run('add', '-A'); run('commit', '-qm', 'base')
        (root / 'src/lead.c').write_text('int lead = 1;\n', encoding='utf-8')  # the lead's own edit, before the grant

        def write(rel, kind='builder', agent_id='a1'):
            out = protect(root, {'agent_id': agent_id, 'agent_type': kind, 'tool_name': 'Write', 'tool_input': {'file_path': str(root / rel)}, 'cwd': str(root)})
            return out['hookSpecificOutput']['permissionDecision'] if out else 'allow'

        check('grant refuses a contract path', bool(grant(root, 'builder', ['todo/T.md'])), True)
        check('grant refuses a non-fleet agent', bool(grant(root, 'general-purpose', ['src/'])), True)
        check('weaken-scan needs a grant', weaken_scan(root), ['no grant snapshot: run grant before dispatching a writer'])
        check('grant src/dec.c and tests/', grant(root, 'builder', ['src/dec.c', 'tests/']), [])
        check('protect: granted file', write('src/dec.c'), 'allow')
        check('protect: granted directory', write('tests/test_y.py'), 'allow')
        check('protect: ungranted file', write('src/other.c'), 'deny')
        check('protect: contract file', write('todo/T.md'), 'deny')
        check('protect: build scratch', write('build/x.log'), 'allow')
        check('protect: the grants file itself', write(GRANTS), 'deny')
        check('grant refuses the grants file', bool(grant(root, 'builder', ['build/agent-routing/'])), True)
        check('protect: lead itself', write('todo/T.md', agent_id=''), 'allow')
        check('protect: non-fleet subagent', write('todo/T.md', kind='other'), 'allow')
        check('clean dispatch has no flags', weaken_scan(root), [])
        check('the codec rejection idiom is a check', bool(WEAKENING[0][2].search('    if (rd16(f) != 0x4D42) return rf_fail(err, RF_ERR_FORMAT, "invalid BMP: signature");')), True)
        check('a != rejection without the idiom is a check', bool(WEAKENING[0][2].search('    if (magic != 7) goto fail;')), True)
        (root / 'src/dec.c').write_text('#define RF_MAX_DIM 65536\nint dec(int w) {\n    return 0; /* TODO bound */\n}\n', encoding='utf-8')
        (root / 'tests/test_x.py').write_text('', encoding='utf-8')
        (root / 'todo/T.md').write_text('- [x] bound the width\n', encoding='utf-8')
        (root / 'src/sneak.c').write_text('int x;\n', encoding='utf-8')
        flags = weaken_scan(root)
        for label, needle in (('limit', 'limit changed'), ('check', 'removed check'), ('stub', 'skip or stub added'), ('test', 'removed test'),
                              ('contract', 'todo/T.md: contract'), ('outside', 'src/sneak.c: changed outside'), ('gate', 'tests/test_x.py'),):
            check(f'weaken-scan flags {label}', any(needle in f for f in flags), True)
        check('weaken-scan ignores the lead\'s pre-grant edit', any('src/lead.c' in f for f in flags), False)
        check('a second instance of the type is refused', write('src/dec.c', agent_id='a2'), 'deny')
        check('the holder keeps writing', write('src/dec.c'), 'allow')
        release(root)
        check('dot directories survive normalization', (grant(root, 'builder', ['./.github/workflows/x.yml']), json.loads((root / GRANTS).read_text())['agents']['builder']), ([], ['.github/workflows/x.yml']))
        check('protect honours a dot-directory grant', write('.github/workflows/x.yml'), 'allow')
        release(root)
        for sneaky in ('.claude/agents/builder.md', './.claude/settings.json', '../outside', 'todo/../todo/T.md', 'src/../todo/T.md', '/etc/passwd'):
            check(f'grant refuses {sneaky}', bool(grant(root, 'builder', [sneaky])), True)
        release(root)
        release(root)
        run('add', '-A'); run('commit', '-qm', 'more')
        check('grant the rename destination only', grant(root, 'builder', ['src/new.c']), [])
        run('mv', 'src/dec.c', 'src/new.c')
        check('a staged rename flags its ungranted source', any(f.startswith('src/dec.c: changed outside') for f in weaken_scan(root)), True)
        run('mv', 'src/new.c', 'src/dec.c')
        (root / '.claude').mkdir(exist_ok=True)
        (root / '.gitignore').write_text('.claude/settings.local.json\n', encoding='utf-8')
        (root / '.claude/settings.local.json').write_text('{"hooks": {}}', encoding='utf-8')
        release(root); run('add', '.gitignore'); run('commit', '-qm', 'ignore')
        check('grant before the ignored write', grant(root, 'builder', ['src/dec.c']), [])
        (root / '.claude/settings.local.json').write_text('{}', encoding='utf-8')
        check('an ignored control file written through Bash is flagged', any('.claude/settings.local.json: contract' in f for f in weaken_scan(root)), True)
        release(root)
        (root / 'src/staged.c').write_text('int s(int w) {\n    if (w > 9) return RF_EBAD;\n    return 0;\n}\n', encoding='utf-8')
        run('add', 'src/staged.c')
        check('grant over a staged file', grant(root, 'builder', ['src/staged.c']), [])
        (root / 'src/staged.c').write_text('int s(int w) {\n    return 0;\n}\n', encoding='utf-8')
        check('a staged check removed by the dispatch is flagged', any('src/staged.c' in f and 'removed check' in f for f in weaken_scan(root)), True)
        release(root)
        (root / 'src/staged.c').write_text('int s(int w) {\n    if (w > 9) return RF_EBAD;\n    return 0;\n}\n', encoding='utf-8')
        run('add', '-A'); run('commit', '-qm', 'staged')
        check('grant before a stage-and-restore', grant(root, 'builder', ['src/staged.c']), [])
        original = (root / 'src/staged.c').read_text(encoding='utf-8')
        (root / 'src/staged.c').write_text('int s(int w) {\n    return 0;\n}\n', encoding='utf-8')
        run('add', 'src/staged.c')
        (root / 'src/staged.c').write_text(original, encoding='utf-8')
        flags = weaken_scan(root)
        check('a weakening staged then hidden in the tree is flagged', (any('staged during the dispatch' in f for f in flags), any('(staged)' in f and 'removed check' in f for f in flags)), (True, True))
        run('reset', '-q')
        release(root)
        lock = root / '.git/index.lock'
        lock.write_text('', encoding='utf-8')
        check('grant fails closed when git cannot snapshot', any('grant failed' in e for e in grant(root, 'builder', ['src/staged.c'])), True)
        check('a failed grant leaves no grant behind', (root / GRANTS).exists(), False)
        lock.unlink()
        for root_file in ('AGENTS.md', 'CLAUDE.md', '.gitignore'):
            check(f'grant refuses {root_file}', bool(grant(root, 'builder', [root_file])), True)
        release(root)
        check('release clears the grant', (root / GRANTS).exists(), False)
        import threading
        (root / AGENTS / 'oracle.md').write_text('---\nname: oracle\ndescription: d\nmodel: sonnet\neffort: high\ntools: Read, Edit\n---\n<!-- agent-class: implementer -->\n', encoding='utf-8')
        grant(root, 'builder', ['src/dec.c'])
        workers = [threading.Thread(target=grant, args=(root, 'oracle', [f'tests/o{i}.py'])) for i in range(8)]
        workers += [threading.Thread(target=write, args=('src/dec.c',)) for _ in range(8)]
        for w in workers:
            w.start()
        for w in workers:
            w.join()
        state = json.loads((root / GRANTS).read_text())
        check('a wildcard grant is refused', bool(grant(root, 'oracle', ['src/*m.c'])), True)
        for clash in ('src/dec.c', 'src/', 'src/dec.c/'):
            check(f'overlapping grant {clash} refused', bool(grant(root, 'oracle', [clash])), True)
        check('disjoint grant across types allowed', grant(root, 'oracle', ['tests/o-extra.py']), [])
        state = json.loads((root / GRANTS).read_text())
        check('concurrent grants and claims lose nothing', (len(state['agents']['oracle']), state['holders'].get('builder')), (9, 'a1'))
        release(root)
    print(f'agent_routing self-test: {checks - failures}/{checks} passed')
    return 1 if failures else 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', nargs='?', choices=('validate', 'guard', 'ledger', 'verify', 'grant', 'protect', 'weaken-scan', 'release'))
    parser.add_argument('rest', nargs='*')
    parser.add_argument('--session')
    parser.add_argument('--record', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == 'validate':
        errors = validate(ROOT)
        for error in errors:
            print(f'FAIL {error}')
        print(f'agent routing: {len(fleet(ROOT)) if (ROOT / AGENTS).is_dir() else 0} agents; {len(errors)} error(s)')
        return 1 if errors else 0
    if args.command == 'guard':
        try:
            out = guard(ROOT, json.load(sys.stdin), dict(os.environ))
        except Exception as exc:  # a broken guard must refuse, never wave a dispatch through
            out = deny(f'agent routing guard failed: {exc}')
        if out:
            print(json.dumps(out))
        return 0
    if args.command == 'ledger':
        # Exit 1 shows stderr to the user. Never exit 2: that would keep the subagent running.
        try:
            _, problems = ledger(ROOT, json.load(sys.stdin))
        except Exception as exc:
            print(f'agent routing ledger failed: {exc}', file=sys.stderr)
            return 1
        if problems:
            print('agent routing: ' + '; '.join(problems), file=sys.stderr)
            return 1
        return 0
    if args.command == 'grant':
        if len(args.rest) < 2:
            parser.error('grant <agent> <path>...')
        errors = grant(ROOT, args.rest[0], args.rest[1:])
        for error in errors:
            print(f'FAIL {error}')
        if not errors:
            print(f'granted {args.rest[0]}: {" ".join(args.rest[1:])}')
        return 1 if errors else 0
    if args.command == 'protect':
        try:
            out = protect(ROOT, json.load(sys.stdin))
        except Exception as exc:  # a broken guard must refuse, never wave a write through
            out = deny(f'agent routing protect failed: {exc}')
        if out:
            print(json.dumps(out))
        return 0
    if args.command == 'weaken-scan':
        try:
            flags = weaken_scan(ROOT)
        except (RuntimeError, OSError, ValueError) as exc:
            flags = [f'scan failed, so nothing is cleared: {exc}']
        for flag in flags:
            print(f'FLAG {flag}')
        print(f'weaken-scan: {len(flags)} flag(s) for the lead to adjudicate')
        return 1 if flags else 0
    if args.command == 'release':
        release(ROOT)
        print('grants released')
        return 0
    if args.command == 'verify':
        return verify(ROOT, args.session, args.record, dict(os.environ))
    parser.print_help()
    return 2


if __name__ == '__main__':
    raise SystemExit(main())
