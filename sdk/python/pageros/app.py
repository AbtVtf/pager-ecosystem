"""``App`` class and routing decorators (PY-001).

This is the surface an SDK user writes apps against:

.. code-block:: python

    from pageros import App

    app = App(name="hello")

    @app.screen("/")
    def home():
        return {
            "v": 1,
            "id": "scr_home",
            "body": [{"t": "text", "s": "Hello, PagerOS!"}],
        }

    if __name__ == "__main__":
        app.run()

The runtime is intentionally thin: routes are dispatched by ``(method,
path)`` to user-registered handlers, return values are serialised to
canonical CBOR via :mod:`pageros.codec`, and a small stdlib HTTP server
wires it into the dev-server contract used by PY-009 (``python app.py
--host H --port P``). Anything outside the §7 envelope (TLS, signing,
sessions, group events) is delegated to later PY-* tasks.
"""

from __future__ import annotations

import argparse
import inspect
import logging
import sys
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable, Mapping
from urllib.parse import parse_qs, urlsplit

from nacl.signing import SigningKey

from pageros.codec import CborDecodeError, decode_frame, encode_frame
from pageros.encryption import AppKeypair
from pageros.lora_budget import check_frame_size
from pageros.push import (
    DEFAULT_PUSH_RELAY_URL,
    PushConfig,
    PushError,
    PushResult,
    send_push,
)

__all__ = ["App", "Request", "Response"]

log = logging.getLogger("pageros.app")

_CBOR_CONTENT_TYPE = "application/cbor"
_CBOR_ACCEPT = "application/cbor; pagerOS=1"

Handler = Callable[..., Any]


# ---------------------------------------------------------------------------
# Request / response value objects
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Request:
    """The decoded request seen by a handler.

    Handlers may declare zero or one positional parameter. With one
    parameter, the SDK passes a ``Request`` instance. With zero, the
    SDK invokes the handler with no arguments — the common case for
    static GET screens.

    ``body`` is the CBOR-decoded request body for ``application/cbor``
    requests; ``None`` for empty bodies; raw ``bytes`` when the body is
    present but the content type is not CBOR. ``query`` is a flat
    ``{key: [values]}`` dict from :func:`urllib.parse.parse_qs`.
    """

    method: str
    path: str
    query: dict[str, list[str]]
    headers: Mapping[str, str]
    body: Any


@dataclass(frozen=True)
class Response:
    """Explicit response envelope.

    Handlers usually return a Frame dict (or ``None`` for 204). Returning
    a ``Response`` lets a handler control the HTTP status, send extra
    headers, or emit a raw byte body without CBOR encoding.
    """

    status: int = 200
    body: Any = None  # dict → CBOR-encoded; bytes → raw; None → empty
    headers: Mapping[str, str] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# App
# ---------------------------------------------------------------------------


class App:
    """A PagerOS app: routes + an HTTP runtime.

    Parameters mirror the manifest fields that affect runtime behaviour:

    - ``name`` — human-readable label used in logs and the default
      ``User-Agent``-style identifier.
    - ``lora_compatible`` — when true, the SDK runs every outbound Frame
      through :func:`pageros.lora_budget.check_frame_size` and logs a
      warning when the encoded size exceeds the 200 B LoRa budget.
    """

    def __init__(
        self,
        name: str | None = None,
        *,
        lora_compatible: bool = False,
        app_id: str | None = None,
        signing_key: SigningKey | None = None,
        keypair: AppKeypair | None = None,
        push_relay_url: str = DEFAULT_PUSH_RELAY_URL,
    ) -> None:
        self.name = name or "pageros-app"
        self.lora_compatible = lora_compatible
        self._routes: dict[tuple[str, str], Handler] = {}

        # Push-relay configuration (PY-006). All four fields are optional at
        # construction time; ``push()`` raises if any required piece is
        # missing when called. This lets apps that never call push() avoid
        # generating keys they don't need (e.g. read-only screen apps).
        self.app_id = app_id
        self.signing_key = signing_key
        self.keypair = keypair
        self.push_relay_url = push_relay_url

        # Per-(app, device) AEAD counter store for crypto-suite.md §1.2.
        # In-process only — apps with cross-reboot durability needs should
        # subclass and persist this map, or pass an explicit ``counter`` to
        # :meth:`push`.
        self._push_counters: dict[bytes, int] = {}
        self._push_counter_lock = threading.Lock()

    # ----- registration -----

    def screen(self, path: str) -> Callable[[Handler], Handler]:
        """Register a ``GET`` handler — the common case for screens."""
        return self.handler(path, method="GET")

    def handler(
        self,
        path: str,
        *,
        method: str = "POST",
    ) -> Callable[[Handler], Handler]:
        """Register a handler for ``(method, path)``.

        Default method is ``POST`` so widget submissions (form, button,
        list item) land here without ceremony. ``method`` is normalised
        to uppercase; only ``GET`` and ``POST`` are used by v1 devices
        (spec §7.1) but the dispatcher does not reject other verbs so
        tests can exercise them.
        """
        method_norm = method.upper()
        if not path.startswith("/"):
            raise ValueError(f"route path must start with '/': {path!r}")

        def decorate(fn: Handler) -> Handler:
            key = (method_norm, path)
            if key in self._routes:
                raise ValueError(
                    f"duplicate route registration for {method_norm} {path}"
                )
            self._routes[key] = fn
            return fn

        return decorate

    # ----- introspection -----

    @property
    def routes(self) -> dict[tuple[str, str], Handler]:
        """Read-only view of registered routes (copy)."""
        return dict(self._routes)

    # ----- dispatch -----

    def dispatch(
        self,
        method: str,
        path: str,
        *,
        body: bytes | None = None,
        headers: Mapping[str, str] | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        """Pure dispatch — no sockets, no threads.

        Returns ``(status, headers, body_bytes)``. The HTTP server uses
        this internally; tests should drive routes through this entry
        point instead of standing up a real server.
        """
        method_norm = method.upper()
        url = urlsplit(path)
        route_path = url.path
        query = parse_qs(url.query, keep_blank_values=True)
        header_map: Mapping[str, str] = headers or {}

        handler = self._routes.get((method_norm, route_path))
        if handler is None:
            # Distinguish 405 (path exists for another method) from 404
            # to give better feedback in dev. Devices only care about
            # status; the body is the per-§7.4 error frame.
            other_methods = sorted(
                m for (m, p) in self._routes if p == route_path
            )
            if other_methods:
                return self._error_response(
                    405,
                    f"Method not allowed: {method_norm} {route_path}",
                    extra_headers={"Allow": ", ".join(other_methods)},
                )
            return self._error_response(
                404, f"Not found: {method_norm} {route_path}"
            )

        decoded_body, body_decode_error = self._decode_body(body, header_map)
        if body_decode_error is not None:
            return self._error_response(400, body_decode_error)

        request = Request(
            method=method_norm,
            path=route_path,
            query=query,
            headers=header_map,
            body=decoded_body,
        )

        try:
            result = self._invoke(handler, request)
        except Exception:  # pragma: no cover - exercised by tests
            log.exception("handler %s %s raised", method_norm, route_path)
            return self._error_response(500, "Server error")

        return self._build_response(result, route_label=f"{method_norm} {route_path}")

    # ----- push -----

    def push(
        self,
        device_id: bytes | str,
        payload: Any,
        *,
        counter: int | None = None,
        timeout: float | None = 10.0,
    ) -> PushResult:
        """Send a notification to ``device_id`` via the Push Relay (PY-006).

        Returns a :class:`pageros.push.PushResult` on HTTP 202. Raises
        :class:`pageros.push.PushRejected` for 4xx (auth, unknown app,
        rate limit, ...) and :class:`pageros.push.PushUnavailable` for
        5xx, malformed responses, or connection failures.

        ``device_id`` accepts the raw 32-byte X25519 device pubkey or its
        base64url (padded or unpadded) string form. ``payload`` is either
        a CBOR-encodable Python value (encoded via :func:`pageros.codec.encode_frame`)
        or pre-encoded ``bytes``.

        ``counter`` should be a monotonic per-device value (crypto-suite.md
        §1.2 forbids random nonces; the app must persist the counter for
        cross-reboot uniqueness). When omitted, the app's in-process
        :attr:`_push_counters` table is used — adequate for one-shot
        scripts but **not** safe across restarts. Long-running apps should
        manage their own counter and pass it explicitly.
        """
        config = self._push_config()
        device_key = self._device_key_from(device_id)
        resolved_counter = (
            counter if counter is not None else self._next_counter(device_key)
        )
        return send_push(
            config,
            device_id,
            payload,
            counter=resolved_counter,
            timeout=timeout,
        )

    def _push_config(self) -> PushConfig:
        missing = [
            name for name, value in (
                ("app_id", self.app_id),
                ("signing_key", self.signing_key),
                ("keypair", self.keypair),
            )
            if value is None
        ]
        if missing:
            raise PushError(
                "app.push() requires "
                + ", ".join(missing)
                + " on the App constructor"
            )
        return PushConfig(
            app_id=self.app_id,  # type: ignore[arg-type]
            signing_key=self.signing_key,  # type: ignore[arg-type]
            keypair=self.keypair,  # type: ignore[arg-type]
            relay_url=self.push_relay_url,
        )

    @staticmethod
    def _device_key_from(device_id: bytes | str) -> bytes:
        if isinstance(device_id, (bytes, bytearray, memoryview)):
            return bytes(device_id)
        return device_id.encode("ascii", errors="strict")

    def _next_counter(self, device_key: bytes) -> int:
        with self._push_counter_lock:
            counter = self._push_counters.get(device_key, 0)
            self._push_counters[device_key] = counter + 1
            return counter

    # ----- HTTP runtime -----

    def run(
        self,
        host: str | None = None,
        port: int | None = None,
        argv: list[str] | None = None,
    ) -> None:
        """Block on a stdlib HTTP server bound to ``host:port``.

        When ``host`` / ``port`` are omitted, parse them from ``argv`` so
        the dev server (PY-009) can spawn an app module with
        ``--host``/``--port`` flags. Default bind is ``127.0.0.1:8000``.
        """
        resolved_host, resolved_port = self._resolve_address(host, port, argv)
        handler_cls = _make_handler_class(self)
        server = ThreadingHTTPServer((resolved_host, resolved_port), handler_cls)
        log.info(
            "pageros app %r listening on http://%s:%d",
            self.name,
            resolved_host,
            resolved_port,
        )
        try:
            server.serve_forever()
        except KeyboardInterrupt:  # pragma: no cover - SIGINT path
            pass
        finally:
            server.server_close()

    # ----- helpers -----

    def _invoke(self, handler: Handler, request: Request) -> Any:
        try:
            params = inspect.signature(handler).parameters
        except (TypeError, ValueError):
            # Builtins / C-implemented callables: assume one positional.
            return handler(request)
        positional = [
            p for p in params.values()
            if p.kind in (
                inspect.Parameter.POSITIONAL_ONLY,
                inspect.Parameter.POSITIONAL_OR_KEYWORD,
            )
        ]
        if positional:
            return handler(request)
        return handler()

    def _decode_body(
        self,
        body: bytes | None,
        headers: Mapping[str, str],
    ) -> tuple[Any, str | None]:
        if not body:
            return None, None
        ctype = _content_type(headers)
        if ctype.startswith(_CBOR_CONTENT_TYPE):
            try:
                return decode_frame(body), None
            except CborDecodeError as exc:
                return None, f"Bad request: malformed CBOR body ({exc})"
        # Non-CBOR bodies are surfaced as raw bytes; the handler can
        # decide what to do with them.
        return body, None

    def _build_response(
        self,
        result: Any,
        *,
        route_label: str,
    ) -> tuple[int, dict[str, str], bytes]:
        if isinstance(result, Response):
            status = result.status
            body = result.body
            extra_headers = dict(result.headers)
        else:
            status = 200
            body = result
            extra_headers = {}

        if body is None:
            # Per spec §7.5: empty response body MUST be 204.
            if status == 200:
                status = 204
            return status, extra_headers, b""

        if isinstance(body, (bytes, bytearray, memoryview)):
            extra_headers.setdefault("Content-Type", _CBOR_CONTENT_TYPE)
            return status, extra_headers, bytes(body)

        # Assume a Frame dict (or any CBOR-encodable value) — encode it
        # canonically. Any TypeError surfaces as 500; we let it bubble
        # so the test suite sees the real cause during dev.
        encoded = encode_frame(body)
        check_frame_size(
            encoded,
            lora_compatible=self.lora_compatible,
            frame_label=route_label,
        )
        extra_headers.setdefault("Content-Type", _CBOR_CONTENT_TYPE)
        return status, extra_headers, encoded

    def _error_response(
        self,
        status: int,
        message: str,
        *,
        extra_headers: Mapping[str, str] | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        # Build a locally-generated error frame matching spec §7.4.1.
        frame = {
            "v": 1,
            "id": "err_local",
            "ttl": 0,
            "title": "Error",
            "body": [{"t": "text", "s": message, "style": "error"}],
        }
        encoded = encode_frame(frame)
        headers = {"Content-Type": _CBOR_CONTENT_TYPE}
        if extra_headers:
            headers.update(extra_headers)
        return status, headers, encoded

    def _resolve_address(
        self,
        host: str | None,
        port: int | None,
        argv: list[str] | None,
    ) -> tuple[str, int]:
        if host is not None and port is not None:
            return host, port
        parser = argparse.ArgumentParser(prog=self.name, add_help=False)
        parser.add_argument("--host", default="127.0.0.1")
        parser.add_argument("--port", type=int, default=8000)
        args, _unknown = parser.parse_known_args(
            argv if argv is not None else sys.argv[1:]
        )
        return host or args.host, port if port is not None else args.port


# ---------------------------------------------------------------------------
# Stdlib HTTP glue
# ---------------------------------------------------------------------------


def _content_type(headers: Mapping[str, str]) -> str:
    for key, value in headers.items():
        if key.lower() == "content-type":
            return value.split(";", 1)[0].strip().lower()
    return ""


def _make_handler_class(app: App) -> type[BaseHTTPRequestHandler]:
    """Build a per-app subclass binding the app instance for dispatch."""

    class PagerOSRequestHandler(BaseHTTPRequestHandler):
        server_version = f"PagerOS-SDK/{app.name}"

        # Quiet the default access log; the SDK has its own structured log.
        def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
            log.debug("%s - %s", self.address_string(), format % args)

        def do_GET(self) -> None:  # noqa: N802
            self._handle("GET")

        def do_POST(self) -> None:  # noqa: N802
            self._handle("POST")

        def _handle(self, method: str) -> None:
            length_header = self.headers.get("Content-Length", "0") or "0"
            try:
                length = max(0, int(length_header))
            except ValueError:
                length = 0
            body = self.rfile.read(length) if length else b""
            request_headers = {k: v for k, v in self.headers.items()}
            status, headers, payload = app.dispatch(
                method,
                self.path,
                body=body,
                headers=request_headers,
            )
            self.send_response(status)
            for name, value in headers.items():
                self.send_header(name, value)
            if payload:
                self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            if payload:
                self.wfile.write(payload)

    return PagerOSRequestHandler
