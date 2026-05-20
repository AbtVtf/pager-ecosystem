# Releasing the PagerOS Simulator

The simulator ships signed desktop binaries for macOS, Linux, and Windows. Builds
are driven by `tauri-apps/tauri-action` from
`.github/workflows/simulator-release.yml`.

## Bundle targets

Defined in `simulator/src-tauri/tauri.conf.json` (`bundle.targets`):

| Platform | Target triple | Bundles |
|---|---|---|
| macOS (Apple Silicon) | `aarch64-apple-darwin` | `dmg`, `app` |
| macOS (Intel) | `x86_64-apple-darwin` | `dmg`, `app` |
| Linux | `x86_64-unknown-linux-gnu` | `appimage`, `deb` |
| Windows | `x86_64-pc-windows-msvc` | `msi`, `nsis` |

The acceptance criterion for SIM-008 is satisfied by the `dmg`, `appimage`, and
`msi` outputs.

## Cutting a release

1. Bump `version` in `simulator/src-tauri/tauri.conf.json` and
   `simulator/src-tauri/Cargo.toml`.
2. Commit: `[SIM-008] simulator vX.Y.Z`.
3. Tag and push:

   ```sh
   git tag simulator/vX.Y.Z
   git push origin simulator/vX.Y.Z
   ```

4. CI builds all targets in parallel, uploads bundles to a GitHub Release named
   `PagerOS Simulator simulator/vX.Y.Z`, and (if the `SIMULATOR_PAGES_DEPLOY_HOOK`
   secret is set) pings the download-site to refresh
   `https://simulator.pageros.org/download`.

## Snapshot / dry-run

Run `simulator release` via **workflow_dispatch** with `snapshot=true`. This:

- Builds all four matrix targets.
- Skips signing if Apple secrets are unset.
- Publishes to a pre-release tag `simulator-snapshot-<run-id>` so you can
  download artifacts without cutting a real release.

`simulator-ci.yml` runs the same matrix on every PR and `main` push (snapshot
mode only).

## macOS signing + notarization

These repository secrets feed `tauri-apps/tauri-action`:

| Secret | Purpose |
|---|---|
| `APPLE_CERTIFICATE` | Base64-encoded Developer ID Application `.p12`. |
| `APPLE_CERTIFICATE_PASSWORD` | Password for the `.p12`. |
| `APPLE_SIGNING_IDENTITY` | `Developer ID Application: PagerOS (TEAM_ID)`. |
| `APPLE_ID` | Apple ID used for notarization. |
| `APPLE_PASSWORD` | App-specific password for that Apple ID. |
| `APPLE_TEAM_ID` | Apple developer team id. |

When all six are present, `tauri-action` codesigns the `.app`, builds the `.dmg`,
notarizes it via `notarytool`, and staples the ticket. When any are missing the
build still succeeds but produces an unsigned dmg (useful for snapshot builds).

## Windows signing (optional, not yet wired)

The `msi` and `nsis` bundles are unsigned. To codesign them, add a
`tauri.windows.certificateThumbprint` (and matching certificate in the runner
keychain) or extend the workflow with a `signtool` step. Not blocking for
SIM-008 acceptance.

## Linux

`AppImage` and `deb` outputs are unsigned (typical for these formats). The
download site SHOULD publish a `SHA256SUMS` and detached signature; that work
lives in the download-site repo, not here.

## Updater

`includeUpdaterJson` is `false`. The simulator does not ship an in-app updater
yet. `TAURI_SIGNING_PRIVATE_KEY` / `..._PASSWORD` secrets are placeholders for
when it does.
