#!/usr/bin/env python3
import os, subprocess, sys, tempfile, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'clients'))
import geistd
model=os.environ['GEIST_TEST_MODEL']
with tempfile.TemporaryDirectory(dir='/tmp',prefix='geistd-contract-') as home:
    path=home+'/sock'
    with open(home+'/log','w+') as log:
        proc=subprocess.Popen([str(ROOT/'geistd'),model,'--socket',path,'--sessions','3'],stdout=log,stderr=log,
            env=os.environ | {'OMP_NUM_THREADS':'2','OMP_WAIT_POLICY':'passive'})
        try:
            for _ in range(600):
                if proc.poll() is not None: log.seek(0); raise AssertionError(log.read())
                try: geistd.Client(path=path).info(); break
                except OSError: time.sleep(.1)
            else: raise AssertionError('daemon failed to start')
            subprocess.run([sys.executable,str(ROOT/'tests/geistd_ops.py'),path],cwd=ROOT,check=True,timeout=180)
        finally:
            proc.terminate(); proc.wait(timeout=10)
