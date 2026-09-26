#!/usr/bin/env python3
"""Fail closed unless the release directory has the complete expected payload."""
import hashlib
from pathlib import Path
import re
import sys


def assemble(directory: Path, version: str):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Expected X.Y.Z version')
    platforms = ('linux-x86_64', 'linux-aarch64', 'macos-arm64')
    binaries = [f'geist-serve-{p}{suffix}' for p in platforms for suffix in ('', '-geistd')]
    packages = [f'geist_{version}_{arch}.deb' for arch in ('amd64', 'arm64')]
    payload = sorted(binaries+packages+['geist-serve.socket','geist-serve.service','geist-serve.default'])
    expected = set(payload)
    actual = {p.name for p in directory.iterdir()}
    if actual != expected:
        raise ValueError(f'Release payload mismatch: missing={sorted(expected-actual)}, unexpected={sorted(actual-expected)}')
    lines = {}
    for name in payload:
        path = directory/name
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f'Invalid release asset: {name}')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        lines[name] = f'{digest}  {name}\n'
    (directory/'SHA256SUMS').write_text(''.join(lines.values()))
    for name in packages:
        (directory/(name+'.sha256')).write_text(lines[name])
    return payload


if __name__ == '__main__':
    try:
        print('\n'.join(assemble(Path(sys.argv[1]), sys.argv[2])))
    except (ValueError, OSError, IndexError) as error:
        sys.exit(str(error))
