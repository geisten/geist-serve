# Geist — Runs here. Stays here.

Manage a local model and connect your terminal and editor to the same service.
Select **Set up and start** to download the platform suggestion, then work offline.
No account or initial task/model/language selection is required.
Small models have limited capabilities: check their answers, and use the
examples to decide whether a model meets your needs.

This is a development preview. No new public release is implied by these
instructions. The Mac DMG is ad-hoc signed for local review; a distributable
Mac release still needs Developer ID signing and notarization.

## Start on a Mac

Open the Geist DMG, drag Geist to Applications, and open its desktop window.
Read the short preview notice and choose **Set up and start**. Geist downloads,
verifies and starts one suggested model. The main screen stays on **Models**,
with the catalog on the left and **Quick test** on the right. In narrow windows
they stack vertically. Each pane scrolls independently. **Connect a program**
opens editor setup; the short test checks a response and its speed. Press **Enter** to send and **Shift + Enter** for a new line. The arrow sends
with a pointer or touch; the stop button preserves a partial response. Follow-up
messages include the visible conversation from this window. The **ⓘ** control offers
examples for rewriting, summarizing and ideas; there is no task-mode selector.

Responses render Markdown headings, lists, quotes, tables and code blocks on a
white reading surface. **Copy** retains the original Markdown; code blocks
have a separate copy control. Wide code and tables scroll inside their blocks.
Model-provided HTML is shown literally. Web addresses can be copied; the chat
does not navigate to them or load remote images. No formatting service is used.

Entering Quick test or clearing the test focuses the composer. Sending and
stopping return to it; background status updates and completed responses do not
steal focus. Escape dismisses the help panel and returns to its control. Tab follows the
visible document order. While text is selected or a response control has focus,
formatting changes to that response wait until selection/focus leaves it.

The transcript scrolls independently above the composer. New output follows only
while you are reading the latest message. Long drafts scroll without being cut.
**Clear test** asks before clearing the conversation and draft. Reloading or quitting
clears the page memory; nothing is saved as chat history. Closing and reopening a
hidden desktop window can retain that window's memory until the app quits.

A chat response can use up to 1,024 output tokens within the shared 4,096-token
context. A limited answer is marked and offers **Continue response**, which preserves any next-message draft. Context or
request limits produce an explicit error; no earlier turns are silently dropped.
Failed requests keep the draft; stopped partial answers remain in context. Session
chat requires preview consent: single-task evidence does not certify follow-ups.
Quick speed tests remain independent of the conversation.

The **Models** screen keeps the catalog visible alongside the short test, with a collapsed
**System & performance** panel. A ring before each model shows download status:
an empty ring means no download, a partial ring shows downloaded bytes, and a
closed green ring with a check means the model file is present. Paused transfers
keep their percentage. File verification is a separate indeterminate state;
receiving all bytes alone does not complete the download. Labels accompany every
ring. Download status is separate from the model's running state and suitability.

The gear opens **Settings**. **System language** is the default: German for a
German OS locale, English for other languages. Native Mac and Ubuntu hosts supply
the OS language; a browser-only session uses its browser language. Explicit
**Deutsch** or **English** overrides persist; choosing **System language** restores
automatic detection. Existing explicit language choices remain respected. Native
menu labels follow the preference. Answer language starts with the interface
language unless a separate answer preference was saved.

The interface uses white, a light gray catalog and blue action accents. Green
rings identify completed downloads, independently of the active model. Speed,
process RAM and model file size appear beneath the active model name; missing
values remain unknown. Icon controls have localized accessible names and tooltips.
Download states, constraints and preview consent remain readable text. The trash
icon clears the test after confirmation and is disabled for an empty test.
**Settings** also holds service controls. **Connect**
sets up Terminal, Continue or OpenCode text chat. On Mac, the menu bar shows the
active model and service status; **Models & performance** and **Connect a program**
reopen the corresponding view without reloading or clearing a test draft.
Closing the window keeps the shared service running. Stopping it requires
confirmation. Start at Login is optional in the native menu.

Apple Silicon and macOS 14 or later are required by the Mac app.
Previously downloaded catalog files in the Geist data folder are reused
after verification. The former Swift application's selected-model preference
is not migrated: choose the model once in the model list. The new Connections panel and bundled `geist` terminal client use the same
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
The quick speed test generates at most 64 tokens; individual task API requests
at most 256. The optional conversation test uses up to 1,024 output tokens.
Tokens/s uses geistd generation wall time (including token streaming), not answer quality or time to first text. End-to-end time includes tokenization and prefill.

## Shared platform selection

The C23 application service (`src/app/core.c`, `platform.c`, `main.c`) owns
product policy. `geistlib` provides inference capabilities; it does not choose
product defaults, ask for consent or select a model by scenario. Native hosts,
the authenticated `/app/status` API and `geist models` see the same recommendation.

| Platform | Initial preview default | Smaller candidate |
| --- | --- | --- |
| Apple Silicon / macOS | Gemma 4 E2B | SmolLM2 360M |
| Raspberry Pi 5 | BitNet b1.58 2B | SmolLM2 360M |
| Other supported Linux AMD64/ARM64 | SmolLM2 360M | None smaller in this catalog |

These defaults follow existing executable/model compatibility checks: the Mac
Gemma task run, physical Pi BitNet runs, and both Linux installer CI jobs using
SmolLM2. They are **not task-quality winners**. The registry is still empty.
Larger defaults require at least four compute cores. Observed session speed
below the product's 8 tokens/s target can rule out an automatic candidate;
unmeasured speed is not advertised as a measurement or a guarantee.

Automatic setup requires supported CPU instructions, known memory/disk values,
the total-RAM guideline (95% allows firmware reservations), available memory
at least the working-memory estimate, and remaining download space plus
256 MiB. Existing partial bytes are credited, installed files reused and
verified. If no candidate fits, setup is blocked. `/app/setup` rechecks the
recommendation before accepting the displayed model ID. Manual conditional
choices remain available in the model list; they never become automatic fallbacks.

The private `selected` file preserves an explicit choice across restarts and
policy updates. Resource pressure can block that choice but cannot silently
replace it. Incomplete downloads reopen without loading a missing file;
removal clears a matching selection. Answer language and versioned preview
consent per immutable model SHA are saved separately in private atomic files.
No prompts, responses or API keys are stored in these preferences. Each
`/app/generate` request still needs the explicit experimental flag unless its
quality evidence passes. Preview consent does not change evidence or approve
agent tools. Custom command-line GGUFs require fresh window-local consent.

## Architecture and memory ownership

`geist-app` is a C23 application in this repository. It uses the public framed protocol of an adjacent `geistd` child process over a private Unix socket. One session is allowed; each independent task closes its session after completion. The daemon stays warm. No application code or
private engine dependency is added to geistlib. The Mac repository supplies the native WKWebView window, lifecycle, menu,
login-item and update integration. Ubuntu uses GTK/WebKitGTK. HTML/CSS/JS is
embedded in the C executable and shared by these hosts and the headless Pi browser.

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
