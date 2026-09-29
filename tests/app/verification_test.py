#!/usr/bin/env python3
"""Verification reuse and invalidation through the real service, without inference."""
import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
from http_test import App, ROOT


def hashes(app):
    return os.pread(app.log.fileno(), 100000, 0).count(b'model verification: hashing ')


def select(app, model):
    assert app.request('/app/select', {'id': model['id']})[0] == 202
    # This integrity-only fixture launches /usr/bin/false. Wait for that owned
    # process to be reaped before mutating the next artifact; worker completion
    # alone does not mean the model lifecycle has reached its terminal state.
    return app.wait(lambda s: not s['busy'] and not s['phase'] and not s['loading'])


with tempfile.TemporaryDirectory(prefix='geist-verification-') as temporary:
    home = Path(temporary)
    app = App(home, binary=ROOT/'build/geist-app-test')
    try:
        catalog = json.loads(app.request('/app/catalog')[1])
        catalog['revision'] += 1
        template = catalog['models'][0]
        models = []
        for i in range(2):
            data = bytes([65 + i]) * 1048576
            model = copy.deepcopy(template)
            model.update(id=f'verify-{i}', name=f'Verify {i}', file=f'verify-{i}.gguf',
                         bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                         working_mib=1, recommended_ram_gib=1, backends=['cpu'],
                         group_id='verification-fixture', group_name='Verification fixture', quantization=f'Q{i}')
            (home/'models'/model['file']).write_bytes(data)
            models.append(model)
        catalog['models'] = models
        assert app.request('/app/catalog', catalog)[0] == 200
        first, second = models
        select(app, first)
        receipt = home/('verified-'+first['sha256'])
        assert receipt.exists(), 'a successful checksum must leave a reusable receipt'
        assert receipt.stat().st_mode & 0o777 == 0o600
        select(app, second)
        select(app, first)
        assert hashes(app) == 2, 'switching back to unchanged data must not hash it again'
        assert app.status()['lifecycle']['receipt']=='hit' and app.status()['lifecycle']['verified_bytes']==0
        print('verification: A → B → A hashes each file once', flush=True)
    finally:
        app.close()
    app = App(home, binary=ROOT/'build/geist-app-test')
    try:
        app.wait(lambda s: not s['busy'] and not s['phase'] and not s['loading'])
        assert hashes(app) == 0, 'unchanged files reuse receipts across process restart'
        target = home/'models'/first['file']
        # A new inode containing identical bytes requires a fresh full checksum.
        replacement = target.with_suffix('.new')
        replacement.write_bytes(target.read_bytes())
        replacement.replace(target)
        select(app, first)
        assert hashes(app) == 1
        receipt.write_text('broken receipt')
        select(app, first)
        assert hashes(app) == 2
        receipt.write_bytes(receipt.read_bytes() + b'\0trailing corruption')
        select(app, first)
        assert hashes(app) == 3
        # Same size and restored mtime must still invalidate via ctime.
        before = target.stat()
        with target.open('r+b') as stream:
            stream.write(b'X')
        os.utime(target, ns=(before.st_atime_ns, before.st_mtime_ns))
        state = select(app, first)
        assert hashes(app) == 4 and 'mismatch' in state['message']
        assert not target.exists() and not state['ready']
        # A symlink cannot turn a receipt into a trusted external file.
        second_target = home/'models'/second['file']
        second_target.unlink()
        second_target.symlink_to(receipt)
        assert app.request('/app/select', {'id':second['id']})[0] == 409
        print('verification: restart reuse, replaced inode, invalid receipt, same-size tampering and symlink rejection passed', flush=True)
    finally:
        app.close()
