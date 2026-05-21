#!/usr/bin/env bash
# Build a Debian package for the exit-node binary.
#
# Usage:
#   exit-node/packaging/build-deb.sh                   # default: host arch, dev version
#   ARCH=arm64  exit-node/packaging/build-deb.sh
#   ARCH=armhf  exit-node/packaging/build-deb.sh       # 32-bit ARM (Raspberry Pi 3 + below)
#   VERSION=0.1.0 ARCH=arm64 exit-node/packaging/build-deb.sh
#
# Produces dist/pageros-exit-node_<version>_<arch>.deb relative to the
# exit-node module root.
#
# Requirements: go (cross-compile; CGO disabled), dpkg-deb,
#   fakeroot (optional but recommended on non-root dpkg builds).

set -euo pipefail

# --- paths -----------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXIT_NODE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEB_SRC="$SCRIPT_DIR/deb"
SYSTEMD_SRC="$SCRIPT_DIR/systemd/exit-node.service"

VERSION="${VERSION:-0.1.0-dev}"
ARCH="${ARCH:-$(dpkg --print-architecture 2>/dev/null || echo amd64)}"

# Map Debian arch → Go GOARCH/GOARM.
case "$ARCH" in
    amd64) GOARCH=amd64; GOARM= ;;
    arm64) GOARCH=arm64; GOARM= ;;
    armhf) GOARCH=arm;   GOARM=7 ;;
    armel) GOARCH=arm;   GOARM=5 ;;
    *) echo "error: unsupported ARCH=$ARCH (want amd64|arm64|armhf|armel)" >&2; exit 2 ;;
esac

OUT_DIR="$EXIT_NODE_DIR/dist"
STAGE="$(mktemp -d -t exit-node-deb-XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$OUT_DIR"

echo "==> Cross-compiling exit-node for $ARCH (GOARCH=$GOARCH${GOARM:+ GOARM=$GOARM})"
(
    cd "$EXIT_NODE_DIR"
    # `env` lets us conditionally pass GOARM only when set, without
    # shell-parameter expansion landing GOARM=7 as a standalone token
    # in front of `go build`.
    env CGO_ENABLED=0 GOOS=linux GOARCH="$GOARCH" ${GOARM:+GOARM="$GOARM"} \
        go build -trimpath -ldflags '-s -w' \
        -o "$STAGE/usr/bin/exit-node" \
        ./cmd/exit-node
)

# --- assemble staging tree -------------------------------------------------

mkdir -p "$STAGE/DEBIAN"
mkdir -p "$STAGE/etc/pager-ecosystem"
mkdir -p "$STAGE/lib/systemd/system"

# Control file: substitute version + arch.
sed -e "s/__VERSION__/$VERSION/" -e "s/__ARCH__/$ARCH/" \
    "$DEB_SRC/DEBIAN/control.tmpl" > "$STAGE/DEBIAN/control"

cp "$DEB_SRC/DEBIAN/postinst"   "$STAGE/DEBIAN/postinst"
cp "$DEB_SRC/DEBIAN/prerm"      "$STAGE/DEBIAN/prerm"
cp "$DEB_SRC/DEBIAN/conffiles"  "$STAGE/DEBIAN/conffiles"
chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm"
chmod 0644 "$STAGE/DEBIAN/control" "$STAGE/DEBIAN/conffiles"

cp "$EXIT_NODE_DIR/configs/exit-node.example.yaml" "$STAGE/etc/pager-ecosystem/exit-node.yaml"
cp "$SYSTEMD_SRC" "$STAGE/lib/systemd/system/exit-node.service"
chmod 0755 "$STAGE/usr/bin/exit-node"

# --- build -----------------------------------------------------------------

OUT="$OUT_DIR/pageros-exit-node_${VERSION}_${ARCH}.deb"

if command -v fakeroot >/dev/null 2>&1; then
    fakeroot dpkg-deb --build "$STAGE" "$OUT" >/dev/null
else
    # Without fakeroot, file ownerships in the resulting .deb reflect
    # the build user. dpkg-deb still warns; the package installs fine.
    dpkg-deb --build "$STAGE" "$OUT" >/dev/null
fi

echo "==> Built $OUT"
ls -l "$OUT"
dpkg-deb --info "$OUT" | sed 's/^/    /'
