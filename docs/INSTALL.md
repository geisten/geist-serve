# Install Geist and connect your tools

These are development candidates, not a published release or Apple-approved
distribution. The manager, browser, terminal and compatible editor share one
loaded geistd. No model is included; choose Download on first use.

## macOS (Apple Silicon, macOS 14+)

Drag Geist.app from the DMG to Applications and open it. The menu bar opens the
local manager. Choose a model, then use the Connections panel. The bundled
terminal client is `/Applications/Geist.app/Contents/MacOS/geist-cli`.
Use its full path, or link it as `geist` in a directory on your PATH. Start at Login is
optional. An actual distributable DMG still requires Developer ID signing and
an Apple Accepted result; see the Mac repository's NOTARIZATION.md.

## Ubuntu (64-bit Intel/AMD or ARM)

The current engine requires x86-64-v3 (AVX2/FMA/BMI2-era CPUs) or ARMv8.2 with
FP16 and dot-product instructions (for example Pi 5). An arbitrary 64-bit CPU
is not sufficient. The manager checks CPU capabilities before suggesting or
loading models. Older CPUs need a separately built compatible engine.

Keep the DEB and its matching `.deb.sha256` file together, then verify with
`sha256sum -c geist_VERSION_ARCH.deb.sha256` before installation.

Install the matching candidate with `sudo apt install ./geist_VERSION_ARCH.deb`.
Open Geist from the application menu, or run `geist open`. Downloads and the
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
The browser Connections panel can copy equivalent configurations.

Only text chat is supported by this gateway: 4096 context tokens, up to 1024
output tokens, one generation at a time. Busy clients receive HTTP 429. Changing
models requires refreshing the client configuration. Tool requests return 422;
`geist test-agent` confirms that rejection and exits 3. This is not coding-agent
acceptance. A successful connection is not an answer-quality recommendation.

## Restart, update and uninstall

`geist stop` stops this user's service and its daemon. `geist restart` restarts
it; a previously selected catalog model is verified and loaded again. Closing
the browser leaves the service available to editors.

On Ubuntu install a newer DEB with apt, run `systemctl --user daemon-reload`, then
`geist restart`. To recover a failed candidate, install the previous verified
DEB with apt's explicit downgrade option and restart. Models stay in
`~/.local/share/geist/models`. Model storage and connection credentials are not
modified by package scripts.

Before uninstalling Ubuntu, run `systemctl --user disable --now geist.service`
and `geist stop`, then `sudo apt remove geist`. Models remain in the user data
folder even after package purge. Delete that folder separately only if desired.
On Mac, disable Start at Login, stop Geist, quit the menu app, then move
Geist.app to Trash. Cached models remain in `~/Library/Application Support/Geist`.
Quit/stop before replacing a Mac development bundle, then open the replacement.

No package scripts obtain secrets or remove user models. Public update feeds,
signed release provenance and independent clean-machine notarization acceptance
must be verified before presenting these candidates as a stable download.
