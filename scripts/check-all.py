#!/usr/bin/env python3
"""Run required host gates with bounded console output and retained full logs."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--skip-build',action='store_true');p.add_argument('--quick',action='store_true')
    p.add_argument('--all-targets',action='store_true',help='also build Windows and both Android ABIs on Linux')
    p.add_argument('--gate',help='run only the named gate')
    a=p.parse_args();py=sys.executable
    gates=[]
    def python(name,path,*args):gates.append((name,[py,path,*args]))
    if not a.skip_build:python('build','scripts/build.py',*(['--target','all'] if a.all_targets else []))
    python('clang-tidy','scripts/tidy.py')
    python('correctness','tests/test_correctness.py')
    python('api','tests/test_api.py')
    python('native consumers and ABI','tests/test_artifacts.py')
    if not a.quick:python('fuzz','tests/fuzz_decode.py','300','1')
    for name,path,args in (
        ('graph self-test','scripts/todo-graph.py',['self-test']),('graph validate','scripts/todo-graph.py',['validate']),
        ('plan projection','scripts/todo-graph.py',['plan','--check']),('milestones','research/milestones.py',['--check']),
        ('vision and order','scripts/plan-audit.py',[]),('document drift','scripts/drift-check.py',[]),
        ('claims','scripts/todo-claims.py',[]),('claims self-test','scripts/todo-claims.py',['--self-test']),
        ('findings','scripts/todo-findings.py',['--check']),('findings self-test','scripts/todo-findings.py',['--self-test']),
        ('runs','scripts/todo-runs.py',['--check']),('runs self-test','scripts/todo-runs.py',['--self-test']),
        ('reviewer pin','scripts/review-dispatch.py',['--check-pin']),
        ('review rounds','scripts/review-dispatch.py',['--list-rounds']),
        ('campaign guard','scripts/campaign_guard.py',['--self-test']),
        ('intelligence','scripts/intelligence.py',['validate']),
    ):python(name,path,*args)
    gates.append(('review and automation tests',[py,'-m','unittest','discover','-s','scripts/tests','-v']))
    gates.append(('document drift tests',[py,'-m','unittest','discover','-s','tests','-p','test_drift.py','-v']))
    gates.append(('documentation site tests',[py,'-m','unittest','discover','-s','tests','-p','test_docs.py','-v']))
    gates.append(('plan audit tests',[py,'-m','unittest','discover','-s','tests','-p','test_plan_audit.py','-v']))
    gates.append(('package tests',[py,'-m','unittest','discover','-s','tests','-p','test_package.py','-v']))
    if os.name != 'nt':
        gates.append(('toolchain installer tests',[py,'-m','unittest','discover','-s','tests','-p','test_toolchain.py','-v']))
    gates.append(('provision',(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File','tools/provision.ps1','-Verify'] if os.name=='nt' else [py,'tools/provision.py','--verify'])))
    if a.gate:
        gates=[g for g in gates if g[0]==a.gate]
        if not gates:
            print(f'check-all: unknown gate: {a.gate}',flush=True)
            return True
    logs=ROOT/'build/logs';logs.mkdir(parents=True,exist_ok=True)
    failed=0
    for name,cmd in gates:
        start=time.monotonic();path=logs/(name.replace(' ','-')+'.log')
        with path.open('w') as log:
            try:code=subprocess.run(cmd,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,timeout=1200).returncode
            except (OSError,subprocess.TimeoutExpired) as exc:log.write(str(exc));code=1
        print(f'{"FAIL" if code else "PASS"} {name} ({time.monotonic()-start:.1f}s)',flush=True)
        if code:
            failed+=1;print('\n'.join(path.read_text(errors='replace').splitlines()[-10:]),flush=True)
    print(f'check-all: {len(gates)-failed}/{len(gates)} passed'+(' (iteration: build or fuzz skipped)' if a.quick or a.skip_build else ''),flush=True)
    return bool(failed)


if __name__=='__main__':raise SystemExit(main())
