# pagerctl

The PagerOS command-line tool. Single Go binary distributed for macOS, Linux,
and Windows.

## Build from source

```sh
cd cli
go build -o pagerctl ./cmd/pagerctl
./pagerctl version
```

## Run your app locally (CLI-002)

```sh
pagerctl dev                                  # app.py in cwd, port 8000
pagerctl dev --port 8001
pagerctl dev --app examples/hello/app.py --python .venv/bin/python
```

The `dev` command spawns `python -m pageros.devserver --app <path>` (PY-009 —
file-change auto-reload) and opens the simulator pointed at the dev server.
One Ctrl+C stops both.

Flags:

- `--app <path>` — app entry (default: `app.py` in cwd).
- `--host <host>` — dev server bind host (default: `127.0.0.1`).
- `--port <n>` — dev server bind port (default: `8000`).
- `--python <bin>` — Python interpreter (default: `$PAGEROS_PYTHON`, then
  `python3` on `PATH`). Point at your venv: `--python .venv/bin/python`.
- `--bin <path>` — simulator binary (same precedence as `pagerctl simulate`).
- `--no-simulator` — skip the simulator (useful when connecting a real device).

Requires Python with the `pageros` package importable, and the
[PagerOS Simulator](../simulator) installed (unless `--no-simulator`).

## Simulate a remote app (CLI-003)

```sh
pagerctl simulate http://localhost:8080/
pagerctl simulate https://app.example.com/
```

The `simulate` command spawns the PagerOS simulator preconfigured for direct
mode against the given URL (via the `PAGEROS_SIMULATOR_URL` env-var contract
defined by SIM-EXT-001). The simulator window stays open until you close it.

Flags:

- `--bin <path>` — simulator binary path. Defaults to `$PAGEROS_SIMULATOR_BIN`,
  then `pageros-simulator` on `PATH`.

Requires the [PagerOS Simulator](../simulator) installed (see
`simulator/RELEASING.md` for bundles).

## Flash firmware (CLI-005)

```sh
pagerctl flash path/to/fw.bin
```

The `flash` command auto-detects an attached ESP32-S3 by USB VID/PID (Espressif
native USB-Serial-JTAG `303a:*`, plus common UART bridges: CP210x, CH340/343,
FT232) and shells out to [`esptool`](https://github.com/espressif/esptool) to
write the binary and verify it via SHA256.

Flags:

- `--port <path>` — bypass auto-detect (e.g. `/dev/ttyACM0`, `COM3`).
- `--chip <name>` — target chip family (default `esp32s3`).
- `--baud <n>` — baud rate (default `460800`).
- `--offset <hex>` — flash offset (default `0x10000`). Use `0x20000` to write
  to the PagerOS factory recovery slot, or `0x0` for a merged image produced
  by `idf.py merge-bin`.

Requires `esptool` (v4.6+) or `esptool.py` on `PATH` (`pipx install esptool`).

## Install

| Platform           | Command                                                                                                  |
| ------------------ | -------------------------------------------------------------------------------------------------------- |
| macOS / Linux      | `brew install pageros/tap/pagerctl`                                                                      |
| Debian / Ubuntu    | Download `pagerctl_<version>_linux_amd64.deb` from the release and `sudo apt install ./pagerctl_*.deb`   |
| Fedora / RHEL      | Download the `.rpm` and `sudo dnf install pagerctl-<version>.x86_64.rpm`                                 |
| Alpine             | Download the `.apk` and `sudo apk add --allow-untrusted pagerctl_<version>_linux_amd64.apk`              |
| Windows            | `scoop bucket add pageros https://github.com/pageros/scoop-bucket && scoop install pagerctl`             |
| Manual (any OS)    | Grab the archive for your platform from the [releases page](https://github.com/pageros/pageros/releases) |

## Release pipeline (CLI-008)

Releases are produced by [GoReleaser](https://goreleaser.com) driven from
`.github/workflows/cli-release.yml`.

Cutting a release:

1. Tag the repo: `git tag cli/v0.1.0 && git push origin cli/v0.1.0`.
2. The `pagerctl release` workflow runs GoReleaser using `cli/.goreleaser.yaml`
   and publishes the GitHub Release plus the Homebrew tap and Scoop bucket
   updates.

Validate the config without publishing:

```sh
cd cli
goreleaser release --snapshot --clean --skip=publish
```

### Required repo secrets

- `HOMEBREW_TAP_GITHUB_TOKEN` — write access to `pageros/homebrew-tap`.
- `SCOOP_BUCKET_GITHUB_TOKEN` — write access to `pageros/scoop-bucket`.

`GITHUB_TOKEN` is provided automatically and is used to publish the release
itself and attach the `.deb`/`.rpm`/`.apk` artifacts.
