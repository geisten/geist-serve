#!/usr/bin/env python3
"""Derive a lifecycle comparison without mutating either raw evidence directory."""
import argparse, hashlib, json, statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('baseline',type=Path);p.add_argument('candidate',type=Path);a=p.parse_args()
def load(root):
    assert json.loads((root/'complete.json').read_text())['passed']
    rows=json.loads((root/'results.json').read_text());groups={}
    for row in rows:
        if row['transition'].startswith('cleanup:'):continue
        groups.setdefault(row['transition'].split(':',1)[1],[]).append(row)
    traces=[json.loads(line) for line in (root/'samples.jsonl').read_text().splitlines()]
    return rows,groups,traces
base,bg,bt=load(a.baseline);candidate,cg,ct=load(a.candidate)
builds=[json.loads((root/'build.json').read_text()) for root in (a.baseline,a.candidate)]
reps=builds[1].get('repetitions',5)
print('# Model lifecycle comparison\n')
print(f'Revisions {builds[0].get("head","?")[:7]} → {builds[1].get("head","?")[:7]}. {reps} fixed-input trials per transition. '
      'Uncontrolled filesystem cache; no cold-disk claim. One owned model at a time. '
      'Raw observations are retained; this is a derived report.\n')
print('| Transition | Ready baseline → candidate (s) | First visible baseline → candidate (s) | Guard |')
print('|---|---:|---:|---|')
flags=[]
for key,old in bg.items():
    new=cg[key];assert len(old)==len(new)==reps
    pairs=[];issues=[]
    for field in ('ready_s','first_answer_from_action_s'):
        before=statistics.median(r[field] for r in old);after=statistics.median(r[field] for r in new)
        pairs.append(f'{before:.3f} → {after:.3f}')
        if after-before>max(.1,before*.1):issues.append(field)
    if issues:flags.append({'transition':key,'regression':issues})
    print('| '+key+' | '+' | '.join(pairs)+' | '+('REVIEW: '+', '.join(issues) if issues else 'within guard')+' |')
print('\n## Initialization attribution\n')
for label,groups in [('baseline',bg),('candidate',cg)]:
    print('### '+label+'\n')
    print('| Transition with replacement | Backend / model / metadata / warmup (median ms) |')
    print('|---|---:|')
    for key,rows in groups.items():
        changed=[r for r in rows if not r['same_pid']]
        if not changed:continue
        values=[]
        for phase in ('backend','model','metadata','warmup'):
            values.append(statistics.median(next(p['duration_ms'] for p in r['lifecycle']['engine_phases'] if p['stage']==phase) for r in changed))
        print('| '+key+' | '+' / '.join(f'{v:.1f}' for v in values)+' |')
    print()
print('## Cleanup and raw ranges\n')
for label,rows,traces in [('baseline',base,bt),('candidate',candidate,ct)]:
    parents=[t['supervisor_rss_bytes'] for t in traces];available=[t['available_bytes'] for t in traces]
    drift=parents[-1]-parents[0];availdrift=available[-1]-available[0]
    parentguard=max(64*2**20,parents[0]*.2);systemguard=max(2*2**30,64*2**30*.05)
    print(f'- {label}: {len(rows)} completed transitions; {sum(not r["same_pid"] for r in rows)} replacements. '
          f'Parent RSS {parents[0]/2**20:.2f} → {parents[-1]/2**20:.2f} MiB (range {min(parents)/2**20:.2f}–{max(parents)/2**20:.2f}); '
          f'available RAM {available[0]/2**30:.2f} → {available[-1]/2**30:.2f} GiB. '
          f'Parent guard {"REVIEW" if drift>parentguard else "passed"}; system-context guard {"REVIEW" if abs(availdrift)>systemguard else "within budget"}.')
    if drift>parentguard or abs(availdrift)>systemguard:flags.append({'run':label,'memory_review':True})
    for key in bg:
        times=[r['ready_s'] for r in rows if r['transition'].split(':',1)[-1]==key]
        print(f'  - {key}: ready range {min(times):.3f}–{max(times):.3f} s.')
print('\nEvery replacement asserted old PID exit/reap before new spawn; final stop asserted exit. '
      'Exact restoration of free RAM is not a release oracle. Ten small-model and three Bonsai CPU/Metal cleanup cycles are included. '
      'These short series cannot exclude all leaks or support tail-percentile claims.\n')
print('## Source integrity\n')
for root in (a.baseline,a.candidate):
    for name in ('build.json','results.json','samples.jsonl','complete.json'):
        file=root/name
        with file.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
        print(f'- `{root.name}/{name}` SHA-256 `{digest}`')
print('\nGuard findings: `'+json.dumps(flags)+'`. Any findings require written review; no failed trial is discarded.')
