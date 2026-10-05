#!/usr/bin/env python3
"""Blocked real service operations using an explicitly synthetic protocol peer."""
from concurrent.futures import ThreadPoolExecutor
import hashlib, http.client, json, os, statistics, tempfile, time
from pathlib import Path
from http_test import App, ROOT

binary=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'build/geist-app-test'))
samples=[]
with tempfile.TemporaryDirectory(prefix='geist-activity-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    catalog=json.loads((ROOT/'models/catalog.json').read_text());catalog['revision']+=1
    model=catalog['models'][2];model.update(bytes=4,sha256=hashlib.sha256(b'test').hexdigest(),working_mib=1,recommended_ram_gib=1,backends=['cpu'])
    (home/'models'/model['file']).write_bytes(b'test')
    (home/'catalog.json').write_text(json.dumps(catalog));(home/'selected').write_text(model['id'])
    def config(**values):(home/'fixture.json').write_text(json.dumps(values))
    def wait_file(name):
        deadline=time.monotonic()+5
        while not (home/name).exists() and time.monotonic()<deadline:time.sleep(.01)
        assert (home/name).exists(),name
    prompt={'model':model['id'],'messages':[{'role':'user','content':'NUMERIC_FIXTURE_SECRET'}],'prompt':'NUMERIC_FIXTURE_SECRET','experimental':True}
    config()
    app=App(home,binary=binary,server=ROOT/'tests/app/reasoning_fixture.py')
    def cancel_body(s,operation): return {'id':operation['id'],'generation':operation['generation'],'instance':s['activity']['instance']}
    def timed_status(_):
        start=time.monotonic(); state=app.status(); return (time.monotonic()-start)*1000,state
    try:
        initial=app.wait(lambda s:s['ready'])
        assert initial['activity']['load']['outcome']=='completed'
        for stage in ['connect','open','prefill','generate']:  # #148: geistd tokenizes inside the send
            op='info' if stage=='connect' else stage
            config(**{op+'_pause':30});(home/(op+'-started')).unlink(missing_ok=True)
            connection=http.client.HTTPConnection('127.0.0.1',app.port,timeout=15)
            connection.request('POST','/app/generate',json.dumps(prompt),{'Authorization':'Bearer '+app.token})
            wait_file(op+'-started')
            state=app.wait(lambda s:s['activity']['request'] and s['activity']['request']['stage']==stage)
            operation=state['activity']['request'];generation=state['process_generation']
            with ThreadPoolExecutor(max_workers=4) as pool:readings=list(pool.map(timed_status,range(20)))
            latencies=[ms for ms,_ in readings];assert sorted(latencies)[18]<=500,latencies
            assert all(s['activity']['request']['sequence']==operation['sequence'] for _,s in readings)
            assert all(s['activity']['request']['progress_events']==0 for _,s in readings)
            assert app.request('/app/activity/cancel',cancel_body(state,operation)|{'id':operation['id']+100})[0]==409
            assert app.request('/app/activity/cancel',cancel_body(state,operation),auth=False)[0]==403
            assert app.request('/app/activity/cancel',cancel_body(state,operation)|{'instance':'previous-service-instance'})[0]==409
            assert app.request('/app/activity/cancel',cancel_body(state,operation)|{'generation':operation['generation']-1})[0]==409
            # Twenty independent cancellation attempts on the same still-admitted
            # operation are safe; a late attempt may correctly return 409.
            def cancel(_):
                start=time.monotonic();code=app.request('/app/activity/cancel',cancel_body(state,operation))[0]
                assert code in (202,409);return (time.monotonic()-start)*1000
            config()
            with ThreadPoolExecutor(max_workers=4) as pool:cancel_times=list(pool.map(cancel,range(20)))
            assert sorted(cancel_times)[18]<=500,cancel_times
            connection.close()
            recovered=app.wait(lambda s:s['ready'] and not s['busy'],timeout=10)
            assert recovered['process_generation']>generation
            assert recovered['activity']['request']['outcome']=='cancelled'
            assert recovered['activity']['request']['id']==operation['id']
            assert 'NUMERIC_FIXTURE_SECRET' not in json.dumps(recovered['activity'])
            samples.append({'stage':stage,'status_ms':latencies,'cancel_ms':cancel_times})
        config();code,body,_=app.request('/app/generate',prompt)
        assert code==200 and b'SECRET' not in body
        complete=app.wait(lambda s:not s['busy'])['activity']['request']
        stages=[p['stage'] for p in complete['phases']]
        assert complete['outcome']=='completed' and 'prefill' in stages and 'preparing' in stages and 'answer' in stages,complete
        # Block load with a TERM-ignoring owned peer. Status must stay responsive
        # while the stop worker waits for the bounded KILL/reap fallback.
        assert app.request('/app/stop',{})[0]==200
        config(load_pause=30,ignore_term=True);(home/'load-started').unlink(missing_ok=True)
        assert app.request('/app/select',{'id':model['id']})[0]==202
        wait_file('load-started');state=app.status();operation=state['activity']['load']
        with ThreadPoolExecutor(max_workers=4) as pool:readings=list(pool.map(timed_status,range(20)))
        assert sorted(ms for ms,_ in readings)[18]<=500,[ms for ms,_ in readings]
        start=time.monotonic();assert app.request('/app/activity/cancel',cancel_body(state,operation))[0]==202
        assert (time.monotonic()-start)*1000<=500
        with ThreadPoolExecutor(max_workers=4) as pool:stopping=list(pool.map(timed_status,range(20)))
        assert sorted(ms for ms,_ in stopping)[18]<=500,[ms for ms,_ in stopping]
        state=app.wait(lambda s:s['activity']['load']['outcome']=='cancelled',timeout=5)
        assert not state['ready'] and not state['loading']
        pid=int((home/'fixture-pid').read_text())
        try:os.kill(pid,0)
        except ProcessLookupError:pass
        else:raise AssertionError('cancelled process survives')
        config();assert app.request('/app/select',{'id':model['id']})[0]==202
        app.wait(lambda s:s['ready']);assert app.request('/app/generate',prompt)[0]==200
    finally:app.close()
print(json.dumps({'fixture':True,'latency_samples':samples}))
print('activity service: blocked phases, cancellation, recovery, stale ID, ownership and status latency passed')
