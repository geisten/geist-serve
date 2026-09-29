#!/usr/bin/env python3
"""Opt-in scoped telemetry trials using owned processes and cached models only.
Alternating on/off trials are measurements, not a negligible-overhead assertion.
"""
import concurrent.futures, hashlib, http.client, json, os, shutil, subprocess, tempfile, time
from pathlib import Path
from http_test import App, ROOT

if not os.environ.get('GEIST_MEMORY_EVIDENCE'):
    print('memory real: SKIPPED (explicit evidence path and local models required)');raise SystemExit()
out=Path(os.environ['GEIST_MEMORY_EVIDENCE']);out.mkdir(parents=True,exist_ok=False)
source=Path(os.environ['GEIST_MEMORY_MODEL_DIR'])
ids=os.environ.get('GEIST_MEMORY_MODELS','smollm2-360m').split(',')
catalog=json.loads((ROOT/'models/catalog.json').read_text())
entries=[next(m for m in catalog['models'] if m['id']==key) for key in ids]
app_bin=out/'geist-app';daemon=out/'geistd'
shutil.copy2(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'geist-app'),app_bin)
shutil.copy2(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution'),daemon)
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
build=json.loads(subprocess.check_output([str(daemon),'--build-info']))
(out/'build.json').write_text(json.dumps({'app_sha256':sha(app_bin),'daemon_sha256':sha(daemon),'build':build,
    'filesystem_cache':'uncontrolled; alternating on/off; no UI process','samples':'2 seconds plus request boundaries'},indent=2)+'\n')
trace=(out/'samples.jsonl').open('x');records=(out/'records.jsonl').open('x');results=[];owned=[]
def gone(pid):
    try:os.kill(pid,0);return False
    except ProcessLookupError:return True
try:
    with tempfile.TemporaryDirectory(prefix='geist-memory-real-') as temporary:
        home=Path(temporary);(home/'models').mkdir()
        for m in entries:
            path=source/m['file'];assert path.stat().st_size==m['bytes']
            if os.uname().sysname=='Darwin':subprocess.run(['cp','-c',str(path),str(home/'models'/m['file'])],check=True)
            else:shutil.copyfile(path,home/'models'/m['file'])
            (home/f"backend-{m['sha256']}").write_text('cpu')
        for trial,enabled in enumerate([True,False,False,True,True,False]):
            env={**os.environ,'GEIST_RESOURCE_SAMPLING':'1' if enabled else '0'}
            app=App(home,binary=app_bin,server=daemon,env=env);label='idle'
            def sample():
                state=app.status();memory=state['memory'];pid=state['lifecycle']['pid']
                if pid and pid not in owned:owned.append(pid)
                assert memory['total_unique_physical_bytes'] is None
                if memory['process_generation']:
                    assert memory['process_generation'].endswith(':'+str(state['lifecycle']['generation']))
                trace.write(json.dumps({'trial':trial,'enabled':enabled,'label':label,'time':time.monotonic(),
                    'pid':pid,'generation':state['lifecycle']['generation'],'ready':state['ready'],
                    'activity':state['activity'],'memory':memory,'available_bytes':state['hardware']['available']})+'\n');trace.flush()
                return state
            def wait(predicate,timeout=240):
                deadline=time.monotonic()+timeout
                while time.monotonic()<deadline:
                    state=sample()
                    if predicate(state):return state
                    time.sleep(.2)
                raise AssertionError(('timeout',label,state))
            def answer(model,long=False):
                prompt=('A local system should report measurements with their scope. '*120+'\n' if long else '')+'Reply with exactly OK.'
                c=http.client.HTTPConnection('127.0.0.1',app.port,timeout=240)
                c.request('POST','/app/generate',json.dumps({'model':model,'experimental':True,'prompt':prompt,
                    'messages':[{'role':'user','content':prompt}],'max_tokens':128,'temperature':0}),{'Authorization':'Bearer '+app.token})
                r=c.getresponse();body=r.read();c.close();assert r.status==200,r.status
                events=[json.loads(line) for line in body.splitlines()]
                assert events[-1].get('done') and not events[-1].get('no_answer'),events[-1]
                assert ''.join(e.get('response','') for e in events).strip()
                return events[-1]
            try:
                start=sample()
                if any(m['bytes']>2**30 for m in entries):assert start['hardware']['available_known'] and start['hardware']['available']>=16*2**30
                # Expensive Bonsai switch/prefill evidence once; paired small trials six times.
                for m in entries if trial==0 else entries[:1]:
                    label=m['id']+':select';assert app.request('/app/select',{'id':m['id']})[0] in (200,202)
                    wait(lambda s:s['ready'] and not s['busy'])
                    modes=['cpu','gpu','cpu'] if sample()['execution']['gpu_available'] else ['cpu']
                    for index,mode in enumerate(modes):
                        label=f"{m['id']}:{mode}:{index}";old=sample()['lifecycle']['pid'];t=time.monotonic()
                        assert app.request('/app/execution',{'mode':mode})[0] in (200,202)
                        state=wait(lambda s:s['ready'] and not s['busy'])
                        load=time.monotonic()-t;pid=state['lifecycle']['pid']
                        assert pid==old or gone(old)
                        memory=state['memory']
                        if enabled:
                            assert memory['status']==(1 if mode=='gpu' else 2),memory
                            if mode=='gpu':assert memory['gpu_allocated_bytes']>0 and memory['gpu_source']=='metal.MTLDevice.currentAllocatedSize'
                        else:assert memory['gpu_allocated_bytes'] is None and memory['status']==0
                        # No status polling during one request: collection is independent of UI.
                        stats=answer(m['id'])
                        wait(lambda s:not s['inference_busy'])
                        result={'trial':trial,'enabled':enabled,'model':m['id'],'mode':mode,'load_s':load,'stats':stats,'memory':sample()['memory']}
                        results.append(result);print(json.dumps(result),flush=True)
                        if enabled and m['bytes']>2**30:
                            label+=':long-prefill'
                            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                                f=pool.submit(answer,m['id'],True)
                                while not f.done():sample();time.sleep(.2)
                                f.result()
                            wait(lambda s:not s['inference_busy'])
                rows=[json.loads(line) for line in app.request('/app/performance/export')[1].splitlines()]
                for r in rows:
                    assert r['engine']['geistlib']==build['geistlib']
                    mem=r['memory'];assert mem['process_rss_bytes']==r['rss'] and mem['total_unique_physical_bytes'] is None
                    if mem['status']==1:assert mem['gpu_allocated_sampled_peak_bytes']>=mem['gpu_allocated_bytes'] and mem['gpu_samples']>0
                    if r['backend'].startswith('cpu'):assert mem['gpu_allocated_bytes'] is None
                    records.write(json.dumps(r)+'\n')
                records.flush()
                old=sample()['lifecycle']['pid'];assert app.request('/app/stop',{})[0]==200
                stopped=sample();assert gone(old) and stopped['memory']['gpu_allocated_bytes'] is None and stopped['memory']['process_rss_bytes'] is None
            finally:
                app.close()
                if (home/'server.log').exists():shutil.copyfile(home/'server.log',out/f'daemon-{trial}.log')
                for p in home.glob('*failure-*'):shutil.copyfile(p,out/f'{trial}-{p.name}')
            assert all(gone(pid) for pid in owned)
            (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (out/'complete.json').write_text(json.dumps({'passed':True,'requests':len(results),'owned_pids':owned})+'\n')
finally:trace.close();records.close()
