#!/usr/bin/env python3
"""Bounded Codex CI repair episodes, replayed from canonical run evidence."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from admission import admission
from campaign import ROOT, run_path, owned, apply_request

PREFIX = '- codex repair: '


def records(root, run_id, workflow):
    rows=[]
    for path in sorted((Path(root)/'docs/phase-runs').glob('*.md')):
        if path.is_symlink():continue
        for line in path.read_text().splitlines():
            if not line.startswith(PREFIX):continue
            row=json.loads(line[len(PREFIX):])
            if not isinstance(row,dict) or set(row)!={'action','workflow','run_id','red','commit','evidence','ci_run','sequence'} or row['action'] not in ('attempt','close'):
                raise ValueError('malformed repair ledger event')
            if row['workflow']==workflow and row['run_id']==run_id:rows.append(row)
    rows.sort(key=lambda row:row['sequence'])
    if [r['sequence'] for r in rows]!=list(range(1,len(rows)+1)):
        raise ValueError('repair ledger sequence is incomplete or duplicated')
    current=[]
    for row in rows:
        if row['action']=='close':current=[]
        else:current.append(row)
    return rows,current


def ci_result(root,run):
    if not str(run).isdigit():raise ValueError('close requires a GitHub workflow run id')
    remote=subprocess.check_output(['git','remote','get-url','origin'],cwd=root,text=True).strip()
    match=re.fullmatch(r'(?:https://github.com/|git@github.com:)([^/]+/[^/]+?)(?:\.git)?',remote)
    if not match:raise ValueError('cannot resolve the canonical GitHub repository')
    return json.loads(subprocess.check_output(['gh','api',f'repos/{match[1]}/actions/runs/{run}'],cwd=root,text=True,timeout=30))


def update(root, session, action, name, workflow, red='', commit='', evidence='', ci_run='', verify=ci_result):
    # Network readback stays outside admission; compare the owner generation
    # afterwards so a concurrent operator stop always wins.
    before=owned(root,session)
    verified=verify(root,ci_run) if action in ('attempt','close') else None
    with admission(root):
        state=apply_request(root,owned(root,session))
        if state['status']!='active' or state['generation']!=before['generation'] or name!=state['run_file']:
            raise ValueError('repair requires the current active owner and its run file')
        path=run_path(root,name)
        rows,current=records(root,state['run_id'],workflow)
        if action=='status':return current
        def oid(ref):return subprocess.check_output(['git','--no-replace-objects','rev-parse','--verify',ref+'^{commit}'],cwd=root,text=True).strip()
        commit=oid(commit)
        subprocess.run(['git','--no-replace-objects','merge-base','--is-ancestor',commit,'HEAD'],cwd=root,check=True)
        if action=='attempt':
            red=oid(red)
            if verified.get('status')!='completed' or verified.get('conclusion') not in ('failure','cancelled','timed_out','action_required','startup_failure') or verified.get('head_sha')!=red or verified.get('name')!=workflow:
                raise ValueError('attempt requires failed CI readback for the exact red commit and workflow')
            if any(row['commit']==commit for row in current):return current
            if len(current)>=3:raise ValueError('three CI repairs exhausted; park and report the blocker')
            parent=current[-1]['commit'] if current else red
            subprocess.run(['git','--no-replace-objects','merge-base','--is-ancestor',parent,commit],cwd=root,check=True)
        elif action=='close':
            if not current or not evidence.strip() or verified.get('status')!='completed' or verified.get('conclusion')!='success' or verified.get('head_sha')!=commit or verified.get('name')!=workflow:
                raise ValueError('close requires independent successful CI readback for this workflow and exact commit')
            subprocess.run(['git','--no-replace-objects','merge-base','--is-ancestor',current[-1]['commit'],commit],cwd=root,check=True)
        else:raise ValueError('unknown repair action')
        row=dict(action=action,workflow=workflow,run_id=state['run_id'],red=red,commit=commit,evidence=evidence,ci_run=str(ci_run),sequence=len(rows)+1)
        with path.open('a') as out:
            out.write(PREFIX+json.dumps(row,sort_keys=True)+'\n');out.flush();os.fsync(out.fileno())
        return row


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=('attempt','close','status'))
    p.add_argument('--session',default=os.environ.get('CODEX_THREAD_ID'))
    p.add_argument('--run-file',required=True);p.add_argument('--workflow',required=True)
    for key in ('red','commit','evidence','ci-run'):p.add_argument('--'+key,default='')
    a=p.parse_args()
    try:print(json.dumps(update(ROOT,a.session,a.action,a.run_file,a.workflow,a.red,a.commit,a.evidence,a.ci_run),indent=2))
    except (OSError,ValueError,KeyError,subprocess.SubprocessError) as exc:p.exit(2,str(exc)+'\n')


if __name__=='__main__':main()
