# Multi-arch container for the PagerOS exit-node binary.
# Build with: docker buildx build --platform linux/amd64,linux/arm64 \
#   -f exit-node/packaging/Dockerfile -t pageros/exit-node:dev exit-node/
#
# EXIT-008 acceptance: image runs on x86 + ARM. Both arches are pure-Go
# cross-compiles (no CGO), so this Dockerfile sets CGO_ENABLED=0 and
# uses TARGETOS/TARGETARCH from buildx to produce a static binary that
# the distroless runtime stage can drop in without an extra libc.

FROM --platform=$BUILDPLATFORM golang:1.22-alpine AS builder

ARG TARGETOS
ARG TARGETARCH
ENV CGO_ENABLED=0 GOOS=$TARGETOS GOARCH=$TARGETARCH

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download

COPY cmd ./cmd
COPY internal ./internal
COPY configs ./configs

RUN go build -trimpath -ldflags '-s -w' -o /out/exit-node ./cmd/exit-node

# Distroless static is multi-arch (amd64, arm64, arm/v7) and ships
# nothing the binary doesn't need — no shell, no package manager.
# `nonroot` provides UID/GID 65532 so the process never runs as root.
FROM gcr.io/distroless/static-debian12:nonroot

LABEL org.opencontainers.image.title="pageros-exit-node"
LABEL org.opencontainers.image.description="PagerOS LoRa <-> HTTPS bridge"
LABEL org.opencontainers.image.source="https://github.com/pageros/pager-ecosystem"
LABEL org.opencontainers.image.licenses="Apache-2.0"

COPY --from=builder /out/exit-node /usr/local/bin/exit-node
COPY --from=builder /src/configs/exit-node.example.yaml /etc/pager-ecosystem/exit-node.yaml

USER nonroot:nonroot
ENTRYPOINT ["/usr/local/bin/exit-node"]
CMD ["--config", "/etc/pager-ecosystem/exit-node.yaml"]
