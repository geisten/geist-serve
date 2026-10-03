#!/usr/bin/env python3
"""scripts/install-geisten.sh against local fixtures (#46). Linux only.

Each test gets a fresh HOME, a fixture Ed25519 key and a release directory
served through the installer's explicit test origin. The payload binaries are
a tiny compiled stand-in for geist (a real ELF for this machine): it answers
--help, and `start` exits with FAKE_START_RC so busy and failed starts can be
simulated. Real-runtime acceptance is tests/install/bootstrap_acceptance.sh.
"""
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT/'scripts/install-geisten.sh'
PLATFORM = {'x86_64': 'linux-x86_64', 'aarch64': 'linux-aarch64'}.get(platform.machine())
FAKE_GEIST = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *log = getenv("FAKE_LOG");
    FILE *f = log ? fopen(log, "a") : NULL;
    for (int i = 1; f && i < argc; i++) fprintf(f, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
    if (f) fclose(f);
    const char *rc = getenv("FAKE_START_RC"), *setup = getenv("FAKE_SETUP_RC");
    if (argc > 1 && !strcmp(argv[1], "start") && rc) return atoi(rc);
    if (argc > 1 && !strcmp(argv[1], "setup") && setup) return atoi(setup);
    return 0;
}
'''


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Release:
    """A signed release directory: manifest, signature and archives."""

    def __init__(self, root: Path, fake: Path, key: Path):
        self.root, self.fake, self.key = root, fake, key

    def make(self, version, *, extra_member=None, link_member=False, channel='stable', crlf=False, sign_key=None, debs=False):
        out = self.root/f'v{version}'
        out.mkdir(parents=True, exist_ok=True)
        for plat in ('linux-x86_64', 'linux-aarch64'):
            top = f'geisten-{version}-{plat}'
            staging = out/'src'/top
            staging.mkdir(parents=True)
            for b in ('geisten', 'geist-app', 'geistd'):
                shutil.copy(self.fake, staging/b)
            (staging/'LICENSE').write_text('Apache-2.0\n')
            (staging/'SHA256SUMS').write_text(''.join(f'{sha256(staging/b)}  {b}\n' for b in ('geisten', 'geist-app', 'geistd')))
            with tarfile.open(out/f'{top}.tar.gz', 'w:gz') as tar:
                tar.add(staging, arcname=top)
                if extra_member:
                    info = tarfile.TarInfo(extra_member); info.size = 4
                    import io; tar.addfile(info, io.BytesIO(b'evil'))
                if link_member:
                    info = tarfile.TarInfo(f'{top}/README.md'); info.type = tarfile.SYMTYPE; info.linkname = '/etc/passwd'
                    tar.addfile(info)
        if debs:  # stand-ins: only their size and checksum are verified here
            for name in (f'geisten_{version}_amd64.deb', f'geisten_{version}_arm64.deb', f'geisten-desktop_{version}_all.deb'):
                (out/name).write_bytes(name.encode() * 50)
        subprocess.run([sys.executable, ROOT/'scripts/installer-manifest.py', out, version, 'a'*40, 'b'*40], check=True)
        manifest = out/'geisten-manifest'
        text = manifest.read_text().replace('channel stable', f'channel {channel}')
        manifest.write_bytes(text.replace('\n', '\r\n').encode() if crlf else text.encode())
        subprocess.run(['sh', ROOT/'scripts/sign-manifest.sh', manifest, sign_key or self.key], check=True)
        return out


@unittest.skipUnless(sys.platform.startswith('linux') and PLATFORM, 'the installer targets 64-bit Linux')
class BootstrapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='geist-bootstrap-')
        t = Path(cls.tmp.name)
        (t/'fake.c').write_text(FAKE_GEIST)
        subprocess.run([os.environ.get('CC', 'cc'), '-O1', '-o', t/'fake', t/'fake.c'], check=True)
        for name in ('key', 'other'):
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'ed25519', '-out', t/f'{name}.pem'], check=True)
        subprocess.run(['openssl', 'pkey', '-in', t/'key.pem', '-pubout', '-out', t/'key.pub'], check=True)
        # Recording stand-ins for the --desktop route: nothing is installed for real.
        fakebin = t/'fakebin'; fakebin.mkdir()
        (fakebin/'sudo').write_text('#!/bin/sh\necho "sudo $*" >> "$FAKE_LOG"\nexec "$@"\n')
        (fakebin/'apt-get').write_text('#!/bin/sh\nfor f in "$@"; do case $f in /*) test -r "$f" || exit 99;; esac; done\n'
                                       'echo "apt-get $*" >> "$FAKE_LOG"\nexit "${FAKE_APT_RC:-0}"\n')
        for tool in ('sudo', 'apt-get'): (fakebin/tool).chmod(0o755)
        (t/'ubuntu').write_text('ID=ubuntu\nVERSION_ID="24.04"\n')
        (t/'debian').write_text('ID=debian\nVERSION_ID="12"\n')
        cls.t, cls.fakebin = t, fakebin

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(prefix='case-', dir=self.t))
        self.home = self.case/'home'
        self.home.mkdir()
        self.release = Release(self.case/'releases', self.t/'fake', self.t/'key.pem')
        self.runtime = self.home/'.local/share/geisten-runtime'
        self.launcher = self.home/'.local/bin/geisten'
        self.alias = self.home/'.local/bin/geist'  # the pre-#92 name

    def run_installer(self, origin, *args, env=None, script=SCRIPT, pubkey=True):
        e = {'HOME': str(self.home), 'PATH': os.environ['PATH'], 'GEIST_INSTALL_TEST_ORIGIN': f'file://{origin}'}
        if pubkey:
            e['GEIST_INSTALL_TEST_PUBKEY'] = str(self.t/'key.pub')
        e.update(env or {})
        # A new session has no controlling terminal, like CI and `curl | sh` without a TTY.
        return subprocess.run(['sh', str(script), *args], env=e, capture_output=True, text=True, timeout=120,
                              start_new_session=True)

    def receipt(self):
        return dict(line.split(' ', 1) for line in (self.runtime/'receipt').read_text().splitlines())

    def assertNothingInstalled(self):
        self.assertFalse(self.launcher.exists() or self.launcher.is_symlink(), 'launcher created')
        self.assertFalse((self.runtime/'receipt').exists(), 'receipt created')
        self.assertFalse((self.runtime/'current').exists(), 'current pointer created')

    def test_fresh_install_idempotent_update_and_rollback_retention(self):
        r1 = self.release.make('1.0.0')
        p = self.run_installer(r1, '--no-start')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(os.readlink(self.launcher), f'{self.runtime}/current/geisten')
        self.assertEqual(os.readlink(self.alias), f'{self.runtime}/current/geisten')
        self.assertEqual(os.readlink(self.runtime/'current'), 'versions/1.0.0')
        self.assertEqual(self.receipt()['version'], '1.0.0')
        self.assertIn('is not on your PATH', p.stdout)
        # same version again: nothing changes
        before = (self.runtime/'receipt').read_text()
        self.assertEqual(self.run_installer(r1, '--no-start').returncode, 0)
        self.assertEqual((self.runtime/'receipt').read_text(), before)
        # two updates: the active and one rollback version remain
        for v in ('1.1.0', '1.2.0'):
            self.assertEqual(self.run_installer(self.release.make(v), '--no-start').returncode, 0)
        self.assertEqual(sorted(p.name for p in (self.runtime/'versions').iterdir()), ['1.1.0', '1.2.0'])
        self.assertEqual((self.receipt()['version'], self.receipt()['previous']), ('1.2.0', '1.1.0'))
        self.assertEqual(list(self.runtime.glob('.stage.*')), [], 'staging left behind')

    def test_start_failure_and_busy_restore_the_previous_version(self):
        self.assertEqual(self.run_installer(self.release.make('1.0.0')).returncode, 0)
        p = self.run_installer(self.release.make('2.0.0'), env={'FAKE_START_RC': '1'})
        self.assertEqual(p.returncode, 15, p.stderr)
        self.assertEqual(os.readlink(self.runtime/'current'), 'versions/1.0.0')
        p = self.run_installer(self.release.make('3.0.0'), env={'FAKE_START_RC': '43'})
        self.assertEqual(p.returncode, 14, p.stderr)
        self.assertEqual(os.readlink(self.runtime/'current'), 'versions/1.0.0')
        self.assertEqual(self.receipt()['version'], '1.0.0')

    def test_verification_failures_change_nothing(self):
        cases = {
            'untrusted signature': dict(sign_key=self.t/'other.pem'),
            'preview channel': dict(channel='preview'),
            'CRLF manifest': dict(crlf=True),
            'unexpected member': dict(extra_member='geisten-1.0.0-%s/extra' % PLATFORM),
            'traversal member': dict(extra_member='../escape'),
            'link member': dict(link_member=True),
        }
        for name, kw in cases.items():
            with self.subTest(name):
                shutil.rmtree(self.case/'releases', ignore_errors=True)
                p = self.run_installer(self.release.make('1.0.0', **kw), '--no-start')
                self.assertEqual(p.returncode, 12, (name, p.stderr))
                self.assertNothingInstalled()
                self.assertFalse((self.case/'escape').exists())

    def test_tampered_archive_and_wrong_version(self):
        r = self.release.make('1.0.0')
        archive = r/f'geisten-1.0.0-{PLATFORM}.tar.gz'
        data = bytearray(archive.read_bytes()); data[-5] ^= 0xff  # same size, other bytes
        archive.write_bytes(bytes(data))
        p = self.run_installer(r, '--no-start')
        self.assertEqual(p.returncode, 12, p.stderr)
        self.assertIn('checksum differs', p.stderr)
        archive.write_bytes(bytes(data) + b'x')  # larger than signed: refused while downloading
        self.assertEqual(self.run_installer(r, '--no-start').returncode, 12)
        self.assertEqual(self.run_installer(self.release.make('2.0.0'), '--version', '1.9.0', '--no-start').returncode, 12)
        self.assertNothingInstalled()

    def test_pinned_production_key_rejects_other_signers(self):
        p = self.run_installer(self.release.make('1.0.0'), '--no-start', pubkey=False)
        self.assertEqual(p.returncode, 12)
        self.assertIn('manifest signature is not valid', p.stderr)
        self.assertNothingInstalled()

    def test_foreign_launcher_and_unowned_runtime_are_left_alone(self):
        r = self.release.make('1.0.0')
        self.launcher.parent.mkdir(parents=True)
        self.launcher.write_text('#!/bin/sh\necho mine\n')
        p = self.run_installer(r, '--no-start')
        self.assertEqual(p.returncode, 13, p.stderr)
        self.assertEqual(self.launcher.read_text(), '#!/bin/sh\necho mine\n')
        self.launcher.unlink()
        # A foreign file at the old name is left alone as well.
        self.alias.write_text('#!/bin/sh\necho mine\n')
        self.assertEqual(self.run_installer(r, '--no-start').returncode, 13)
        self.assertEqual(self.alias.read_text(), '#!/bin/sh\necho mine\n')
        self.alias.unlink()
        self.runtime.mkdir(parents=True)
        (self.runtime/'notes.txt').write_text('mine')
        self.assertEqual(self.run_installer(r, '--no-start').returncode, 13)
        self.assertEqual((self.runtime/'notes.txt').read_text(), 'mine')

    def test_usage_and_host_errors_before_any_change(self):
        r = self.release.make('1.0.0')
        for args, code in [(['--bogus'], 2), (['--version', '1.2'], 2),
                           (['--uninstall', '--dry-run'], 2)]:
            with self.subTest(args):
                self.assertEqual(self.run_installer(r, *args).returncode, code)
        self.assertEqual(self.run_installer(r, env={'GEIST_INSTALL_TEST_ARCH': 'riscv64'}).returncode, 10)
        cpu = self.case/'cpuinfo'
        cpu.write_text('flags\t: fpu sse2\nFeatures\t: fp asimd\n')
        self.assertEqual(self.run_installer(r, env={'GEIST_INSTALL_TEST_CPUINFO': str(cpu)}).returncode, 10)
        self.assertFalse(self.runtime.exists(), 'runtime directory created by a rejected run')

    def test_truncated_script_installs_nothing(self):
        text, release = SCRIPT.read_bytes(), self.release.make('1.0.0')
        for cut in (len(text)//3, len(text)//2, len(text)-40):
            with self.subTest(cut=cut):
                partial = self.case/'partial.sh'
                partial.write_bytes(text[:cut])
                self.run_installer(release, '--no-start', script=partial)
                self.assertNothingInstalled()

    def test_first_model_setup(self):
        r, log = self.release.make('1.0.0'), self.case/'calls'
        # Fresh, no terminal, no --model: never sets up a model, only says how.
        p = self.run_installer(r, env={'FAKE_LOG': str(log)})
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertNotIn('setup', log.read_text())
        self.assertIn('setup', p.stdout)
        # Explicit and unattended: geist setup --yes.
        log.unlink()
        self.assertEqual(self.run_installer(r, '--model', 'recommended', '--yes', env={'FAKE_LOG': str(log)}).returncode, 0)
        self.assertIn('setup --yes', log.read_text())
        # A failed model setup keeps the installation and says how to resume.
        p = self.run_installer(r, '--model', 'recommended', '--yes', env={'FAKE_SETUP_RC': '1'})
        self.assertEqual(p.returncode, 17, p.stderr)
        self.assertIn(f"Resume with: '{self.launcher}' setup", p.stderr)  # full path: not on PATH here
        self.assertEqual(self.receipt()['version'], '1.0.0')
        self.assertTrue(self.launcher.is_symlink())
        for args in (['--model', 'best'], ['--model'], ['--model', 'recommended', '--no-start'], ['--uninstall', '--model', 'recommended']):
            with self.subTest(args):
                self.assertEqual(self.run_installer(r, *args).returncode, 2)

    def test_desktop_route(self):
        log = self.case/'calls'
        base = {'GEIST_INSTALL_TEST_OS_RELEASE': str(self.t/'ubuntu'), 'PATH': f'{self.fakebin}:{os.environ["PATH"]}', 'FAKE_LOG': str(log)}
        calls = lambda: log.read_text() if log.exists() else ''
        r = self.release.make('1.0.0', debs=True)
        arch = 'amd64' if PLATFORM == 'linux-x86_64' else 'arm64'
        cases = [  # name, args, extra env, exit code
            ('not Ubuntu 24.04', [], {'GEIST_INSTALL_TEST_OS_RELEASE': str(self.t/'debian')}, 10),
            ('no terminal to confirm', [], {}, 2),
            ('--yes is no consent for sudo', ['--yes'], {}, 2),
            ('dry run', ['--dry-run'], {}, 0),
            ('with --uninstall', ['--uninstall'], {}, 2),
        ]
        for name, args, env, code in cases:
            with self.subTest(name):
                p = self.run_installer(r, '--desktop', *args, env=base | env)
                self.assertEqual(p.returncode, code, (name, p.stdout, p.stderr))
                self.assertEqual(calls(), '', f'{name}: apt or sudo was called')
        # Confirmed: exactly the verified matching pair goes to apt, through sudo; no rootless files.
        p = self.run_installer(r, '--desktop', env=base | {'GEIST_INSTALL_TEST_CONFIRM': 'yes'})
        self.assertEqual(p.returncode, 0, p.stderr)
        lines = calls().splitlines()
        self.assertEqual(len(lines), 2, lines)
        self.assertTrue(lines[0].startswith('sudo apt-get install -y ') and lines[1].startswith('apt-get install -y '), lines)
        self.assertIn(f'/geisten_1.0.0_{arch}.deb ', lines[1])
        self.assertTrue(lines[1].endswith('/geisten-desktop_1.0.0_all.deb'), lines)
        self.assertFalse(self.runtime.exists() or self.launcher.is_symlink(), 'the desktop route created rootless files')
        log.unlink()
        # apt fails: reported, not hidden.
        p = self.run_installer(r, '--desktop', env=base | {'GEIST_INSTALL_TEST_CONFIRM': 'yes', 'FAKE_APT_RC': '100'})
        self.assertEqual(p.returncode, 15, p.stderr)
        log.unlink()
        # A tampered package (same size) and a release without packages are refused before apt.
        desk = r/'geisten-desktop_1.0.0_all.deb'
        data = bytearray(desk.read_bytes()); data[0] ^= 1; desk.write_bytes(bytes(data))
        p = self.run_installer(r, '--desktop', env=base | {'GEIST_INSTALL_TEST_CONFIRM': 'yes'})
        self.assertEqual(p.returncode, 12, p.stderr)
        self.assertIn('checksum differs', p.stderr)
        p = self.run_installer(self.release.make('2.0.0'), '--desktop', env=base | {'GEIST_INSTALL_TEST_CONFIRM': 'yes'})
        self.assertEqual(p.returncode, 12, p.stderr)
        self.assertEqual(calls(), '')
        # An existing rootless installation is never crossed over automatically.
        r3 = self.release.make('3.0.0', debs=True)
        self.assertEqual(self.run_installer(r3, '--no-start').returncode, 0)
        p = self.run_installer(r3, '--desktop', env=base | {'GEIST_INSTALL_TEST_CONFIRM': 'yes'})
        self.assertEqual(p.returncode, 13, p.stderr)
        self.assertIn('--uninstall', p.stderr)
        self.assertEqual(calls(), '')

    def test_dry_run_then_uninstall_keeps_data(self):
        r = self.release.make('1.0.0')
        p = self.run_installer(r, '--dry-run')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('Dry run', p.stdout)
        self.assertNothingInstalled()
        self.assertEqual(self.run_installer(r, '--no-start').returncode, 0)
        data = self.home/'.local/share/geisten/models'
        data.mkdir(parents=True)
        (data/'model.gguf').write_text('weights')
        p = self.run_installer(r, '--uninstall')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertFalse(self.launcher.is_symlink() or self.alias.is_symlink() or self.runtime.exists())
        self.assertEqual((data/'model.gguf').read_text(), 'weights')


if __name__ == '__main__':
    unittest.main()
