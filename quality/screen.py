#!/usr/bin/env python3
"""Sequential screening on one host. Failed model loads remain explicit failures."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
from evidence import ROOT

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--models',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--device',choices=['apple-silicon','pi5'],required=True)
a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=False)
source=(ROOT/'src/app/core.c').read_text().split('const struct app_model app_models')[1].split('\n};',1)[0]
models=re.findall(r'\{"([^"]+)",\s*"[^"]+",\s*"([^"]+)".*?"([a-f0-9]{64})"',source,re.S)
assert len(models)==6
results=[]
for model_id,filename,digest in models:
    with (a.output/(model_id+'.log')).open('x') as log:
        r=subprocess.run([sys.executable,str(ROOT/'quality/evaluate.py'),'--model',str(a.models/filename),
            '--model-id',model_id,'--expected-sha256',digest,'--split','screening','--device',a.device,
            '--output',str(a.output/model_id)],stdout=log,stderr=subprocess.STDOUT)
    results.append({'model_id':model_id,'exit_code':r.returncode,'status':'screened' if r.returncode==0 else 'runtime_or_setup_failed'})
    print(json.dumps(results[-1]),flush=True)
    (a.output/'progress.json').write_text(json.dumps(results,indent=2)+'\n')
