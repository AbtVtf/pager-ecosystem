# syntax=docker/dockerfile:1.7
FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/push-relay ./cmd/push-relay

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/push-relay /push-relay
EXPOSE 8443
USER nonroot:nonroot
ENTRYPOINT ["/push-relay"]
