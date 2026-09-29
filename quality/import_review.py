#!/usr/bin/env python3
"""Create a new reviewed bundle without replacing original run/review artifacts."""
import argparse
import json
from pathlib import Path
import shutil
from evidence import load_bundle, sha

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--run', required=True, type=Path)
p.add_argument('--reviews', required=True, type=Path)
p.add_argument('--reviewer', required=True)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
if not a.reviewer.strip(): raise ValueError('Human reviewer attribution required')
a.output.mkdir(parents=True, exist_ok=False)
for name in ('run.json', 'corpus.json', 'adverse.json', 'responses.jsonl'):
    shutil.copyfile(a.run/name, a.output/name)
shutil.copyfile(a.reviews, a.output/'reviews.json')
(a.output/'manifest.json').write_text(json.dumps({'human_reviewer':a.reviewer,
    'sha256':{n:sha(a.output/n) for n in ('run.json','reviews.json','corpus.json','adverse.json','responses.jsonl')}},indent=2)+'\n')
run, cells = load_bundle(a.output)
(a.output/'summary.json').write_text(json.dumps(cells,indent=2)+'\n')
print(json.dumps(cells,indent=2))
