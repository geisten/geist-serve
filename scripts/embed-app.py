#!/usr/bin/env python3
"""Build-time fallback for C23 compilers without #embed. Never shipped as runtime."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
out = ['/* Generated from web/ by scripts/embed-app.py. */']
for symbol, filename in [('page', 'index.html'), ('style', 'app.css'), ('script', 'app.js')]:
    data = (root / 'web' / filename).read_bytes()
    out.append(f'static const unsigned char {symbol}[] = {{')
    for offset in range(0, len(data), 24):
        out.append(','.join(str(byte) for byte in data[offset:offset + 24]) + ',')
    out.append('};')
target = root / 'build/app_assets.h'
target.parent.mkdir(exist_ok=True)
target.write_text('\n'.join(out) + '\n')
