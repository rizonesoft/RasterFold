"""Fixture-only tests of native lifecycle, isolation, review and intelligence."""
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / '.codex/runner'))
import campaign
import intelligence
import panel_slots
import repair
from admission import admission, busy, require_idle

spec = importlib.util.spec_from_file_location('codex_hooks', REPO / '.codex/hooks/adapter.py')
hooks = importlib.util.module_from_spec(spec); spec.loader.exec_module(hooks)


class Runner(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.root = Path(self.temp.name)
        self.run = 'docs/phase-runs/fixture.md'
        (self.root / self.run).parent.mkdir(parents=True)
        (self.root / self.run).write_text('# Fixture run\n')
        (self.root / '.gitignore').write_text('build/\n.codex/state/\n.runner/\n')
        (self.root / '.intelligence/codex').mkdir(parents=True)
        (self.root / '.intelligence/codex/register.json').write_text(json.dumps(dict(schema=1,family='codex',next_id=1,entries=[],archive=[])))
        self.git('init','-q');self.git('add','.')
        self.git('-c','user.email=fixture@example.test','-c','user.name=Fixture','commit','-qm','fixture')

    def tearDown(self): self.temp.cleanup()

    def git(self,*args):
        return subprocess.check_output(['git',*args],cwd=self.root,stderr=subprocess.DEVNULL).decode().strip()

    def start(self, session='session-one', scope='plan'):
        return campaign.start(self.root,session,0,self.run,scope)

    def capture(self):
        return intelligence.update(self.root,'capture',title='Measured defect',evidence='tests/failing-case: exit 1',proposal='Correct the cause',owner='D00 T04 §3')

    def check_discovery_origin(self, run_id):
        spec=importlib.util.spec_from_file_location('origin_graph',REPO/'scripts/todo-graph.py')
        graph=importlib.util.module_from_spec(spec);sys.modules[spec.name]=graph;spec.loader.exec_module(graph)
        path=self.root/'todo/00-workspace/TODO-04-fixture.md'
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(f'## 1. Discovered fixture\n\n**Origin:** discovered run={run_id} 2026-09-30 -- fixture\n')
        origins,problems=graph.section_origins([graph.parse_todo(path)])
        self.assertEqual(problems,[])
        self.assertEqual(origins['D00 T04 §1']['run'],run_id)

    def test_campaign_discovery_origin_matches_canonical_validator(self):
        state=self.start()
        self.check_discovery_origin(state['run_id'])
        self.assertRegex(state['generation'],r'^[0-9a-f]{32}$')

    def test_skill_discovery_commands_match_canonical_validator(self):
        for name in ('add-todo','groom-plan'):
            with self.subTest(skill=name):
                text=(REPO/'.codex/skills'/name/'SKILL.md').read_text()
                commands=re.findall(r'`(python3 -c "[^"]*uuid[^\"]*")`',text)
                self.assertEqual(len(commands),1)
                run_id=subprocess.check_output(shlex.split(commands[0]),cwd=self.root,text=True).strip()
                self.check_discovery_origin(run_id)

    def test_one_owner_threads(self):
        def attempt(n):
            try: return self.start('session-'+str(n))
            except ValueError: return None
        with ThreadPoolExecutor(max_workers=6) as pool:
            results=list(pool.map(attempt,range(6)))
        self.assertEqual(sum(bool(r) for r in results),1)

    def test_sibling_busy_and_malformed(self):
        path=self.root/'build/claude-campaign-guard.json';path.parent.mkdir(exist_ok=True)
        for text in ('{"session_id":"claude-session"}', '{bad', '{}'):
            path.write_text(text)
            with self.assertRaises(ValueError): self.start()

    def test_foreign_session_cannot_stop(self):
        self.start()
        with self.assertRaises(ValueError): campaign.transition(self.root,'foreign','stop','test')
        self.assertEqual(campaign.stop_hook(self.root,'foreign'),{})

    def test_stop_continuation_stall_and_resume(self):
        state=self.start()
        self.assertEqual(campaign.stop_hook(self.root,'session-one')['decision'],'block')
        campaign.stop_hook(self.root,'session-one')
        self.assertIn('systemMessage',campaign.stop_hook(self.root,'session-one'))
        self.assertEqual(busy(self.root,'codex')['status'],'parked')
        with self.assertRaises(ValueError): require_idle(self.root)
        with self.assertRaises(ValueError):campaign.transition(self.root,'session-one','resume','agent wants to continue')
        hooks.handle({'hook_event_name':'UserPromptSubmit','session_id':'session-one','prompt':'resume the campaign'},self.root)
        new=campaign.transition(self.root,'session-one','resume','operator requested resume')
        self.assertNotEqual(new['generation'],state['generation'])

    def test_progress_resets_stall(self):
        self.start();campaign.stop_hook(self.root,'session-one');campaign.stop_hook(self.root,'session-one')
        (self.root/'code.c').write_text('real change')
        self.assertEqual(campaign.stop_hook(self.root,'session-one')['decision'],'block')
        self.assertEqual(busy(self.root,'codex')['unchanged_stops'],0)

    def test_operator_stop_and_generation(self):
        state=self.start()
        hooks.handle({'hook_event_name':'UserPromptSubmit','session_id':'session-one','prompt':'Do not stop the campaign'},self.root)
        self.assertTrue(busy(self.root,'codex'))
        hooks.handle({'hook_event_name':'UserPromptSubmit','session_id':'session-one','prompt':'Please stop the campaign.'},self.root)
        self.assertIsNone(busy(self.root,'codex'))
        self.assertFalse(campaign.tick(self.root,'session-one',state['generation'],0,lambda _:self.fail('stale queue')))

    def test_interrupt_terminal_and_sessionend_advisory(self):
        self.start()
        hooks.handle({'hook_event_name':'SessionEnd','session_id':'session-one'},self.root)
        self.assertTrue(busy(self.root,'codex'))
        hooks.handle({'hook_event_name':'Interrupt','session_id':'session-one'},self.root)
        self.assertIsNone(busy(self.root,'codex'))

    def test_heartbeat_bounded(self):
        state=self.start();messages=[]
        for _ in range(2): self.assertTrue(campaign.tick(self.root,'session-one',state['generation'],0,messages.append))
        self.assertFalse(campaign.tick(self.root,'session-one',state['generation'],0,messages.append))
        self.assertEqual(len(messages),2)
        self.assertEqual(busy(self.root,'codex')['status'],'parked')

    def test_heartbeat_delivery_failure(self):
        state=self.start()
        def fail(_):raise OSError('delivery failed')
        self.assertFalse(campaign.tick(self.root,'session-one',state['generation'],0,fail))
        self.assertEqual(busy(self.root,'codex')['status'],'parked')

    def test_completion_requires_run_marker_and_readiness(self):
        state=self.start()
        with self.assertRaises(ValueError): campaign.transition(self.root,'session-one','complete','test')
        (self.root/self.run).write_text('## Closeout run='+state['run_id']+'\n')
        (self.root/'scripts').mkdir();(self.root/'scripts/todo-graph.py').write_text('print("1 runnable now")')
        with self.assertRaises(ValueError): campaign.transition(self.root,'session-one','complete','test')
        (self.root/'scripts/todo-graph.py').write_text('print("0 runnable now")')
        self.assertEqual(campaign.transition(self.root,'session-one','complete','ready checked')['status'],'complete')

    def test_phase_does_not_chain(self):
        self.start(scope='phase')
        with self.assertRaises(ValueError): campaign.transition(self.root,'session-one','advance','next',1,self.run)

    def test_run_path_escape(self):
        (self.root/'AGENTS.md').write_text('must not become a run file')
        (self.root/'docs/phase-runs/link.md').symlink_to(self.root/'AGENTS.md')
        for path in ('/tmp/run.md','docs/phase-runs/../../../outside.md','docs/phase-runs/../../AGENTS.md','docs/phase-runs/link.md'):
            with self.assertRaises(ValueError): campaign.start(self.root,'session-one',0,path)

    def test_tool_admission(self):
        self.start()
        result=hooks.handle({'hook_event_name':'PreToolUse','session_id':'foreign','tool_name':'exec_command'},self.root)
        self.assertEqual(result['hookSpecificOutput']['permissionDecision'],'deny')
        self.assertEqual(hooks.handle({'hook_event_name':'PreToolUse','session_id':'session-one','tool_name':'exec_command'},self.root),{})

    def test_intelligence_capture_dedup_and_stopped_boundary(self):
        self.start();one=self.capture();two=self.capture();self.assertEqual(one['id'],two['id'])
        for action in ('defer','apply','reject','archive'):
            with self.assertRaises(ValueError): intelligence.update(self.root,action,ident=one['id'],reason='measured reason',commit='HEAD')
        campaign.transition(self.root,'session-one','stop','operator stop')
        row=intelligence.update(self.root,'defer',ident=one['id'],reason='owned follow-up')
        self.assertEqual(row['status'],'deferred')
        with self.assertRaises(ValueError): intelligence.update(self.root,'archive',ident=one['id'],reason='cannot discard')
        row=intelligence.update(self.root,'apply',ident=one['id'],reason='verified fixture implementation',commit='HEAD')
        self.assertEqual(row['status'],'applied')
        intelligence.update(self.root,'archive',ident=one['id'],reason='retained history')
        self.assertEqual(len(intelligence.read(self.root)['archive']),1)
        recurrence=self.capture()
        self.assertGreater(recurrence['id'],one['id'])
        self.assertEqual(recurrence['history'][0]['previous_id'],one['id'])

    def test_intelligence_cross_family_and_corrupt(self):
        p=self.root/'.intelligence/codex/register.json';data=json.loads(p.read_text());data['family']='claude';p.write_text(json.dumps(data))
        with self.assertRaises(ValueError): self.capture()

    def test_intelligence_concurrent_capture(self):
        with ThreadPoolExecutor(max_workers=5) as pool:rows=list(pool.map(lambda _:self.capture(),range(5)))
        self.assertEqual({r['id'] for r in rows},{1})
        self.assertEqual(intelligence.read(self.root)['next_id'],2)

    def test_private_panel_same_family_rejected(self):
        table=panel_slots.load()
        for slot in panel_slots.SLOTS:
            argv=panel_slots.argv_for_slot(slot,table)
            self.assertEqual(argv[0],'claude');self.assertEqual(argv[-2:],['--tools',''])
        source=(REPO/'.codex/runner/panel.toml').read_text().replace('family = "claude"','family = "codex"')
        bad=self.root/'panel.toml';bad.write_text(source)
        with self.assertRaises(ValueError):panel_slots.load(bad)

    def test_independent_skill_copies(self):
        for source in (REPO/'.claude/skills').glob('*/SKILL.md'):
            copy=REPO/'.codex/skills'/source.parent.name/'SKILL.md'
            self.assertTrue(copy.is_file());self.assertFalse(copy.is_symlink())
            self.assertEqual((REPO/'.agents/skills'/source.parent.name).resolve(),copy.parent.resolve())
        for p in (REPO/'.codex/skills').rglob('*.md'):
            self.assertNotIn('scripts/campaign_guard.py',p.read_text(),str(p))

    def test_new_writer_provenance_and_legacy_panels(self):
        spec=importlib.util.spec_from_file_location('fixture_graph',REPO/'scripts/todo-graph.py')
        graph=importlib.util.module_from_spec(spec);sys.modules[spec.name]=graph;spec.loader.exec_module(graph)
        self.assertTrue(graph.codex_implementer('Codex (gpt-6-astra)'))
        self.assertFalse(graph.codex_implementer('Codex (unregistered-model)'))
        self.assertFalse(graph.codex_implementer('Claude (gpt-6-astra)'))
        self.assertEqual(graph.panel_fallback('## Claude panel\n','2026-09-30','Codex (gpt-6-astra)'),(False,False))
        self.assertEqual(graph.panel_fallback('## GPT panel\n','2026-09-30','Codex (gpt-6-astra)'),(True,False))
        self.assertEqual(graph.panel_fallback('## Claude panel\n','2026-09-30'),(True,False))
        self.assertEqual(graph.panel_fallback('## GPT panel\n','2026-09-30'),(False,False))

    def test_tool_admission_without_run_and_after_stop(self):
        payload={'hook_event_name':'PreToolUse','session_id':'session-one','tool_name':'exec_command'}
        self.assertEqual(hooks.handle(payload,self.root),{})
        self.start();campaign.transition(self.root,'session-one','stop','operator')
        self.assertEqual(hooks.handle(payload,self.root),{})

    def test_stop_error_does_not_loop(self):
        import io
        source=io.StringIO(json.dumps({'hook_event_name':'Stop','session_id':'session-one'}))
        output=io.StringIO()
        with patch.object(hooks,'handle',side_effect=ValueError('corrupt state')),patch('sys.stdin',source),patch('sys.stdout',output):
            self.assertEqual(hooks.main(),0)
        self.assertNotIn('decision',json.loads(output.getvalue()))

    def test_durable_interrupt_during_admission(self):
        state=self.start()
        with ThreadPoolExecutor(max_workers=1) as pool:
            with admission(self.root):
                result=pool.submit(campaign.request_stop,self.root,'session-one','stop')
                deadline=time.monotonic()+2
                while not campaign.pending_request(self.root,state) and time.monotonic()<deadline:time.sleep(.01)
                self.assertTrue(campaign.pending_request(self.root,state))
            result.result(timeout=3)
        self.assertIsNone(busy(self.root,'codex'))

    def test_stale_heartbeat_prompt_is_mechanically_blocked(self):
        state=self.start()
        prompt=f"RasterFold Codex heartbeat run={state['run_id']} generation={state['generation']}. Resume"
        payload=dict(hook_event_name='UserPromptSubmit',session_id='session-one',prompt=prompt)
        self.assertEqual(hooks.handle(payload,self.root),{})
        campaign.request_stop(self.root,'session-one')
        self.assertEqual(hooks.handle(payload,self.root)['decision'],'block')

    def test_bookkeeping_is_not_progress_and_tools_are_live(self):
        state=self.start();first=campaign.fingerprint(self.root)
        with (self.root/self.run).open('a') as out:out.write('heartbeat note\n')
        self.capture()
        self.assertEqual(campaign.fingerprint(self.root),first)
        campaign.record_activity(self.root,'session-one',True)
        self.assertTrue(campaign.tick(self.root,'session-one',state['generation'],0,lambda _:self.fail('nudged a running tool')))
        campaign.record_activity(self.root,'session-one',False)

    def test_advance_cannot_reuse_closeout(self):
        state=self.start();(self.root/self.run).write_text('## Closeout run='+state['run_id']+'\n')
        with self.assertRaises(ValueError):campaign.transition(self.root,'session-one','advance','next',1,self.run)

    def test_private_admission_implementations_exclude_each_other(self):
        spec=importlib.util.spec_from_file_location('fixture_claude_admission',REPO/'.claude/runner/admission.py')
        claude=importlib.util.module_from_spec(spec);spec.loader.exec_module(claude)
        marks=[]
        def second():
            with claude.admission(self.root):marks.append('claude')
        with ThreadPoolExecutor(max_workers=1) as pool:
            with admission(self.root):
                future=pool.submit(second);time.sleep(.1);self.assertEqual(marks,[])
            future.result(timeout=2)
        self.assertEqual(marks,['claude'])

    def test_repair_bound_owner_and_verified_close(self):
        state=self.start();base=self.git('rev-parse','HEAD')
        for i in range(3):
            (self.root/'code.c').write_text(str(i));self.git('add','code.c')
            self.git('-c','user.email=fixture@example.test','-c','user.name=Fixture','commit','-qm',f'repair {i}')
            head=self.git('rev-parse','HEAD')
            red_result=lambda *_:dict(status='completed',conclusion='failure',head_sha=base,name='build')
            repair.update(self.root,'session-one','attempt',self.run,'build',base,head,ci_run='11',verify=red_result)
        with self.assertRaises(ValueError):repair.update(self.root,'foreign','attempt',self.run,'build',base,head)
        bad=lambda *_:dict(status='completed',conclusion='failure',head_sha=head,name='build')
        with self.assertRaises(ValueError):repair.update(self.root,'session-one','close',self.run,'build',commit=head,evidence='green claim',ci_run='12',verify=bad)
        rows,current=repair.records(self.root,state['run_id'],'build');self.assertEqual(len(current),3)
        good=lambda *_:dict(status='completed',conclusion='success',head_sha=head,name='build')
        repair.update(self.root,'session-one','close',self.run,'build',commit=head,evidence='confirmed result',ci_run='12',verify=good)
        self.assertEqual(repair.records(self.root,state['run_id'],'build')[1],[])

    def test_resume_never_deletes_a_fresh_stop(self):
        self.start();campaign.transition(self.root,'session-one','pause','operator')
        campaign.grant_resume(self.root,'session-one')
        original=campaign.busy
        def interrupted(root,family):
            if family=='claude':
                hooks.durable_operator_request(dict(hook_event_name='Interrupt',session_id='session-one'),root)
            return original(root,family)
        with patch.object(campaign,'busy',side_effect=interrupted):
            resumed=campaign.transition(self.root,'session-one','resume','operator grant')
        self.assertTrue(campaign.pending_request(self.root,resumed))
        self.assertFalse(campaign.tick(self.root,'session-one',resumed['generation'],0,lambda _:self.fail('queued after Interrupt')))
        self.assertIsNone(busy(self.root,'codex'))

    def test_terminal_run_cannot_become_paused(self):
        self.start();terminal=campaign.transition(self.root,'session-one','stop','operator')
        campaign.request_stop(self.root,'session-one','pause')
        self.assertIsNone(busy(self.root,'codex'))
        state=campaign.read_json(self.root/campaign.STATE)
        self.assertEqual(state['generation'],terminal['generation'])

    def test_used_run_file_and_nonmarkdown_paths_refused(self):
        state=self.start();(self.root/self.run).write_text('## Closeout run='+state['run_id']+'\n')
        second='docs/phase-runs/second.md';(self.root/second).write_text('# next\n')
        campaign.transition(self.root,'session-one','advance','next',1,second)
        (self.root/second).write_text('## Closeout run='+state['run_id']+'\n')
        with self.assertRaises(ValueError):campaign.transition(self.root,'session-one','advance','reuse',2,self.run)
        for name in ('docs/phase-runs/x.txt','docs/phase-runs/nested/x.md'):
            (self.root/name).parent.mkdir(exist_ok=True);(self.root/name).write_text('x')
            with self.assertRaises(ValueError):campaign.run_path(self.root,name)

    def test_tool_calls_cannot_reset_nudge_budget(self):
        state=self.start()
        for _ in range(2):
            self.assertTrue(campaign.tick(self.root,'session-one',state['generation'],0,lambda _:None))
            campaign.record_activity(self.root,'session-one',True)
            campaign.record_activity(self.root,'session-one',False)
        self.assertFalse(campaign.tick(self.root,'session-one',state['generation'],0,lambda _:self.fail('third nudge')))

    def test_generated_prompt_error_blocks_and_broken_import_keeps_stop(self):
        import builtins,io
        original=builtins.__import__
        def broken(name,*args,**kwargs):
            if name=='admission':raise SyntaxError('fixture import failure')
            return original(name,*args,**kwargs)
        for event,prompt in (('UserPromptSubmit','RasterFold Codex heartbeat run=x generation=y.'),('PreToolUse',''),('Interrupt','')):
            out=io.StringIO();source=io.StringIO(json.dumps(dict(hook_event_name=event,session_id='session-one',prompt=prompt)))
            with patch('builtins.__import__',side_effect=broken),patch.object(hooks,'ROOT',self.root),patch('sys.stdin',source),patch('sys.stdout',out):hooks.main()
            result=json.loads(out.getvalue())
            if event=='UserPromptSubmit':self.assertEqual(result['decision'],'block')
            elif event=='PreToolUse':self.assertEqual(result['hookSpecificOutput']['permissionDecision'],'deny')
            else:
                requests=campaign.operator_requests(self.root,'session-one')
                self.assertEqual(len(requests),1)
                self.assertEqual(next(iter(requests.values()))['action'],'stop')

    def test_controller_state_excluded_without_gitignore(self):
        self.start();(self.root/'.gitignore').unlink();first=campaign.fingerprint(self.root)
        campaign.record_activity(self.root,'session-one',True)
        self.assertEqual(campaign.fingerprint(self.root),first)

    def test_pending_stop_cannot_be_overwritten_by_pause(self):
        state=self.start()
        for prompt in ('stop the campaign','pause the campaign'):
            hooks.durable_operator_request(dict(hook_event_name='UserPromptSubmit',session_id='session-one',prompt=prompt),self.root)
        with admission(self.root):state=campaign.apply_request(self.root,state)
        self.assertEqual(state['status'],'stopped')

    def test_operator_stop_survives_clock_rollback(self):
        state=self.start()
        with patch.object(campaign.time,'time',return_value=state['started']-3600):
            campaign.request_stop(self.root,'session-one')
        self.assertIsNone(busy(self.root,'codex'))

    def test_start_ignores_old_requests_but_observes_fresh_interrupt(self):
        hooks.durable_operator_request(dict(hook_event_name='Interrupt',session_id='session-one'),self.root)
        state=self.start();self.assertEqual(state['status'],'active')
        campaign.transition(self.root,'session-one','stop','fixture boundary')
        original=campaign.fingerprint
        def interrupted(root):
            hooks.durable_operator_request(dict(hook_event_name='Interrupt',session_id='session-one'),root)
            return original(root)
        with patch.object(campaign,'fingerprint',side_effect=interrupted):state=self.start()
        self.assertEqual(state['status'],'stopped')

    def test_run_file_hardlink_cannot_reuse_closeout(self):
        state=self.start();(self.root/self.run).write_text('## Closeout run='+state['run_id']+'\n')
        alias='docs/phase-runs/alias.md';os.link(self.root/self.run,self.root/alias)
        with self.assertRaises(ValueError):campaign.transition(self.root,'session-one','advance','next',1,alias)

    def test_fingerprint_does_not_hold_admission_or_continue_after_stop(self):
        for action in ('stop_hook','tick'):
            with self.subTest(action=action):
                state=self.start();original=campaign.fingerprint
                with ThreadPoolExecutor(max_workers=1) as pool:
                    def interrupted(root):
                        result=pool.submit(campaign.request_stop,root,'session-one')
                        result.result(timeout=1)
                        return original(root)
                    with patch.object(campaign,'fingerprint',side_effect=interrupted):
                        if action=='stop_hook':self.assertEqual(campaign.stop_hook(self.root,'session-one'),{})
                        else:self.assertFalse(campaign.tick(self.root,'session-one',state['generation'],0,lambda _:self.fail('queued after stop')))

    def test_supervisor_launch_failure_parks_owner(self):
        fingerprint=campaign.fingerprint(self.root)
        with patch.object(campaign,'fingerprint',return_value=fingerprint),patch.object(campaign.subprocess,'Popen',side_effect=OSError('fixture spawn failure')):
            with self.assertRaises(OSError):campaign.start(self.root,'session-one',0,self.run,supervise=True)
        self.assertEqual(busy(self.root,'codex')['status'],'parked')

    def test_supervisor_inspection_error_parks_owner(self):
        state=self.start()
        with patch.object(campaign,'tick',side_effect=ValueError('fixture inspection failure')):
            self.assertEqual(campaign.watch(self.root,'session-one',state['generation']),2)
        self.assertEqual(busy(self.root,'codex')['status'],'parked')

    def test_status_folds_durable_operator_request(self):
        self.start();hooks.durable_operator_request(dict(hook_event_name='Interrupt',session_id='session-one'),self.root)
        out=subprocess.check_output([sys.executable,str(REPO/'.codex/runner/campaign.py'),'--root',str(self.root),'status'],text=True)
        self.assertEqual(json.loads(out)['codex']['status'],'stopped')

    def test_admission_thread_wait_is_bounded_in_both_families(self):
        import admission as codex_admission
        spec=importlib.util.spec_from_file_location('bounded_claude_admission',REPO/'.claude/runner/admission.py')
        claude_admission=importlib.util.module_from_spec(spec);spec.loader.exec_module(claude_admission)
        for module in (codex_admission,claude_admission):
            with self.subTest(family=module.__name__),ThreadPoolExecutor(max_workers=1) as pool:
                def acquire():
                    with module.admission(self.root,timeout=.05):self.fail('acquired a held lock')
                with module.admission(self.root):
                    future=pool.submit(acquire)
                    with self.assertRaises(module.AdmissionBusy):future.result(timeout=1)
                with module.admission(self.root,timeout=.05):pass

    def test_broken_controller_allows_only_proven_idle_maintenance(self):
        import builtins,io
        original=builtins.__import__
        def broken(name,*args,**kwargs):
            if name=='campaign':raise SyntaxError('fixture controller import failure')
            return original(name,*args,**kwargs)
        def invoke():
            out=io.StringIO();source=io.StringIO(json.dumps(dict(hook_event_name='PreToolUse',session_id='session-one',tool_name='exec_command')))
            with patch('builtins.__import__',side_effect=broken),patch.object(hooks,'ROOT',self.root),patch('sys.stdin',source),patch('sys.stdout',out):hooks.main()
            return json.loads(out.getvalue())
        self.assertEqual(invoke(),{})
        guard=self.root/'build/claude-campaign-guard.json';guard.parent.mkdir(exist_ok=True);guard.write_text('{bad')
        self.assertEqual(invoke()['hookSpecificOutput']['permissionDecision'],'deny')
        guard.unlink();self.start()
        self.assertEqual(invoke()['hookSpecificOutput']['permissionDecision'],'deny')

    def test_native_interrupt_records_one_event(self):
        import io
        self.start()
        source=io.StringIO(json.dumps(dict(hook_event_name='Interrupt',session_id='session-one')))
        with patch.object(hooks,'ROOT',self.root),patch('sys.stdin',source),patch('sys.stdout',io.StringIO()):self.assertEqual(hooks.main(),0)
        self.assertEqual(len(campaign.operator_requests(self.root,'session-one')),1)
        self.assertEqual(campaign.status(self.root)['codex']['status'],'stopped')

    def test_supervisor_error_preserves_corrupt_terminal_and_replaced_state(self):
        import io
        state=self.start()
        values=('{bad',json.dumps(dict(state,status='stopped')),json.dumps(dict(state,generation='replacement')))
        for value in values:
            with self.subTest(value=value):
                (self.root/campaign.STATE).write_text(value)
                with patch.object(campaign,'tick',side_effect=ValueError('fixture inspection failure')),patch('sys.stderr',io.StringIO()):
                    self.assertEqual(campaign.watch(self.root,'session-one',state['generation']),2)
                self.assertEqual((self.root/campaign.STATE).read_text(),value)

    def test_supervisor_retries_bounded_admission_contention(self):
        state=self.start()
        with patch.object(campaign,'tick',side_effect=[campaign.AdmissionBusy('locked'),False]),patch.object(campaign.time,'sleep') as sleep:
            self.assertEqual(campaign.watch(self.root,'session-one',state['generation']),0)
        sleep.assert_called_once_with(5)
        self.assertEqual(busy(self.root,'codex')['status'],'active')

    def test_malformed_request_parks_valid_owner_without_rewriting_request(self):
        import io
        state=self.start();directory=campaign.request_directory(self.root,'session-one');directory.mkdir(parents=True)
        path=directory/('session-one.'+'1'*32+'.json');path.write_text('{bad')
        with patch('sys.stderr',io.StringIO()):
            self.assertEqual(campaign.watch(self.root,'session-one',state['generation']),2)
        self.assertEqual(busy(self.root,'codex')['status'],'parked')
        self.assertEqual(path.read_text(),'{bad')

    def test_heartbeat_rechecks_tool_activity_after_measurement(self):
        state=self.start();original=campaign.fingerprint
        def activity(root):
            campaign.record_activity(root,'session-one',True)
            return original(root)
        with patch.object(campaign,'fingerprint',side_effect=activity):
            self.assertTrue(campaign.tick(self.root,'session-one',state['generation'],0,lambda _:self.fail('nudged an executing tool')))

    def test_interrupt_during_native_trust_check_cancels_start(self):
        import harness,io
        def interrupted(*args,**kwargs):
            hooks.durable_operator_request(dict(hook_event_name='Interrupt',session_id='session-one'),self.root)
            return {}
        args=['campaign.py','--root',str(self.root),'start','--session','session-one','--phase','0','--run-file',self.run]
        with patch.object(harness,'rpc',side_effect=interrupted),patch.object(harness,'check'),patch.object(campaign,'_launch_watch_locked') as launch,patch('sys.argv',args),patch('sys.stdout',io.StringIO()):
            self.assertEqual(campaign.main(),0)
        launch.assert_not_called()
        self.assertEqual(campaign.status(self.root)['codex']['status'],'stopped')

    def test_activation_and_supervisor_launch_share_one_admission(self):
        with ThreadPoolExecutor(max_workers=1) as pool:
            def competing():
                with admission(self.root,timeout=.05):self.fail('activation released admission before launch')
            def launch(*args,**kwargs):
                self.assertEqual(campaign.read_json(self.root/campaign.STATE)['status'],'active')
                with self.assertRaises(campaign.AdmissionBusy):pool.submit(competing).result(timeout=1)
            # Only fake the supervisor process; fingerprint still runs real git.
            original=campaign.fingerprint
            with patch.object(campaign,'fingerprint',return_value=original(self.root)),patch.object(campaign.subprocess,'Popen',side_effect=launch):
                state=campaign.start(self.root,'session-one',0,self.run,supervise=True)
                campaign.transition(self.root,'session-one','pause','fixture')
                campaign.grant_resume(self.root,'session-one')
                state=campaign.transition(self.root,'session-one','resume','fixture',supervise=True)
                (self.root/self.run).write_text('## Closeout run='+state['run_id']+'\n')
                second='docs/phase-runs/second.md';(self.root/second).write_text('# next\n')
                campaign.transition(self.root,'session-one','advance','fixture',1,second,supervise=True)

    def test_supervisor_failure_parking_retries_contended_admission(self):
        from contextlib import contextmanager
        state=self.start();calls=[]
        @contextmanager
        def contended(root):
            calls.append(1)
            if len(calls)==1:raise campaign.AdmissionBusy('fixture contention')
            with admission(root):yield
        with patch.object(campaign,'admission',contended),patch.object(campaign.time,'sleep') as sleep:
            campaign.park_failed_watch(self.root,'session-one',state['generation'],'fixture failure')
        self.assertEqual(len(calls),2);sleep.assert_called_once_with(5)
        self.assertEqual(busy(self.root,'codex')['status'],'parked')

    def test_copied_closeout_cannot_become_next_phase(self):
        state=self.start();marker='## Closeout run='+state['run_id']+'\n'
        (self.root/self.run).write_text(marker)
        second='docs/phase-runs/copy.md';(self.root/second).write_text(marker)
        with self.assertRaises(ValueError):campaign.transition(self.root,'session-one','advance','next',1,second)

    def test_launch_failure_parks_even_if_a_malformed_request_appears(self):
        fingerprint=campaign.fingerprint(self.root)
        def failed(*args,**kwargs):
            directory=campaign.request_directory(self.root,'session-one');directory.mkdir(parents=True,exist_ok=True)
            (directory/('session-one.'+'1'*32+'.json')).write_text('{bad')
            raise OSError('fixture spawn failure')
        with patch.object(campaign,'fingerprint',return_value=fingerprint),patch.object(campaign.subprocess,'Popen',side_effect=failed):
            with self.assertRaises(OSError):campaign.start(self.root,'session-one',0,self.run,supervise=True)
        self.assertEqual(busy(self.root,'codex')['status'],'parked')


if __name__ == '__main__': unittest.main()
