# pagerctl

The PagerOS command-line tool. Single Go binary distributed for macOS, Linux,
and Windows.

## Build from source

```sh
cd cli
go build -o pagerctl ./cmd/pagerctl
./pagerctl version
```

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
