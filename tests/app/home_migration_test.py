#!/usr/bin/env python3
"""#92: the data folder moves from Geist/geist to geisten once, safely.

Drives the real CLI (which resolves the folder exactly like geist-app) with a
temporary HOME; no service is started.
"""
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from http_test import ROOT

cli = ROOT/'geisten'
app = ROOT/'build/geist-app-test'
mac = sys.platform == 'darwin'


def run(home, *args, binary=cli, **env):
    clean = {k: v for k, v in os.environ.items() if k not in ('GEIST_HOME', 'GEISTEN_HOME', 'XDG_DATA_HOME')}
    return subprocess.run([str(binary), *args], env=clean | {'HOME': str(home)} | env, capture_output=True, text=True, timeout=30)


def folders(home):
    base = home/('Library/Application Support' if mac else '.local/share')
    return base/('Geist' if mac else 'geist'), base/'geisten'


def earlier_data(old):
    (old/'models').mkdir(parents=True)
    (old/'models'/'keep.gguf').write_text('weights')
    (old/'api-key').write_text('k' * 64)


with tempfile.TemporaryDirectory(prefix='geisten-home-') as temporary:
    root = Path(temporary)

    # A fresh installation: nothing is moved and nothing invented.
    home = root/'fresh'; home.mkdir()
    old, new = folders(home)
    run(home, 'status')
    assert not old.exists() and not old.is_symlink()

    # The earlier folder moves once; the old path stays usable through a relative symlink.
    home = root/'move'; home.mkdir()
    old, new = folders(home)
    earlier_data(old)
    run(home, 'status')
    assert new.is_dir() and not new.is_symlink() and (new/'models'/'keep.gguf').read_text() == 'weights'
    assert old.is_symlink() and os.readlink(old) == 'geisten' and (old/'api-key').read_text() == 'k' * 64
    run(home, 'status')  # a second start changes nothing
    assert new.is_dir() and old.is_symlink()

    # Both exist: the new one is used, the old one is left alone.
    home = root/'both'; home.mkdir()
    old, new = folders(home)
    earlier_data(old); new.mkdir(parents=True)
    run(home, 'status')
    assert old.is_dir() and not old.is_symlink() and (old/'models'/'keep.gguf').exists()

    # An earlier service still holds the old folder: it is not moved now, but later.
    home = root/'busy'; home.mkdir()
    old, new = folders(home)
    earlier_data(old)
    with (old/'app.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        run(home, 'status')
        assert old.is_dir() and not old.is_symlink() and not new.exists(), 'moved a folder in use'
    run(home, 'status')
    assert new.is_dir() and old.is_symlink()

    # An explicit folder (GEISTEN_HOME, the earlier GEIST_HOME, geist-app --home) never moves the default one.
    for env in ({'GEISTEN_HOME': str(root/'explicit-a')}, {'GEIST_HOME': str(root/'explicit-b')}):
        home = root/('env-' + next(iter(env))); home.mkdir()
        old, new = folders(home)
        earlier_data(old)
        run(home, 'status', **env)
        assert old.is_dir() and not old.is_symlink() and not new.exists(), env
    if app.exists():
        home = root/'app-home'; home.mkdir()
        old, new = folders(home)
        earlier_data(old)
        run(home, '--home', str(root/'explicit-c'), '--check', binary=app)
        assert old.is_dir() and not old.is_symlink() and not new.exists(), 'geist-app --home moved the default folder'
print('data folder: one safe move from Geist to geisten, symlink kept, busy/both/explicit folders untouched')
