#!/usr/bin/env python3
"""#83: every server message that can reach the German UI has a German text.

Collects the user-facing literals from src/app/*.c (notices in app.message,
/app error responses, failure reasons and fit reasons), fills printf
placeholders with sample values, and asks web/i18n.js (in node, with minimal
browser stubs) to translate each one. A missing entry or pattern fails.
"""
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
# Calls whose string literals are shown in the app UI.
CALLS = [r'snprintf\(\s*app\.message\s*,', r'error_response\(\s*fd\s*,\s*\d+\s*,', r'snprintf\(\s*why\s*,',
         r'\berror\s*=', r'\.reason\s*=', r'snprintf\(\s*app\.backend\.notice\s*,']
# Only for OpenAI-compatible clients (/v1), never shown in the app UI: English by design.
API_ONLY_FILES = {'compat.c'}
API_ONLY = {
    'Requested model is not loaded. Refresh /v1/models after changing models.',
}
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
PLACEHOLDER = re.compile(r'%(?:\.\d+)?(?:l{0,2}[duf]|s|zu|llu)')


def literals(span):
    """String literals in a C span; adjacent ones ("a" "b") are one string."""
    groups, last = [], None
    for m in LITERAL.finditer(span):
        if last is not None and not span[last:m.start()].strip():
            groups[-1] += m.group(1)
        else:
            groups.append(m.group(1))
        last = m.end()
    return groups


def messages():
    found = {}
    for path in sorted((ROOT/'src/app').glob('*.c')):
        if path.name in API_ONLY_FILES:
            continue
        source = path.read_text()
        for call in CALLS:
            for match in re.finditer(call, source):
                end = source.find(';', match.end())
                for text in literals(source[match.end():end]):
                    text = bytes(text, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')
                    sample = PLACEHOLDER.sub('42', text).replace('%%', '%')
                    if len(sample) > 12 and ' ' in sample and sample[0].isupper():
                        found.setdefault(sample, path.name)
    return found


STUB = r'''
const window = {geistLanguage: 'de'}; const navigator = {language: 'de'};
const localStorage = {getItem() { return 'de'; }, setItem() {}};
const NodeFilter = {SHOW_TEXT: 4};
const document = {body: {}, documentElement: {}, createTreeWalker() { return {nextNode() { return false; }}; },
                  querySelectorAll() { return []; }, addEventListener() {}, getElementById() { return null; }};
'''


def untranslated(texts):
    script = STUB + (ROOT/'web/i18n.js').read_text() + f'''
const texts = {json.dumps(texts)};
console.log(JSON.stringify(texts.filter(x => t(x) === x)));'''
    out = subprocess.run(['node', '-e', script], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


if __name__ == '__main__':
    found = messages()
    assert len(found) > 40, f'extraction broke: only {len(found)} messages found'
    missing = [m for m in untranslated(sorted(set(found) - API_ONLY))]
    assert not missing, 'German text missing for server messages:\n' + '\n'.join(f'  {found[m]}: {m}' for m in missing)
    print(f'i18n: all {len(found) - len(API_ONLY & set(found))} UI-facing server messages have German text')
    # #92: the product is "geisten"; the old name stays only in deliberate migration notes.
    root = Path(__file__).resolve().parents[2]
    old = [f'{p.relative_to(root)}:{n}' for p in [*root.glob('web/*.js'), *root.glob('web/*.html'), root/'docs/INSTALL.md', root/'README.md']
           if p.exists() for n, line in enumerate(p.read_text().splitlines(), 1) if re.search(r'\bGeist(?![-_\w])', line)]
    # The visible wordmark too: lowercase, so the check above cannot see it.
    assert re.search(r'class="brand"[^>]*>geisten<', (root/'web/index.html').read_text()), 'the header wordmark must read geisten'
    assert not old, 'the old product name is still visible:\n  ' + '\n  '.join(old)
