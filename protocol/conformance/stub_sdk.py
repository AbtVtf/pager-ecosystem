"""Stub PagerOS SDK that implements the PROTO-005 conformance contract.

The stub looks vectors up by name (or by content hash for /decode) under a
given vectors directory and serves the precomputed canonical CBOR / decoded
JSON / refusal as documented in ../README.md. It is *only* a fixture for the
runner's own self-test — real SDK conformance endpoints implement the
contract over their actual encoder/decoder.

Usage:
    server = StubSDK(vectors_dir=Path("protocol/test-vectors/ui"))
    server.serve_forever()  # blocking; usually run on a background thread

The stub accepts an optional `break_vector` argument to corrupt a single
vector's response, used by the self-test to confirm the runner reports
failures correctly.
"""

from __future__ import annotations

import http.server
import json
import threading
from pathlib import Path
from typing import Optional


def _load_vectors(root: Path) -> dict[str, dict]:
    index = json.loads((root / "index.json").read_text())
    out: dict[str, dict] = {}
    for entry in index["vectors"]:
        desc_path = root / entry["files"]["descriptor"]
        cbor_path = root / entry["files"]["cbor"]
        d = json.loads(desc_path.read_text())
        d["_cbor"] = cbor_path.read_bytes()
        out[d["name"]] = d
    return out


def _by_input_hex(vectors: dict[str, dict]) -> dict[str, dict]:
    """Map input-bytes (hex) to vector descriptors for /decode lookups.

    `decode_only` and `negative` vectors carry `input_cbor_hex`; `encode`
    vectors are matched by their `expected_cbor_hex` so the runner can also
    sanity-check round-trips if it ever decides to.
    """
    out: dict[str, dict] = {}
    for v in vectors.values():
        key = v.get("input_cbor_hex") or v.get("expected_cbor_hex")
        if key:
            out[key.lower()] = v
    return out


class _Handler(http.server.BaseHTTPRequestHandler):
    server: "StubSDK"

    def log_message(self, format: str, *args) -> None:  # noqa: A003 — match base API
        # Silence default access log; tests don't need the noise.
        return

    def _send_json(self, status: int, body: dict) -> None:
        payload = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _send_cbor(self, body: bytes, canonical: bool) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "application/cbor")
        if canonical:
            self.send_header("X-PagerOS-Canonical", "true")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 — http.server convention
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        if self.path.rstrip("/") == "/conformance/encode":
            self._handle_encode(body)
        elif self.path.rstrip("/") == "/conformance/decode":
            self._handle_decode(body)
        else:
            self._send_json(404, {"error": "not_found", "message": self.path})

    def _handle_encode(self, body: bytes) -> None:
        try:
            req = json.loads(body.decode("utf-8"))
        except Exception as exc:
            self._send_json(400, {"error": "bad_request", "message": str(exc)})
            return
        name = req.get("name")
        if not isinstance(name, str):
            self._send_json(400, {"error": "bad_request", "message": "missing name"})
            return
        vec = self.server.vectors.get(name)
        if vec is None:
            self._send_json(404, {"error": "unknown_vector", "message": name})
            return
        if vec["kind"] != "encode":
            self._send_json(
                400,
                {"error": "wrong_kind", "message": f"{name} is {vec['kind']}, not encode"},
            )
            return
        cbor_bytes = vec["_cbor"]
        if self.server.break_vector == name:
            # Corrupt exactly one byte to force a mismatch report.
            cbor_bytes = b"\x00" + cbor_bytes[1:]
        self._send_cbor(cbor_bytes, canonical=True)

    def _handle_decode(self, body: bytes) -> None:
        key = body.hex().lower()
        vec = self.server.by_hex.get(key)
        if vec is None:
            self._send_json(400, {"error": "unknown_vector", "message": key[:80]})
            return
        if vec["kind"] == "negative":
            if self.server.break_vector == vec["name"]:
                # Force an inversion: accept the negative bytes anyway.
                self._send_json(200, {"raw_widget": {}})
                return
            self._send_json(
                400,
                {"error": vec.get("expect_error", "unknown"), "message": vec.get("description", "")},
            )
            return
        if vec["kind"] == "decode_only":
            decoded = vec.get("expected_decoded")
            if self.server.break_vector == vec["name"]:
                decoded = {"unexpected": "shape"}
            self._send_json(200, decoded if decoded is not None else {})
            return
        # encode-kind vector being decoded — return its `input` as a friendly
        # round-trip; the runner doesn't currently exercise this path.
        self._send_json(200, vec.get("input", {}))


class StubSDK(http.server.ThreadingHTTPServer):
    """Tiny conformance-contract-speaking HTTP server backed by precomputed
    vector outputs. Used only by the runner self-test."""

    def __init__(
        self,
        vectors_dir: Path,
        *,
        host: str = "127.0.0.1",
        port: int = 0,
        break_vector: Optional[str] = None,
    ) -> None:
        super().__init__((host, port), _Handler)
        self.vectors = _load_vectors(vectors_dir)
        self.by_hex = _by_input_hex(self.vectors)
        self.break_vector = break_vector

    @property
    def endpoint(self) -> str:
        host, port = self.server_address[0], self.server_address[1]
        if host in ("0.0.0.0", "::"):
            host = "127.0.0.1"
        return f"http://{host}:{port}"

    def run_in_background(self) -> threading.Thread:
        t = threading.Thread(target=self.serve_forever, name="stub-sdk", daemon=True)
        t.start()
        return t
