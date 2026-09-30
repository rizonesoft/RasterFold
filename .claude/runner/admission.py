"""Claude-owned admission implementation. Only its lock file is shared."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import threading
import time

_threads = {}
_mutex = threading.Lock()


class AdmissionBusy(ValueError):
    """The bounded admission wait expired."""


@contextmanager
def admission(root, timeout=10):
    path = Path(root).resolve() / '.runner/admission.lock'
    path.parent.mkdir(parents=True, exist_ok=True)
    with _mutex:
        lock = _threads.setdefault(str(path), threading.Lock())
    deadline = time.monotonic() + timeout
    if not lock.acquire(timeout=timeout):
        raise AdmissionBusy('runner admission is locked; retry after the current transition')
    try:
        with path.open('a+b') as handle:
            _lock_file(handle, deadline)
            try:
                yield
            finally:
                if os.name == 'nt':
                    import msvcrt
                    handle.seek(0); msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
    finally:
        lock.release()


def _lock_file(handle, deadline):
    while True:
        try:
            if os.name == 'nt':
                import msvcrt
                handle.seek(0); msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise AdmissionBusy('runner admission is locked; retry after the current transition')
            time.sleep(.05)


def read_json(path):
    try:
        value = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except FileNotFoundError:
        return None
    if not isinstance(value, dict):
        raise ValueError(f'invalid ownership record: {path}')
    return value


def busy(root, family):
    root = Path(root)
    if family == 'claude':
        state = read_json(root / 'build/claude-campaign-guard.json')
        if state is not None and not state.get('session_id'):
            raise ValueError('Claude ownership record lacks a session')
        return state
    state = read_json(root / '.codex/state/campaign.json')
    if state is not None:
        required={'schema','family','run_id','generation','session_id','status','scope','phase','run_file','started'}
        if not required <= state.keys() or state['schema']!=1 or state['family']!='codex' or state['scope'] not in ('phase','plan') or type(state['phase']) is not int or state['phase']<0 or not isinstance(state['started'],(int,float)) or any(not isinstance(state[key],str) or not state[key] for key in ('session_id','run_id','generation','run_file')):
            raise ValueError('Codex ownership record is malformed')
        if state.get('status') not in ('active', 'paused', 'parked', 'stopped', 'complete') or not state.get('session_id'):
            raise ValueError('Codex ownership record is malformed')
        if state['status'] in ('stopped', 'complete'):
            return None
    return state


def require_idle(root):
    for family in ('claude', 'codex'):
        if busy(root, family):
            raise ValueError(f'{family} owns a run; stop it explicitly before maintenance')
