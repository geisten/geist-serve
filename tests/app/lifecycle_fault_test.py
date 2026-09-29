#!/usr/bin/env python3
"""Owned-process failure/recovery contract; protocol peer, not model evidence."""
import hashlib, json, os, shutil, signal, subprocess, tempfile, time
from pathlib import Path
from http_test import App, ROOT

binary=Path(os.environ.get('GEIST_APP_TEST_BINARY',ROOT/'build/geist-app-test'))
with tempfile.TemporaryDirectory(prefix='geist-lifecycle-fault-') as temporary:
    home=Path(temporary);(home/'models').mkdir()
    catalog=json.loads((ROOT/'models/catalog.json').read_text());catalog['revision']+=1
    model=catalog['models'][2];model.update(bytes=4,sha256=hashlib.sha256(b'test').hexdigest(),working_mib=1,recommended_ram_gib=1,backends=['cpu','metal'])
    (home/'models'/model['file']).write_bytes(b'test')
    (home/'catalog.json').write_text(json.dumps(catalog))
    peer=home/'peer';shutil.copy2(ROOT/'tests/app/reasoning_fixture.py',peer)
    def configure(**values):(home/'fixture.json').write_text(json.dumps(values))
    def dead(pid):
        try:os.kill(pid,0)
        except ProcessLookupError:return
        raise AssertionError(('owned process survives',pid))
    def selected():
        assert app.request('/app/select',{'id':model['id']})[0]==202
        return app.wait(lambda s:not s['busy'] and not s['phase'] and not s['loading'])
    def pid():return json.loads(app.request('/app/connections')[1])['daemon_pid']
    def archives():return {p.name:p.read_bytes() for pattern in ('load-failure-*','gpu-failure-*') for p in home.glob(pattern)}
    foreign=subprocess.Popen(['/bin/sleep','120'])
    configure();app=App(home,binary=binary,server=peer,env=dict(os.environ,GEIST_FIXTURE_GPU='1'))
    try:
        s=selected();assert s['ready'];old=pid()
        # Ready crash gets reaped; retained log must survive a subsequent retry.
        os.kill(old,signal.SIGKILL)
        s=app.wait(lambda s:not s['ready'] and not s['loading']);dead(old)
        assert not s['resources']['rss_bytes'];saved=archives();assert saved
        configure(crash_load=True);s=selected();assert not s['ready'] and s['activity']['load']['outcome']=='failed'
        assert len(archives())>len(saved);assert all(archives()[k]==v for k,v in saved.items())
        # posix_spawn failure after initial executable discovery cleans its socket.
        peer.rename(home/'hidden-peer');s=selected();assert not s['ready'] and not s['loading']
        assert 'Cannot start geistd' in s['message'];(home/'hidden-peer').rename(peer)
        configure();s=selected();assert s['ready'];old=pid()
        assert app.request('/app/stop',{})[0]==200;dead(old)
        # Both normal TERM and bounded KILL paths preserve cancelled load output.
        for ignore in (False,True):
            configure(load_pause=30,ignore_term=ignore);(home/'load-started').unlink(missing_ok=True)
            assert app.request('/app/select',{'id':model['id']})[0]==202
            s=app.wait(lambda s:s['loading'] and (home/'load-started').exists());old=pid();a=s['activity']['load'];before=archives()
            assert app.request('/app/activity/cancel',{'instance':s['activity']['instance'],'id':a['id'],'generation':a['generation']})[0]==202
            s=app.wait(lambda s:s['activity']['load']['outcome']=='cancelled');dead(old)
            assert not s['ready'] and not s['loading'] and len(archives())>len(before)
        configure(crash_gpu=True);s=selected();assert s['ready']
        assert app.request('/app/execution',{'mode':'gpu'})[0]==202
        s=app.wait(lambda s:s['ready'] and s['execution']['active']=='cpu' and bool(s['execution']['notice']))
        assert list(home.glob('gpu-failure-*'))
        assert s['lifecycle']['reaped_ms']<=s['lifecycle']['spawned_ms']
        assert foreign.poll() is None
    finally:
        app.close();foreign.terminate();foreign.wait()
print('lifecycle fault peer: crashes, spawn failure, TERM/KILL cancellation, GPU fallback, evidence and foreign PID isolation passed')
