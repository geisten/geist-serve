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
