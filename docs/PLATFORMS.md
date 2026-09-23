# Platforms

Where geist-serve is known to run, as of 2026-09-23. The server is plain
POSIX C23 — sockets, `recv` with `MSG_DONTWAIT`, `clock_gettime`,
`gmtime_r`, `sigaction` — on top of libgeist, so "does it run" is mostly
"does the engine build there", and the engine's own targets are in
`geistlib/mk/target-*.mk`.

| Platform | Engine target | Status | Evidence |
| :-- | :-- | :-- | :-- |
| Linux x86-64 | `linux` | works | CI: build, unit test, full HTTP smoke against SmolLM2 on every PR |
| Linux arm64 | `linux` | works | CI: same, on the arm64 runner |
| macOS arm64 | `mac-omp` | works | developed and tested here; `ollama` CLI end to end |
| macOS x86-64 | `mac-omp` | compiles | `clang -arch x86_64` compile check of the server sources; no Intel Mac to run on |
| Raspberry Pi 5 (64-bit OS) | `pi5` | pending | `.github/workflows/pi5.yml` runs the whole suite on the reference board through its self-hosted runner. The runner is currently registered to another repo; the board's SSH was refusing connections on 2026-09-23, so no run yet. The Linux arm64 CI result is the same code on the same ISA without the cortex-a76 tuning. |
| Android (Termux) | none yet | expected | Bionic has everything the server uses. Blocked on an engine target for Termux's clang + libomp; no NDK on the dev machine to cross-check. Untested. |
| iOS / iPadOS | none yet | compiles | The server sources compile for `arm64-apple-ios16` with the iPhoneOS SDK. An app cannot run a daemon with a `main()`, so embedding means calling the accept loop from the app with a listener it created — a small refactor, not a port. Blocked on an engine target. |
| Windows native | none | no | POSIX sockets and signals, and the engine has no MSVC/MinGW target. |
| Windows via WSL2 | `linux` | works | It is Linux arm64/x86-64; reachable from Windows clients at `localhost:11434` with WSL's default port forwarding. |

Compile checks that exist without a device: `xcrun --sdk iphoneos clang
-std=c23 -arch arm64 -miphoneos-version-min=16 -c src/serve.c` and
`src/template.c`.
