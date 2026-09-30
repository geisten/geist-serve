#!/usr/bin/env python3
"""installer-manifest.py DIST VERSION SOURCE ENGINE [PLATFORM...]: write DIST/geist-manifest.

The strict line format that scripts/install-geist.sh parses without eval
(docs/INSTALL-LINUX.md). One `archive` line per Linux platform, naming the
portable archive with its byte size and SHA-256 (a release lists both
platforms; one-platform manifests are for CI acceptance). Signing is separate
(scripts/sign-manifest.sh), so the release job signs exactly these bytes.
"""
import hashlib
from pathlib import Path
import re
import sys

# What each archive's engine needs from the CPU; the installer checks the flags.
PLATFORMS = {'linux-x86_64': 'x86-64-v3', 'linux-aarch64': 'armv8.2-a+dotprod+fp16'}


def build(dist: Path, version: str, source: str, engine: str, platforms=tuple(PLATFORMS)) -> str:
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Expected X.Y.Z version')
    if not re.fullmatch(r'[0-9a-f]{40}', source):
        raise ValueError('source must be a 40-hex commit')
    if not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}|unknown', engine):
        raise ValueError('engine must be a commit, a digest or "unknown"')
    lines = ['geist-manifest 1', 'product geist', f'version {version}', 'channel stable',
             f'source {source}', f'engine {engine}']
    if not platforms or set(platforms) - set(PLATFORMS):
        raise ValueError(f'platforms must be among {sorted(PLATFORMS)}')
    for platform in platforms:
        cpu = PLATFORMS[platform]
        name = f'geist-{version}-{platform}.tar.gz'
        path = dist/name
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f'missing archive {name}')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        lines.append(f'archive {platform} {cpu} {name} {path.stat().st_size} {digest}')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    try:
        dist = Path(sys.argv[1])
        platforms = tuple(sys.argv[5:]) or tuple(PLATFORMS)
        (dist/'geist-manifest').write_text(build(dist, *sys.argv[2:5], platforms), newline='\n')
    except (ValueError, OSError, TypeError) as error:
        sys.exit(f'installer-manifest: {error}')
