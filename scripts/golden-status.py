#!/usr/bin/env python3
"""golden-status.py <geist-app> <home> [geistd] — the fit, verdict and ranking parts of
/app/status for a copy of <home> (its models folder linked, not copied), under
each intent. Measurements count only for the engine that took them, so
pass the geistd that measured. For geist-serve#148: the status must be identical before and
after fit and ranking moved to geist-runtime."""
import json, shutil, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests/app'))
from http_test import App

binary, source = sys.argv[1], Path(sys.argv[2])
daemon = str(Path(sys.argv[3]).resolve()) if len(sys.argv) > 3 else None
KEYS = ('id', 'resource_fit', 'fit', 'reason', 'installed', 'verdict', 'performance', 'speed')
with tempfile.TemporaryDirectory(prefix='golden-') as tmp:
    home = Path(tmp) / 'home'
    shutil.copytree(source, home, ignore=shutil.ignore_patterns('models', '*.lock', '*.sock', 'server.log'))
    (home / 'models').symlink_to(source / 'models')
    app = App(home, binary=binary, server=daemon)
    try:
        out = {}
        for intent in ('chat', 'classify', 'context'):
            if intent != 'chat':
                assert app.request('/app/verdict-settings', {'fast_s': 10, 'usable_s': 30, 'reliable': .9, 'intent': intent})[0] == 200
            s = app.status()
            out[intent] = {'models': [{k: m.get(k) for k in KEYS} for m in s['models']],
                           'ranking': s['ranking'], 'best_choice': s['best_choice']}
        print(json.dumps(out, indent=1, sort_keys=True))
    finally:
        app.close()
