#!/usr/bin/env bash
# Generate a self-signed TLS cert+key for local push-relay development.
# Output: ./certs/cert.pem + ./certs/key.pem (relative to push-relay/).
#
# NEVER use these certs in production. PUSH-009 will document the prod cert path.
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p certs

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout certs/key.pem \
  -out certs/cert.pem \
  -days 365 \
  -subj "/CN=push-relay.local" \
  -addext "subjectAltName=DNS:push-relay.local,DNS:localhost,IP:127.0.0.1"

chmod 600 certs/key.pem
echo "Wrote certs/cert.pem and certs/key.pem (self-signed, dev only)."
