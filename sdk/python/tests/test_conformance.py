"""PY-012 conformance integration test.

Boots the :mod:`pageros.conformance` HTTP adapter on a free port, then
runs the cross-language PROTO-005 runner against it as a subprocess.
The acceptance criterion in ``TASKS.md`` is:

    pytest passes; conformance runner reports 100%.

This single test enforces both halves — the runner exits non-zero on
any vector failure and the test asserts a 100% pass rate from its JSON
report.

Skipped (not failed) when the SDK is installed somewhere other than
the in-repo source tree and the runner script / vectors aren't
reachable on disk. That keeps `pip install pageros && pytest` working
for downstream consumers without dragging the cross-language asset
tree into the sdist.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

from pageros.conformance import serve_in_background


def _repo_root() -> Path | None:
    """Walk up from this test file to find the repo root (heuristic: a
    ``protocol/`` directory exists alongside ``sdk/``)."""
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / "protocol" / "conformance" / "proto_conformance.py").exists():
            return parent
    return None


REPO_ROOT = _repo_root()


pytestmark = pytest.mark.skipif(
    REPO_ROOT is None,
    reason="protocol/ not available — likely running from an installed sdist without the cross-language test assets.",
)


def test_proto_conformance_runner_reports_100_percent() -> None:
    runner = REPO_ROOT / "protocol" / "conformance" / "proto_conformance.py"
    vectors = REPO_ROOT / "protocol" / "test-vectors" / "ui"
    assert runner.exists(), f"missing runner script at {runner}"
    assert (vectors / "index.json").exists(), f"missing vector index at {vectors}"

    server, _thread = serve_in_background()
    host, port = server.server_address
    endpoint = f"http://{host}:{port}"
    try:
        completed = subprocess.run(
            [
                sys.executable,
                str(runner),
                "--endpoint",
                endpoint,
                "--vectors",
                str(vectors),
                "--report",
                "json",
            ],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
    finally:
        server.shutdown()
        server.server_close()

    # Always parse the JSON report so a non-zero exit prints the
    # specific failing vector names instead of a wall of CBOR hex.
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError:
        report = None

    if completed.returncode != 0 or (report and report.get("failed", 0)):
        fail_details = []
        if report:
            for res in report.get("results", []):
                if not res.get("passed") and not res.get("skipped"):
                    fail_details.append(f"  - {res['name']}: {res.get('detail', '')[:280]}")
        raise AssertionError(
            "proto_conformance runner reported failures\n"
            f"exit={completed.returncode}\n"
            f"stdout (tail):\n{completed.stdout[-2000:]}\n"
            f"stderr (tail):\n{completed.stderr[-1000:]}\n"
            "Failing vectors:\n" + "\n".join(fail_details)
        )

    assert report is not None, "runner produced no parseable JSON report"
    assert report["total"] > 0, "runner found no vectors at all"
    assert report["failed"] == 0, f"expected 0 failures, got {report['failed']}"
    assert report["passed"] == report["total"] - report["skipped"]


# --------------------------------------------------------------------------- #
# Direct in-process tests of the adapter — fast unit-level coverage that
# does not need the runner subprocess. These catch most regressions
# without paying the subprocess cost.
# --------------------------------------------------------------------------- #


def test_conformance_encode_handles_bytes_marker() -> None:
    from pageros.conformance import conformance_encode

    out = conformance_encode(
        "nfc_scan_marker",
        {
            "type": "nfc_scan",
            "payload": {"uid": {"$bytes": "04a1b2c3"}, "records": []},
        },
    )
    assert isinstance(out, bytes)
    # Sanity: bytes are present somewhere — the canonical encoding
    # embeds them as a 4-byte string after the bstr head 0x44.
    assert b"\x04\xa1\xb2\xc3" in out


def test_conformance_decode_drops_unknown_top_level_field() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import conformance_decode

    raw = encode_frame({
        "v": 1,
        "id": "scr_test",
        "body": [{"t": "text", "s": "hi"}],
        "secret_future_field": {"hello": "world"},
    })
    decoded = conformance_decode(raw)
    assert "secret_future_field" not in decoded
    assert decoded["body"] == [{"t": "text", "s": "hi"}]


def test_conformance_decode_drops_unknown_widget_field() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import conformance_decode

    raw = encode_frame({
        "v": 1,
        "id": "scr_test",
        "body": [{"t": "text", "s": "Greetings", "future_style": "rainbow"}],
    })
    decoded = conformance_decode(raw)
    assert decoded["body"] == [{"t": "text", "s": "Greetings"}]


def test_conformance_decode_wraps_unknown_widget() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import conformance_decode

    raw = encode_frame({
        "v": 1,
        "id": "scr_test",
        "body": [{"t": "futurewidget", "data": "x"}],
    })
    decoded = conformance_decode(raw)
    assert decoded["body"][0] == {
        "$render_placeholder": "[unsupported: futurewidget]",
        "raw_widget": {"t": "futurewidget", "data": "x"},
    }


def test_conformance_decode_splits_subscribe_on_unknowns() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import conformance_decode

    raw = encode_frame({
        "v": 1,
        "id": "scr_test",
        "body": [{"t": "text", "s": "ok"}],
        "subscribe": ["nfc_scan", "future_event", 7, 2],
    })
    decoded = conformance_decode(raw)
    assert "subscribe" not in decoded
    assert decoded["subscribe_effective"] == ["nfc_scan", 2]
    assert decoded["subscribe_dropped"] == ["future_event", 7]


def test_conformance_decode_keeps_subscribe_when_all_known() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import conformance_decode

    raw = encode_frame({
        "v": 1,
        "id": "scr_test",
        "body": [{"t": "text", "s": "ok"}],
        "subscribe": ["nfc_scan", 2],
    })
    decoded = conformance_decode(raw)
    assert decoded["subscribe"] == ["nfc_scan", 2]
    assert "subscribe_effective" not in decoded


def test_conformance_decode_rejects_non_map() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import ConformanceError, conformance_decode

    raw = encode_frame([1, 2, 3])
    with pytest.raises(ConformanceError) as excinfo:
        conformance_decode(raw)
    assert excinfo.value.error == "invalid_frame_shape"


def test_conformance_decode_rejects_unknown_major_version() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import ConformanceError, conformance_decode

    raw = encode_frame({"v": 99, "id": "scr_future", "body": []})
    with pytest.raises(ConformanceError) as excinfo:
        conformance_decode(raw)
    assert excinfo.value.error == "unknown_major_version"


def test_conformance_decode_rejects_unknown_event_type() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import ConformanceError, conformance_decode

    raw = encode_frame({"type": "future_event", "payload": {"x": 1}})
    with pytest.raises(ConformanceError) as excinfo:
        conformance_decode(raw)
    assert excinfo.value.error == "unknown_event_dropped"


def test_conformance_decode_surfaces_bytes_as_marker_string() -> None:
    from pageros.codec import encode_frame
    from pageros.conformance import conformance_decode

    raw = encode_frame({
        "v": 1,
        "id": "scr_t",
        "body": [{"t": "futurewidget", "blob": b"\x01\x02\x03"}],
    })
    decoded = conformance_decode(raw)
    assert decoded["body"][0]["raw_widget"]["blob"] == "$bytes:010203"


def test_make_handler_round_trip_over_http() -> None:
    """Drive the actual HTTP handler — the path most cases hit in CI."""
    import urllib.request

    server, _thread = serve_in_background()
    host, port = server.server_address
    try:
        # /conformance/encode
        body = json.dumps(
            {"name": "smoke", "input": {"v": 1, "id": "scr_x", "body": []}}
        ).encode("utf-8")
        req = urllib.request.Request(
            f"http://{host}:{port}/conformance/encode",
            data=body,
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=5) as resp:
            assert resp.status == 200
            assert resp.headers.get("X-PagerOS-Canonical") == "true"
            cbor_bytes = resp.read()
        assert cbor_bytes  # non-empty

        # /conformance/decode round-trip
        req2 = urllib.request.Request(
            f"http://{host}:{port}/conformance/decode",
            data=cbor_bytes,
            method="POST",
            headers={"Content-Type": "application/cbor"},
        )
        with urllib.request.urlopen(req2, timeout=5) as resp:
            assert resp.status == 200
            decoded = json.loads(resp.read())
        assert decoded == {"v": 1, "id": "scr_x", "body": []}
    finally:
        server.shutdown()
        server.server_close()
