#!/usr/bin/env python3
"""Explicit, serialized lifecycle matrix; no model download or owner app mutation."""
import concurrent.futures, hashlib, http.client, json, os, shutil, subprocess, tempfile, time
from pathlib import Path
from http_test import App, ROOT
models=os.environ.get('GEIST_LIFECYCLE_MODELS','')
if not models:
    print('lifecycle real: SKIPPED (opt-in local models)');raise SystemExit()
ids=models.split(',');out=Path(os.environ['GEIST_LIFECYCLE_EVIDENCE']);out.mkdir(parents=True,exist_ok=False)
source=Path(os.environ['GEIST_LIFECYCLE_MODEL_DIR'])
app_input=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'geist-app'))
daemon_input=Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution'))
app_bin=out/'geist-app';daemon=out/'geistd';shutil.copy2(app_input,app_bin);shutil.copy2(daemon_input,daemon)
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
metadata={'app_sha256':sha(app_bin),'daemon_sha256':sha(daemon),'engine':json.loads(subprocess.check_output([str(daemon),'--build-info'])),
    'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
    'guard':'39-PREDECLARED-GUARDS.md','repetitions':int(os.environ.get('GEIST_LIFECYCLE_REPS','5')),'filesystem_cache':'uncontrolled; repeated launches'}
(out/'build.json').write_text(json.dumps(metadata,indent=2)+'\n')
catalog=json.loads((ROOT/'models/catalog.json').read_text());entries=[next(m for m in catalog['models'] if m['id']==key) for key in ids]
results=[]
with tempfile.TemporaryDirectory(prefix='geist-lifecycle-real-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    for entry in entries:
        subprocess.run(['cp','-c',str(source/entry['file']),str(home/'models'/entry['file'])],check=True)
        (home/f"backend-{entry['sha256']}").write_text('cpu')
    app=App(home,binary=app_bin,server=daemon)
    trace=(out/'samples.jsonl').open('x');sequence=0;label='idle'
    def sample():
        global sequence
        sequence+=1;s=app.status()
        rss=int(subprocess.check_output(['ps','-o','rss=','-p',str(app.process.pid)],text=True).strip())*1024
        row={'sequence':sequence,'label':label,'monotonic_ms':time.monotonic()*1000,'activity':s['activity'],
             'lifecycle':s['lifecycle'],'ready':s['ready'],'resources':s['resources'],'available_bytes':s['hardware']['available'],'supervisor_rss_bytes':rss}
        trace.write(json.dumps(row)+'\n');trace.flush();return s
    def wait(predicate,limit=180):
        deadline=time.monotonic()+limit
        while time.monotonic()<deadline:
            s=sample()
            if predicate(s):return s
            time.sleep(.1)
        raise AssertionError('lifecycle trial timed out')
    def pid_gone(pid):
        if not pid:return True
        try:os.kill(pid,0);return False
        except ProcessLookupError:return True
    def answer():
        conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=180)
        prompt='Reply with exactly OK.'
        conn.request('POST','/app/generate',json.dumps({'experimental':True,'model':app.status()['active_id'],'prompt':prompt,'messages':[{'role':'user','content':prompt}],
            'max_tokens':128,'temperature':0}),{'Authorization':'Bearer '+app.token})
        r=conn.getresponse();data=r.read();conn.close();assert r.status==200,r.status
        events=[json.loads(line) for line in data.splitlines()];assert events[-1].get('done') and not events[-1].get('no_answer'),events[-1]
        assert ''.join(e.get('response','') for e in events).strip()
        return {k:v for k,v in events[-1].items() if k!='response'}
    def transition(name,path,body,generate=True):
        global label
        label=name;before=sample();old=before['lifecycle']['pid'];started=time.monotonic()
        code=app.request(path,body)[0];assert code in (200,202),(name,code)
        ready=wait(lambda s:s['ready'] and not s['busy']);ready_s=time.monotonic()-started
        life=ready['lifecycle'];phases=life['engine_phases']
        assert [p['stage'] for p in phases]==['backend','model','metadata','warmup','ready'],phases
        if old and old!=life['pid']:
            assert pid_gone(old) and life['previous_pid']==old and life['reaped_ms']<=life['spawned_ms'],life
        stat=None
        if generate:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                future=pool.submit(answer)
                while not future.done():sample();time.sleep(.2)
                stat=future.result()
        final=wait(lambda s:not s['busy'])
        result={'transition':name,'http':code,'old_pid':old,'same_pid':old==life['pid'],'ready_s':ready_s,
            'first_answer_from_action_s':ready_s+(stat['first_answer_ns']/1e9 if stat and stat.get('first_answer_ns') else 0),
            'lifecycle':life,'load':ready['activity']['load'],'request':final['activity']['request'],'stats':stat}
        results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(json.dumps({'transition':name,'ready_s':ready_s,'same_pid':result['same_pid']}),flush=True)
        return result
    try:
        initial=sample();assert initial['hardware']['available_known']
        if any(m['bytes']>2**30 for m in entries):assert initial['hardware']['available']>=16*2**30,initial['hardware']['available']
        # Receipt miss/hit and each CPU↔Metal/model-selection transition are retained.
        for repeat in range(metadata['repetitions']):
            for entry in entries:
                key=entry['id']
                transition(f'{repeat}:{key}:select','/app/select',{'id':key})
                transition(f'{repeat}:{key}:cpu','/app/execution',{'mode':'cpu'})
                transition(f'{repeat}:{key}:active','/app/select',{'id':key})
                if app.status()['execution']['gpu_available']:
                    transition(f'{repeat}:{key}:gpu','/app/execution',{'mode':'gpu'})
                    if entry['bytes']>=2**30:transition(f'{repeat}:{key}:auto','/app/execution',{'mode':'auto'})
                    transition(f'{repeat}:{key}:return-cpu','/app/execution',{'mode':'cpu'})
                if entry['bytes']<2**30:transition(f'{repeat}:{key}:auto','/app/execution',{'mode':'auto'})
        for entry in entries:
            key=entry['id'];transition(f'cleanup:{key}:select','/app/select',{'id':key},False)
            for cycle in range(3 if entry['bytes']>2**30 else 10):
                for mode in ['cpu','gpu'] if app.status()['execution']['gpu_available'] else ['cpu']:
                    transition(f'cleanup:{key}:{cycle}:{mode}','/app/execution',{'mode':mode},False)
        label='final-stop';last=sample()['lifecycle']['pid'];assert app.request('/app/stop',{})[0]==200
        sample();assert pid_gone(last)
    except BaseException as error:
        (out/'failure.json').write_text(json.dumps({'label':label,'error':str(error)})+'\n');raise
    finally:
        app.close();trace.close()
        for log in home.glob('*failure-*'):shutil.copyfile(log,out/log.name)
        if (home/'server.log').exists():shutil.copyfile(home/'server.log',out/'daemon.log')
(out/'complete.json').write_text(json.dumps({'passed':True,'transitions':len(results)})+'\n')
