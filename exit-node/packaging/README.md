# exit-node packaging (EXIT-008)

Three deploy paths are supported; pick whichever matches your target.

| Target            | Artifact                              | Build command                                |
|-------------------|---------------------------------------|----------------------------------------------|
| Docker (x86/ARM)  | `pageros/exit-node:<tag>` OCI image   | `docker buildx build ...` (see below)        |
| Debian/Pi (`.deb`)| `pageros-exit-node_<v>_<arch>.deb`    | `ARCH=arm64 packaging/build-deb.sh`              |
| Raspberry Pi image| RPi OS + the `.deb` above             | `pi-gen` overlay; see [Pi image](#pi-image)  |

All three install the same binary and ship the same systemd unit. The
binary is a pure-Go cross-compile (`CGO_ENABLED=0`), so the same source
produces amd64, arm64 (Pi 4/5 64-bit), and armhf (Pi 3 32-bit).

## Docker

### Local single-arch (x86 dev loop)

```sh
cd exit-node
docker build -f packaging/Dockerfile -t pageros/exit-node:dev .
```

Run with the example config and a USB LoRa dongle passed through:

```sh
docker run --rm \
    --device /dev/ttyUSB0 \
    -v "$PWD/configs/exit-node.example.yaml":/etc/pager-ecosystem/exit-node.yaml:ro \
    pageros/exit-node:dev --status
```

### Multi-arch (CI release)

```sh
cd exit-node
docker buildx create --use --name pageros-builder 2>/dev/null || true
docker buildx build \
    --platform linux/amd64,linux/arm64,linux/arm/v7 \
    -f packaging/Dockerfile \
    -t ghcr.io/pageros/exit-node:0.1.0 \
    --push \
    .
```

`linux/arm/v7` covers Raspberry Pi 3 and earlier; `linux/arm64` is Pi 4
+ 5 64-bit. The image base is `gcr.io/distroless/static-debian12`
which publishes the same three architectures, so no per-arch tweaks
are needed.

### Compose

`packaging/docker-compose.yaml` is a convenience for single-host x86 dev.
Copy your real config to `packaging/exit-node.local.yaml` and run
`docker compose -f packaging/docker-compose.yaml up`.

## Debian package (`.deb`)

```sh
cd exit-node
ARCH=amd64 packaging/build-deb.sh           # for x86 servers
ARCH=arm64 packaging/build-deb.sh           # for Raspberry Pi 4/5 64-bit
ARCH=armhf packaging/build-deb.sh           # for Raspberry Pi 3 / Zero 2 W
VERSION=0.1.0 ARCH=arm64 packaging/build-deb.sh
```

Output lands in `exit-node/dist/`. The `.deb` contains:

```
/usr/bin/exit-node                          # binary
/etc/pager-ecosystem/exit-node.yaml         # example config (conffile)
/lib/systemd/system/exit-node.service       # systemd unit
```

Install on a target:

```sh
sudo dpkg -i pageros-exit-node_0.1.0_arm64.deb
sudo apt --fix-broken install               # if any deps missing
sudo nano /etc/pager-ecosystem/exit-node.yaml
sudo systemctl restart exit-node
journalctl -u exit-node -f
```

The postinst creates a dedicated `pageros-exit` system user and adds
it to the `dialout` group so the service can open `/dev/ttyUSB*`
without root. The systemd unit is locked down per `systemd-analyze
security`; see `packaging/systemd/exit-node.service` for the full hardening
profile.

## Pi image

There is no separate "PagerOS Pi image" build pipeline in this repo —
the supported flow is **standard Raspberry Pi OS + our `.deb`**, which
gives operators a familiar base they can patch and keeps us out of the
image-distribution business.

Two equivalent ways to bake it:

**Option A — pi-gen overlay (CI-friendly).**
[`pi-gen`](https://github.com/RPi-Distro/pi-gen) is the same tool the
RPi Foundation uses to build official images. Add a stage that copies
the `.deb` and installs it on first boot:

```sh
# In your pi-gen stage's package list:
echo "pageros-exit-node" >> stage-pageros/00-pageros-install/00-packages

# Or drop the .deb directly into the image and install in postrun:
cp dist/pageros-exit-node_0.1.0_arm64.deb \
   stage-pageros/00-pageros-install/files/
```

**Option B — flash + install (operator-friendly).**
Flash a stock Raspberry Pi OS Lite (64-bit) image with `rpi-imager`,
boot it, SSH in, and `sudo dpkg -i pageros-exit-node_<v>_arm64.deb`.

Either route ends at the same place: the systemd unit running, the
config waiting for a real `identity_pubkey_hex`, and the USB LoRa
dongle reachable.

## Verification matrix

What this packaging build was verified against in CI/dev:

| Acceptance criterion                          | Verified |
|-----------------------------------------------|---------|
| Docker image builds for `linux/amd64`         | ✅ local docker build |
| Docker image builds for `linux/arm64`         | ⚠️ requires buildx + QEMU; CI to confirm |
| Docker image runs `--version`/`--status`      | ✅ on amd64 host |
| `.deb` builds for amd64 / arm64 / armhf       | ✅ `build-deb.sh` on amd64 host (cross-compile) |
| `.deb` installs cleanly on Raspberry Pi OS    | ⚠️ requires Pi hardware to confirm |
| Pi image boots + finds USB LoRa + runs service| ⚠️ requires Pi hardware to confirm |

The ⚠️ rows are honestly outside what an automated CI without ARM
runners and physical Pi hardware can attest to. The build artifacts
are correct by construction (pure-Go cross-compile, distroless base
that publishes the same arches, systemd unit + dialout membership for
serial-port access); CEO/QA should sign off the on-hardware checks once
they have a target board.
