# geist-serve

**Runs here. Stays here.** Start with one model suggested for your hardware,
and use the same `geistd` from the model manager, terminal and editor.
`geist-app` owns the private inference process; the C23 `geisten` client (`geist`
still works as its earlier name) and
authenticated `/v1/chat/completions` gateway connect to that same service.

Mac DMG and Ubuntu amd64/arm64 DEB candidates are under development. See
[installation and connections](docs/INSTALL.md) for setup, Continue in VS Code,
OpenCode, restart, update and removal. Text chat and agent support are distinct:
this gateway rejects tool calls explicitly. These are not published releases;
the Mac candidate has not yet received Apple notarization acceptance.

Build with `make app` after building `geistd`, then run `./geisten open`.
The [app guide](docs/APP.md) explains resource advice and model downloads.

## Building

```sh
make                                   # pins and builds the engine and geist-runtime, then ./geistd
make fetch-model && make test          # unit tests + every geistd op against SmolLM2-360M
make app test-app                      # the C23 app and its tests (after plain `make`: they need ./geistd)
make coverage                          # line coverage: C, Python and the web UI (scripts/coverage.sh)
```

## Boundary

- [geistlib](https://github.com/geisten/geistlib) is the engine and stays
  application-neutral.
- [geist-runtime](https://github.com/geisten/geist-runtime) holds chat
  templates, streaming, context, the catalog and device fit. It is pinned
  like geistlib (`runtime.mk`).
- geistd loads the model and keeps the conversation (`docs/GEISTD.md`).
- geist-app owns geistd and serves the UI and the authenticated
  `/v1/chat/completions` gateway.

The standalone `geist-serve` HTTP server, its `install.sh`, systemd socket
units and raw release binaries are retired (#148).

## Task quality

The local app requires explicit experimental opt-in unless a model, task,
language and device combination has reviewed evidence. See
[task-quality acceptance](docs/TASK-QUALITY.md) for the frozen corpus, human
review and reproducibility limits. Resource fit alone does not establish quality.

## License

Apache-2.0, like the engine.
