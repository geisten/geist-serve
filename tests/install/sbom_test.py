#!/usr/bin/env python3
"""The release SBOM names what actually ships: the engine pin, vendored files."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('sbom', ROOT/'scripts/sbom.py')
sbom = importlib.util.module_from_spec(spec); spec.loader.exec_module(sbom)


class SbomTests(unittest.TestCase):
    def setUp(self):
        self.bom = json.loads(json.dumps(sbom.build('1.2.3')))
        self.by_name = {c['name']: c for c in self.bom['components']}

    def test_cyclonedx_shape_and_release_version(self):
        self.assertEqual((self.bom['bomFormat'], self.bom['specVersion']), ('CycloneDX', '1.5'))
        self.assertEqual(self.bom['metadata']['component']['version'], '1.2.3')

    def test_engine_is_the_makefile_pin(self):
        pin = re.search(r'^GEIST_REF\s*\?=\s*(\S+)', (ROOT/'Makefile').read_text(), re.M).group(1)
        self.assertEqual(self.by_name['geistlib']['version'], pin)

    def test_vendored_files_are_hashed_as_shipped(self):
        for name, files in [('marked', ['marked.umd.js']), ('katex', ['katex.min.js']), ('jsmn', None)]:
            digests = {h['content'] for h in self.by_name[name]['hashes']}
            paths = [ROOT/'src/jsmn.h'] if files is None else [ROOT/'web/vendor'/f for f in files]
            for p in paths:
                self.assertIn(hashlib.sha256(p.read_bytes()).hexdigest(), digests, p)

    def test_web_versions_come_from_the_vendor_manifests(self):
        for manifest in (ROOT/'web/vendor').glob('*manifest.json'):
            m = json.loads(manifest.read_text())
            self.assertEqual(self.by_name[m['name']]['version'], m['version'])

    def test_deterministic_and_rejects_bad_version(self):
        self.assertEqual(sbom.build('1.2.3'), sbom.build('1.2.3'))
        with self.assertRaises(ValueError):
            sbom.build('v1.2')


if __name__ == '__main__':
    unittest.main()
