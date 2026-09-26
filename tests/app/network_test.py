#!/usr/bin/env python3
"""Opt-in production HTTPS smoke: downloads a small prefix, then cancels.

Unlike download_test.py, this contacts the catalog's actual Hugging Face URL
and uses the production executable and the host's CA store.
"""
import os
from pathlib import Path
import tempfile
from http_test import App

if os.environ.get('GEIST_NETWORK_TEST') != '1':
    raise SystemExit('Set GEIST_NETWORK_TEST=1 to allow a real model-download test.')

with tempfile.TemporaryDirectory(prefix='geist-tls-') as home:
    app = App(home)
    try:
        assert app.request('/app/download', {'id': 'smollm2-360m'})[0] == 202
        state = app.wait(lambda s: s['received'] > 1048576 or not s['busy'], timeout=90)
        assert state['received'] > 1048576, state
        assert app.request('/app/cancel', {})[0] == 200
        app.wait(lambda s: not s['busy'])
        part = Path(home) / 'models/smollm2-360m-instruct-q8_0.gguf.part'
        assert part.exists() and part.stat().st_size > 1048576
        print('production HTTPS: catalog URL, redirects, host CA validation, write and cancel passed')
    finally:
        app.close()
