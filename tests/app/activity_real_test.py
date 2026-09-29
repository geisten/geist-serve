#!/usr/bin/env python3
"""Opt-in serialized activity trials. Existing artifacts only; numeric evidence.
GEIST_ACTIVITY_MODELS lists catalog IDs; GEIST_ACTIVITY_MODEL_DIR supplies GGUFs.
GEIST_ACTIVITY_EVIDENCE must name a new directory. Does not touch an installed app.
"""
import concurrent.futures, hashlib, http.client, json, os, shutil, subprocess, tempfile, time
from pathlib import Path
from http_test import App, ROOT
ids=os.environ.get('GEIST_ACTIVITY_MODELS','').split(',')
if not ids[0]:
    print('activity real: SKIPPED (explicit local artifacts required)');raise SystemExit()
source=Path(os.environ['GEIST_ACTIVITY_MODEL_DIR']);evidence=Path(os.environ['GEIST_ACTIVITY_EVIDENCE'])
evidence.mkdir(parents=True,exist_ok=False)
catalog=json.loads((ROOT/'models/catalog.json').read_text())
server=Path(os.environ.get('GEIST_EXECUTION_DAEMON',ROOT/'build/geistd-execution')).resolve()
binary=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'geist-app')).resolve()
# Freeze executables for a sustained trial even if another edit rebuilds the checkout.
original_binary,original_server=binary,server
binary=evidence/'geist-app';server=evidence/'geistd'
shutil.copy2(original_binary,binary);shutil.copy2(original_server,server)
def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
(evidence/'build.json').write_text(json.dumps({'app_sha256':sha(binary),'daemon_sha256':sha(server),
    'daemon':json.loads(subprocess.check_output([str(server),'--build-info'])),
    'source_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()},indent=2)+'\n')
results=[]
for model_id in ids:
    entry=next(m for m in catalog['models'] if m['id']==model_id)
    with tempfile.TemporaryDirectory(prefix='geist-activity-real-') as temporary:
        home=Path(temporary);(home/'models').mkdir();target=home/'models'/entry['file']
        subprocess.run(['cp','-c',str(source/entry['file']),str(target)],check=True)
        (home/f"backend-{entry['sha256']}").write_text('cpu')
        app=App(home,binary=binary,server=server)
        traces=[]
        def sample():
            start=time.monotonic();s=app.status()
            traces.append({'monotonic_s':start,'status_ms':(time.monotonic()-start)*1000,
                'activity':s['activity'],'generation':s['process_generation'],'ready':s['ready'],
                'resources':s['resources'],'available_bytes':s['hardware']['available']})
            return s
        def wait(predicate,seconds=180):
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                s=sample()
                if predicate(s):return s
                time.sleep(.2)
            raise AssertionError('real activity timeout')
        def generate(prompt):
            conn=http.client.HTTPConnection('127.0.0.1',app.port,timeout=900)
            body={'experimental':True,'model':model_id,'messages':[{'role':'user','content':prompt}],
                'prompt':prompt,'temperature':0,'max_tokens':256}
            conn.request('POST','/app/generate',json.dumps(body),{'Authorization':'Bearer '+app.token})
            response=conn.getresponse();raw=response.read();conn.close()
            assert response.status==200, response.status
            events=[json.loads(line) for line in raw.splitlines()]
            answer=''.join(x.get('response','') for x in events)
            assert answer.strip() and '<think>' not in answer and '</think>' not in answer
            assert events[-1].get('done') and not events[-1].get('no_answer')
            return {k:v for k,v in events[-1].items() if k!='response'}
        try:
            initial=sample();available=initial['hardware']['available']
            minimum=(16 if entry['bytes']>2**30 else 2)*2**30
            assert initial['hardware']['available_known'] and available>=minimum, ('resource guard',available,minimum)
            assert app.request('/app/select',{'id':model_id})[0]==202
            wait(lambda s:s['ready'])
            for mode in ('cpu','gpu'):
                if mode=='gpu' and not app.status()['execution']['gpu_available']:continue
                assert app.request('/app/execution',{'mode':mode})[0] in (200,202)
                ready=wait(lambda s:s['ready']);assert ready['execution']['active']==mode and ready['execution']['verified']
                for label,prompt in [('short','Reply with exactly OK.'),('long','Read this synthetic input: '+ 'item '*920 + '\nReply with exactly OK.')]:
                    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                        task=pool.submit(generate,prompt)
                        while not task.done():sample();time.sleep(.2)
                        stats=task.result()
                    final=wait(lambda s:not s['busy'])
                    request=final['activity']['request'];assert request['outcome']=='completed' and request['engine']['geistlib']['revision']
                    phases={p['stage'] for p in request['phases']};assert {'connect','open','tokenize','prefill','generate','answer'}<=phases,phases
                    results.append({'model':model_id,'sha256':entry['sha256'],'mode':mode,'input':label,'stats':stats,'activity':request,'load':final['activity']['load']})
                    (evidence/'results.json').write_text(json.dumps(results,indent=2)+'\n')
                    print(json.dumps({'model':model_id,'mode':mode,'input':label,'elapsed_ms':request['elapsed_ms'],'phases':list(phases)}),flush=True)
        finally:
            app.close()
            (evidence/f'{model_id}-trace.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in traces))
            if (home/'server.log').exists():shutil.copyfile(home/'server.log',evidence/f'{model_id}-daemon.log')
print('activity real: all explicitly selected model/backend/input trials passed')
