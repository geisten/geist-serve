# geistd — libgeist over a socket

For agents that run as many short processes per task, with the model local
or on another machine. The session and its KV cache live in the daemon and
outlive the connection; each process resumes by id and pays only for the
tokens it adds. Nothing is interpreted: no chat template, no sampling
policy beyond the engine's own session options, logits on request.

What it buys: the stateless chat API re-prefills the whole conversation
every turn, so an agent pays for its system prompt on every call and the
cost grows with the conversation; geistd prefills only what is new. With
SmolLM2-360M and a 430-token system prompt on an M-series Mac, one chat
turn costs about 1.8 s on an idle machine, so five turns are ~10 s there,
against ~2 s through geistd (five processes, 1884 tokens reused, 501
prefilled). The gap grows linearly with turns and is far larger on a
Raspberry Pi, where prefill is the expensive part. `tests/agent_short_calls.py`
runs the comparison and prints the load average, because under contention
the stateless path degrades much faster (measured 187 s vs 4.4 s at load 215).

**Raspberry Pi 5 (4 GB), measured 2026-09-25**, model on the board, agent
on a Mac over an SSH tunnel to the board's Unix socket, `OMP_NUM_THREADS=4`,
board idle and under 60 °C:

| model | prefill | decode | 4–5 agent turns as separate processes | same turns, stateless chat API |
| :-- | --: | --: | --: | --: |
| SmolLM2-360M Q8_0 | — | — | 6.2 s (5 turns, 1884 tokens reused) | 23.8 s |
| BitNet b1.58 2B-4T i2_s | 41.9 tok/s | 17.8 tok/s | 15.3 s (4 turns, 1569 tokens reused) | 41.9 s |

geistshell's constrained decoder ran unchanged against both, through the
tunnel: with BitNet it answered "list the files" with
`(recommend (kind local_shell) … (command "ls") …)` in about 15 s including
the vocabulary fetch. The full `make test` suite passes on the board in
under five minutes.

```sh
geistd model.gguf                                 # $XDG_RUNTIME_DIR/geistd.sock (0600)
geistd model.gguf --socket /tmp/g.sock --sessions 8 --idle 600
GEISTD_TOKEN=... geistd model.gguf --host 0.0.0.0 --port 7433   # a trusted LAN only
```

Remote (a Pi): prefer an SSH tunnel to the Pi's Unix socket:

```sh
ssh -N -L /tmp/pi-geistd.sock:/run/user/1000/geistd.sock pi &
python3 -c 'import geistd; print(geistd.Client(path="/tmp/pi-geistd.sock").info())'
```

## Wire format

One connection carries any number of request/response frames, serially.
A frame is

```
u32 header_len   little-endian
u32 body_len     little-endian
header           JSON object, header_len bytes
body             raw bytes, body_len bytes (int32[] token ids, float32[] logits)
```

Requests carry `"op"` and, for session ops, `"session"`. Responses carry
`"ok": true` plus the result, or `"ok": false, "error": "…"`. Caps: header
64 KiB, body 16 MiB; an oversize or unparsable frame gets one error reply
and the connection is closed (there is no resynchronising a broken frame).
An error reply to a well-formed request leaves the connection open.

geistd serves one connection at a time. A client must not hold a
connection open while idle: everyone else waits behind it (the server
closes an idle read after 30 s). Connect per call; a Unix socket connect
costs microseconds. `clients/geistd.py` does this.

## Ops

| op | header fields | body in | reply |
| :-- | :-- | :-- | :-- |
| `hello` | `token` | | `ok`. Required as the first frame when the daemon runs off loopback with `GEISTD_TOKEN`. Constant-time compare. |
| `info` | | | `model, arch, eos, eot[], bos, add_bos, ctx, vocab, template, sessions, max_sessions` |
| `open` | `temperature, top_p, top_k, seed` (engine session options; fixed for the session's life) | | `session` (16 hex). Evicts the least recently used session when the table is full. |
| `close` | `session` | | frees the session |
| `reset` | `session` | | truncates the KV cache and history to the pinned prefix (0 without a pin); replies `n` |
| `pin` | `session, n` | | pins the first `n` history tokens (`geist_session_pin_prefix`): `reset` keeps them, a `prefill` that differs inside them is refused. Once per session. How geistshell amortises a constant system prompt. |
| `tokenize` | `text` (≤ 32 KiB) | | body int32[] ids, `n`. No BOS is added: `info.add_bos` says whether the model expects one (`info.bos`). |
| `str` | `session` | int32[] ids | `pieces[]`, `null` for control tokens |
| `prefill` | `session` | int32[] ids: the whole intended context | `prefilled, reused, n`. See semantics below. |
| `step` | `session, topk?` | | `token, piece, stop, n`, and `top: [[id, logprob]…]` of the distribution this step sampled from |
| `peek` | `session, topk?, full?` | int32[] ids (optional) | `n`; `logits[], logprobs[]` for the given ids (`null` out of range); `top` for the top-k; body float32[] of the whole vector with `full` |
| `generate` | `session, max, stop_ids[], stop_strings[]` | | one frame per token `{token, piece, done:false}`, then `{done:true, reason, generated, n}`. `reason`: `stop`, `max`, `context`, `client`, `error`. |

### prefill semantics

The request is the complete context the session should hold. geistd finds
the longest common prefix with what the KV cache already holds (prefilled
tokens and tokens produced by `step`/`generate`):

- history is a proper prefix of the request → only the tail is prefilled,
  `reused` = history length;
- request equals history → nothing happens, `prefilled` = 0;
- anything else (divergence) → the session is reset and everything after
  the pinned prefix is prefilled (`reused` = pinned length, 0 without a
  pin). The engine has no rollback to a position. A request that differs
  inside a pinned prefix is refused.

The context is capped at 4096 tokens (geistlib#428): a request that would
exceed it is refused before the engine is touched.

### The constrained-decoding loop

```python
s = c.open(temperature=0)
s.prefill(ids)                                   # prompt
cands = [c.tokenize(" yes")[0], c.tokenize(" no")[0]]
lp = s.peek(ids=cands)["logprobs"]               # log-softmax over the vocab
chosen = cands[lp.index(max(lp))]
s.prefill(ids + [chosen])                        # feed it back: 1 token prefilled
```

`peek` before `step` shows the distribution `step` would sample from;
`step(topk=k)` returns it alongside the sampled token.

## Sessions

`--sessions N` (default 4, max 16) resident sessions; each holds a full KV
cache, so on a 4 GB Pi keep it small. `--idle S` (default 1800) evicts a
session not used for S seconds; a full table evicts the least recently
used. An evicted id answers `unknown session`; the client re-opens and
prefills (the whole context, once).

## Security

- Unix socket file mode 0600: the file is the local access control.
- TCP off loopback refuses to start without `GEISTD_TOKEN` (≥ 32
  characters) and every connection must `hello` first. No TLS: the token is
  for a trusted LAN; across anything else, tunnel.
- The daemon never reads files on request, never writes, and holds no
  secrets beyond the token.
