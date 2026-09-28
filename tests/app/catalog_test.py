#!/usr/bin/env python3
"""Real catalog API: fail closed, persist atomically, retain existing model files."""
import copy
import json
import os
import http.client
from pathlib import Path
import tempfile
from http_test import App, ROOT

binary=Path(os.environ.get('GEIST_APP_TEST_BINARY', ROOT/'geist-app'))
base=json.loads((ROOT/'models/catalog.json').read_text())
with tempfile.TemporaryDirectory(prefix='geist-catalog-') as temporary:
    home=Path(temporary)
    app=App(home,binary=binary)
    try:
        assert app.status()['catalog_revision']==base['revision']
        assert app.request('/app/catalog',auth=False)[0]==403
        assert json.loads(app.request('/app/catalog')[1])==base
        good=copy.deepcopy(base);good['revision']+=1
        good['models'].append({**good['models'][1], 'id':'another-small-model', 'name':'Another small model', 'file':'another.gguf'})
        invalid=[]
        for key,value in [('schema',2),('revision',0),('models',[]),('extra',True)]:
            bad=copy.deepcopy(good);bad[key]=value;invalid.append(bad)
        for key,value in [('id','../escape'),('file','../escape.gguf'),('file','model.sh'),('url','https://evil.example/model.gguf'),('sha256','0'*63),('backends',['metal']),('backends',['cpu','cpu']),('bytes',-1),('bytes',1.5),('name','bad\nname')]:
            bad=copy.deepcopy(good);bad['models'][0][key]=value;invalid.append(bad)
        bad=copy.deepcopy(good);bad['models'].append(bad['models'][0]);invalid.append(bad)
        for bad in invalid:
            assert app.request('/app/catalog',bad)[0]==400, bad
            assert app.status()['catalog_revision']==base['revision']
            assert not (home/'catalog.json').exists()
        # Preserve raw JSON so duplicate fields cannot be normalized away by a client.
        for raw in [json.dumps(good).replace('"schema": 1','"schema": 1, "schema": 1'), json.dumps(good)+'{}']:
            connection=http.client.HTTPConnection('127.0.0.1',app.port,timeout=10)
            connection.request('POST','/app/catalog',raw,{'Authorization':'Bearer '+app.token})
            response=connection.getresponse();assert response.status==400;response.read();connection.close()
        too_many=copy.deepcopy(good)
        too_many['models']=[{**good['models'][1], 'id':f'model-{n}', 'file':f'model-{n}.gguf'} for n in range(33)]
        assert app.request('/app/catalog',too_many)[0]==400
        assert app.request('/app/catalog',good,auth=False)[0]==403
        assert app.request('/app/catalog',good)[0]==200
        assert len(app.status()['models'])==7
        assert app.request('/app/catalog',good)[0]==409
        assert json.loads((home/'catalog.json').read_text())==good
        assert (home/'catalog.json').stat().st_mode & 0o777 == 0o600
        retained=home/'models'/good['models'][1]['file'];retained.write_bytes(b'preserve existing download')
        partial=home/'models'/(good['models'][0]['file']+'.part');partial.write_bytes(b'preserve partial')
        for i in [0,1]:
            bad=copy.deepcopy(good);bad['revision']+=1;bad['models'][i]['sha256']='0'*64
            assert app.request('/app/catalog',bad)[0]==409
        bad=copy.deepcopy(good);bad['revision']+=1;bad['models'][1]['bytes']+=1
        assert app.request('/app/catalog',bad)[0]==409
        assert retained.read_bytes()==b'preserve existing download' and partial.read_bytes()==b'preserve partial'
    finally: app.close()
    app=App(home,binary=binary)
    try:
        assert app.status()['catalog_revision']==good['revision'] and len(app.status()['models'])==7
    finally: app.close()
    (home/'catalog.json').write_text('{broken')
    app=App(home,binary=binary)
    try:
        assert app.status()['catalog_revision']==base['revision'] and len(app.status()['models'])==6
        assert 'Saved catalog' in app.status()['message']
    finally: app.close()
print('catalog: bundled JSON, validation, auth, monotonic revision, atomic private persistence, restart, full/partial file protection and corrupt-file fallback passed')
