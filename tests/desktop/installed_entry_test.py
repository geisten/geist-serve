#!/usr/bin/python3
"""Verify the installed shell/Python entry attaches its AppArmor profile."""
from pathlib import Path
import os
import subprocess
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='geist-entry-') as temporary:
    environment = dict(os.environ, GEIST_HOME=temporary, XDG_CONFIG_HOME=temporary)
    # GApplication service mode stays alive without activating a window/daemon.
    child = subprocess.Popen(['/usr/bin/geist-desktop', '--gapplication-service'], env=environment)
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            assert child.poll() is None, 'Installed desktop entry exited unexpectedly'
            process = Path('/proc') / str(child.pid)
            command = (process / 'cmdline').read_bytes().split(b'\0')
            profile = (process / 'attr/current').read_text().strip()
            if b'/usr/lib/geist-desktop/geist_desktop.py' in command:
                assert profile.startswith('geist-desktop ('), f'Wrong installed entry profile: {profile}'
                print('PASS: installed desktop shell/Python entry attaches geist-desktop')
                break
            time.sleep(.05)
        else:
            raise AssertionError('Installed entry never reached its Python host')
    finally:
        if child.poll() is None: child.terminate()
        child.wait(timeout=10)
