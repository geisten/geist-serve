# Rootless Linux install (#46)

`scripts/install-geist.sh` installs the shared Geist runtime (`geist`,
`geist-app`, `geistd`) for the current user on 64-bit Linux, without root and
without a package manager. The Ubuntu DEBs (docs/INSTALL.md) stay the system-wide
route; the installer refuses to shadow them.

```sh
curl -fsSL https://geisten.net/install.sh | sh
sh install-geist.sh --version 0.6.0 --no-start
sh install-geist.sh --uninstall
```

Options: `--version X.Y.Z`, `--no-start`, `--dry-run`, `--uninstall` (alone),
`--yes`, `--help`. `--desktop` and `--model` are reserved and exit 2 until the
desktop route and `geist setup` exist.

## Layout

| Path | Content |
|------|---------|
| `~/.local/share/geist-runtime/versions/<v>/` | one unpacked release |
| `~/.local/share/geist-runtime/current` | symlink `versions/<v>`, swapped atomically (`mv -T`) |
| `~/.local/share/geist-runtime/receipt` | `receipt 1`, `version`, `platform`, `source`, `engine`, `launcher`, `previous` |
| `~/.local/share/geist-runtime/.lock/` | pid of the running installer; stale locks are taken over |
| `~/.local/bin/geist` | launcher symlink to `current/geist` |

Only the active and the previous version are kept. Uninstall stops the service,
removes the launcher and the runtime directory, and keeps `~/.local/share/geist`
(models, keys, settings). The installer only touches files its receipt owns: a
foreign `~/.local/bin/geist`, an APT install, or a runtime directory without a
receipt ends with exit 13 and no change.

## Release assets

| Asset | Made by |
|-------|---------|
| `geist-<v>-linux-{x86_64,aarch64}.tar.gz` | `scripts/package-app.sh`, tested by `tests/install/bootstrap_acceptance.sh` in `installers.yml` |
| `geist-manifest` | `scripts/installer-manifest.py` |
| `geist-manifest.sig` | `scripts/sign-manifest.sh` |
| `install-geist.sh` | copied from `scripts/` |

The release ships all of them or none (`scripts/release-manifest.py`): without
the signing secret the job prints a notice and publishes no installer.

## Manifest

LF-only text, every line required, in this order, nothing else:

```
geist-manifest 1
product geist
version X.Y.Z
channel stable
source <40-hex geist-serve commit>
engine <geistlib commit | digest | unknown>
archive <platform> <cpu baseline> <file> <bytes> <sha256>
```

One `archive` line per platform: `linux-x86_64` needs x86-64-v3 (avx2, bmi2,
f16c, fma, movbe) and `linux-aarch64` needs armv8.2-a with dotprod and fp16;
the installer checks the CPU flags before downloading. The installer parses the
manifest line by line and never evaluates it.

An archive holds exactly `geist-<v>-<platform>/` with `geist`, `geist-app`,
`geistd`, `SHA256SUMS` and the packaged extras (licenses, `README.md`,
`ENGINE.json`, `BUILD-PACKAGES.txt`, `Start Geist.sh`): regular files
and directories only, at most 16 members, at most 512 MiB unpacked. Before
activation the installer checks size and SHA-256 against the manifest, the
member list, the ELF machine of each binary, the inner `SHA256SUMS`, and that
`geist --help` runs.

## Signing

`geist-manifest.sig` is the base64 Ed25519 signature over the exact manifest
bytes, checked with `openssl pkeyutl -verify -rawin` (OpenSSL 3 or newer). The
private key lives only in the `release` environment secret
`GEIST_MANIFEST_SIGNING_KEY` (PEM); its public key is pinned in
`GEIST_MANIFEST_PUBKEY` in `scripts/install-geist.sh`. If that constant is
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
| 14 | Geist or another installer is busy (previous version restored) |
| 15 | start failed (previous version restored) |
| 16 | local file system problem |

## Tests

`tests/install/bootstrap_test.py` (CI, Linux) drives the installer against
signed fixture releases through the test-only environment:
`GEIST_INSTALL_TEST_ORIGIN` (a `file://` or `http://` release directory),
`GEIST_INSTALL_TEST_PUBKEY`, `GEIST_INSTALL_TEST_ARCH` and
`GEIST_INSTALL_TEST_CPUINFO`. None of them is read unless the origin is set.
`tests/install/bootstrap_acceptance.sh` installs the real archive, runs real
inference, reruns, and uninstalls.
