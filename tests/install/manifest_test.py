#!/usr/bin/env python3
"""Release upload contract: all platform pairs, DEBs and service files."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('manifest', ROOT/'scripts/release-manifest.py')
manifest=importlib.util.module_from_spec(spec); spec.loader.exec_module(manifest)

class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.assets=[f'geist-serve-{p}{s}' for p in ['linux-x86_64','linux-aarch64','macos-arm64'] for s in ['', '-geistd']]+['geist-serve.socket','geist-serve.service','geist-serve.default','geist_1.2.3_amd64.deb','geist_1.2.3_arm64.deb']
        for name in self.assets: (self.root/name).write_text(name)

    def test_all_payloads_and_package_sidecars_verified(self):
        self.assertEqual(set(manifest.assemble(self.root,'1.2.3')),set(self.assets))
        entries=(self.root/'SHA256SUMS').read_text().splitlines()
        self.assertEqual(len(entries),11)
        for line in entries:
            digest,name=line.split()
            self.assertEqual(digest,hashlib.sha256((self.root/name).read_bytes()).hexdigest())
        for arch in ['amd64','arm64']:
            self.assertIn((self.root/f'geist_1.2.3_{arch}.deb.sha256').read_text().strip(),entries)

    def test_missing_platform_package_fails_without_manifest(self):
        (self.root/'geist_1.2.3_arm64.deb').unlink()
        with self.assertRaises(ValueError): manifest.assemble(self.root,'1.2.3')
        self.assertFalse((self.root/'SHA256SUMS').exists())

    def test_unexpected_upload_is_rejected(self):
        (self.root/'private.txt').write_text('not for upload')
        with self.assertRaises(ValueError): manifest.assemble(self.root,'1.2.3')

    def test_wrong_package_version_is_rejected(self):
        with self.assertRaises(ValueError): manifest.assemble(self.root,'1.2.4')

    def test_symlink_asset_is_rejected(self):
        p=self.root/'geist-serve.socket'; p.unlink(); p.symlink_to('geist-serve.service')
        with self.assertRaises(ValueError): manifest.assemble(self.root,'1.2.3')

if __name__=='__main__': unittest.main()
