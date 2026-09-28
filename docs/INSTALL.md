# Install Geist and connect your tools

These are development candidates, not a published release or Apple-approved
distribution. The desktop manager, terminal and compatible editor share one
loaded geistd. No model is included; choose Set up and start on first use.

## macOS (Apple Silicon, macOS 14+)

Drag Geist.app from the DMG to Applications and open it. Geist opens its own
desktop window. Read the preview notice and choose **Set up and start**. A
platform check suggests one model, including a smaller fallback when needed.
Once ready, the main screen shows the model and **Connect a program**.
**Change model** shows all catalog models with download progress rings. A closed
green ring and check mean downloaded; paused downloads retain their percentage.
**Quick test** is optional: Enter sends, Shift + Enter inserts a newline, and
follow-up questions use this window's conversation. **Clear test** clears it.
Language preferences are in **Settings**. **System & performance**, below the
model, shows OS/CPU details, available RAM, process RAM/CPU and reply timings.
These measurements describe performance, not answer quality. The Mac menu bar
provides **Models & performance** and **Connect a program** shortcuts.
The bundled
terminal client is `/Applications/Geist.app/Contents/MacOS/geist-cli`.
Use its full path, or link it as `geist` in a directory on your PATH. Start at Login is
optional. An actual distributable DMG still requires Developer ID signing and
an Apple Accepted result; see the Mac repository's NOTARIZATION.md.

## Ubuntu 24.04 (64-bit Intel/AMD or ARM)

The current engine requires x86-64-v3 (AVX2/FMA/BMI2-era CPUs) or ARMv8.2 with
FP16 and dot-product instructions (for example Pi 5). An arbitrary 64-bit CPU
is not sufficient. The manager checks CPU capabilities before suggesting or
loading models. Older CPUs need a separately built compatible engine.

Keep the DEB and its matching `.deb.sha256` file together, then verify with
`sha256sum -c geist_VERSION_ARCH.deb.sha256` before installation.

For a desktop, download both the matching `geist_VERSION_ARCH.deb` and
`geist-desktop_VERSION_all.deb`, with their SHA-256 sidecars. Install both:

```sh
sudo apt install ./geist_VERSION_ARCH.deb ./geist-desktop_VERSION_all.deb
```

Open Geist from the application menu, or run `geist-desktop`. This opens a GTK
window with the system WebKit renderer; no browser tab is opened. `geist open`
also uses the desktop app when installed.

The Ubuntu 24.04 desktop package installs a scoped AppArmor profile permitting
WebKit sandbox user namespaces for the Geist desktop process tree. It leaves
global namespace policy unchanged; the host profile does not add filesystem
or network confinement. Package updates reload the profile; removal unloads it.

Terminal-only installations need
only `geist_VERSION_ARCH.deb`, without GTK/WebKit dependencies. A browser/SSH
interface remains available on headless systems. Downloads and the
daemon run as your user, never as root. The optional per-user service uses
`systemctl --user enable --now geist.service` for login startup.

In a headless login session with a user systemd manager, use `geist start`,
`geist models`, `geist download MODEL` and `geist use MODEL`. Check progress with
`geist status`. Without a user systemd manager, run
`/usr/lib/geist/geist-app` in the foreground. For browser access use an SSH
tunnel to port 8766 and the private URL; there is no public network listener.

## Verify and connect

`geist test` makes a real text-generation request through the shared editor API.
`geist chat "Say hello"` uses the same service. `geist connection` returns its
local URL, current model and private API key. Treat the key as a credential.
It is stored with user-only permissions and survives service restarts.

`geist config continue` prints JSON that is also valid YAML for a local Continue
config.yaml. Preserve existing settings; add its model and select Chat mode.
`geist config opencode` prints a private opencode.json for an isolated folder.
Its geist-chat profile denies all tools. Do not commit either generated file.
The desktop Connect step can copy equivalent configurations.

Only text chat is supported by this gateway: 4096 context tokens, up to 1024
output tokens, one generation at a time. Busy clients receive HTTP 429. Changing
models requires refreshing the client configuration. Tool requests return 422;
`geist test-agent` confirms that rejection and exits 3. This is not coding-agent
acceptance. A successful connection is not an answer-quality recommendation.

## Restart, update and uninstall

`geist stop` stops this user's service and its daemon. `geist restart` restarts
it; a previously selected catalog model is verified and loaded again. Closing
the application window leaves the service available to editors.

On Ubuntu install both newer DEBs with apt, run `systemctl --user daemon-reload`, then
`geist restart`. To recover a failed candidate, install the previous verified
DEB with apt's explicit downgrade option and restart. Models stay in
`~/.local/share/geist/models`. Model storage and connection credentials are not
modified by package scripts.

Before uninstalling Ubuntu, run `systemctl --user disable --now geist.service`
and `geist stop`, then `sudo apt remove geist-desktop geist`. Models remain in the user data
folder even after package purge. Delete that folder separately only if desired.
On Mac, disable Start at Login, stop Geist, quit the desktop app, then move
Geist.app to Trash. Cached models remain in `~/Library/Application Support/Geist`.
Quit/stop before replacing a Mac development bundle, then open the replacement.

No package scripts obtain secrets or remove user models. Public update feeds,
signed release provenance and independent clean-machine notarization acceptance
must be verified before presenting these candidates as a stable download.

## Legacy standalone installer

`install.sh` installs the older standalone `geist-serve` plus `geistd`; it does
not install the shared model manager, `geist` client or desktop interface.
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
restricted to user-activated Geisten GitHub links. Clipboard integration can
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

Install the newer `geist` and matching `geist-desktop` packages with APT. Their
package names and installation paths stay the same across minor versions, so
APT replaces the prior files. Models and the local API key stay in your user
data directory. Close the previous window and reopen Geist after installation.

From 0.5.3, starting a newer client replaces an older versioned service only
when it is idle. A running download, model load or response blocks the handoff;
finish it and reconnect. A newer service is never silently downgraded. For
older releases without version discovery, finish your work and run
`geist restart` once (the Mac app offers the equivalent migration dialog).

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

Reply speed uses the backend’s generated-token count and generation duration,
including streaming. First text and total use the interface’s monotonic clock,
including prompt processing and the local connection. No chunk/character-based
estimates are shown. Incomplete streams never get a final speed. Each completed
reply retains its own summary in this window; the current summary is cleared on
new chat/request or model change. Benchmarks do not replace the chat summary.
