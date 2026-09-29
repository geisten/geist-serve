#!/usr/bin/env python3
"""Re-signing the same engine must not invalidate a local performance series."""
import hashlib,os
from pathlib import Path
import shutil,subprocess,sys,tempfile
from http_test import ROOT
probe=ROOT/'build/test_app_engine_identity'
with tempfile.TemporaryDirectory(prefix='geist-engine-identity-') as temporary:
    root=Path(temporary);plain=root/'plain';plain.write_bytes(b'engine fixture')
    assert subprocess.check_output([str(probe),str(plain)],text=True).strip()==hashlib.sha256(plain.read_bytes()).hexdigest()
    if sys.platform=='darwin':
        source=Path(os.environ.get('GEIST_EXECUTION_DAEMON',(ROOT/'build/geistd-execution') if (ROOT/'build/geistd-execution').exists() else ROOT/'geistd'))
        target=root/'signed';shutil.copyfile(source,target);target.chmod(0o700)
        expected=subprocess.check_output([str(probe),str(source)],text=True).strip()
        subprocess.run(['codesign','--force','--sign','-','--identifier','org.geisten.profile-repackaging-test',str(target)],check=True,capture_output=True)
        assert hashlib.sha256(target.read_bytes()).digest()!=hashlib.sha256(source.read_bytes()).digest()
        assert subprocess.check_output([str(probe),str(target)],text=True).strip()==expected
        data=bytearray(target.read_bytes());data[16384]^=1;target.write_bytes(data)
        assert subprocess.check_output([str(probe),str(target)],text=True).strip()!=expected
        print('engine identity: signing envelope ignored; changed executable payload detected')
    else:print('engine identity: ordinary executable SHA-256 verified; Mach-O case only on macOS')
