#!/usr/bin/env python3
"""Reproducible CPU comparison of identical GGUF / token IDs, with cache enabled.
This is an engine smoke benchmark, NOT evidence of task quality or HA safety.
Raw JSON is append-only per output path; no energy claim without a meter.
"""
import argparse, hashlib, json, math, os, platform, socket, statistics, subprocess, sys, tempfile, time
from pathlib import Path
from urllib.request import Request, urlopen
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'clients'))
import geistd

PROMPT='<|im_start|>user\nWhat is the capital of France? Answer in one word.<|im_end|>\n<|im_start|>assistant\n'

def sha(path):
    h=hashlib.sha256()
    with open(path,'rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''): h.update(chunk)
    return h.hexdigest()

def http(url,data=None):
    request=Request(url,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
    with urlopen(request,timeout=120) as r: return json.load(r)

def summary(rows):
    groups={}
    for row in rows:
        if not row.get('complete') or not row.get('output','').strip(): continue
        if not isinstance(row.get('wall_ms'),(float,int)) or not math.isfinite(row['wall_ms']) or row['wall_ms'] < 0: raise ValueError('invalid duration')
        groups.setdefault(row['phase'],[]).append(row['wall_ms'])
    return {phase:{'samples':len(v),'median_wall_ms':statistics.median(v),'min_wall_ms':min(v),'max_wall_ms':max(v)} for phase,v in groups.items()}

def compatible(a,b):
    keys=('model_sha256','prompt_token_ids','threads','context','temperature','top_p','top_k','seed','max_tokens','backend','kv_type')
    mismatches=[k for k in keys if a.get(k) != b.get(k) or k not in a or k not in b]
    if not a.get('cache_enabled') or not b.get('cache_enabled'): mismatches.append('cache_enabled')
    return mismatches

def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=10)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()

def run(args):
    output=Path(args.output)
    if output.exists(): raise ValueError('output exists: choose a new path to preserve raw data')
    # The default prompt is explicitly ChatML: no hidden per-server template.
    prompt=Path(args.prompt_file).read_text() if args.prompt_file else PROMPT
    config={'model_sha256':sha(args.model),'hardware':platform.platform(),'machine':platform.machine(),
        'threads':args.threads,'context':4096,'temperature':0,'top_p':1,'top_k':0,'seed':1,'max_tokens':16,
        'backend':'CPU','kv_type':'f32','cache_enabled':True,'prompt':prompt,
        'created_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'quality_status':'unverified'}
    if sys.platform != 'darwin': raise ValueError('this paired configuration currently verifies CPU f32 KV on macOS only')
    results={'schema':1,'scope':'engine-smoke','config':config,'engines':{},'energy_joules':None,
        'limitations':['Small smoke sample; not a general performance ranking.',
                       'CPU comparison; a Metal product baseline is separate.',
                       'RSS is sampled at phase boundaries, not measured peak RSS.',
                       'Cold means process/model startup, not a flushed OS file cache.',
                       'No task-quality or Home Assistant execution claim.']}
    env=os.environ.copy()
    for key in list(env):
        if key.startswith('GEIST_KV_'): env.pop(key)
    env.update(OMP_NUM_THREADS=str(args.threads),OMP_WAIT_POLICY='passive')
    results['source'] = {
        'repository_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'working_tree':subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).splitlines(),
        'engine_pin':'25861c0bd197f1a98f17e49efe0cdc48a0e40713'}
    try:
        with tempfile.TemporaryDirectory(dir='/tmp',prefix='geist-compare-') as home:
            for name,binary in [('geistd',args.geistd),('llama.cpp',args.llama_server)]:
                with open(home+'/'+name+'.log','w+') as log:
                    if name=='geistd':
                        path=home+'/inference.sock'; cmd=[binary,args.model,'--socket',path,'--sessions','1']
                    else:
                        with socket.socket() as probe: probe.bind(('127.0.0.1',0)); port=probe.getsockname()[1]
                        url=f'http://127.0.0.1:{port}'
                        cmd=[binary,'-m',args.model,'-c','4096','-t',str(args.threads),'-tb',str(args.threads),
                             '-ngl','0','--no-op-offload','-ctk','f32','-ctv','f32','-np','1','--slots','--host','127.0.0.1','--port',str(port)]
                    t0=time.perf_counter(); proc=subprocess.Popen(cmd,stdout=log,stderr=log,env=env)
                    entry={'binary_sha256':sha(binary),'command':cmd,'rows':[]}; results['engines'][name]=entry
                    try:
                        for _ in range(1200):
                            if proc.poll() is not None: log.seek(0); raise RuntimeError(log.read()[-3000:])
                            try:
                                if name=='geistd': client=geistd.Client(path=path,timeout=120); info=client.info()
                                else: http(url+'/health')
                                break
                            except (OSError, ValueError): time.sleep(.1)
                        else: raise TimeoutError('model startup')
                        entry['startup_ms']=(time.perf_counter()-t0)*1000
                        if name=='geistd':
                            ids=client.tokenize(prompt)
                            if info['add_bos'] and info['bos']>=0 and (not ids or ids[0]!=info['bos']): ids.insert(0,info['bos'])
                            config['prompt_token_ids']=ids
                            entry['daemon_info']=info
                            session=client.open(temperature=0,top_p=1,top_k=0,seed=1)
                        else:
                            tokens=http(url+'/tokenize',{'content':prompt,'add_special':False})['tokens']
                            if info['add_bos'] and info['bos']>=0 and (not tokens or tokens[0]!=info['bos']): tokens.insert(0,info['bos'])
                            if ids!=tokens: raise ValueError('tokenizers disagree; no comparable benchmark')
                            entry['version']=subprocess.check_output([binary,'--version'],stderr=subprocess.STDOUT,text=True).strip()
                        for i in range(args.repeats+1):
                            row={'phase':'first' if i==0 else 'warm','complete':False}; entry['rows'].append(row)
                            started=time.perf_counter()
                            if name=='geistd':
                                prefill=session.prefill(ids)
                                text=''.join(session.generate(max=16))
                                result=session.last
                                complete=result.get('reason') in ('stop','max')
                                row.update(prefill=prefill,engine_result=result)
                            else:
                                result=http(url+'/completion',{'prompt':ids,'n_predict':16,'temperature':0,'top_p':1,'top_k':0,
                                    'seed':1,'cache_prompt':True,'id_slot':0,'stream':False})
                                text=result.get('content',''); complete=result.get('stop',False)
                                row['engine_result']=result
                            row.update(wall_ms=(time.perf_counter()-started)*1000,output=text,complete=complete)
                            rss=subprocess.check_output(['ps','-o','rss=','-p',str(proc.pid)],text=True).strip()
                            row['sampled_rss_bytes']=int(rss)*1024
                        if name=='geistd': session.close()
                        entry['summary']=summary(entry['rows'])
                    finally: stop(proc)
    except Exception as exc:
        results['run_error'] = str(exc)
        raise
    finally:
        output.parent.mkdir(parents=True,exist_ok=True)
        with output.open('x') as f: json.dump(results,f,indent=2,ensure_ascii=False)
    if any(not r['complete'] for e in results['engines'].values() for r in e['rows']): raise RuntimeError('incomplete run; inspect raw results')
    print(json.dumps({k:v['summary'] for k,v in results['engines'].items()},indent=2))

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--model',required=True);ap.add_argument('--geistd',default=str(ROOT/'geistd'))
    ap.add_argument('--llama-server',required=True);ap.add_argument('--threads',type=int,default=2)
    ap.add_argument('--repeats',type=int,default=3);ap.add_argument('--prompt-file');ap.add_argument('--output',required=True)
    args=ap.parse_args()
    if args.threads<1 or args.repeats<1: ap.error('positive threads and repeats required')
    run(args)
