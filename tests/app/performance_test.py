#!/usr/bin/env python3
"""Numeric history contract, storage recovery and idle-only comparison.
The deterministic protocol peer validates accounting, not model quality/speed.
"""
import concurrent.futures
import copy
import json
import os
from pathlib import Path
import shlex
import shutil
import sys
import tempfile
import time
import subprocess
from http_test import App, ROOT
from background_download_test import peer, model_catalog, chat, Transfer

BINARY=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'build/geist-app-test'))

def profile(app): return app.status()['performance_profile']
def exported(app):
    code,body,_=app.request('/app/performance/export');assert code==200
    return [json.loads(line) for line in body.splitlines()]
def request(app,api=False):
    # A non-streaming API body can arrive before request accounting has
    # relinquished the shared runtime. Wait for the documented idle state.
    app.wait(lambda s:s['ready'] and not s['inference_busy'])
    chat(app,api)
def main():
    with tempfile.TemporaryDirectory(prefix='geist-profile-') as temporary:
        root=Path(temporary)
        queue_home=root/'queue';queue_home.mkdir()
        subprocess.run([str(ROOT/'build/test_app_performance'),str(queue_home)],env={**os.environ,'GEIST_TEST_PERFORMANCE_SLOW':'1'},check=True)
        source=root/'fixture.gguf';source.write_bytes(b'x'*1048576)
        wrapper=root/'peer';wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(sys.executable)+' '+shlex.quote(str(Path(__file__).resolve()))+' --peer "$@"\n');wrapper.chmod(0o700)
        home=root/'data';home.mkdir();(home/'models').mkdir();catalog=model_catalog(source)
        for m in catalog['models']:m['backends']=['cpu','metal']
        model=catalog['models'][0];shutil.copyfile(source,home/'models'/model['file'])
        (home/'catalog.json').write_text(json.dumps(catalog));(home/'selected').write_text(model['id'])
        gate=home/'gate';identity={'geistlib':{'version':'0.11.0','revision':'a'*40,'source_state':'clean'}}
        env={**os.environ,'GEIST_PEER_GATE':str(gate),'GEIST_PEER_ENGINE':json.dumps(identity)}
        def start(binary=BINARY):
            app=App(home,binary=binary,server=wrapper,env=env);app.wait(lambda s:s['ready'] and not s['busy']);return app
        app=start()
        try:
            assert profile(app)['cpu'] is None and profile(app)['retained']==0
            for i in range(6):request(app,i%2==1)
            app.wait(lambda s:s['performance_profile']['persisted']>=6)
            p=profile(app);assert p['retained']==6 and p['cpu']['count']==5,p
            assert p['cpu']['rate']==20 and p['gpu'] is None,p
            rows=exported(app);assert len({r['id'] for r in rows})==6
            assert {r['source'] for r in rows}=={'app','api'}
            assert all(r['first_ns']>0 and r['total_ns']>=r['first_ns'] for r in rows)
            assert all(r['schema']==2 and r['engine']['geistlib']==identity['geistlib'] and len(r['engine']['payload_sha256'])==64 for r in rows)
            assert rows[0]['cold'] and all(not r['cold'] for r in rows[1:])
            assert all(r['rss']>0 and r['peak_rss']>=r['rss'] for r in rows)
            assert 'Say hello' not in json.dumps(rows) and 'Fixture response' not in json.dumps(rows)
            assert app.request('/app/performance/export',auth=False)[0]==403
            # #101: no export snapshot is written into the data folder any more.
            assert app.request('/app/performance/export',{})[0] in (404,405)
            assert not (home/'performance/export.jsonl').exists()
            for days in [0,1,90.5,999]:assert app.request('/app/performance/settings',{'enabled':True,'days':days})[0]==400
            for invalid in [None,'true',1,[]]:assert app.request('/app/performance/settings',{'enabled':invalid,'days':90})[0]==400
            assert app.request('/app/performance/settings',{'enabled':False,'days':30})[0]==200
            request(app);assert len(exported(app))==6
            assert app.request('/app/performance/settings',{'enabled':True,'days':90})[0]==200
            gate.touch()
            with concurrent.futures.ThreadPoolExecutor() as pool:
                future=pool.submit(request,app,True);app.wait(lambda s:s['inference_busy'])
                assert app.request('/app/performance/compare',{'confirm':True})[0]==409
                # Sampling without a UI/status request for more than one interval.
                time.sleep(2.3);gate.unlink();future.result()
            app.wait(lambda s:s['performance_profile']['persisted']>=7)
            assert exported(app)[-1]['samples']>=3
            before=app.status()['execution']['mode']
            assert app.request('/app/performance/compare',{})[0]==400
            assert app.request('/app/performance/compare',{'confirm':True})[0]==202
            state=app.wait(lambda s:not s['comparison']['running'],timeout=30)
            assert state['comparison']['result']=='completed' and state['execution']['mode']==before,state['comparison']
            measured=[r for r in exported(app) if r['source']=='controlled_test']
            assert len(measured)==8 and sum(r['warmup'] for r in measured)==2
            assert len({r['run'] for r in measured})==1 and len({r['backend'] for r in measured})==2
            assert profile(app)['cpu']['count']==3 and profile(app)['gpu']['count']==3
            # #103: the controlled run is the verdict's speed basis (ordinary history stays separate).
            snapshot=app.status();verdict=next(m for m in snapshot['models'] if m['id']==snapshot['active_id'])['verdict']
            for r in (measured[3],measured[7]):  # the last measured reply per processor (CPU first, then GPU)
                slot='gpu' if r['backend']==snapshot['execution']['gpu_backend'] else 'cpu'
                assert abs(verdict['seconds'][slot]-(r['first_ns']/1e9+200/(r['output']/(r['generation_ns']/1e9))))<0.01,(slot,verdict,r)
            # Models that are not installed get a labelled estimate from these measurements.
            assert verdict['basis']=='measured'
            catalog=json.loads(app.request('/app/catalog')[1]);catalog['revision']+=1
            twice={**catalog['models'][0],'id':'not-installed','file':'not-installed.gguf','sha256':'e'*64,'bytes':catalog['models'][0]['bytes']*2,'group_id':'not-installed'}
            twice.pop('quality',None);catalog['models'].append(twice)
            assert app.request('/app/catalog',catalog)[0]==200
            estimate=next(m for m in app.status()['models'] if m['id']=='not-installed')['verdict']
            assert estimate['basis']=='estimated' and estimate['reason']=='quality_unknown',estimate
            # What a verdict rests on: when each processor was measured; how many models an estimate scales from.
            assert all(time.time()-3600<verdict['measured_at'][p]<=time.time()+5 for p in ('cpu','gpu')),verdict
            assert estimate['estimated_from']>=1 and estimate['measured_at']=={'cpu':None,'gpu':None},estimate
            assert estimate['seconds']['cpu']>verdict['seconds']['cpu'],(estimate,verdict)  # twice the bytes, slower
            gate.touch();assert app.request('/app/performance/compare',{'confirm':True})[0]==202
            app.wait(lambda s:s['comparison']['phase']=='warmup')
            assert app.status()['execution']['mode']==before,"#83: the user's processor choice is shown during a comparison"
            assert app.request('/app/execution',{'mode':'gpu'})[0]==409
            assert app.request('/app/performance/cancel',{})[0]==200
            state=app.wait(lambda s:not s['comparison']['running'],timeout=30);gate.unlink()
            assert state['ready'] and state['comparison']['result']=='cancelled' and state['execution']['mode']==before
            assert exported(app)[-1]['outcome']=='cancelled'
        finally:gate.unlink(missing_ok=True);app.close()
        # UI-only app version change retains the same engine/configuration series.
        if (ROOT/'build/geist-app-new').exists():
            app=start(ROOT/'build/geist-app-new')
            try:assert profile(app)['retained']>=15 and profile(app)['cpu']['rate']==20
            finally:app.close()
        original=wrapper.read_text();wrapper.write_text(original+'\n# changed engine build fixture\n')
        app=start()
        try:
            p=profile(app);assert p['retained']>=15 and p['cpu'] is None and p['gpu'] is None
            assert all(r['historical'] for r in p['recent'])
            # Only an older engine build measured it: say so, never "not measured".
            card=next(m for m in app.status()['models'] if m['id']==model['id']);reason=card['reason']
            # #90: the numbers behind it, the newest from the earlier build, and none for this one.
            assert card['speed']=={'cpu':None,'gpu':None} and card['earlier']['engine']=='0.11.0' and card['earlier']['cpu']>0,card
            assert 'not measured' not in reason and 'not been measured' not in reason, reason
            assert 'measured' not in reason or reason.endswith('earlier geisten version.'), reason
        finally:app.close()
        wrapper.write_text(original)
        journal=home/'performance/observations.jsonl'
        rows=[json.loads(line) for line in journal.read_text().splitlines()]
        # Golden dataset: groups, exact interpolated quartiles, failures and outliers.
        base=next(r for r in rows if r['source']=='app' and not r['cold'])
        golden=[]
        for i,rate in enumerate([10,20,30,40,10000]):
            r={**base,'total_ns':10e9,'first_answer_ns':None if i<2 else i*1e9,'id':f'gold-{i}','output':64,'generation_ns':64/rate*1e9};golden.append(r)
        legacy_row={**golden[0],'id':'schema-1','schema':1};legacy_row.pop('engine');golden.insert(0,legacy_row)
        other=copy.deepcopy(golden[-1]);other['id']='other-engine';other['engine']['geistlib']['revision']='b'*40;golden.insert(1,other)
        golden.insert(1,{**golden[0],'id':'gpu-other-workload','backend':'metal','output':2})
        golden.insert(2,{**golden[0],'id':'failed','outcome':'error','generation_ns':1})
        journal.write_text(''.join(json.dumps(r)+'\n' for r in golden)+'{"schema":')
        (home/'performance/observations.1.jsonl').unlink(missing_ok=True)
        (home/'performance/profiles.json').write_text('broken cache')
        staging=home/'performance/.profiles.json.tmp';staging.write_text('interrupted replacement');staging.chmod(0o600)
        app=start()
        try:
            assert not staging.exists()
            p=profile(app);assert p['invalid']==1 and p['cpu']['count']==5,p
            assert (p['cpu']['rate'],p['cpu']['q25'],p['cpu']['q75'])==(30,20,40),p
            assert p['cpu']['first_answer']==3 and p['cpu']['first_answer_count']==3,p
            assert p['gpu'] is None,'An older engine build never fills the current GPU profile'
            request(app);app.wait(lambda s:s['performance_profile']['persisted']>=10)
        finally:app.close()
        app=start()
        try:
            restored=exported(app)
            assert len(restored)==10 and profile(app)['invalid']==1
            assert next(r for r in restored if r['id']=='schema-1')['schema']==1
            assert 'engine' not in next(r for r in restored if r['id']=='schema-1')
            assert next(r for r in restored if r['id']=='other-engine')['engine']['geistlib']['revision']=='b'*40
        finally:app.close()
        # #81, as reported: warm GPU replies, then a switch and the first (cold) CPU reply,
        # then a reply that overlapped a download. Each processor keeps its own workload;
        # the download overlap neither hides samples nor becomes the "last reply".
        timing = lambda r, rate, **kw: {**base, **kw, 'output': 64, 'generation_ns': 64 / rate * 1e9, 'total_ns': 10e9}
        scenario = [timing(base, 80, id=f'gpu-warm-{i}', backend='metal', cold=False) for i in range(3)]
        scenario += [timing(base, 18, id='cpu-cold', cold=True), timing(base, 5, id='cpu-download', contention=True)]
        journal.write_text(''.join(json.dumps(r) + '\n' for r in scenario))
        (home/'performance/observations.1.jsonl').unlink(missing_ok=True)
        (home/'performance/profiles.json').unlink(missing_ok=True)
        app = start()
        try:
            p = profile(app)
            assert p['gpu']['count'] == 3 and p['gpu']['rate'] == 80, p
            assert p['cpu']['count'] == 1 and p['cpu']['rate'] == 18, p
            assert p['cpu_group']['cold'] and not p['gpu_group']['cold'], p
            assert abs(app.status()['execution']['performance']['rate'] - 18) < .001, 'download overlap is not the last reply'
            card = next(m for m in app.status()['models'] if m['id'] == model['id'])
            assert abs(card['speed']['cpu'] - 18) < .01 and abs(card['speed']['gpu'] - 80) < .01 and card['earlier'] is None, card
        finally:app.close()
        # Legacy import survives restart, stays archived, and deletion never reimports.
        legacy=home/f'performance-{base["artifact"]}-{base["backend"]}'
        legacy.write_text(f'v1 0.5.1\nHistorical computer|arm64|macOS|64|10\n20 0.1 1 20 123 {int(time.time())}')
        legacy.chmod(0o600)
        app=start()
        try:assert len([r for r in exported(app) if r['source']=='legacy_last_reply'])==1
        finally:app.close()
        app=start()
        try:
            assert len([r for r in exported(app) if r['source']=='legacy_last_reply'])==1
            assert app.request('/app/performance/clear',{})[0]==400
            assert app.request('/app/performance/clear',{'confirm':True})[0]==200
            assert not exported(app) and app.status()['ready'] and (home/'models'/model['file']).exists()
        finally:app.close()
        app=start()
        try:assert not exported(app)
        finally:app.close()
        # No-follow / inability to save does not prevent generation.
        victim=root/'untouched';victim.write_text('Do not modify')
        journal.unlink(missing_ok=True);journal.symlink_to(victim)
        app=start()
        try:
            request(app);app.wait(lambda s:s['performance_profile']['error'])
            assert profile(app)['retained']==1 and victim.read_text()=='Do not modify'
        finally:app.close()
        journal.unlink()
        full_env={**env,'GEIST_TEST_PERFORMANCE_FULL':'1'}
        app=App(home,binary=BINARY,server=wrapper,env=full_env)
        try:
            app.wait(lambda s:s['ready']);request(app)
            app.wait(lambda s:s['performance_profile']['dropped']>0)
            assert profile(app)['retained']==1 and app.status()['ready']
        finally:app.close()
        journal.unlink(missing_ok=True)
        storage=home/'performance';storage.chmod(0o500)
        app=start()
        try:
            request(app);app.wait(lambda s:s['performance_profile']['dropped']>0)
            assert app.status()['ready']
        finally:app.close();storage.chmod(0o700)
        # Bound both disk segments and recover after a real rotation.
        line=(json.dumps({**base,'id':'rotation-base'})+'\n').encode()
        size=10*1024*1024-100
        journal.write_bytes(line*(size//len(line))+b' '*(size%len(line)));journal.chmod(0o600)
        app=start()
        try:
            request(app);app.wait(lambda s:s['performance_profile']['persisted']>=2)
        finally:app.close()
        assert journal.stat().st_size<8192
        assert (home/'performance/observations.1.jsonl').stat().st_size<=10*1024*1024
        app=start()
        try:assert len(exported(app))==2
        finally:app.close()
        (home/'performance/observations.1.jsonl').unlink()
        # Retention removes expired numeric data from both disk and export.
        journal.write_text(json.dumps({**base,'id':'expired','timestamp':time.time()-366*86400})+'\n');journal.chmod(0o600)
        app=start()
        try:assert not exported(app) and 'expired' not in journal.read_text()
        finally:app.close()
    print('performance: shared app/API accounting, no-content export, persistence, groups/median/quartiles, opt-out, retention, migration, delete, torn tail, symlink failure, independent sampling and comparison/cancel passed')
if __name__=='__main__':
    if '--peer' in sys.argv:
        if '--backends' in sys.argv:print(json.dumps({'gpu':{'name':'metal','available':True}}))
        else:peer()
    else:main()
