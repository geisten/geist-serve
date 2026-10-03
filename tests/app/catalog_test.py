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
        good['models'].append({**good['models'][1], 'id':'another-small-model', 'name':'Another small model', 'file':'another.gguf', 'group_id':'another-small-model', 'group_name':'Another small model'})
        invalid=[]
        for key,value in [('schema',3),('revision',0),('models',[]),('extra',True)]:
            bad=copy.deepcopy(good);bad[key]=value;invalid.append(bad)
        for key,value in [('id','../escape'),('file','../escape.gguf'),('file','model.sh'),('url','https://evil.example/model.gguf'),('sha256','0'*63),('backends',['metal']),('backends',[]),('unsupported_format','unknown'),('unsupported_format','pq2_0'),('backends',['cpu','cpu']),('backends',['cpu\x00suffix']),('backends',['cpu','metal\x00suffix']),('bytes',-1),('bytes',1.5),('name','bad\nname'),('group_id','../bad'),('group_id','custom'),('group_name',''),('quantization',''),('quantization','Q4/0'),('reasoning_format','guess'),('reasoning_format',None),('reasoning_format','think_tags\x00suffix')]:
            bad=copy.deepcopy(good);bad['models'][0][key]=value;invalid.append(bad)
        bad=copy.deepcopy(good);bad['models'].append(bad['models'][0]);invalid.append(bad)
        # #102: reference benchmark evidence is optional, but never malformed.
        evidence={'suite':'0123456789ab','date':'2026-10-03','engine':'33db79d7764b','evidence':'a'*64,'tasks':{'classify':{'de':[19,20],'en':[0,20]},'context':{'de':[20,20],'en':[18,20]}}}
        good['models'][-1]['quality']=evidence
        for change in [lambda q:q.update(suite='0123'),lambda q:q.update(suite='0123456789AB'),lambda q:q.update(date='03.10.2026'),lambda q:q.update(engine='a b'),
                       lambda q:q.update(evidence='a'*63),lambda q:q.update(extra=1),lambda q:q.pop('tasks'),lambda q:q.update(tasks={}),lambda q:q.update(tasks=[]),
                       lambda q:q['tasks'].update(classify={'de':[21,20],'en':[0,20]}),lambda q:q['tasks'].update(classify={'de':[1,0],'en':[0,20]}),
                       lambda q:q['tasks'].update(classify={'de':[-1,20],'en':[0,20]}),lambda q:q['tasks'].update(classify={'de':[1.5,20],'en':[0,20]}),
                       lambda q:q['tasks'].update(classify={'de':[1,20,3],'en':[0,20]}),lambda q:q['tasks'].update(classify={'de':[1,20]}),
                       lambda q:q['tasks'].update(classify={'de':['1',20],'en':[0,20]}),lambda q:q['tasks'].update({'../x':{'de':[1,20],'en':[0,20]}})]:
            bad=copy.deepcopy(good);change(bad['models'][-1]['quality']);invalid.append(bad)
        bad=copy.deepcopy(good);bad['models'][-1]['quality']='146/160';invalid.append(bad)
        for field in ['group_id','group_name','quantization']:
            bad=copy.deepcopy(good);del bad['models'][0][field];invalid.append(bad)
        for field,value in [('group_name','Conflicting name'),('quantization','Q4_0')]:
            bad=copy.deepcopy(good);bad['models'][-2][field]=value;invalid.append(bad)
        # Two Qwen variants share a group; Bonsai is explicitly separate.
        status={m['id']:m for m in app.status()['models']}
        assert status['qwen38-27b-q4']['group_id']==status['qwen38-27b-q8']['group_id']=='qwen38-27b'
        assert status['bonsai2-27b-pq2']['group_id']!='qwen38-27b'
        assert status['qwen38-27b-q4']['quantization']=='Q4_0'
        for bad in invalid:
            assert app.request('/app/catalog',bad)[0]==400, bad
            assert app.status()['catalog_revision']==base['revision']
            assert not (home/'catalog.json').exists()
        bonsai=next(m for m in app.status()['models'] if m['id']=='bonsai2-27b-pq2')
        assert 'PQ2_0' not in bonsai['reason']
        assert bonsai['working_mib']==20480 and bonsai['ram_gib']==24
        # Retain validation of explicit unsupported entries independently of
        # Bonsai, which the new bundled engine can now execute.
        for marker in [None,'other','pq2_0\x00suffix']:
            bad=copy.deepcopy(good)
            blocked=next(m for m in bad['models'] if m['id']==bonsai['id'])
            blocked['backends']=[]
            if marker is not None: blocked['unsupported_format']=marker
            assert app.request('/app/catalog',bad)[0]==400
        # Preserve raw JSON so duplicate fields cannot be normalized away by a client.
        for raw in [json.dumps(good).replace('"schema": 2','"schema": 2, "schema": 2'), json.dumps(good)+'{}']:
            connection=http.client.HTTPConnection('127.0.0.1',app.port,timeout=10)
            connection.request('POST','/app/catalog',raw,{'Authorization':'Bearer '+app.token})
            response=connection.getresponse();assert response.status==400;response.read();connection.close()
        too_many=copy.deepcopy(good)
        too_many['models']=[{**good['models'][1], 'id':f'model-{n}', 'file':f'model-{n}.gguf', 'group_id':f'group-{n}'} for n in range(33)]
        assert app.request('/app/catalog',too_many)[0]==400
        assert app.request('/app/catalog',good,auth=False)[0]==403
        assert app.request('/app/catalog',good)[0]==200
        assert len(app.status()['models'])==len(good['models'])
        quality={m['id']:m['quality_evidence'] for m in app.status()['models']}
        assert quality.pop('another-small-model')==evidence and quality=={m['id']:m.get('quality') for m in base['models']}
        assert len(app.status()['quality_suite'])==12
        # #103: one verdict per model, never "good" without measured speed and evidence.
        snapshot=app.status()
        for m in snapshot['models']:
            v=m['verdict']
            assert v['value'] in ('good','usable','not_recommended','unknown') and v['value']!='good', v
            assert (v['passed'] is None)==(m['quality_evidence'] is None)
        assert {m['id']:m['verdict']['reason'] for m in snapshot['models']}['another-small-model']=='unreliable'  # 57/80 in the fixture
        assert snapshot['best_choice'] is None and snapshot['limits']=={'fast_s':10,'usable_s':30,'reliable':0.9}
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
        assert app.status()['catalog_revision']==good['revision'] and len(app.status()['models'])==len(good['models'])
    finally: app.close()
    (home/'catalog.json').write_text('{broken')
    app=App(home,binary=binary)
    try:
        assert app.status()['catalog_revision']==base['revision'] and len(app.status()['models'])==len(base['models'])
        assert 'Saved catalog' in app.status()['message']
    finally: app.close()
print('catalog: bundled JSON, validation, auth, monotonic revision, atomic private persistence, restart, full/partial file protection and corrupt-file fallback passed')

# A previously selected, fully present unsupported artifact cannot start or hash
# on reopening. Use a tiny controlled artifact, never allocate large model data.
with tempfile.TemporaryDirectory(prefix='geist-catalog-blocked-') as temporary:
    import hashlib
    home=Path(temporary);(home/'models').mkdir()
    custom=copy.deepcopy(base);custom['revision']+=1
    blocked=next(m for m in custom['models'] if m['id']=='bonsai2-27b-pq2')
    blocked['backends']=[];blocked['unsupported_format']='pq2_0'
    blocked['bytes']=4;blocked['sha256']=hashlib.sha256(b'test').hexdigest()
    (home/'catalog.json').write_text(json.dumps(custom))
    (home/'selected').write_text(blocked['id'])
    artifact=home/'models'/blocked['file'];artifact.write_bytes(b'test')
    app=App(home,binary=binary)
    try:
        state=app.status()
        assert not state['busy'] and not state['loading'] and not state['ready'],state
        assert 'PQ2_0' in state['message']
        for endpoint in ['/app/select','/app/download','/app/setup']:
            assert app.request(endpoint,{'id':blocked['id']})[0]==409, endpoint
        assert not list((home/'models').glob('*.part'))
        assert artifact.read_bytes()==b'test'
        assert not (home/'server.log').exists()
    finally: app.close()
print('catalog: unsupported artifact remains blocked on service restart without reading weights')

# A saved catalog from the disabled-Bonsai app must not shadow the new bundle.
with tempfile.TemporaryDirectory(prefix='geist-catalog-upgrade-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    old=copy.deepcopy(base);old['revision']-=1
    entry=next(m for m in old['models'] if m['id']=='bonsai2-27b-pq2')
    entry.update(backends=[],unsupported_format='pq2_0',working_mib=12288,recommended_ram_gib=16)
    (home/'catalog.json').write_text(json.dumps(old))
    retained=home/'models'/entry['file'];retained.write_bytes(b'preserve existing download')
    app=App(home,binary=binary)
    try:
        assert app.status()['catalog_revision']==base['revision']
        current=next(m for m in json.loads(app.request('/app/catalog')[1])['models'] if m['id']==entry['id'])
        assert current['backends']==['cpu','metal'] and 'unsupported_format' not in current
        assert len(app.status()['models'])==len(base['models'])==9
        assert retained.read_bytes()==b'preserve existing download'
        assert json.loads((home/'catalog.json').read_text())==old
    finally: app.close()
print('catalog: older disabled-Bonsai catalog uses the new bundle and retains downloaded files')

# Explicit legacy normalization never guesses grouping from similar names.
with tempfile.TemporaryDirectory(prefix='geist-catalog-legacy-') as temporary:
    home=Path(temporary)
    legacy=copy.deepcopy(base);legacy.update(schema=1,revision=base['revision']+1)
    for m in legacy['models']:
        for field in ['group_id','group_name','quantization']: del m[field]
    (home/'catalog.json').write_text(json.dumps(legacy))
    app=App(home,binary=binary)
    try:
        assert app.status()['catalog_revision']==legacy['revision']
        assert all(m['group_id']==m['id'] and m['group_name']==m['name'] and m['quantization']=='' for m in app.status()['models'])
        full=home/'models'/legacy['models'][7]['file'];full.write_bytes(b'existing full data')
        part=home/'models'/(legacy['models'][8]['file']+'.part');part.write_bytes(b'existing partial data')
        receipt=home/('verified-'+legacy['models'][7]['sha256']);receipt.write_text('existing receipt')
        before=[(f.stat().st_ino,f.stat().st_mtime_ns,f.read_bytes()) for f in (full,part,receipt)]
        upgraded=copy.deepcopy(base);upgraded['revision']=legacy['revision']+1
        assert app.request('/app/catalog',upgraded)[0]==200
        assert [(f.stat().st_ino,f.stat().st_mtime_ns,f.read_bytes()) for f in (full,part,receipt)]==before
        assert next(m for m in app.status()['models'] if m['id']=='qwen38-27b-q8')['group_id']=='qwen38-27b'
        malformed=copy.deepcopy(upgraded);malformed.update(schema=1,revision=upgraded['revision']+1)
        assert app.request('/app/catalog',malformed)[0]==400, 'schema 1 cannot silently accept schema 2 fields'
    finally: app.close()
    app=App(home,binary=binary)
    try:
        assert app.status()['catalog_revision']==upgraded['revision']
        assert [(f.stat().st_ino,f.stat().st_mtime_ns,f.read_bytes()) for f in (full,part,receipt)]==before
    finally: app.close()
print('catalog: explicit groups, strict variant metadata, legacy isolation and schema-2 migration preserve files/receipts across restart')
