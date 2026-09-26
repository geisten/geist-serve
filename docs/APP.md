# Geist — Runs here. Stays here.

Try rewriting, summarizing and generating ideas on your own computer.
Choose a task and a model, download it once, then work offline. No account is required.
Small models have limited capabilities: check their answers, and use the
examples to decide whether a model meets your needs.

This is a development preview. No new public release is implied by these
instructions. The Mac DMG is ad-hoc signed for local review; a distributable
Mac release still needs Developer ID signing and notarization.

## Start on a Mac

Open the Geist DMG, drag Geist to Applications, and open Geist. Its menu bar
icon opens the local interface in your default browser. Choose a task, then Download on a model that fits your resources. After verification and loading, try an example.
Use Stop to cancel an answer, Unload to free model memory, or Quit Geist
to stop the local runtime. Closing the browser or quitting the menu bar leaves the shared service running. Start at Login is optional in the native menu.

Apple Silicon and macOS 14 or later are required by the Mac app.
Previously downloaded catalog files in the Geist data folder are reused
after verification. The former Swift application's selected-model preference
is not migrated: choose Use once. The new Connections panel and bundled `geist` terminal client use the same
loaded daemon. The older standalone geist-serve server is a separate legacy
entry point; do not start it to connect an editor to the manager.

## Start on a Raspberry Pi

Use 64-bit Raspberry Pi OS. Pi 5 with at least 4 GB RAM is the initial target.
Extract the `geist-…-linux-aarch64.tar.gz` archive. On the desktop, run
`Start Geist.sh` from the extracted folder (choose Execute when your file
manager asks). It opens your browser. If the file manager opens the script
as text, run `sh './Start Geist.sh'` in that folder's terminal.

The package contains three static executables: geist, geist-app and geistd. It does not need a compiler,
Python, a package manager, Docker or an inference service installation.
HTTPS downloads use the operating system's CA certificate store.

For a Pi without a screen, start the script over SSH and leave it running:

```sh
ssh your-user@your-pi
cd /path/to/extracted/geist-folder
sh './Start Geist.sh'
```

In a second terminal on your Mac, open the encrypted tunnel:

```sh
ssh -N -L 8766:127.0.0.1:8766 your-user@your-pi
```

Open the full private link printed by the Pi in the Mac browser. It includes
a session key after `#`; opening just the port will not authorize access.
Keep that link private. The model, device assessment and inference run on
the Pi. The browser sends the prompt through SSH and displays the answer
on the Mac. Both terminals can be closed after Quit Geist. If port 8766 is
occupied, start with `--port 8767` and forward 8767 at both ends instead.
No direct LAN listener or Internet exposure is needed.

## Model advice and measurements

The initial catalog reuses six SHA-256 and file-size pins from
geist-serve-mac. Only this allowlisted catalog can be downloaded in the UI.
Original weights have their own licenses; consult each linked model page.

| Model | Download, decimal GB | Planning memory, MiB | Recommended system RAM |
| --- | ---: | ---: | ---: |
| [BitNet b1.58 2B](https://huggingface.co/microsoft/bitnet-b1.58-2B-4T-gguf) | 1.19 | 2304 | 4 GiB |
| [SmolLM2 360M](https://huggingface.co/HuggingFaceTB/SmolLM2-360M-Instruct-GGUF) | 0.39 | 768 | 2 GiB |
| [Qwen3 0.6B](https://huggingface.co/Qwen/Qwen3-0.6B-GGUF) | 0.64 | 1280 | 4 GiB |
| [Qwen3.5 0.8B](https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF) | 0.81 | 2048 | 4 GiB |
| [Gemma 4 E2B](https://huggingface.co/unsloth/gemma-4-E2B-it-GGUF) | 3.11 | 5120 | 8 GiB |
| [Gemma 4 E4B](https://huggingface.co/unsloth/gemma-4-E4B-it-GGUF) | 4.98 | 8192 | 16 GiB |

Planning memory is a conservative application estimate for short text tasks
and up to 4096 context tokens, not measured peak RSS. Available RAM changes
with other applications. Model weight size alone is not enough to establish
that a model will run well.

- **Recommended:** both resource fit and versioned, per-model task-quality evidence pass. No current task/model pair has that evidence yet.
- **Conditional:** task quality is unverified, or resources/speed need testing. These models remain selectable. Hardware advice and task evidence are separate.
- **Unavailable:** unsupported architecture, physical RAM smaller than the
  model file, or insufficient disk for the remaining download plus 256 MiB.
  The disabled button includes a visible reason.

The 8 tokens/s threshold is a product guideline, not a research result.
The Pi 5 BitNet reference of 17.8 tokens/s comes from the existing
[geist-serve platform notes](https://github.com/geisten/geist-serve/blob/main/docs/PLATFORMS.md). It is explicitly labelled as a
reference, never as a measurement of the current machine. All other initial
speed advice is qualitative. Local results of at least 16 generated tokens
update the model's card for the current app session. They are not saved.
The quick test generates at most 64 tokens; normal tasks at most 256.
Tokens/s uses geistd generation wall time (including token streaming), not answer quality or time to first text. End-to-end time includes tokenization and prefill.

## Architecture and memory ownership

`geist-app` is a C23 application in this repository. It uses the public framed protocol of an adjacent `geistd` child process over a private Unix socket. One session is allowed; each independent task closes its session after completion. The daemon stays warm. No application code or
private engine dependency is added to geistlib. The Mac repository supplies
only native lifecycle, menu, login-item and update integration. HTML/CSS/JS
is embedded in the C executable and shared by Mac and Pi.

- Each HTTP worker owns one 256 KiB arena, freed on every exit. Typed arena
  allocation checks multiplication, addition, alignment and capacity using
  C23 checked arithmetic. Response buffers have sticky overflow failures.
- There are at most eight HTTP workers, one model/download worker and one
  generation. Headers are limited to 8 KiB, bodies to 32 KiB and prompts to
  12000 UTF-8 bytes. Libraries and OS thread stacks have separate allocations;
  the arena limit is not a total process-RAM claim.
- Headers and body share a five-second absolute receive deadline. A client
  sending occasional bytes cannot reserve an HTTP worker indefinitely.
- Models stream to `.part` files with resumable downloads, bounded writes,
  TLS validation, SHA-256 verification and atomic rename. Cancellation of
  verification preserves cached models. A confirmed size/hash mismatch
  removes only the owned invalid catalog file. I/O errors preserve it.
- The supervisor owns its process, sockets, download thread and curl handles.
  Stop/quit/error paths release them explicitly. C23 itself does not provide
  Rust-style borrow checking, garbage collection or automatic memory safety.
- The service binds loopback. Host/Origin checks, a random per-launch bearer
  capability, no CORS and a restrictive CSP protect the UI/API boundary.
  Inference uses a mode-0600 Unix socket in a random mode-0700 directory. There is no child TCP port. Same-user native processes remain trusted.

Data lives in `~/Library/Application Support/Geist` on Mac and
`$XDG_DATA_HOME/geist` (normally `~/.local/share/geist`) on Linux. Files contain
models, the selected catalog ID, a runtime log, a process lock, a mode-0600 API
key and mode-0600 connection.json. The connection descriptor is removed on
shutdown; the API key survives restarts. The app
does not save prompt history or send telemetry. Prompts and answers exist
in browser/process memory. Browser history may retain the local launch URL.
Model downloads contact Hugging Face; manual Mac update checks contact the
configured release feed. Neither sends prompts.

## Build and test

Build the inference daemon, then the application:

```sh
make geistd GEIST_STATIC_OMP=1     # macOS portable runtime; Linux uses its own target
make app                     # C23, libcurl; Linux also needs OpenSSL headers/libs
./geist-app --check           # device advice without loading a model
./geist-app --port 0          # choose a free loopback port
make test-app                # sanitizers + model-free HTTP tests
make -f App.mk build/geist-app-test
GEIST_APP_TEST_BINARY=build/geist-app-test GEIST_TEST_MODEL=/path/to/smollm2-360m-instruct-q8_0.gguf python3 tests/app/http_test.py
GEIST_TEST_MODEL=/path/to/smollm2-360m-instruct-q8_0.gguf python3 tests/app/download_test.py
```

GCC 14 or a recent C23 Clang is required. GCC 14 uses a generated asset
header; compilers with `#embed` embed the same source files directly. Python
is only a build/test dependency. macOS uses CommonCrypto SHA-256; Linux uses
OpenSSL EVP. Production does not accept the test-only download URL override.

`--home DIRECTORY`, `--daemon EXECUTABLE` and `--model FILE` support isolated
development. An explicitly supplied custom GGUF bypasses catalog hash
verification and does not update a catalog model's recommendation.

For portable Linux packaging, build inside Alpine with
`scripts/build-app-static.sh`, supply a matching static `geistd`, then
run `VERSION=dev sh scripts/package-app.sh linux-aarch64`. Keep release
provenance and SHA256SUMS alongside the package. Build dependencies are
recorded in BUILD-PACKAGES.txt; the build script uses maintained Alpine
packages and is not a bit-for-bit reproducibility claim.

The [Mac shell](https://github.com/geisten/geist-serve-mac) accepts
`RUNTIME_DIR=/path/to/this/checkout`. Publish and pin the shared app runtime
before publishing a dependent Mac release. This change does not publish
either project or modify the existing command-line installer.

## Versioned tasks and the existing agent

`tasks/catalog.json` is the single source for the task selector and C input/output limits. Unknown task IDs and stale task versions are rejected. Text tasks are bounded experiments with no tools; none carries a validated quality claim. Home Assistant links to the existing `geist-home-assistant` integration, which retains entity exposure and action execution. It is not executed by this app.

For bounded tool use, `geistagent` supplies an optional `geist-remote` adapter to this daemon (`agent_api: 1`). It needs a separate, dedicated daemon with two sessions for generation and routing. No second agent loop is added here. The app still reserves its own one-session daemon for short text tasks.

Socket operations have a 120-second deadline; an app request has a 180-second ceiling and checks browser disconnect/quit during I/O. A running engine kernel cannot be preempted through the protocol. Unload/quit terminate the app-owned daemon. Incomplete output is never counted as a completed measurement.

See [EVALUATION.md](EVALUATION.md) for the controlled llama.cpp smoke comparison and the separate Home Assistant evidence gates.

The launcher starts at most two compute threads on Macs/unknown hardware and four on Pi 5, with passive OpenMP waiting. These are conservative resource limits, not a claim of optimal throughput. BitNet uses the existing Llama-3 renderer, matching geistagent's framing policy; generation stops and output-limit truncation are distinct UI states.

## Shared editor endpoint

See [installation and client setup](INSTALL.md). The authenticated `/v1/models`
and `/v1/chat/completions` routes use the same geistd as the browser. Text
history, streaming and usage are supported. Tool requests fail explicitly;
this preview does not claim coding-agent support.
