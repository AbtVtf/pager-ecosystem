"""PROTO-005 conformance adapter for the Python SDK (PY-012).

A thin, CI-only HTTP surface that satisfies the contract documented in
``protocol/conformance/README.md`` so the cross-language conformance
runner can drive every ``protocol/test-vectors/ui/`` case against this
SDK.

The two endpoints (``POST /conformance/encode`` and ``POST
/conformance/decode``) are intentionally tiny wrappers that:

* feed a JSON ``input`` straight into :func:`pageros.codec.encode_frame`
  (canonical CBOR — the helper already understands the descriptor's
  ``{"$bytes": "<hex>"}`` marker so no pre-processing is needed); and
* decode CBOR bytes through :func:`pageros.codec.decode_frame`, then
  normalize the structural result into the conformance-runner JSON
  form: unknown widget tags become ``$render_placeholder + raw_widget``,
  unknown event tags in ``subscribe`` split into
  ``subscribe_effective`` / ``subscribe_dropped``, unknown top-level /
  per-widget fields are silently dropped, and Python ``bytes`` are
  surfaced as ``"$bytes:<hex>"`` strings (the on-the-wire JSON marker
  the runner expects for decoded byte-string fields).

This adapter is *not* required at runtime — production deployments may
exclude it. Importing the module costs nothing beyond loading the
stdlib HTTP server; the constants and helpers are small.

CLI usage (used by ``tests/test_conformance.py`` and by humans poking
at the runner against the SDK directly):

.. code-block:: bash

    python -m pageros.conformance --host 127.0.0.1 --port 8080
"""

from __future__ import annotations

import argparse
import http.server
import json
import socketserver
import sys
import threading
from typing import Any, Iterable

from pageros.codec import CborDecodeError, decode_frame, encode_frame
from pageros.widgets import EVENT_TAGS, WIDGET_TAGS

__all__ = [
    "ConformanceError",
    "ERR_INVALID_FRAME_SHAPE",
    "ERR_UNKNOWN_EVENT_DROPPED",
    "ERR_UNKNOWN_MAJOR_VERSION",
    "SUPPORTED_MAJOR_VERSION",
    "conformance_decode",
    "conformance_encode",
    "make_handler",
    "main",
    "marshal_bytes_to_string",
    "serve",
    "unmarshal_bytes_marker",
]

# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

# Conformance error classes that map onto the runner's ``expect_error``
# checks. New negative vectors that introduce new classes can extend
# this list in lockstep with ``protocol/conformance/README.md``.
ERR_INVALID_FRAME_SHAPE = "invalid_frame_shape"
ERR_UNKNOWN_MAJOR_VERSION = "unknown_major_version"
ERR_UNKNOWN_EVENT_DROPPED = "unknown_event_dropped"

SUPPORTED_MAJOR_VERSION = 1

# Per-widget field allowlists pinned to ``SPEC.md`` §5.3. Decoded
# widgets keep only these keys; anything else is silently dropped
# (spec §9.2 forward-compat rule). The ``t`` field is always preserved
# so renderers can still dispatch by widget type.
_WIDGET_FIELDS: dict[str, frozenset[str]] = {
    "text": frozenset({"t", "s", "style"}),
    "list": frozenset({"t", "items"}),
    "input": frozenset({"t", "name", "label", "type", "value", "max"}),
    "form": frozenset({"t", "action", "method", "fields", "submit"}),
    "button": frozenset({"t", "label", "href", "method", "confirm"}),
    "image": frozenset({"t", "src", "w", "h", "alt"}),
    "map": frozenset({"t", "lat", "lon", "zoom", "markers"}),
    "notification": frozenset({"t", "s", "level"}),
    "presence_list": frozenset({"t", "group_id", "members"}),
    "chat": frozenset({"t", "group_id", "messages", "compose"}),
}

# Sub-shape allowlists for nested items inside known widgets. Same
# spec section as ``_WIDGET_FIELDS``.
_LIST_ITEM_FIELDS = frozenset({"label", "href", "sub", "method"})
_MAP_MARKER_FIELDS = frozenset({"lat", "lon", "label"})
_PRESENCE_MEMBER_FIELDS = frozenset({"id", "name", "online"})
_CHAT_MESSAGE_FIELDS = frozenset({"from", "s", "ts"})
_CHAT_COMPOSE_FIELDS = frozenset({"name", "submit"})

# Top-level Frame field allowlist (``SPEC.md`` §5.2).
_FRAME_FIELDS = frozenset(
    {"v", "id", "title", "ttl", "body", "actions", "subscribe", "subscribe_groups"}
)

_WIDGET_NAME_BY_NUM: dict[int, str] = {num: name for name, num in WIDGET_TAGS.items()}
_EVENT_NAME_BY_NUM: dict[int, str] = {num: name for name, num in EVENT_TAGS.items()}


# --------------------------------------------------------------------------- #
# Errors
# --------------------------------------------------------------------------- #


class ConformanceError(Exception):
    """Raised by :func:`conformance_decode` to signal a refusal.

    Carries the runner-facing error class string. The HTTP handler
    surfaces this as ``HTTP 400 { "error": <class>, "message": ... }``.
    """

    def __init__(self, error: str, message: str = "") -> None:
        super().__init__(message or error)
        self.error = error
        self.message = message


# --------------------------------------------------------------------------- #
# Byte-marker helpers
# --------------------------------------------------------------------------- #


def unmarshal_bytes_marker(value: Any) -> Any:
    """Recursively expand ``{"$bytes": "<hex>"}`` markers to ``bytes``.

    ``conformance_encode`` does not actually need this — the codec
    handles the marker map natively on the leaf — but the helper is
    exported so tests / sibling tools can normalize input shapes the
    same way.
    """
    if isinstance(value, dict):
        if (
            len(value) == 1
            and "$bytes" in value
            and isinstance(value["$bytes"], str)
        ):
            return bytes.fromhex(value["$bytes"])
        return {k: unmarshal_bytes_marker(v) for k, v in value.items()}
    if isinstance(value, list):
        return [unmarshal_bytes_marker(v) for v in value]
    return value


def marshal_bytes_to_string(value: Any) -> Any:
    """Recursively replace ``bytes`` with ``"$bytes:<hex>"`` strings.

    Mirrors the on-the-wire JSON form the runner compares against for
    decoded byte-string fields (e.g. NFC tag ``uid`` payloads, opaque
    blobs inside unknown widgets).
    """
    if isinstance(value, (bytes, bytearray, memoryview)):
        return f"$bytes:{bytes(value).hex()}"
    if isinstance(value, dict):
        return {k: marshal_bytes_to_string(v) for k, v in value.items()}
    if isinstance(value, list):
        return [marshal_bytes_to_string(v) for v in value]
    return value


# --------------------------------------------------------------------------- #
# Tag resolution
# --------------------------------------------------------------------------- #


def _widget_name(tag: Any) -> str | None:
    if isinstance(tag, int) and not isinstance(tag, bool):
        return _WIDGET_NAME_BY_NUM.get(tag)
    if isinstance(tag, str):
        return tag if tag in WIDGET_TAGS else None
    return None


def _event_known(tag: Any) -> bool:
    if isinstance(tag, int) and not isinstance(tag, bool):
        return tag in _EVENT_NAME_BY_NUM
    if isinstance(tag, str):
        return tag in EVENT_TAGS
    return False


# --------------------------------------------------------------------------- #
# Encode adapter
# --------------------------------------------------------------------------- #


def conformance_encode(name: str, input_value: Any) -> bytes:
    """Encode an ``input`` payload from a descriptor as canonical CBOR.

    The name is accepted purely so the HTTP handler can echo it on
    error paths; the encoder MUST NOT branch on it (per the conformance
    contract).
    """
    _ = name  # explicit no-use to silence linters
    return encode_frame(input_value)


# --------------------------------------------------------------------------- #
# Decode adapter
# --------------------------------------------------------------------------- #


def conformance_decode(cbor_bytes: bytes) -> Any:
    """Decode CBOR through the SDK + normalize to the runner JSON shape.

    Raises :class:`ConformanceError` for the negative-vector cases:
    non-map top-level, unknown major version, or top-level inbound
    event with an unknown ``type``.
    """
    try:
        decoded = decode_frame(cbor_bytes)
    except CborDecodeError as exc:
        raise ConformanceError(
            ERR_INVALID_FRAME_SHAPE, f"malformed CBOR: {exc}"
        ) from exc

    if not isinstance(decoded, dict):
        raise ConformanceError(
            ERR_INVALID_FRAME_SHAPE,
            f"top-level CBOR value is {type(decoded).__name__}, expected map",
        )

    # Inbound event (server→device) — distinct from a Frame because it
    # carries `type` (event discriminator) instead of `v` (Frame
    # version). Spec §6.3: unknown event types are dropped.
    if "v" not in decoded and "type" in decoded:
        if not _event_known(decoded.get("type")):
            raise ConformanceError(
                ERR_UNKNOWN_EVENT_DROPPED,
                f"unknown event type {decoded.get('type')!r}",
            )
        # Known events pass through structurally — no v1 vectors
        # exercise this path on the decoder side, but it keeps the
        # adapter symmetrical with the encode side.
        return marshal_bytes_to_string(decoded)

    if "v" not in decoded:
        raise ConformanceError(
            ERR_INVALID_FRAME_SHAPE,
            "frame missing required 'v' field",
        )

    version = decoded.get("v")
    if version != SUPPORTED_MAJOR_VERSION:
        raise ConformanceError(
            ERR_UNKNOWN_MAJOR_VERSION,
            f"frame declares major version {version!r}; SDK supports v{SUPPORTED_MAJOR_VERSION}",
        )

    return _normalize_frame(decoded)


def _normalize_frame(frame: dict) -> dict:
    """Drop unknown top-level fields and walk known nested shapes."""
    out: dict[str, Any] = {}
    for key, value in frame.items():
        if key not in _FRAME_FIELDS:
            # Spec §9.2: silently ignore unknown top-level fields.
            continue
        if key == "body" and isinstance(value, list):
            out[key] = [_normalize_widget(w) for w in value]
        elif key == "actions" and isinstance(value, list):
            # Actions have no widget-style ``t`` discriminator (SPEC
            # §5.2); pass through with bytes marshaled but otherwise
            # untouched. No vector exercises decode of unknown action
            # fields today.
            out[key] = marshal_bytes_to_string(value)
        elif key == "subscribe" and isinstance(value, list):
            effective, dropped = _split_subscribe(value)
            if dropped:
                out["subscribe_effective"] = effective
                out["subscribe_dropped"] = dropped
            else:
                out[key] = effective
        else:
            out[key] = marshal_bytes_to_string(value)
    return out


def _split_subscribe(values: list) -> tuple[list, list]:
    effective: list = []
    dropped: list = []
    for v in values:
        if _event_known(v):
            effective.append(v)
        else:
            dropped.append(v)
    return effective, dropped


def _normalize_widget(widget: Any) -> Any:
    if not isinstance(widget, dict):
        # Body entries should be widget maps; non-maps survive
        # unchanged (no vector exercises this, but we don't want to
        # silently swallow them either).
        return marshal_bytes_to_string(widget)

    tag = widget.get("t")
    name = _widget_name(tag)
    if name is None:
        return {
            "$render_placeholder": f"[unsupported: {tag}]",
            "raw_widget": marshal_bytes_to_string(widget),
        }
    return _normalize_known_widget(name, widget)


def _normalize_known_widget(name: str, widget: dict) -> dict:
    allowed = _WIDGET_FIELDS[name]
    out: dict[str, Any] = {}
    for key, value in widget.items():
        if key not in allowed:
            continue
        out[key] = _normalize_widget_field(name, key, value)
    return out


def _normalize_widget_field(widget_name: str, field: str, value: Any) -> Any:
    if widget_name == "list" and field == "items" and isinstance(value, list):
        return [_filter_keys(item, _LIST_ITEM_FIELDS) for item in value]
    if widget_name == "form" and field == "fields" and isinstance(value, list):
        return [_normalize_widget(item) for item in value]
    if widget_name == "map" and field == "markers" and isinstance(value, list):
        return [_filter_keys(item, _MAP_MARKER_FIELDS) for item in value]
    if (
        widget_name == "presence_list"
        and field == "members"
        and isinstance(value, list)
    ):
        return [_filter_keys(item, _PRESENCE_MEMBER_FIELDS) for item in value]
    if widget_name == "chat" and field == "messages" and isinstance(value, list):
        return [_filter_keys(item, _CHAT_MESSAGE_FIELDS) for item in value]
    if widget_name == "chat" and field == "compose":
        if isinstance(value, dict):
            return _filter_keys(value, _CHAT_COMPOSE_FIELDS)
        return marshal_bytes_to_string(value)
    return marshal_bytes_to_string(value)


def _filter_keys(value: Any, allowed: Iterable[str]) -> Any:
    if not isinstance(value, dict):
        return marshal_bytes_to_string(value)
    return {
        k: marshal_bytes_to_string(v) for k, v in value.items() if k in allowed
    }


# --------------------------------------------------------------------------- #
# HTTP surface
# --------------------------------------------------------------------------- #


_ENCODE_PATH = "/conformance/encode"
_DECODE_PATH = "/conformance/decode"


def make_handler() -> type[http.server.BaseHTTPRequestHandler]:
    """Return a ``BaseHTTPRequestHandler`` subclass implementing the contract.

    Stateless — every request stands on its own. The handler suppresses
    the default access log so test runs don't drown in HTTP noise.
    """

    class _Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
            return

        def do_POST(self) -> None:  # noqa: N802
            path = self.path.rstrip("/")
            length = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(length) if length else b""
            if path == _ENCODE_PATH:
                self._handle_encode(body)
            elif path == _DECODE_PATH:
                self._handle_decode(body)
            else:
                self._send_json(404, {"error": "not_found", "message": self.path})

        # ----- /conformance/encode -----

        def _handle_encode(self, body: bytes) -> None:
            try:
                req = json.loads(body.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError) as exc:
                self._send_json(
                    400, {"error": "bad_request", "message": f"invalid JSON: {exc}"}
                )
                return
            name = req.get("name")
            if not isinstance(name, str):
                self._send_json(
                    400, {"error": "bad_request", "message": "missing 'name'"}
                )
                return
            if "input" not in req:
                self._send_json(
                    400, {"error": "bad_request", "message": "missing 'input'"}
                )
                return
            try:
                cbor_bytes = conformance_encode(name, req["input"])
            except Exception as exc:  # encode failures are real bugs
                self._send_json(
                    400, {"error": "encode_failed", "message": str(exc)}
                )
                return
            self._send_cbor(cbor_bytes, canonical=True)

        # ----- /conformance/decode -----

        def _handle_decode(self, body: bytes) -> None:
            try:
                value = conformance_decode(body)
            except ConformanceError as exc:
                self._send_json(
                    400, {"error": exc.error, "message": exc.message}
                )
                return
            self._send_json(200, value)

        # ----- response helpers -----

        def _send_json(self, status: int, body: Any) -> None:
            payload = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def _send_cbor(self, body: bytes, *, canonical: bool) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "application/cbor")
            if canonical:
                # Pinned: codec.encode_frame produces RFC 8949 §4.2
                # canonical output (codec.py module docstring), so the
                # runner can byte-equal-compare against the vector's
                # expected_cbor_hex without falling back to structural
                # compare.
                self.send_header("X-PagerOS-Canonical", "true")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return _Handler


class _ConformanceServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(host: str = "127.0.0.1", port: int = 0) -> _ConformanceServer:
    """Build a ready-to-serve HTTP server bound to ``host:port``.

    ``port=0`` lets the OS pick a free port — handy in tests that want
    the actual bound port via ``server.server_address``.
    """
    return _ConformanceServer((host, port), make_handler())


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m pageros.conformance",
        description="PROTO-005 conformance HTTP adapter for the Python SDK.",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args(argv)

    server = serve(args.host, args.port)
    bound_host, bound_port = server.server_address[0], server.server_address[1]
    print(
        f"pageros conformance adapter listening on http://{bound_host}:{bound_port}",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":  # pragma: no cover - CLI entry
    sys.exit(main())


# --------------------------------------------------------------------------- #
# Background-thread convenience for tests
# --------------------------------------------------------------------------- #


def serve_in_background(
    host: str = "127.0.0.1", port: int = 0
) -> tuple[_ConformanceServer, threading.Thread]:
    """Spin up the server on a daemon thread; return ``(server, thread)``.

    Caller closes via ``server.shutdown()`` then ``server.server_close()``.
    """
    server = serve(host, port)
    thread = threading.Thread(
        target=server.serve_forever, name="pageros-conformance", daemon=True
    )
    thread.start()
    return server, thread
