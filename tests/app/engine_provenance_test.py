#!/usr/bin/env python3
"""Source/link/package provenance: real Git fixtures, no remote access."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('provenance', ROOT/'scripts/engine-provenance.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class Provenance(unittest.TestCase):
    def test_source_capture_and_packages(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)/'engine'; root.mkdir()
            def git(*args):
                return subprocess.check_output(['git','-C',str(root),*args], stderr=subprocess.DEVNULL)
            git('init'); git('config','user.email','test@example.invalid'); git('config','user.name','Fixture')
            (root/'include').mkdir()
            (root/'include/geist.h').write_text('#define GEIST_VERSION_STRING "0.11.0"\n')
            git('add','.'); git('commit','-qm','first')
            first=module.source(root)
            self.assertEqual(first['source_state'],'clean')
            git('commit','--allow-empty','-qm','same version, new revision')
            second=module.source(root)
            self.assertNotEqual(first['source_sha256'],second['source_sha256'])
            archive=Path(tmp)/'libgeist.a';archive.write_bytes(b'actual linked archive fixture')
            output=Path(tmp)/'build.h'
            with self.assertRaises(ValueError): module.capture(root,archive,output,first['source_sha256'])
            module.capture(root,archive,output,second['source_sha256'])
            self.assertEqual(json.loads(output.with_suffix('.json').read_text())['revision'],second['revision'])
            (root/'include/geist.h').write_text('#define GEIST_VERSION_STRING "0.11.0"\n/* changed */')
            self.assertEqual(module.source(root)['source_state'],'modified')
            self.assertNotEqual(module.source(root)['source_sha256'],second['source_sha256'])
            bare=Path(tmp)/'source-archive';bare.mkdir();(bare/'include').mkdir()
            (bare/'include/geist.h').write_text('#define GEIST_VERSION_STRING "0.11.0"\n')
            module.capture(bare,archive,output,'unknown')
            self.assertIsNone(json.loads(output.with_suffix('.json').read_text())['revision'])
            binary=Path(tmp)/'daemon'
            identity={'geistlib':{'version':'0.11.0','revision':second['revision'],'source_state':'clean'},'archive_sha256':'a'*64}
            def emit():
                binary.write_text('#!/usr/bin/env python3\nprint('+repr(json.dumps(identity))+')\n');binary.chmod(0o700)
            emit(); module.package(binary,Path(tmp)/'ENGINE.json',True)
            self.assertEqual(json.loads((Path(tmp)/'ENGINE.json').read_text()),identity)
            identity['geistlib']['source_state']='modified';emit()
            with self.assertRaises(ValueError):module.package(binary,Path(tmp)/'ENGINE.json',True)
            identity['geistlib']['version']='<script>';emit()
            with self.assertRaises(ValueError):module.package(binary,Path(tmp)/'ENGINE.json')

if __name__=='__main__': unittest.main()
