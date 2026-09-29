# Model lifecycle comparison

Apple M1 Max, 64 GiB; geistlib 0.11.0/e264369. Five fixed-input trials per transition. Uncontrolled filesystem cache; no cold-disk claim. One owned model at a time. Raw observations are retained; this is a derived report.

| Transition | Ready baseline → candidate (s) | First visible baseline → candidate (s) | Guard |
|---|---:|---:|---|
| smollm2-360m:select | 0.483 → 0.465 | 0.576 → 0.550 | within guard |
| smollm2-360m:cpu | 0.006 → 0.005 | 0.094 → 0.087 | within guard |
| smollm2-360m:active | 0.120 → 0.005 | 0.211 → 0.086 | within guard |
| smollm2-360m:gpu | 0.405 → 0.360 | 0.514 → 0.447 | within guard |
| smollm2-360m:return-cpu | 0.152 → 0.162 | 0.243 → 0.251 | within guard |
| smollm2-360m:auto | 0.006 → 0.005 | 0.096 → 0.088 | within guard |
| bonsai2-27b-pq2:select | 1.468 → 1.363 | 15.319 → 14.429 | within guard |
| bonsai2-27b-pq2:cpu | 0.006 → 0.005 | 13.734 → 13.153 | within guard |
| bonsai2-27b-pq2:active | 1.652 → 0.007 | 15.539 → 13.121 | within guard |
| bonsai2-27b-pq2:gpu | 35.704 → 21.654 | 38.185 → 24.076 | within guard |
| bonsai2-27b-pq2:auto | 0.006 → 0.009 | 2.476 → 2.430 | within guard |
| bonsai2-27b-pq2:return-cpu | 1.483 → 1.342 | 15.125 → 14.389 | within guard |

## Initialization attribution

### baseline

| Transition with replacement | Backend / model / metadata / warmup (median ms) |
|---|---:|
| smollm2-360m:select | 0.0 / 10.5 / 5.6 / 320.8 |
| smollm2-360m:active | 0.0 / 11.0 / 5.6 / 49.2 |
| smollm2-360m:gpu | 37.7 / 221.3 / 5.5 / 100.4 |
| smollm2-360m:return-cpu | 0.0 / 11.1 / 5.7 / 48.8 |
| bonsai2-27b-pq2:select | 0.0 / 1143.8 / 27.9 / 245.4 |
| bonsai2-27b-pq2:active | 0.0 / 1162.2 / 28.3 / 254.8 |
| bonsai2-27b-pq2:gpu | 37.7 / 34035.7 / 27.6 / 1141.8 |
| bonsai2-27b-pq2:return-cpu | 0.0 / 1101.8 / 27.9 / 250.5 |

### candidate

| Transition with replacement | Backend / model / metadata / warmup (median ms) |
|---|---:|
| smollm2-360m:select | 0.0 / 9.9 / 5.5 / 321.4 |
| smollm2-360m:gpu | 33.5 / 179.2 / 5.3 / 90.1 |
| smollm2-360m:return-cpu | 0.0 / 14.5 / 6.8 / 47.8 |
| bonsai2-27b-pq2:select | 0.0 / 994.0 / 27.4 / 229.4 |
| bonsai2-27b-pq2:gpu | 32.2 / 20256.8 / 27.2 / 885.3 |
| bonsai2-27b-pq2:return-cpu | 0.0 / 960.0 / 27.2 / 225.0 |

## Cleanup and raw ranges

- baseline: 88 completed transitions; 66 replacements. Parent RSS 8.06 → 7.66 MiB (range 6.39–9.25); available RAM 19.27 → 15.16 GiB. Parent guard passed; system-context guard REVIEW.
  - smollm2-360m:select: ready range 0.344–0.532 s.
  - smollm2-360m:cpu: ready range 0.005–0.007 s.
  - smollm2-360m:active: ready range 0.117–0.123 s.
  - smollm2-360m:gpu: ready range 0.371–0.429 s.
  - smollm2-360m:return-cpu: ready range 0.147–0.157 s.
  - smollm2-360m:auto: ready range 0.006–0.007 s.
  - bonsai2-27b-pq2:select: ready range 1.426–7.655 s.
  - bonsai2-27b-pq2:cpu: ready range 0.005–0.007 s.
  - bonsai2-27b-pq2:active: ready range 1.574–1.772 s.
  - bonsai2-27b-pq2:gpu: ready range 31.035–36.710 s.
  - bonsai2-27b-pq2:auto: ready range 0.006–0.008 s.
  - bonsai2-27b-pq2:return-cpu: ready range 1.420–1.717 s.
- candidate: 88 completed transitions; 56 replacements. Parent RSS 8.02 → 9.48 MiB (range 8.02–9.55); available RAM 24.20 → 12.60 GiB. Parent guard passed; system-context guard REVIEW.
  - smollm2-360m:select: ready range 0.271–0.491 s.
  - smollm2-360m:cpu: ready range 0.005–0.006 s.
  - smollm2-360m:active: ready range 0.005–0.005 s.
  - smollm2-360m:gpu: ready range 0.347–0.631 s.
  - smollm2-360m:return-cpu: ready range 0.156–0.164 s.
  - smollm2-360m:auto: ready range 0.005–0.006 s.
  - bonsai2-27b-pq2:select: ready range 1.316–5.008 s.
  - bonsai2-27b-pq2:cpu: ready range 0.005–0.010 s.
  - bonsai2-27b-pq2:active: ready range 0.005–0.009 s.
  - bonsai2-27b-pq2:gpu: ready range 21.009–22.434 s.
  - bonsai2-27b-pq2:auto: ready range 0.006–0.010 s.
  - bonsai2-27b-pq2:return-cpu: ready range 1.290–1.438 s.

Every replacement asserted old PID exit/reap before new spawn; final stop asserted exit. Exact restoration of free RAM is not a release oracle. Ten small-model and three Bonsai CPU/Metal cleanup cycles are included. These short series cannot exclude all leaks or support tail-percentile claims.

## Source integrity

- `39-baseline-1/build.json` SHA-256 `25f50b8ea170756f6098f19228e2c30606734afcf0452e89201abc69bf248f35`
- `39-baseline-1/results.json` SHA-256 `4b541d22d0103ce78725a8d37af481000374182cf2553b5b0bd041f07547a7f9`
- `39-baseline-1/samples.jsonl` SHA-256 `9e6e7fa9e3bd5860b5a4197395c68f6daa2651f4d092dd136601bae67d8ff7f9`
- `39-baseline-1/complete.json` SHA-256 `1d6cd819d86afd3d6f8211cea0b4bca960cd2f5b1a08600a333d07af745c43e8`
- `39-candidate-1/build.json` SHA-256 `17566bc7892c0b1115dd19d5e6c3f7de0b76deaf6a97a207d7d048a64ab7832e`
- `39-candidate-1/results.json` SHA-256 `972fa05ceed9217f4d5d7db921905bc8857558c0380b4f6c9a2b8cdd19317622`
- `39-candidate-1/samples.jsonl` SHA-256 `9e9dc30422d33c5027fc59de641b3c7c0b00b63c25c6a5c013abd771e0f0c84e`
- `39-candidate-1/complete.json` SHA-256 `1d6cd819d86afd3d6f8211cea0b4bca960cd2f5b1a08600a333d07af745c43e8`

Guard findings: `[{"run": "baseline", "memory_review": true}, {"run": "candidate", "memory_review": true}]`. Any findings require written review; no failed trial is discarded.
