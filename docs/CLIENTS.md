# Client matrix

The matrix below applies to the legacy standalone `geist-serve` server. The
new manager exposes an authenticated shared-daemon text-chat endpoint; use
[INSTALL.md](INSTALL.md) and its Connections panel for that installation.
Do not launch a second standalone server to connect to the manager.


What to type into each client to point it at geist-serve, and whether
someone has actually done it. A ticked box means a person ran that client
against this server and got a streamed answer; an unticked box is a
documented setting nobody has verified yet. Tick it when you do.

geist-serve listens on `127.0.0.1:11434` by default, the Ollama port, so
every client's "Ollama" preset works with no address change. The model name
is the GGUF's basename without `.gguf`; `GET /api/tags` or `GET /v1/models`
prints it.

| Client | Protocol | Setting | Verified |
| :-- | :-- | :-- | :-- |
| curl | both | see README | [x] every PR (tests/smoke.sh) |
| `ollama` CLI | Ollama | `OLLAMA_HOST=127.0.0.1:11434 ollama run <model>` | [x] 2026-09-23, macOS, `run` / `list` / `show` / `ps` |
| VS Code Copilot Chat | Ollama | Manage Models → Ollama; default endpoint `http://localhost:11434` | [ ] |
| VS Code Copilot Chat | OpenAI | Manage Models → OpenAI compatible; URL `http://127.0.0.1:11434/v1`, any key, model id from `/v1/models` | [ ] |
| Continue | Ollama | `config.yaml`: `provider: ollama`, `model: <name>`, `apiBase: http://127.0.0.1:11434` | [ ] |
| Continue | OpenAI | `provider: openai`, `apiBase: http://127.0.0.1:11434/v1`, `apiKey: none` | [ ] |
| Cline / Roo Code | Ollama | API provider Ollama, base URL `http://127.0.0.1:11434`, pick the model from the list | [ ] |
| Cline / Roo Code | OpenAI compatible | base URL `http://127.0.0.1:11434/v1`, model id from `/v1/models` | [ ] |
| Zed | Ollama | Assistant settings → Ollama, API URL `http://127.0.0.1:11434` | [ ] |
| Zed | OpenAI compatible | `language_models.openai_compatible` with `api_url: http://127.0.0.1:11434/v1` | [ ] |
| Cursor | OpenAI | Models → override OpenAI base URL `http://127.0.0.1:11434/v1`, add the model id, any key | [ ] |
| Open WebUI | Ollama | `OLLAMA_BASE_URL=http://host.docker.internal:11434` (from Docker) | [ ] |
| Open WebUI | OpenAI | Connections → OpenAI API, URL `http://host.docker.internal:11434/v1` | [ ] |
| Enchanted / Msty | Ollama | server address `http://<host>:11434` | [ ] |

Known gaps clients will notice:

- One request at a time. A client that fires a title-generation request in
  parallel with the chat queues behind it; nothing fails, the second answer
  just starts later.
- Context is 4096 tokens (geistlib#428). Editor clients with long system
  prompts still work — the oldest turns are dropped — but a single message
  over the limit is refused with 400.
- No tool calling, no images, no embeddings. Tool definitions in a request
  are ignored, not rejected; `/api/embed` answers 404.
- `/api/version` reports `0.1.0`. A client that gates features on Ollama's
  version number sees an old Ollama.

Reaching it from another machine: `--host 0.0.0.0`. There is no
authentication; put it behind something if the network is not yours.
