#!/usr/bin/env python3
"""sbom.py OUT VERSION: CycloneDX 1.5 SBOM for a geist-serve release (#34).

Lists what the release assets are built from, read from the repository
itself rather than a hand-kept list: the geist-serve source at VERSION, the
engine pinned by GEIST_REF in the Makefile, the vendored web libraries
(web/vendor/*manifest.json) and the vendored jsmn header. Hashes are of the
vendored files as shipped. No timestamp, so the same tree gives the same file.
"""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def engine_pin():
    text = (ROOT/'Makefile').read_text()
    ref = re.search(r'^GEIST_REF\s*\?=\s*([0-9a-f]{40})\s*$', text, re.M)
    repo = re.search(r'^GEIST_REPO\s*\?=\s*(\S+)\s*$', text, re.M)
    if not ref or not repo:
        raise ValueError('Makefile has no GEIST_REF/GEIST_REPO pin')
    return repo.group(1), ref.group(1)


def vendored_web():
    components = []
    for manifest in sorted((ROOT/'web/vendor').glob('*manifest.json')):
        m = json.loads(manifest.read_text())
        components.append({
            'type': 'library', 'name': m['name'], 'version': m['version'],
            'purl': f"pkg:npm/{m['name']}@{m['version']}",
            'licenses': [{'license': {'id': 'MIT'}}],
            'hashes': [{'alg': 'SHA-256', 'content': sha256(ROOT/'web/vendor'/f)} for f in sorted(m['files'])],
            'externalReferences': [{'type': 'distribution', 'url': m['tarball']},
                                   {'type': 'vcs', 'url': m['upstream']}],
        })
    return components


def build(version):
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Expected X.Y.Z version')
    repo, ref = engine_pin()
    return {
        'bomFormat': 'CycloneDX', 'specVersion': '1.5', 'version': 1,
        'metadata': {'component': {
            'type': 'application', 'name': 'geist-serve', 'version': version,
            'purl': f'pkg:github/geisten/geist-serve@v{version}',
            'licenses': [{'license': {'id': 'Apache-2.0'}}]}},
        'components': [
            {'type': 'library', 'name': 'geistlib', 'version': ref,
             'purl': f'pkg:github/geisten/geistlib@{ref}',
             'description': 'inference engine, statically linked into geist-serve and geistd',
             'externalReferences': [{'type': 'vcs', 'url': repo}]},
            {'type': 'library', 'name': 'jsmn', 'version': 'unversioned (vendored header)',
             'purl': 'pkg:github/zserge/jsmn',
             'licenses': [{'license': {'id': 'MIT'}}],
             'hashes': [{'alg': 'SHA-256', 'content': sha256(ROOT/'src/jsmn.h')}],
             'externalReferences': [{'type': 'vcs', 'url': 'https://github.com/zserge/jsmn'}]},
            *vendored_web(),
        ],
    }


if __name__ == '__main__':
    try:
        out, version = Path(sys.argv[1]), sys.argv[2]
        out.write_text(json.dumps(build(version), indent=2) + '\n')
    except (ValueError, OSError, IndexError, KeyError) as error:
        sys.exit(f'sbom: {error}')
