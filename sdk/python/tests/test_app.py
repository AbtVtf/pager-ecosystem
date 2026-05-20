"""Tests for the PY-001 ``App`` / routing decorators.

The acceptance criterion is that a hello-world app of ≤ 20 LOC returns a
Frame from ``GET /``. The tests below exercise:

- Decorator registration (``@app.screen`` defaults to ``GET``;
  ``@app.handler(path, method=...)`` registers explicit verbs).
- Pure dispatch (no sockets): status codes, content type, canonical
  CBOR body, 404 / 405 / 400 error frames.
- Handler arity flexibility (zero-arg or one-arg).
- CBOR request-body decoding.
- The shipped hello example actually works through ``dispatch()`` and
  is ≤ 20 LOC.
- A live HTTP round-trip through ``app.run()`` to a real socket.
- LoRa size warning fires for an over-budget Frame when the app opts in.
"""

from __future__ import annotations

import importlib.util
import logging
import socket
import sys
import threading
import time
from http.client import HTTPConnection
from pathlib import Path

import pytest

from pageros import App, Request, Response, decode_frame, encode_frame
from pageros.app import _CBOR_CONTENT_TYPE


REPO_ROOT = Path(__file__).resolve().parents[3]
HELLO_EXAMPLE = REPO_ROOT / "examples" / "hello" / "app.py"


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------


def test_screen_registers_get_route() -> None:
    app = App(name="t")

    @app.screen("/")
    def home():
        return {"v": 1, "id": "scr_home", "body": []}

    assert ("GET", "/") in app.routes


def test_handler_defaults_to_post() -> None:
    app = App(name="t")

    @app.handler("/save")
    def save(req):
        return None

    assert ("POST", "/save") in app.routes


def test_handler_explicit_method_normalised_to_upper() -> None:
    app = App(name="t")

    @app.handler("/x", method="put")
    def put_x():
        return None

    assert ("PUT", "/x") in app.routes


def test_route_path_must_start_with_slash() -> None:
    app = App(name="t")
    with pytest.raises(ValueError):
        app.handler("missing-slash")


def test_duplicate_route_is_rejected() -> None:
    app = App(name="t")

    @app.screen("/")
    def a():
        return None

    with pytest.raises(ValueError):

        @app.screen("/")
        def b():
            return None


# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------


def test_dispatch_returns_canonical_cbor_frame() -> None:
    app = App(name="t")

    frame = {"v": 1, "id": "scr_home", "body": [{"t": "text", "s": "hi"}]}

    @app.screen("/")
    def home():
        return frame

    status, headers, body = app.dispatch("GET", "/")
    assert status == 200
    assert headers["Content-Type"] == _CBOR_CONTENT_TYPE
    assert body == encode_frame(frame)
    assert decode_frame(body) == frame


def test_dispatch_passes_request_to_one_arg_handler() -> None:
    app = App(name="t")
    captured: dict = {}

    @app.handler("/echo")
    def echo(req: Request):
        captured["req"] = req
        return None

    app.dispatch(
        "POST",
        "/echo?x=1",
        body=encode_frame({"hello": "world"}),
        headers={"Content-Type": "application/cbor"},
    )
    req = captured["req"]
    assert isinstance(req, Request)
    assert req.method == "POST"
    assert req.path == "/echo"
    assert req.query == {"x": ["1"]}
    assert req.body == {"hello": "world"}


def test_dispatch_omits_request_for_zero_arg_handler() -> None:
    app = App(name="t")
    invocations = []

    @app.screen("/")
    def home():
        invocations.append(True)
        return {"v": 1, "id": "scr_home", "body": []}

    status, _h, _b = app.dispatch("GET", "/")
    assert status == 200
    assert invocations == [True]


def test_dispatch_returns_204_for_none() -> None:
    app = App(name="t")

    @app.handler("/event")
    def event():
        return None

    status, _h, body = app.dispatch("POST", "/event")
    assert status == 204
    assert body == b""


def test_dispatch_404_for_unknown_route() -> None:
    app = App(name="t")
    status, headers, body = app.dispatch("GET", "/missing")
    assert status == 404
    assert headers["Content-Type"] == _CBOR_CONTENT_TYPE
    decoded = decode_frame(body)
    assert decoded["id"] == "err_local"
    assert decoded["body"][0]["style"] == "error"


def test_dispatch_405_for_wrong_method_on_known_path() -> None:
    app = App(name="t")

    @app.screen("/")
    def home():
        return {"v": 1, "id": "scr_home", "body": []}

    status, headers, _body = app.dispatch("POST", "/")
    assert status == 405
    assert "Allow" in headers
    assert "GET" in headers["Allow"]


def test_dispatch_400_for_malformed_cbor_body() -> None:
    app = App(name="t")

    @app.handler("/save")
    def save(req: Request):
        return None

    # truncated map header
    status, _h, body = app.dispatch(
        "POST",
        "/save",
        body=b"\xa1\x61",  # claims map(1), text-string key of length 1, then nothing
        headers={"Content-Type": "application/cbor"},
    )
    assert status == 400
    decoded = decode_frame(body)
    assert "Bad request" in decoded["body"][0]["s"]


def test_dispatch_raw_bytes_body_for_non_cbor_content_type() -> None:
    app = App(name="t")
    seen: dict = {}

    @app.handler("/raw")
    def raw(req: Request):
        seen["body"] = req.body
        return None

    app.dispatch(
        "POST",
        "/raw",
        body=b"\x00\x01\x02",
        headers={"Content-Type": "application/octet-stream"},
    )
    assert seen["body"] == b"\x00\x01\x02"


def test_handler_can_return_response_with_status_and_headers() -> None:
    app = App(name="t")

    @app.screen("/redirect")
    def redirect():
        return Response(status=303, headers={"Location": "/elsewhere"}, body=None)

    status, headers, body = app.dispatch("GET", "/redirect")
    assert status == 303
    assert headers["Location"] == "/elsewhere"
    assert body == b""


def test_handler_can_return_raw_bytes() -> None:
    app = App(name="t")

    @app.screen("/raw")
    def raw():
        return Response(body=b"\xaa\xbb")

    status, headers, body = app.dispatch("GET", "/raw")
    assert status == 200
    assert headers["Content-Type"] == _CBOR_CONTENT_TYPE
    assert body == b"\xaa\xbb"


def test_dispatch_handles_uncaught_exception_as_500() -> None:
    app = App(name="t")

    @app.screen("/boom")
    def boom():
        raise RuntimeError("nope")

    status, _h, body = app.dispatch("GET", "/boom")
    assert status == 500
    assert decode_frame(body)["body"][0]["s"] == "Server error"


# ---------------------------------------------------------------------------
# Hello example (acceptance check)
# ---------------------------------------------------------------------------


def _load_hello_module():
    spec = importlib.util.spec_from_file_location(
        "_pageros_hello_test_module", HELLO_EXAMPLE
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_hello_example_under_twenty_lines() -> None:
    text = HELLO_EXAMPLE.read_text()
    line_count = len(text.splitlines())
    assert line_count <= 20, f"hello example is {line_count} lines (>20)"


def test_hello_example_serves_frame_from_get_root() -> None:
    mod = _load_hello_module()
    status, headers, body = mod.app.dispatch("GET", "/")
    assert status == 200
    assert headers["Content-Type"] == _CBOR_CONTENT_TYPE
    frame = decode_frame(body)
    assert frame["v"] == 1
    assert frame["id"] == "scr_home"
    assert frame["body"][0]["s"] == "Hello, PagerOS!"


# ---------------------------------------------------------------------------
# Live HTTP server (verifies the run() entry point)
# ---------------------------------------------------------------------------


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_run_serves_real_http_request() -> None:
    app = App(name="t")

    @app.screen("/")
    def home():
        return {"v": 1, "id": "scr_home", "body": [{"t": "text", "s": "live"}]}

    port = _free_port()
    server_thread = threading.Thread(
        target=app.run, kwargs={"host": "127.0.0.1", "port": port}, daemon=True
    )
    server_thread.start()
    # Spin briefly until the socket is accepting.
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                break
        except OSError:
            time.sleep(0.02)
    else:  # pragma: no cover
        pytest.fail("app.run() never bound")

    try:
        conn = HTTPConnection("127.0.0.1", port, timeout=2.0)
        conn.request("GET", "/", headers={"Accept": "application/cbor; pagerOS=1"})
        resp = conn.getresponse()
        assert resp.status == 200
        assert resp.getheader("Content-Type") == _CBOR_CONTENT_TYPE
        body = resp.read()
        frame = decode_frame(body)
        assert frame["body"][0]["s"] == "live"
    finally:
        # ThreadingHTTPServer doesn't expose itself back to us here; the
        # daemon thread dies with the test process. Close the connection
        # explicitly so the OS releases the socket cleanly.
        conn.close()


# ---------------------------------------------------------------------------
# CLI arg parsing
# ---------------------------------------------------------------------------


def test_resolve_address_reads_argv_host_port() -> None:
    app = App(name="t")
    host, port = app._resolve_address(
        None, None, argv=["--host", "0.0.0.0", "--port", "9001"]
    )
    assert host == "0.0.0.0"
    assert port == 9001


def test_resolve_address_explicit_kwargs_override_argv() -> None:
    app = App(name="t")
    host, port = app._resolve_address(
        "1.2.3.4", 1234, argv=["--host", "0.0.0.0", "--port", "9000"]
    )
    assert (host, port) == ("1.2.3.4", 1234)


def test_resolve_address_defaults() -> None:
    app = App(name="t")
    host, port = app._resolve_address(None, None, argv=[])
    assert host == "127.0.0.1"
    assert port == 8000


# ---------------------------------------------------------------------------
# LoRa budget hook
# ---------------------------------------------------------------------------


def test_lora_warning_fires_when_app_opts_in_and_frame_oversized(
    caplog: pytest.LogCaptureFixture,
) -> None:
    app = App(name="t", lora_compatible=True)
    big_text = "x" * 500  # encoded frame will overshoot the 200 B budget

    @app.screen("/")
    def home():
        return {"v": 1, "id": "scr_big", "body": [{"t": "text", "s": big_text}]}

    with caplog.at_level(logging.WARNING, logger="pageros.lora"):
        status, _h, _b = app.dispatch("GET", "/")

    assert status == 200
    assert any("GET /" in r.getMessage() for r in caplog.records)


def test_no_lora_warning_when_app_not_lora_compatible(
    caplog: pytest.LogCaptureFixture,
) -> None:
    app = App(name="t", lora_compatible=False)

    @app.screen("/")
    def home():
        return {"v": 1, "id": "scr_big", "body": [{"t": "text", "s": "y" * 500}]}

    with caplog.at_level(logging.WARNING, logger="pageros.lora"):
        app.dispatch("GET", "/")
    assert caplog.records == []
