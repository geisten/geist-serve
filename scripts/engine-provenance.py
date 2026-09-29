#!/usr/bin/env python3
"""Bind build metadata to the actual source inputs and linked static archive.

Source identity partitions build directories, preventing stale incremental objects
from a different revision. No source paths enter the published metadata.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path


def source(root):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL)
    try:
        revision = git('rev-parse', 'HEAD').decode().strip()
        tracked = git('ls-files', '-z').split(b'\0')
        others = git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0')
        dirty = bool(git('status', '--porcelain', '--untracked-files=normal'))
        paths = sorted(set(tracked + others))
        digest = hashlib.sha256(revision.encode())
        for name in paths:
            if not name:
                continue
            p = root / os.fsdecode(name)
            digest.update(name + b'\0')
            if p.is_symlink():
                digest.update(b'link:' + str(p.readlink()).encode())
            elif p.is_file():
                with p.open('rb') as f:
                    digest.update(hashlib.file_digest(f, 'sha256').digest())
            else:
                digest.update(b'missing')
        return dict(revision=revision, source_state='modified' if dirty else 'clean',
                    source_sha256=digest.hexdigest())
    except subprocess.CalledProcessError:
        # No unverifiable source-archive revision is promoted to clean.
        return dict(revision=None, source_state='unknown', source_sha256=None)


def write_changed(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != value:
        tmp = path.with_name(path.name + '.tmp')
        tmp.write_text(value)
        tmp.replace(path)


def capture(root, archive, output, expected):
    identity = source(root)
    if (identity['source_sha256'] or 'unknown') != expected:
        raise ValueError('Engine source changed during build; rebuild from a stable checkout')
    with archive.open('rb') as f:
        identity['archive_sha256'] = hashlib.file_digest(f, 'sha256').hexdigest()
    text = (root / 'include/geist.h').read_text()
    identity['header_version'] = re.search(r'#define GEIST_VERSION_STRING "([^"]+)"', text)[1]
    write_changed(output.with_suffix('.json'), json.dumps(identity, indent=2) + '\n')
    macros = {'GEIST_SOURCE_REVISION': identity['revision'] or '',
              'GEIST_SOURCE_STATE': identity['source_state'],
              'GEIST_ARCHIVE_SHA256': identity['archive_sha256'],
              'GEIST_HEADER_VERSION': identity['header_version']}
    write_changed(output, '#pragma once\n' + ''.join(
        f'#define {key} {json.dumps(value)}\n' for key, value in macros.items()))


def package(binary, output, require_clean=False):
    result = subprocess.run([str(binary.resolve()), '--build-info'], check=True,
                            capture_output=True, timeout=10)
    if len(result.stdout) > 8192:
        raise ValueError('Oversized runtime build metadata')
    identity = json.loads(result.stdout)
    lib = identity['geistlib']
    if not isinstance(lib['version'], str) or not re.fullmatch(r'[A-Za-z0-9.+_-]{1,63}', lib['version']):
        raise ValueError('Invalid library version')
    if not re.fullmatch('[0-9a-f]{40}|[0-9a-f]{64}', lib.get('revision') or ''):
        raise ValueError('Missing resolved library revision')
    if lib['source_state'] not in ['clean', 'modified', 'unknown']:
        raise ValueError('Invalid source state')
    if require_clean and lib['source_state'] != 'clean':
        raise ValueError('Release requires clean library source')
    if not re.fullmatch('[0-9a-f]{64}', identity.get('archive_sha256') or ''):
        raise ValueError('Missing archive identity')
    write_changed(output, json.dumps(identity, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['identity', 'capture', 'package'])
    parser.add_argument('root', type=Path)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--expected')
    parser.add_argument('--require-clean', action='store_true')
    args = parser.parse_args()
    if args.command == 'identity':
        print(source(args.root)['source_sha256'] or 'unknown')
    elif args.command == 'capture':
        capture(args.root, args.archive, args.output, args.expected)
    else:
        package(args.root, args.output, args.require_clean)
