#!/usr/bin/env python3
"""Real shared service: preferences, explicit consent and missing-model recovery."""
from pathlib import Path
import json
import stat
import tempfile
from http_test import App

with tempfile.TemporaryDirectory(prefix='geist-setup-') as temporary:
    home = Path(temporary)
    (home/'selected').write_text('smollm2-360m')
    app = App(home)
    try:
        state = app.status()
        rec = state['recommendation']
        assert rec['id'] == 'smollm2-360m' and rec['source'] == 'saved'
        assert not state['loading'] and not state['busy'] and not state['message']
        assert not any(m['preview_accepted'] for m in state['models'])
        for endpoint in ['/app/preview', '/app/setup', '/app/preferences']:
            assert app.request(endpoint, {}, auth=False)[0] == 403
        for value in [None, False, 'true', 1, [], {}]:
            assert app.request('/app/preview', {'id':'smollm2-360m', 'experimental':value})[0] == 400
        assert app.request('/app/preview', {'id':'../bad', 'experimental':True})[0] == 400
        assert app.request('/app/preview', {'id':'smollm2-360m', 'experimental':True})[0] == 200
        models = app.status()['models']
        small = next(m for m in models if m['id'] == 'smollm2-360m')
        assert small['preview_accepted'] and small['quality'] == 'unverified'
        assert sum(m['preview_accepted'] for m in models) == 1
        consent = home/('preview-'+small['sha256'])
        assert stat.S_IMODE(consent.stat().st_mode) == 0o600
        assert app.request('/app/preferences', {'language':'fr'})[0] == 400
        assert app.request('/app/preferences', {'language':'de'})[0] == 200
        assert app.status()['answer_language'] == 'de'
        # A stale setup ID must not cause a different download behind the UI.
        assert app.request('/app/setup', {'id':'bitnet-2b'})[0] == 409
        assert not list((home/'models').glob('*.part'))
        # Removing another model keeps the explicit choice.
        assert app.request('/app/remove', {'id':'bitnet-2b'})[0] == 200
        assert app.status()['recommendation']['id'] == 'smollm2-360m'
        assert app.request('/app/remove', {'id':'smollm2-360m'})[0] == 200
        assert app.status()['recommendation']['source'] != 'saved'
        assert (home/'selected').read_text() == ''
    finally:
        app.close()
    app = App(home)
    try:
        state = app.status()
        assert state['answer_language'] == 'de'
        assert next(m for m in state['models'] if m['id']=='smollm2-360m')['preview_accepted']
        assert not state['busy'] and not state['message']
    finally:
        app.close()
    # Never follow a substituted preference symlink on read or write.
    outside = home/'keep'; outside.write_text('keep')
    (home/'answer-language').unlink(); (home/'answer-language').symlink_to(outside)
    consent.unlink(); consent.symlink_to(outside)
    app = App(home)
    try:
        state = app.status()
        assert state['answer_language'] == ''
        assert not next(m for m in state['models'] if m['id']=='smollm2-360m')['preview_accepted']
        assert app.request('/app/preferences', {'language':'en'})[0] == 200
        assert outside.read_text() == 'keep'
        assert not (home/'answer-language').is_symlink()
    finally:
        app.close()
print('shared setup: preserved choice, resource recheck, explicit hash-bound consent, private persistence, deletion/reopen and symlink safety passed')
