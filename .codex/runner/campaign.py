#!/usr/bin/env python3
"""Codex campaign lifecycle in the existing checkout, with native Stop hooks."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

from admission import AdmissionBusy, admission, busy, read_json

ROOT = Path(__file__).resolve().parents[2]
STATE = '.codex/state/campaign.json'
TERMINAL = ('stopped', 'complete')


def request_directory(root, session):
    if not re.fullmatch(r'[a-zA-Z0-9_-]{8,100}',session or ''):
        raise ValueError('invalid native session identity')
    return Path(root)/'.codex/state/requests'


def request_stop(root,session,action='stop'):
    # Each event has its own immutable record. Pause cannot overwrite Stop,
    # and request identity never depends on wall-clock ordering.
    if action not in ('stop','pause'):raise ValueError('invalid operator action')
    directory=request_directory(root,session);directory.mkdir(parents=True,exist_ok=True)
    ident=uuid.uuid4().hex;path=directory/f'{session}.{ident}.json'
    data=dict(schema=1,id=ident,session_id=session,action=action,created=time.time())
    temp=path.with_suffix('.tmp')
    with temp.open('x') as out:
        json.dump(data,out);out.flush();os.fsync(out.fileno())
    temp.replace(path)
    if os.name!='nt':
        descriptor=os.open(directory,os.O_RDONLY)
        try:os.fsync(descriptor)
        finally:os.close(descriptor)
    fold_operator_requests(root,session)


def fold_operator_requests(root,session):
    try:
        with admission(root):
            state=read_json(Path(root)/STATE)
            if state and state.get('session_id')==session: apply_request(root,state)
    except AdmissionBusy:
        pass  # durable request survives; next admitted call folds it into state


def operator_requests(root,session):
    requests={}
    for path in sorted(request_directory(root,session).glob(f'{session}.*.json')):
        request=read_json(path)
        if not request or request.get('schema')!=1 or request.get('session_id')!=session or request.get('action') not in ('stop','pause') or not isinstance(request.get('id'),str) or not re.fullmatch(r'[0-9a-f]{32}',request['id']) or path.name!=f"{session}.{request['id']}.json":
            raise ValueError('malformed operator request: '+path.name)
        requests[request['id']]=request
    return requests


def pending_request(root,state):
    handled=state.get('handled_requests',[])
    if not isinstance(handled,list) or any(not isinstance(i,str) for i in handled):
        raise ValueError('malformed handled operator requests')
    pending={key:value for key,value in operator_requests(root,state['session_id']).items() if key not in handled}
    if not pending:return None
    return dict(action='stop' if any(r['action']=='stop' for r in pending.values()) else 'pause',ids=sorted(pending))


def apply_request(root,state):
    if state['status'] in TERMINAL:return state
    request=pending_request(root,state)
    if request:
        state.update(status='paused' if request['action']=='pause' else 'stopped',
                     reason='native operator '+request['action'],
                     handled_requests=sorted(set(state.get('handled_requests',[]))|set(request['ids'])),generation=uuid.uuid4().hex)
        save(root,state)
    return state


def grant_resume(root,session):
    with admission(root):
        state=read_json(Path(root)/STATE)
        if not state or state.get('session_id')!=session:return
        state=apply_request(root,state)
        if state['status'] not in ('paused','parked'):return
        state['resume_grant']=state['generation'];save(root,state)


def record_activity(root,session,started):
    with admission(root):
        state=busy(root,'codex')
        if state and state['session_id']==session:
            state=apply_request(root,state)
            if state['status']=='active':
                state.update(tool_started=time.time() if started else 0,heartbeat=time.time())
                save(root,state)


def save(root, state):
    path = Path(root) / STATE
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + f'.{os.getpid()}.tmp')
    with temp.open('w', encoding='utf-8') as out:
        json.dump(state, out, indent=2); out.write('\n'); out.flush(); os.fsync(out.fileno())
    temp.replace(path)


def fingerprint(root):
    result = hashlib.sha256()
    deadline=time.monotonic()+12
    excluded=['.',':(exclude)docs/phase-runs/**',':(exclude).intelligence/**',':(exclude).codex/state/**',':(exclude).runner/**',':(exclude)build/**',':(exclude)bin/**']
    for args in (['ls-files','--stage','-z','--',*excluded], ['diff', 'HEAD', '--binary','--',*excluded], ['ls-files', '--others', '--exclude-standard', '-z','--',*excluded]):
        proc = subprocess.run(['git', *args], cwd=root, capture_output=True, check=True, timeout=max(.1,min(3,deadline-time.monotonic())))
        result.update(proc.stdout)
        if '--others' in args:
            for name in proc.stdout.split(b'\0'):
                if name:
                    path = Path(root) / os.fsdecode(name)
                    if path.is_symlink():
                        result.update(os.fsencode(os.readlink(path)))
                    elif path.is_file():
                        with path.open('rb') as source:
                            while data := source.read(1024 * 1024):
                                if time.monotonic()>deadline:raise TimeoutError('progress fingerprint deadline exceeded')
                                result.update(data)
    return result.hexdigest()


def run_path(root, name):
    path = Path(root) / name
    phase_root=(Path(root)/'docs/phase-runs').resolve()
    if not phase_root.is_relative_to(Path(root).resolve()) or Path(name).is_absolute() or not path.resolve().is_relative_to(phase_root) or path.parent != Path(root)/'docs/phase-runs' or path.suffix!='.md' or path.is_symlink() or '..' in Path(name).parts or not name.startswith('docs/phase-runs/'):
        raise ValueError('run file must be inside docs/phase-runs/')
    if not path.is_file():
        raise ValueError('create the run findings file before arming')
    return path


def owned(root, session):
    state = read_json(Path(root) / STATE)
    if not state or state.get('session_id') != session:
        raise ValueError('this session does not own the Codex run')
    return state


def run_identity(root,name):
    stat=run_path(root,name).stat()
    if not stat.st_ino:raise ValueError('run file identity is unavailable')
    return f'{stat.st_dev}:{stat.st_ino}'


def start(root, session, phase, run_file, scope='plan', *, handled_requests=None, supervise=False):
    if not re.fullmatch(r'[a-zA-Z0-9_-]{8,100}', session or ''):
        raise ValueError('a native session identity is required')
    if phase < 0 or scope not in ('phase', 'plan'):
        raise ValueError('invalid phase or scope')
    run_file=run_path(root, run_file).relative_to(root).as_posix()
    handled=sorted(operator_requests(root,session)) if handled_requests is None else list(handled_requests)
    initial_fingerprint=fingerprint(root)
    with admission(root):
        if busy(root, 'claude'):
            raise ValueError('Claude owns the checkout; stop that run first')
        old = busy(root, 'codex')
        if old:
            raise ValueError('Codex run already owns the checkout; resume or stop it explicitly')
        # Canonical TODO Origin records use 12 hex digits. Ownership and stale
        # prompt protection retain the independent full-width generation token.
        state = dict(schema=1, family='codex', session_id=session, run_id=uuid.uuid4().hex[:12],
                     generation=uuid.uuid4().hex, status='active', scope=scope, phase=phase,
                     run_file=run_file, used_run_files=[run_file], used_run_file_ids=[run_identity(root,run_file)],
                     handled_requests=handled, started=time.time(), last_progress=time.time(),
                     fingerprint=initial_fingerprint, unchanged_stops=0, nudges=0, heartbeat=time.time())
        state=apply_request(root,state)
        save(root, state)
        if supervise and state['status']=='active':state=_launch_watch_locked(root,state)
        return state


def transition(root, session, action, reason, phase=None, run_file=None, *, supervise=False):
    readiness=None
    if action=='complete':
        before=owned(root,session)
        readiness=subprocess.run([sys.executable,str(Path(root)/'scripts/todo-graph.py'),'query','ready'],cwd=root,capture_output=True,text=True,timeout=120)
    with admission(root):
        state = apply_request(root,owned(root, session))
        if state['status'] in TERMINAL:
            raise ValueError('the run is terminal; start a new run explicitly')
        if action == 'advance':
            if state['scope'] != 'plan' or state['status'] != 'active':
                raise ValueError('only an active plan run advances to another phase')
            if phase is None or phase < 0:
                raise ValueError('advance requires a phase')
            run_file=run_path(root,run_file or '').relative_to(root).as_posix()
            identity=run_identity(root,run_file)
            used_ids=state.setdefault('used_run_file_ids',[run_identity(root,state['run_file'])])
            if run_file in state.get('used_run_files',[state['run_file']]) or identity in used_ids or phase==state['phase']:
                raise ValueError('advance requires a different phase and run file')
            marker=r'^(?:## Closeout|PARKED)\b[^\n]*\brun=' + re.escape(state['run_id']) + r'\b'
            if re.search(marker,run_path(root,run_file).read_text(),re.M):
                raise ValueError('the next phase file already contains this run closeout or park record')
            old_text = run_path(root, state['run_file']).read_text()
            if not re.search(marker, old_text, re.M):
                raise ValueError('the current phase needs a run-specific closeout or park record')
            state.setdefault('used_run_files',[state['run_file']]).append(run_file)
            used_ids.append(identity)
            state.update(phase=phase, run_file=run_file, unchanged_stops=0)
        elif action == 'complete':
            text = run_path(root, state['run_file']).read_text()
            if not re.search(r'^## Closeout run=' + re.escape(state['run_id']) + r'\s*$', text, re.M):
                raise ValueError('completion needs the matching run closeout heading')
            if before['generation']!=state['generation']:raise ValueError('ownership changed during readiness check')
            proc=readiness
            if state['scope'] == 'plan' and (proc.returncode or not re.search(r'\b0 runnable now\b', proc.stdout)):
                raise ValueError('plan still has runnable work, or readiness could not be proved')
            state['status'] = 'complete'
        elif action == 'resume':
            if state['status'] not in ('paused', 'parked'):
                raise ValueError('resume requires a paused or parked run')
            if busy(root, 'claude'):
                raise ValueError('Claude owns the checkout')
            if state.pop('resume_grant',None)!=state['generation']:
                raise ValueError('resume requires a fresh native operator resume command')
            state.update(status='active', unchanged_stops=0, nudges=0, heartbeat=time.time())
        elif action in ('pause', 'park', 'stop'):
            state['status'] = {'pause':'paused', 'park':'parked', 'stop':'stopped'}[action]
        else:
            raise ValueError('unknown transition')
        if not reason.strip():
            raise ValueError('record the reason for the transition')
        state.update(reason=reason, updated=time.time(), generation=uuid.uuid4().hex)
        save(root, state)
        if supervise and state['status']=='active':state=_launch_watch_locked(root,state)
        return state


def active_state(root,session,generation=None):
    """Read and fold operator requests while the caller holds admission."""
    state=busy(root,'codex')
    if not state or state['session_id']!=session or (generation is not None and state['generation']!=generation):
        return None
    state=apply_request(root,state)
    return state if state['status']=='active' else None


def stop_hook(root, session):
    with admission(root):
        state=active_state(root,session)
        if not state:return {}
        generation=state['generation']
    # Git and file reads must not delay an operator stop behind admission.
    now=fingerprint(root)
    with admission(root):
        state=active_state(root,session,generation)
        if not state:return {}
        state['unchanged_stops'] = state['unchanged_stops'] + 1 if now == state['fingerprint'] else 0
        if now != state['fingerprint']:
            state.update(last_progress=time.time(), nudges=0)
        state['fingerprint'] = now
        state.update(heartbeat=time.time())
        if state['unchanged_stops'] >= 3:
            state.update(status='parked', reason='three Stop events without repository progress; report the blocker', generation=uuid.uuid4().hex)
            save(root, state)
            return {'systemMessage': 'RasterFold campaign parked after three unchanged turns. Report the blocker; resume requires an explicit operator request.'}
        save(root, state)
        return {'decision': 'block', 'reason': f"RasterFold Codex continuation run={state['run_id']} generation={state['generation']}. Continue $process-{state['scope']} from {state['run_file']}; verify readiness and finish the open section. Respect any explicit operator stop. This is generated continuation, not an operator instruction."}


def tick(root, session, generation, interval=300, queue=None):
    with admission(root):
        state=active_state(root,session,generation)
        if not state:return False
        if not heartbeat_due(state,interval):return True
    current=fingerprint(root)
    # Recheck ownership, operator requests and tool activity after measurement.
    # Only bounded queue delivery holds admission; generated prompts also check
    # durable requests before allowing a queued continuation to execute.
    with admission(root):
        state=active_state(root,session,generation)
        if not state:return False
        if not heartbeat_due(state,interval):return True
        if current != state['fingerprint']:
            state.update(fingerprint=current, last_progress=time.time(), nudges=0)
        if state['nudges'] >= 2:
            state.update(status='parked', reason='two heartbeat nudges without progress', generation=uuid.uuid4().hex)
            save(root, state)
            return False
        message = f"RasterFold Codex heartbeat run={state['run_id']} generation={generation}. First read .codex/state/campaign.json. Continue only if this session and generation still own an active run. Otherwise discard this generated message. Resume $process-{state['scope']} at {state['run_file']}."
        if queue is None:
            queue = lambda msg: subprocess.run(['codex', 'queue', '--thread', session, '--message', msg, '-C', str(root)], capture_output=True, text=True, timeout=5, check=True)
        try:
            queue(message)
        except (OSError, subprocess.SubprocessError) as exc:
            state.update(status='parked', reason=f'heartbeat delivery failed: {type(exc).__name__}', generation=uuid.uuid4().hex)
            save(root, state)
            return False
        state.update(heartbeat=time.time(), nudges=state['nudges'] + 1)
        save(root, state)
        return True


def heartbeat_due(state,interval):
    now=time.time()
    return not (state.get('tool_started') and now-state['tool_started']<1800) and now-state['heartbeat']>=interval


def _launch_watch_locked(root, state):
    """Called inside the SAME admission that saved the active generation."""
    current=active_state(root,state['session_id'],state['generation'])
    if not current:return owned(root,state['session_id'])
    try:
        logs = Path(root) / '.codex/state/heartbeat.log'
        with logs.open('ab') as log:
            subprocess.Popen([sys.executable, __file__, '--root', str(root), 'watch', '--session', state['session_id'], '--generation', state['generation']], cwd=root, stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
    except (OSError,ValueError,subprocess.SubprocessError) as exc:
        try:current=apply_request(root,current)
        except ValueError:
            pass  # a malformed request must not prevent parking after spawn failure
        if current['status']=='active':
            current.update(status='parked',reason='heartbeat launch failed: '+type(exc).__name__,generation=uuid.uuid4().hex)
            save(root,current)
        raise
    return current


def park_failed_watch(root,session,generation,reason):
    while True:
        try:
            with admission(root):
                state=busy(root,'codex')
                if state and state['session_id']==session and state['generation']==generation and state['status']=='active':
                    try:state=apply_request(root,state)
                    except ValueError:
                        pass  # malformed requests still require a stopped supervisor
                    if state['status']=='active':
                        state.update(status='parked',reason=reason,generation=uuid.uuid4().hex)
                        save(root,state)
            return
        except AdmissionBusy:
            # Stay alive but quiescent until we can park or observe replacement.
            # No continuation is queued while error cleanup waits for admission.
            time.sleep(5)


def watch(root,session,generation):
    while True:
        try:
            if not tick(root,session,generation):return 0
        except AdmissionBusy:
            pass  # bounded contention is transient, not a supervisor failure
        except Exception as exc:
            # Never overwrite malformed, terminal or replaced ownership. A
            # readable active owner is parked before this supervisor exits.
            try:
                park_failed_watch(root,session,generation,'heartbeat inspection failed: '+type(exc).__name__)
            except Exception as park_error:
                print('heartbeat could not park ownership: '+type(park_error).__name__,file=sys.stderr)
            print('heartbeat inspection failed: '+type(exc).__name__,file=sys.stderr)
            return 2
        time.sleep(5)


def status(root):
    with admission(root):
        busy(root,'codex')  # malformed ownership is an error, never idle
        state=read_json(Path(root)/STATE)
        if state:state=apply_request(root,state)
        return {'codex':state,'claude':busy(root,'claude')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('action', choices=('start','status','advance','pause','park','stop','resume','complete','watch'))
    parser.add_argument('--session', default=os.environ.get('CODEX_THREAD_ID'))
    parser.add_argument('--phase', type=int)
    parser.add_argument('--run-file')
    parser.add_argument('--scope', choices=('plan','phase'), default='plan')
    parser.add_argument('--reason', default='')
    parser.add_argument('--generation')
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.action == 'status':
            print(json.dumps(status(root), indent=2))
            return 0
        if args.action == 'watch':
            return watch(root,args.session,args.generation)
        if args.action in ('stop','pause'):
            request_stop(root,args.session,args.action)
            print(json.dumps({'operator_request':args.action,'session_id':args.session}))
            return 0
        # Capture the start boundary before native discovery can block. An
        # interrupt arriving during trust inspection must still cancel start.
        baseline=sorted(operator_requests(root,args.session)) if args.action=='start' else None
        if args.action in ('start', 'resume'):
            from harness import check, rpc
            check(rpc('hooks/list', {'cwds':[str(root)]}, cwd=root), root)
        if args.action == 'start':
            state = start(root, args.session, args.phase if args.phase is not None else -1, args.run_file or '', args.scope, handled_requests=baseline, supervise=True)
        else:
            state = transition(root, args.session, args.action, args.reason, args.phase, args.run_file, supervise=True)
        print(json.dumps(state, indent=2))
        return 0
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f'campaign: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
