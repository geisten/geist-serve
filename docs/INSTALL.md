# Install geisten and connect your tools

These are development candidates, not a published release or Apple-approved
distribution. The desktop manager, terminal and compatible editor share one
loaded geistd. No model is included; click a suggested model on first use.

## macOS (Apple Silicon, macOS 14+)

Drag geisten.app from the DMG to Applications and open it. geisten opens its own
desktop window. Click a model name or its download icon to download and start it.
An installed model starts directly; progress, pause and resume stay in its row.
A platform check suggests one model, including a smaller fallback when needed.
The main screen shows the catalog beside **Quick test** (stacked in narrow
windows). **Connect a program** opens connection setup. Catalog rows show download progress rings. A closed
green ring and check mean downloaded; paused downloads retain their percentage.
**Quick test** is optional: Enter sends, Shift + Enter inserts a newline, and
follow-up questions use this window's conversation. **Clear test** clears it.
Language preferences are in **Settings**. The metric row below the active model
shows speed, process RAM and file size. Click it to expand OS/CPU details, system
and available RAM and reply timings in the same place.
These measurements describe performance, not answer quality. The Mac menu bar
provides **Models** and **Connect a program** shortcuts.
The bundled
terminal client is `/Applications/geisten.app/Contents/MacOS/geist-cli`.
Use its full path, or link it as `geisten` in a directory on your PATH. Start at Login is
optional. An actual distributable DMG still requires Developer ID signing and
an Apple Accepted result; see the Mac repository's NOTARIZATION.md.

## Ubuntu 24.04 (64-bit Intel/AMD or ARM)

The current engine requires x86-64-v3 (AVX2/FMA/BMI2-era CPUs) or ARMv8.2 with
FP16 and dot-product instructions (for example Pi 5). An arbitrary 64-bit CPU
is not sufficient. The manager checks CPU capabilities before suggesting or
loading models. Older CPUs need a separately built compatible engine.

Keep the DEB and its matching `.deb.sha256` file together, then verify with
`sha256sum -c geisten_VERSION_ARCH.deb.sha256` before installation.

For a desktop, download both the matching `geisten_VERSION_ARCH.deb` and
`geisten-desktop_VERSION_all.deb`, with their SHA-256 sidecars. Install both:

```sh
sudo apt install ./geisten_VERSION_ARCH.deb ./geisten-desktop_VERSION_all.deb
```

**From an earlier `geist` package (#92):** the same command replaces `geist` and
`geist-desktop` in one step; `geist` stays available as a command, and models,
key and settings move to `~/.local/share/geisten` on the next start. If you had
enabled login startup and were logged in during the upgrade, enable it again
with `systemctl --user enable --now geisten.service`.

Open geisten from the application menu, or run `geisten-desktop`. This opens a GTK
window with the system WebKit renderer; no browser tab is opened. `geisten open`
also uses the desktop app when installed.

The Ubuntu 24.04 desktop package installs a scoped AppArmor profile permitting
WebKit sandbox user namespaces for the geisten desktop process tree. It leaves
global namespace policy unchanged; the host profile does not add filesystem
or network confinement. Package updates reload the profile; removal unloads it.

Terminal-only installations need
only `geisten_VERSION_ARCH.deb`, without GTK/WebKit dependencies. A browser/SSH
interface remains available on headless systems. Downloads and the
daemon run as your user, never as root. The optional per-user service uses
`systemctl --user enable --now geisten.service` for login startup.

In a headless login session with a user systemd manager, use `geisten start`,
`geisten models`, `geisten download MODEL` and `geisten use MODEL`. Check progress with
`geisten status`. Without a user systemd manager, run
`/usr/lib/geisten/geist-app` in the foreground. For browser access use an SSH
tunnel to port 8766 and the private URL; there is no public network listener.

## Verify and connect

`geisten test` makes a real text-generation request through the shared editor API.
`geisten chat "Say hello"` uses the same service. `geisten connection` returns its
local URL, current model and private API key. Treat the key as a credential.
It is stored with user-only permissions and survives service restarts.

The same API is also on a Unix socket, `api.sock` in the data folder (its path
is `socket` in `geisten connection`, `null` if the path is too long). Only your
user account can open it, and the key is still required:
`curl --unix-socket <path> -H "Authorization: Bearer <key>" http://localhost/v1/models`.

`geisten config continue` prints JSON that is also valid YAML for a local Continue
config.yaml. Preserve existing settings; add its model and select Chat mode.
`geisten config opencode` prints a private opencode.json for an isolated folder.
Its geist-chat profile denies all tools. Do not commit either generated file.
The desktop Connect step can copy equivalent configurations.

Only text chat is supported by this gateway: 4096 context tokens, up to 1024
output tokens, one generation at a time. Busy clients receive HTTP 429. Changing
models requires refreshing the client configuration. Tool requests return 422;
`geisten test-agent` confirms that rejection and exits 3. This is not coding-agent
acceptance. A successful connection is not an answer-quality recommendation.

## Restart, update and uninstall

`geisten stop` stops this user's service and its daemon. `geisten restart` restarts
it; a previously selected catalog model is verified and loaded again. Closing
the application window leaves the service available to editors.

On Ubuntu install both newer DEBs with apt, run `systemctl --user daemon-reload`, then
`geisten restart`. To recover a failed candidate, install the previous verified
DEB with apt's explicit downgrade option and restart. Models stay in
`~/.local/share/geisten/models`. Model storage and connection credentials are not
modified by package scripts.

Before uninstalling Ubuntu, run `systemctl --user disable --now geisten.service`
and `geisten stop`, then `sudo apt remove geisten-desktop geisten`. Models remain in the user data
folder even after package purge. Delete that folder separately only if desired.
On Mac, disable Start at Login, stop geisten, quit the desktop app, then move
geisten.app to Trash. Cached models remain in `~/Library/Application Support/geisten`.
Quit/stop before replacing a Mac development bundle, then open the replacement.

No package scripts obtain secrets or remove user models. Public update feeds,
signed release provenance and independent clean-machine notarization acceptance
must be verified before presenting these candidates as a stable download.

## Legacy standalone installer

`install.sh` installs the older standalone `geist-serve` plus `geistd`; it does
not install the shared model manager, `geisten` client or desktop interface.
Use the Mac/Ubuntu package above for those features.

The revised script requires exactly one SHA-256 entry for both binaries and
all three service/config files. Missing or mismatched assets abort before
installed files change. It stages verified executables, serializes installation
with a prefix lock and restores previous files after ordinary replacement
errors or catchable signals. Existing administrator configuration is retained.
Older release manifests without unit checksums are deliberately rejected.

This is not atomic across power loss or SIGKILL, and a checksum manifest from
the same host is not an independent provenance signature. A stale lock after
an uncatchable interruption must be inspected before retrying; preserve its
recovery files. No new release is published by these changes.

## Desktop privacy and recovery

The Mac host uses an ephemeral WKWebView; the Ubuntu host uses an ephemeral
WebKitGTK NetworkSession. Neither stores browser history or private links to
disk. Only the chosen interface language is saved. External navigation is
restricted to user-activated geisten GitHub links. Clipboard integration can
write text but cannot read the clipboard or execute commands.

Closing the window preserves the shared service. **Stop model service** asks
for confirmation because it also disconnects editors. The native window then
offers **Start / reconnect**. **Unload model** frees the active model while
keeping the service alive. Remove an inactive model with **Remove download**;
active models must be unloaded first. Interrupted downloads can also be removed.

The initial native support targets macOS 14+ on Apple Silicon and Ubuntu 24.04.
GTK/Xvfb tests do not establish manual GNOME/Wayland acceptance. Local Mac
ad-hoc signatures are not an Apple notarization result.


### Updating an existing installation

Install the newer `geisten` and matching `geisten-desktop` packages with APT. Their
package names and installation paths stay the same across minor versions, so
APT replaces the prior files. Models and the local API key stay in your user
data directory. Close the previous window and reopen geisten after installation.

From 0.5.3, starting a newer client replaces an older versioned service only
when it is idle. A running download, model load or response blocks the handoff;
finish it and reconnect. A newer service is never silently downgraded. For
older releases without version discovery, finish your work and run
`geisten restart` once (the Mac app offers the equivalent migration dialog).

## Reading performance values

`GET /app/status` remains authenticated and local. Its `hardware` object adds
`os` and `logical_cpus`; `cores` still means the compute-core count used by the
model recommendation policy. No scheduling or model-selection policy changes.

`resources.scope` is `geistd`: `rss_bytes` is its current resident memory,
including shared pages, excluding the UI and manager. It is an OS snapshot, not
peak or exclusive memory. `cpu_percent` is the CPU-time delta divided by elapsed
monotonic wall time and logical CPU count (100% = all logical CPUs). Sampling
uses at least 500 ms, shares the window across readers and resets after a process
change, counter reversal or more than 10 seconds without sampling. Missing
counters are JSON null, not zero. The UI refreshes about every 1.8 seconds.
Mac CPU time is converted from Mach ticks with the system timebase; Linux uses
/proc clock ticks and resident pages. The independent process-clock regression
checks units on both platforms. Available RAM remains a conservative OS estimate.
GPU utilization and energy are not measured.

The `memory` object adds independent, timestamped process-RSS and Metal-allocation
snapshots from the owning runtime, including during long load/prefill. The UI uses
these scoped values and expires samples after six seconds. Shared-memory values
must not be summed. See [the memory contract](MEMORY-TELEMETRY.md) for sources,
unsupported/error states and journal fields.

Reply speed uses the backend’s generated-token count and generation duration,
including streaming. First text and total use the interface’s monotonic clock,
including prompt processing and the local connection. No chunk/character-based
estimates are shown. Incomplete streams never get a final speed. Each completed
reply retains its own summary in this window; the current summary is cleared on
new chat/request or model change. Benchmarks do not replace the chat summary.
