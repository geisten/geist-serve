# geist-serve

An Ollama- and OpenAI-compatible HTTP front for the
[geist engine](https://github.com/geisten/geistlib): one GGUF, one process,
one request at a time, no dependencies. Point VS Code Copilot Chat, Continue,
Cline, Zed, Cursor, Open WebUI or the `ollama run` terminal chat at it.

```sh
make                                   # pins and builds the engine, then ./geist-serve
./geist-serve model.gguf               # 127.0.0.1:11434, the Ollama port
./geist-serve model.gguf --host 0.0.0.0 --port 8080
```

## What it speaks

| Family | Endpoints | Streaming |
| :-- | :-- | :-- |
| OpenAI (also what llama.cpp's server exposes) | `GET /v1/models` · `POST /v1/chat/completions` · `POST /v1/completions` · `GET /health` | SSE |
| Ollama | `GET /api/tags` · `GET /api/version` · `POST /api/show` · `POST /api/generate` · `POST /api/chat` | NDJSON |

Registry endpoints (`/api/pull`, `/api/push`, `/api/create`, `/api/delete`)
answer 404: the model is the GGUF given on the command line, named after its
basename, `:latest` suffix accepted.

## Process model

A new-style foreground daemon in the Unix sense: no fork, no pid file, logs
to stderr, exits cleanly on SIGTERM. The listening socket is taken from
systemd (`LISTEN_FDS`) or inetd wait-mode when handed in, otherwise bound
from `--host`/`--port`. `--stdio` serves one connection on stdin/stdout and
exits, which is inetd accept-mode and the test harness in one flag:

```sh
printf 'POST /api/generate HTTP/1.1\r\nContent-Length: 41\r\n\r\n{"model":"m","prompt":"Hi","stream":false}' \
  | ./geist-serve model.gguf --stdio
```

Requests are served serially on one thread. That is the ceiling on purpose:
the target is a Raspberry Pi 5 with one model resident, where a second
concurrent inference is a thermal problem, not a throughput gain.

## Boundary

geistlib is the engine and stays application-neutral. Everything that is
policy lives here: chat templates (fingerprinted from the GGUF's
`tokenizer.chat_template`, rendered by four hand-written functions for
Gemma, ChatML/Qwen, Llama 3 and BitNet), sampling defaults, stop strings,
context truncation, model naming.

Status: skeleton. The build links against the pinned engine and loads a
model; transport, templates and endpoints are tracked in the
[v0.1 milestone](https://github.com/geisten/geist-serve/milestones).

## License

Apache-2.0, like the engine.
