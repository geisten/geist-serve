# Rootless Linux install (#46)

`scripts/install-geisten.sh` installs the shared geisten runtime (`geisten`,
`geist-app`, `geistd`) for the current user on 64-bit Linux, without root and
without a package manager. The Ubuntu DEBs (docs/INSTALL.md) stay the system-wide
route; the installer refuses to shadow them.

```sh
curl -fsSL https://geisten.net/install.sh | sh
sh install-geisten.sh --version 0.6.0 --no-start
sh install-geisten.sh --model recommended --yes      # unattended, with the first model
sh install-geisten.sh --uninstall
```

Options: `--version X.Y.Z`, `--no-start`, `--model recommended` (not with
`--no-start`), `--desktop`, `--dry-run`, `--uninstall` (alone), `--yes`, `--help`.

## Desktop route (`--desktop`)

On Ubuntu 24.04 (x86_64/ARM64) only, `--desktop` installs the native window
instead of the rootless runtime: the matching `geisten_<v>_<arch>.deb` and
`geisten-desktop_<v>_all.deb`, through APT.

- Both packages are authenticated by the signed manifest (`deb`/`desktop`
  lines: size and SHA-256) before APT sees them; a different file stops with
  exit 12 and nothing installed.
- The installer explains what it installs and asks on the terminal before it
  runs `sudo apt-get install`. `--yes` never authorises sudo; without a
  terminal it stops (exit 2) and names the packages to install by hand.
- An existing rootless installation is not crossed over (exit 13): remove it
  first with `--uninstall` (models and settings stay), then use `--desktop`.
  An existing APT installation is upgraded by the same command.
- `--dry-run` downloads and verifies the pair only; `--model recommended` runs
  `geisten setup` after the install. Open the window from the application menu
  or with `geisten-desktop`.

## First model

The installer never chooses a model itself. `geisten setup` asks the running
service for its recommendation for this computer, shows name and download
size, and asks once on the terminal (`/dev/tty`, because stdin is the script
under `curl | sh`). After a yes, the service downloads, verifies and loads the
model through `/app/setup`, which refuses if the recommendation changed in the
meantime. `geisten setup` then sends one real generation through `/v1` and
reports installed, model ready and test passed separately.

| Run | Model step |
|-----|------------|
| fresh install with a terminal | `geisten setup` asks; no means no download |
| no terminal (CI, pipes, SSH without TTY), or an update | none; prints `geisten setup` as the next command |
| `--model recommended` | `geisten setup`, which asks |
| `--model recommended --yes` | `geisten setup --yes`: downloads without asking |

A terminal yes also records the model's preview consent, like the model button
in the app. `--yes` does not: the app still asks before its first task with
that model. If the model step fails, the installation stays and the exit code
is 17 for `--model`, or 0 with a resume hint after the interactive offer.
Interrupting `geisten setup` leaves the download running in the service; run it
again to follow or resume.

## Layout

| Path | Content |
|------|---------|
| `~/.local/share/geisten-runtime/versions/<v>/` | one unpacked release |
| `~/.local/share/geisten-runtime/current` | symlink `versions/<v>`, swapped atomically (`mv -T`) |
| `~/.local/share/geisten-runtime/receipt` | `receipt 1`, `version`, `platform`, `source`, `engine`, `launcher`, `previous` |
| `~/.local/share/geisten-runtime/.lock/` | pid of the running installer; stale locks are taken over |
| `~/.local/bin/geisten` | launcher symlink to `current/geisten` |
| `~/.local/bin/geist` | the same, under the earlier command name (#92) |

Only the active and the previous version are kept. Uninstall stops the service,
removes the launcher and the runtime directory, and keeps `~/.local/share/geisten`
(models, keys, settings). The installer only touches files its receipt owns: a
foreign `~/.local/bin/geisten` or `~/.local/bin/geist`, an APT install, or a runtime directory without a
receipt ends with exit 13 and no change. The rootless `geisten` starts its own
`geist-app` as the user; only the packaged CLI in `/usr/lib/geisten` uses the
packaged systemd user unit, so a later APT install cannot capture it.

## Release assets

| Asset | Made by |
|-------|---------|
| `geisten-<v>-linux-{x86_64,aarch64}.tar.gz` | `scripts/package-app.sh`, tested by `tests/install/bootstrap_acceptance.sh` in `installers.yml` |
| `geisten-manifest` | `scripts/installer-manifest.py` |
| `geisten-manifest.sig` | `scripts/sign-manifest.sh` |
| `install-geisten.sh` | copied from `scripts/` |

The release ships all of them or none (`scripts/release-manifest.py`): without
the signing secret the job prints a notice and publishes no installer.

## Manifest

LF-only text, every line required, in this order, nothing else:

```
geisten-manifest 1
product geisten
version X.Y.Z
channel stable
source <40-hex geist-serve commit>
engine <geistlib commit | digest | unknown>
archive <platform> <cpu baseline> <file> <bytes> <sha256>
deb <amd64|arm64> <file> <bytes> <sha256>       (all three package lines or none)
desktop all <file> <bytes> <sha256>
```

One `archive` line per platform: `linux-x86_64` needs x86-64-v3 (avx2, bmi2,
f16c, fma, movbe) and `linux-aarch64` needs armv8.2-a with dotprod and fp16;
the installer checks the CPU flags before downloading. The installer parses the
manifest line by line and never evaluates it.

An archive holds exactly `geisten-<v>-<platform>/` with `geisten`, `geist-app`,
`geistd`, `SHA256SUMS` and the packaged extras (licenses, `README.md`,
`ENGINE.json`, `BUILD-PACKAGES.txt`, `Start geisten.sh`): regular files
and directories only, at most 16 members, at most 512 MiB unpacked. Before
activation the installer checks size and SHA-256 against the manifest, the
member list, the ELF machine of each binary, the inner `SHA256SUMS`, and that
`geist --help` runs.

## Signing

`geisten-manifest.sig` is the base64 Ed25519 signature over the exact manifest
bytes, checked with `openssl pkeyutl -verify -rawin` (OpenSSL 3 or newer). The
private key lives only in the `release` environment secret
`GEIST_MANIFEST_SIGNING_KEY` (PEM); its public key is pinned in
`GEIST_MANIFEST_PUBKEY` in `scripts/install-geisten.sh`. If that constant is
empty, every install refuses with exit 12.

Rotating the key: add the new public key to the installer, release, then drop
the old one. An installer only trusts the key it was downloaded with.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | ok |
| 2 | usage |
| 10 | unsupported host (root, non-Linux, 32-bit, CPU baseline) |
| 11 | missing prerequisite (curl, OpenSSL 3, `mv -T`, …) |
| 12 | download or verification failed |
| 13 | another installation owns the files |
| 14 | geisten or another installer is busy (previous version restored) |
| 15 | start failed (previous version restored) |
| 16 | local file system problem |
| 17 | installed, but the `--model` setup did not finish (`geisten setup` resumes) |

## Tests

`tests/install/bootstrap_test.py` (CI, Linux) drives the installer against
signed fixture releases through the test-only environment:
`GEIST_INSTALL_TEST_ORIGIN` (a `file://` or `http://` release directory),
`GEIST_INSTALL_TEST_PUBKEY`, `GEIST_INSTALL_TEST_ARCH` and
`GEIST_INSTALL_TEST_CPUINFO`. None of them is read unless the origin is set.
`tests/install/bootstrap_acceptance.sh` installs the real archive, runs real
inference, reruns, and uninstalls.
