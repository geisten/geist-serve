#!/usr/bin/env python3
"""Derive a report without changing raw memory evidence."""
import hashlib,json,statistics,sys
from pathlib import Path
root=Path(sys.argv[1]);rows=json.loads((root/'results.json').read_text());complete=json.loads((root/'complete.json').read_text())
assert complete['passed']
trace=[json.loads(line) for line in (root/'samples.jsonl').read_text().splitlines()]
build=json.loads((root/'build.json').read_text())
print('# Scoped memory acceptance — Apple M1 Max, 64 GiB, 2026-09-29\n')
print('Source: `review/observability-implementation-20260929/40-real-1`. Frozen binaries and raw numeric observations are retained locally. No prompts/replies are in this report.\n')
print('Engine: `'+build['build']['geistlib']['revision']+'`. Process source: `macos.proc_pid_rusage.ri_resident_size`. Metal source: the owning backend’s `MTLDevice.currentAllocatedSize`.\n')
print('## Real Bonsai PQ2_0 switch\n\n| Execution | Process RSS after short response | Metal allocated |\n| --- | ---: | ---: |')
for r in rows:
 if r['model']=='bonsai2-27b-pq2':
  m=r['memory'];gpu='Unsupported' if m['gpu_allocated_bytes'] is None else f"{m['gpu_allocated_bytes']/2**30:.3f} GiB"
  print(f"| {r['mode']} | {m['process_rss_bytes']/2**30:.3f} GiB | {gpu} |")
print('\nThese scopes overlap and must not be added. This reproduces the RSS/allocation discrepancy, not the owner’s exact 0.3-GiB sample. A synthetic 0.3-GiB RSS plus 10-GiB allocation is separately covered by serialization and native UI tests.\n')
print('## Long prefill and lifetime\n')
for mode in ['cpu','gpu']:
 samples=[r for r in trace if 'bonsai2' in r['label'] and f':{mode}:' in r['label'] and r['activity']['request']['stage']=='prefill' and not r['activity']['request']['outcome']]
 print(f"- {mode.upper()}: {len(samples)} status observations during prefill; maximum backend-sample age {max(r['memory']['gpu_sample_age_ms'] or 0 for r in samples)/1000:.3f} seconds. CPU reports unsupported GPU allocation while continuing RSS sampling.")
print(f"\nThe 1,223-token prompts completed on CPU and Metal. All {len(complete['owned_pids'])} recorded owned child PIDs were gone after stop/restart trials; old generation values were rejected. Collection and request history worked without a WebView. Exact restoration of system-free RAM is not asserted.\n")
print('## Alternating sampler on/off observations\n\nSame cached SmolLM2 artifact, deterministic prompt, 29 generated tokens, fresh CPU/Metal transitions; cache and OS background load were uncontrolled. All CPU trials include both initial and return-to-CPU requests.\n\n| Backend | Sampling | n | Total response median | Observed range |\n| --- | --- | ---: | ---: | ---: |')
for mode in ['cpu','gpu']:
 med={}
 for enabled in [False,True]:
  values=[r['stats']['total_duration']/1e6 for r in rows if r['model']=='smollm2-360m' and r['mode']==mode and r['enabled']==enabled]
  med[enabled]=statistics.median(values)
  print(f"| {mode.upper()} | {'on' if enabled else 'off'} | {len(values)} | {med[enabled]:.3f} ms | {min(values):.3f}–{max(values):.3f} ms |")
 print(f"\nObserved {mode.upper()} median difference: {(med[True]/med[False]-1)*100:+.2f}%.\n")
print('These short trials do not isolate a causal overhead or establish negligible overhead. The implementation adds no GPU flush, inference-mutex wait or per-token persistence; longer repeated trials would be needed for a narrow overhead bound.\n')
print('## Scope and evidence hashes\n\nReal app/API/controlled-comparison and restart checks are separate from this switch matrix. Golden fixtures cover missing/zero/stale/error/legacy, cancellation and journal export. Native CI covers Ubuntu CPU and macOS UI; Linux GPU allocation remains unsupported.\n')
for name in ['build.json','results.json','samples.jsonl','records.jsonl','complete.json']:
 p=root/name;print(f'- `{name}`: `{hashlib.sha256(p.read_bytes()).hexdigest()}`')
