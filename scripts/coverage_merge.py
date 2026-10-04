#!/usr/bin/env python3
"""coverage_merge.py OUT.info IN.info...: union of per-binary lcov line counts.

llvm-cov does not merge a source file compiled differently into several
binaries (geist-app, the -DAPP_TESTING test build, version-pinned upgrade
builds); it reports one of them. A line counts as covered here when any
binary executed it. Prints the per-file table and the total line percentage."""
import collections
import sys


def merge(paths):
    lines = collections.defaultdict(dict)  # file -> line -> hits
    for path in paths:
        current = None
        for row in open(path, encoding='utf-8'):
            if row.startswith('SF:'):
                current = row[3:].strip()
            elif row.startswith('DA:') and current:
                number, hits = row[3:].split(',')[:2]
                seen = lines[current]
                seen[int(number)] = seen.get(int(number), 0) + int(hits)
    return lines


def main(out, inputs):
    lines = merge(inputs)
    with open(out, 'w', encoding='utf-8') as f:
        for name in sorted(lines):
            f.write(f'SF:{name}\n' + ''.join(f'DA:{n},{h}\n' for n, h in sorted(lines[name].items())) + 'end_of_record\n')
    rows, covered, total = [], 0, 0
    for name, seen in sorted(lines.items()):
        hit = sum(1 for h in seen.values() if h)
        rows.append((hit / len(seen) * 100 if seen else 100, name, hit, len(seen)))
        covered, total = covered + hit, total + len(seen)
    for percent, name, hit, count in sorted(rows):
        print(f'{percent:6.1f}%  {count - hit:5} missed  {name}')
    percent = covered / total * 100 if total else 100
    print(f'{percent:6.1f}%  {total - covered:5} missed  TOTAL')
    return percent


if __name__ == '__main__':
    print(f'coverage: {main(sys.argv[1], sys.argv[2:]):.2f}% of lines')
