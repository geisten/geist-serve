#!/usr/bin/env python3
"""Isolated release/install failure tests; never touches system services."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
ASSET = 'geist-serve-linux-x86_64'
NAMES = [ASSET, ASSET+'-geistd', 'geist-serve.socket', 'geist-serve.service', 'geist-serve.default']


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='geist-installer-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.release = self.root/'release'; self.release.mkdir()
        self.prefix = self.root/'prefix with spaces'
        (self.prefix/'bin').mkdir(parents=True)
        self.shims = self.root/'shims'; self.shims.mkdir()
        self.write_shim('uname', 'case "$1" in -s) echo Linux;; -m) echo x86_64;; esac')
        self.write_shim('id', 'echo 1000')
        for name in NAMES:
            (self.release/name).write_text('#!/bin/sh\nexit 2\n' if name.startswith(ASSET) else 'verified unit\n')
        self.manifest()
        self.old = {}
        for name in ['geist-serve', 'geistd']:
            target = self.prefix/'bin'/name
            target.write_text(f'previous {name}\n'); target.chmod(0o755)
            self.old[name] = target.read_bytes()
        self.env = os.environ | {'GEIST_SERVE_BASE': self.release.as_uri(),
                                 'GEIST_SERVE_PREFIX': str(self.prefix),
                                 'PATH': str(self.shims)+os.pathsep+os.environ['PATH']}

    def write_shim(self, name, body):
        p = self.shims/name; p.write_text('#!/bin/sh\n'+body+'\n'); p.chmod(0o755)

    def manifest(self):
        (self.release/'SHA256SUMS').write_text(''.join(
            hashlib.sha256((self.release/n).read_bytes()).hexdigest()+f'  {n}\n' for n in NAMES))

    def run_install(self, success=False):
        result = subprocess.run(['sh', str(ROOT/'install.sh')], env=self.env,
                                capture_output=True, text=True, timeout=25)
        self.assertEqual(result.returncode == 0, success, result.stdout+result.stderr)
        return result

    def unchanged(self):
        for name, data in self.old.items():
            self.assertEqual((self.prefix/'bin'/name).read_bytes(), data)
            self.assertEqual((self.prefix/'bin'/name).stat().st_mode & 0o777, 0o755)
        self.assertFalse(list((self.prefix/'bin').glob('.geist-install.*')))

    def test_success_both_executables_and_no_staging_files(self):
        self.run_install(True)
        for source, target in [(ASSET, 'geist-serve'), (ASSET+'-geistd', 'geistd')]:
            self.assertEqual((self.release/source).read_bytes(), (self.prefix/'bin'/target).read_bytes())
        self.assertFalse((self.prefix/'.geist-serve-install.lock').exists())
        self.assertFalse(list((self.prefix/'bin').glob('.geist-install.*')))

    def test_missing_daemon_preserves_both(self):
        (self.release/(ASSET+'-geistd')).unlink()
        self.run_install(); self.unchanged()

    def test_modified_daemon_preserves_both(self):
        (self.release/(ASSET+'-geistd')).write_text('tampered')
        self.run_install(); self.unchanged()

    def test_units_must_be_verified(self):
        (self.release/'geist-serve.service').write_text('tampered')
        self.run_install(); self.unchanged()

    def test_missing_unit_checksum_is_rejected(self):
        p = self.release/'SHA256SUMS'
        p.write_text(''.join(l for l in p.read_text().splitlines(True) if not l.endswith('geist-serve.socket\n')))
        self.run_install(); self.unchanged()

    def test_duplicate_checksum_is_rejected(self):
        p = self.release/'SHA256SUMS'; p.write_text(p.read_text()+p.read_text().splitlines(True)[0])
        self.run_install(); self.unchanged()

    def test_bad_executable_preserves_both(self):
        (self.release/(ASSET+'-geistd')).write_text('#!/bin/sh\nexit 1\n')
        self.manifest(); self.run_install(); self.unchanged()

    def test_partial_replacement_rolls_back(self):
        real_mv = shutil.which('mv')
        self.write_shim('mv', f'''for arg in "$@"; do target=$arg; done
if [ "$target" = "$GEIST_SERVE_PREFIX/bin/geistd" ] && [ ! -f "$GEIST_SERVE_PREFIX/fault-seen" ]; then
    touch "$GEIST_SERVE_PREFIX/fault-seen"; exit 1
fi
exec "{real_mv}" "$@"''')
        self.run_install(); self.unchanged()
        self.assertFalse((self.prefix/'.geist-serve-install.lock').exists())

    def test_termination_during_replacement_rolls_back(self):
        real_mv = shutil.which('mv')
        self.write_shim('mv', f'''for arg in "$@"; do target=$arg; done
if [ "$target" = "$GEIST_SERVE_PREFIX/bin/geistd" ] && [ ! -f "$GEIST_SERVE_PREFIX/fault-seen" ]; then
    touch "$GEIST_SERVE_PREFIX/fault-seen"; kill -TERM "$PPID"; exit 1
fi
exec "{real_mv}" "$@"''')
        self.run_install(); self.unchanged()
        self.assertFalse((self.prefix/'.geist-serve-install.lock').exists())

    def test_new_install_failure_removes_partial_binary(self):
        for name in self.old: (self.prefix/'bin'/name).unlink()
        real_mv = shutil.which('mv')
        self.write_shim('mv', f'''for arg in "$@"; do target=$arg; done
[ "$target" != "$GEIST_SERVE_PREFIX/bin/geistd" ] || exit 1
exec "{real_mv}" "$@"''')
        self.run_install()
        self.assertFalse((self.prefix/'bin/geist-serve').exists())
        self.assertFalse((self.prefix/'bin/geistd').exists())

    def test_symlink_target_is_not_overwritten(self):
        target = self.prefix/'bin/geistd'; target.unlink()
        external = self.root/'owner-file'; external.write_text('keep')
        target.symlink_to(external)
        self.run_install()
        self.assertTrue(target.is_symlink()); self.assertEqual(external.read_text(), 'keep')
        self.assertEqual((self.prefix/'bin/geist-serve').read_bytes(), self.old['geist-serve'])

    def test_other_installer_lock_is_preserved(self):
        lock = self.prefix/'.geist-serve-install.lock'; lock.mkdir()
        marker = lock/'owner'; marker.write_text('other install')
        self.run_install(); self.unchanged()
        self.assertEqual(marker.read_text(), 'other install')

    def test_insecure_transport_rejected(self):
        self.env['GEIST_SERVE_BASE'] = 'http://127.0.0.1:1'
        self.run_install(); self.unchanged()


if __name__ == '__main__': unittest.main()
