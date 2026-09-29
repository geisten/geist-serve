#!/usr/bin/env python3
"""Authenticated catalog deletion must not follow symlinks or delete arbitrary files."""
from pathlib import Path
import tempfile
from http_test import App

with tempfile.TemporaryDirectory(prefix='geist-remove-') as temporary:
    home = Path(temporary)
    app = App(home)
    try:
        models = home / 'models'
        filename = models / 'smollm2-360m-instruct-q8_0.gguf'
        partial = filename.with_suffix('.gguf.part')
        unrelated = home / 'keep.txt'
        unrelated.write_text('keep')
        assert app.request('/app/remove', {'id': 'smollm2-360m'}, auth=False)[0] == 403
        assert app.request('/app/remove', {'id': '../keep.txt'})[0] == 400
        partial.write_bytes(b'partial download')
        assert app.request('/app/remove', {'id': 'smollm2-360m'})[0] == 200
        assert not partial.exists()
        filename.symlink_to(unrelated)
        partial.write_bytes(b'retain if unsafe')
        assert app.request('/app/remove', {'id': 'smollm2-360m'})[0] == 409
        assert unrelated.read_text() == 'keep' and partial.exists() and filename.is_symlink()
        filename.unlink()
        filename.write_bytes(b'incomplete model')
        assert app.request('/app/remove', {'id': 'smollm2-360m'})[0] == 200
        assert not filename.exists() and not partial.exists()
        assert app.request('/app/remove', {'id': 'smollm2-360m'})[0] == 200
        models.rename(home / 'original-models')
        models.symlink_to(home / 'original-models', target_is_directory=True)
        assert app.request('/app/remove', {'id': 'smollm2-360m'})[0] == 409
        print('model removal: catalog allowlist, authentication, partial cleanup, idempotency and symlink rejection passed')
    finally:
        app.close()

# A confirmed removal can stop an idle owned model atomically, without deleting
# its catalog choice or unrelated user data. The real file lives only in a temp home.
import os
import shutil
import http.client
import json
from http_test import ROOT
fixture = os.environ.get('GEIST_TEST_MODEL')
if fixture:
    with tempfile.TemporaryDirectory(prefix='geist-remove-running-') as temporary:
        home = Path(temporary)
        (home/'models').mkdir()
        target = home/'models/smollm2-360m-instruct-q8_0.gguf'
        shutil.copyfile(fixture, target)
        app = App(home, server=ROOT/'geistd')
        try:
            assert app.request('/app/select', {'id':'smollm2-360m'})[0] == 202
            app.wait(lambda s:s['ready'],timeout=60)
            connection = http.client.HTTPConnection('127.0.0.1', app.port, timeout=30)
            connection.request('POST', '/app/generate', json.dumps({
                'experimental':True, 'prompt':'List one hundred animal names.'}),
                {'Authorization':'Bearer '+app.token})
            response = connection.getresponse()
            assert response.status == 200 and response.readline()
            assert app.request('/app/remove', {'id':'smollm2-360m'})[0] == 409
            assert target.exists() and app.status()['ready']
            response.close(); connection.close()
            app.wait(lambda s:not s['busy'],timeout=15)
            # Unsafe paths must not stop a healthy running model.
            backup=target.with_suffix('.saved')
            target.rename(backup); target.symlink_to(backup)
            assert app.request('/app/remove', {'id':'smollm2-360m'})[0] == 409
            assert app.status()['ready'] and backup.exists()
            target.unlink(); backup.rename(target)
            assert app.request('/app/remove', {'id':'smollm2-360m'})[0] == 200
            state=app.status()
            assert not state['ready'] and not target.exists()
            entry=next(m for m in state['models'] if m['id']=='smollm2-360m')
            assert not entry['installed'] and len(state['models'])==len(json.loads((ROOT/'models/catalog.json').read_text())['models'])
            assert (home/'selected').read_text()==''
            assert app.request('/app/remove', {'id':'smollm2-360m'})[0] == 200
            print('model removal: idle active model stopped, unsafe paths retain running model, catalog choice retained')
        finally:
            app.close()
