# Scoped memory acceptance — Apple M1 Max, 64 GiB, 2026-09-29

Source: `review/observability-implementation-20260929/40-real-1`. Frozen binaries and raw numeric observations are retained locally. No prompts/replies are in this report.

Engine: `33db79d7764b4f6177d944e8ae4fd9f7fadea9be`. Process source: `macos.proc_pid_rusage.ri_resident_size`. Metal source: the owning backend’s `MTLDevice.currentAllocatedSize`.

## Real Bonsai PQ2_0 switch

| Execution | Process RSS after short response | Metal allocated |
| --- | ---: | ---: |
| cpu | 12.727 GiB | Unsupported |
| gpu | 1.050 GiB | 6.821 GiB |
| cpu | 13.750 GiB | Unsupported |

These scopes overlap and must not be added. This reproduces the RSS/allocation discrepancy, not the owner’s exact 0.3-GiB sample. A synthetic 0.3-GiB RSS plus 10-GiB allocation is separately covered by serialization and native UI tests.

## Long prefill and lifetime

- CPU: 2023 status observations during prefill; maximum backend-sample age 2.084 seconds. CPU reports unsupported GPU allocation while continuing RSS sampling.
- GPU: 72 status observations during prefill; maximum backend-sample age 2.024 seconds. CPU reports unsupported GPU allocation while continuing RSS sampling.

The 1,223-token prompts completed on CPU and Metal. All 26 recorded owned child PIDs were gone after stop/restart trials; old generation values were rejected. Collection and request history worked without a WebView. Exact restoration of system-free RAM is not asserted.

## Alternating sampler on/off observations

Same cached SmolLM2 artifact, deterministic prompt, 29 generated tokens, fresh CPU/Metal transitions; cache and OS background load were uncontrolled. All CPU trials include both initial and return-to-CPU requests.

| Backend | Sampling | n | Total response median | Observed range |
| --- | --- | ---: | ---: | ---: |
| CPU | off | 6 | 380.538 ms | 371.627–388.100 ms |
| CPU | on | 6 | 391.278 ms | 380.870–411.415 ms |

Observed CPU median difference: +2.82%.

| GPU | off | 3 | 511.388 ms | 495.367–529.221 ms |
| GPU | on | 3 | 502.909 ms | 488.275–506.119 ms |

Observed GPU median difference: -1.66%.

These short trials do not isolate a causal overhead or establish negligible overhead. The implementation adds no GPU flush, inference-mutex wait or per-token persistence; longer repeated trials would be needed for a narrow overhead bound.

## Scope and evidence hashes

Real app/API/controlled-comparison and restart checks are separate from this switch matrix. Golden fixtures cover missing/zero/stale/error/legacy, cancellation and journal export. Native CI covers Ubuntu CPU and macOS UI; Linux GPU allocation remains unsupported.

- `build.json`: `88e6a413b7cedefef96d27e2a2a2c9af5da0d990e5ab505b2618a7a993ad3d7d`
- `results.json`: `ec3f2a52976a22eef5919abc28b2192354c87c0f50a740a418da991ef3075ebc`
- `samples.jsonl`: `5fbabe0ad32065f0eb7adbe7086bf7081eb778ce3a2da9aa6583789791bf1f73`
- `records.jsonl`: `027c7e3dae4449cdd28422c373092f10e3d93238708659f063c4a810418818d6`
- `complete.json`: `1fdafd69220578aad7a75544b7b12d37234acd94aad80f0374a52c7efa2df9fe`
