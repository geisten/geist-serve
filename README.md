# geist-serve

**Runs here. Stays here.** The optional `geist-app` adds a local interface,
hardware-aware model choices and verified first-run downloads for Mac and
Raspberry Pi. Build with `make app` after building the server.
See [the app guide](docs/APP.md) for desktop and headless setup.
The app is currently a local development build, not a published download.

An Ollama- and OpenAI-compatible HTTP front for the
[geist engine](https://github.com/geisten/geistlib): one GGUF, one process,
one request at a time, no dependencies. Point VS Code Copilot Chat, Continue,
Cline, Zed, Cursor, Open WebUI or the `ollama run` terminal chat at it.

Install a release binary (Linux x86-64/arm64 static, macOS arm64), checksum
verified, with the systemd units when run as root on a systemd host:

```sh
curl -fsSL https://raw.githubusercontent.com/geisten/geist-serve/main/install.sh | sh
sudo systemctl enable --now geist-serve.socket   # after setting GEIST_MODEL in /etc/default/geist-serve
```

Or build it:

```sh
make                                   # pins and builds the engine, then ./geist-serve
make fetch-model && make test          # unit test + 70-odd HTTP checks against SmolLM2-360M
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

Status: both API families serve, streamed, with chat templates for Gemma 3
and 4, ChatML (Qwen, SmolLM2), Llama 3 and BitNet. The real `ollama` CLI
runs against it. Where it runs: [`docs/PLATFORMS.md`](docs/PLATFORMS.md).
What to type into each editor or chat client: [`docs/CLIENTS.md`](docs/CLIENTS.md).
Deploying: `deploy/systemd/` (socket activation on 11434, hardened service,
model path in `/etc/default/geist-serve`), `install.sh`, and static release
binaries from `.github/workflows/release.yml` on every `v*` tag.

## License

Apache-2.0, like the engine.
