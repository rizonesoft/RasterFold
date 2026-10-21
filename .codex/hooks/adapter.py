#!/usr/bin/env python3
"""Native Codex hook protocol; never arms a campaign or changes hook trust."""
import json
import os
from pathlib import Path
import re
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / '.codex/runner'))

def durable_operator_request(payload,root):
    """Stdlib-only safety path: an edited runner module cannot lose Interrupt."""
    event=payload.get('hook_event_name');prompt=payload.get('prompt','');session=payload.get('session_id','')
    action='stop' if event=='Interrupt' else None
    if event=='UserPromptSubmit' and isinstance(prompt,str) and re.fullmatch(r'\s*(?:please\s+)?(?:stop|pause|cancel)(?:\s+(?:the\s+)?(?:plan|campaign|runner))?[.!]?\s*',prompt,re.I):
        action='pause' if re.match(r'\s*(?:please\s+)?pause\b',prompt,re.I) else 'stop'
    if action and re.fullmatch(r'[a-zA-Z0-9_-]{8,100}',session or ''):
        ident=uuid.uuid4().hex
        path=Path(root)/'.codex/state/requests'/f'{session}.{ident}.json';path.parent.mkdir(parents=True,exist_ok=True)
        temp=path.with_suffix('.tmp')
        with temp.open('x') as out:
            json.dump(dict(schema=1,id=ident,session_id=session,action=action,created=time.time()),out);out.flush();os.fsync(out.fileno())
        temp.replace(path)
        if os.name!='nt':
            descriptor=os.open(path.parent,os.O_RDONLY)
            try:os.fsync(descriptor)
            finally:os.close(descriptor)
        return True


def handle(payload, root=ROOT, request_recorded=False):
    event = payload.get('hook_event_name')
    session = payload.get('session_id', '')
    if event == 'SessionStart':
        return {'hookSpecificOutput': {'hookEventName': event, 'additionalContext':
            'RasterFold: read AGENTS.md and .codex/README.md. Codex skills live in .codex/skills and are discovered through .agents/skills. Check .codex/runner/campaign.py status before writing. Use only the Codex controller/review panel for Codex campaigns. No worktrees or recovery snapshots. Review .intelligence/codex/register.json for open evidence. Session start does not start or resume a campaign.'}}
    from admission import admission, busy
    if event == 'PreToolUse':
        with admission(root):
            claude, codex = busy(root, 'claude'), busy(root, 'codex')
            other = claude or (codex if codex and codex['session_id'] != session else None)
        # A read-only reviewer the live Claude run started names that run
        # (scripts/panel_slots.py, every review slot): it is not a writer.
        reviewer = os.environ.get('RASTERFOLD_REVIEW_FOR_RUN', '')
        if claude and not codex and reviewer and reviewer == claude.get('run_id'):
            other = None
        if other and payload.get('tool_name') not in ('Read', 'Glob', 'Grep', 'view_image'):
            return {'hookSpecificOutput': {'hookEventName': event, 'permissionDecision': 'deny',
                'permissionDecisionReason': 'Another runner session owns this checkout. Stop it explicitly before writing or running shell commands.'}}
        if codex and codex['session_id']==session:
            from campaign import record_activity
            record_activity(root, session, True)
        return {}
    if event == 'PostToolUse':
        with admission(root):codex=busy(root,'codex')
        if codex and codex['session_id']==session:
            from campaign import record_activity
            record_activity(root, session, False)
        return {}
    if event == 'Stop':
        with admission(root):codex=busy(root,'codex')
        if not codex or codex['session_id']!=session:return {}
        from campaign import stop_hook
        return stop_hook(root, session)
    if event in ('UserPromptSubmit', 'Interrupt'):
        # Only a complete operator stop command is automatic. Quoted text,
        # generated heartbeat/continuation prose and negated commands never match.
        prompt = payload.get('prompt', '')
        heartbeat = re.match(r'^RasterFold Codex (?:heartbeat|continuation) run=(\S+) generation=(\S+)\.', prompt) if isinstance(prompt,str) else None
        if heartbeat:
            from campaign import pending_request
            with admission(root):
                state = busy(root,'codex')
            if not state or state['session_id'] != session or state['status'] != 'active' or state['run_id'] != heartbeat[1] or state['generation'] != heartbeat[2] or pending_request(root, state):
                return {'decision':'block','reason':'Discarded a stale RasterFold heartbeat. It cannot start or resume a campaign.'}
            return {}
        if isinstance(prompt,str) and re.fullmatch(r'\s*(?:please\s+)?resume(?:\s+(?:the\s+)?(?:plan|campaign|runner))?[.!]?\s*',prompt,re.I):
            from campaign import grant_resume
            grant_resume(root,session)
            return {}
        action = 'stop' if event == 'Interrupt' else None
        if isinstance(prompt, str) and re.fullmatch(r'\s*(?:please\s+)?(?:stop|pause|cancel)(?:\s+(?:the\s+)?(?:plan|campaign|runner))?[.!]?\s*', prompt, re.I):
            action = 'pause' if re.match(r'\s*(?:please\s+)?pause\b', prompt, re.I) else 'stop'
        if action:
            from campaign import fold_operator_requests, request_stop
            if request_recorded:fold_operator_requests(root,session)
            else:request_stop(root,session,action)
        return {}
    # SessionEnd is advisory: a crash or transport disconnect is not completion.
    return {}


def main():
    event = None
    payload={}
    try:
        payload = json.load(sys.stdin)
        if not isinstance(payload, dict):
            raise ValueError('hook payload must be an object')
        event = payload.get('hook_event_name')
        recorded=durable_operator_request(payload,ROOT)
        print(json.dumps(handle(payload,ROOT,request_recorded=recorded)))
        return 0
    except Exception as exc:
        # An operator interrupt must always remain terminal. Other unexpected
        # failures are visible; tool admission fails closed without overwriting state.
        message = f'RasterFold Codex hook failed: {type(exc).__name__}: {exc}'
        if event == 'PreToolUse':
            print(json.dumps({'hookSpecificOutput': {'hookEventName': event, 'permissionDecision': 'deny', 'permissionDecisionReason': message}}))
        elif event=='UserPromptSubmit' and re.match(r'^RasterFold Codex (?:heartbeat|continuation) run=',str(payload.get('prompt',''))):
            print(json.dumps({'decision':'block','reason':message+'; generated continuation discarded.'}))
        else:
            # Never spend unbounded continuation turns on a broken hook. State
            # remains owned for manual diagnosis; this is not a successful end.
            print(json.dumps({'systemMessage': message}))
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
