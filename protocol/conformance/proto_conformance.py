#!/usr/bin/env python3
"""PagerOS UI-protocol conformance runner (PROTO-005).

Walks `protocol/test-vectors/ui/index.json` and drives each vector against an
SDK HTTP endpoint that implements the contract documented in
`protocol/conformance/README.md`. Prints a pass/fail report, exits 0 if every
vector passed, 1 if any failed, 2 on a runner-level error.

Pure stdlib. Run as:

    python3 protocol/conformance/proto_conformance.py --endpoint http://localhost:8080
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import struct
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

# ---------------------------------------------------------------------------
# Minimal canonical CBOR decoder.
#
# The runner needs to decode SDK responses (and expected_cbor_hex bytes) when
# the SDK does not advertise canonical output and we need a structural
# compare. This is a strict-subset decoder: it accepts every shape the v1 UI
# protocol uses (uint, nint, bstr, tstr, arrays, maps, true/false/null, f32,
# f64) and rejects everything else.
# ---------------------------------------------------------------------------

_MT_UINT, _MT_NINT, _MT_BSTR, _MT_TSTR, _MT_ARRAY, _MT_MAP, _MT_TAG, _MT_SIMPLE = range(8)


class CborDecodeError(ValueError):
    """Raised on malformed or unsupported CBOR input."""


def _read_head(buf: bytes, pos: int) -> tuple[int, int, int]:
    if pos >= len(buf):
        raise CborDecodeError(f"truncated head at offset {pos}")
    b = buf[pos]
    mt = b >> 5
    info = b & 0x1F
    pos += 1
    if info < 24:
        return mt, info, pos
    if info == 24:
        if pos + 1 > len(buf):
            raise CborDecodeError("truncated 1-byte arg")
        return mt, buf[pos], pos + 1
    if info == 25:
        if pos + 2 > len(buf):
            raise CborDecodeError("truncated 2-byte arg")
        return mt, int.from_bytes(buf[pos : pos + 2], "big"), pos + 2
    if info == 26:
        if pos + 4 > len(buf):
            raise CborDecodeError("truncated 4-byte arg")
        return mt, int.from_bytes(buf[pos : pos + 4], "big"), pos + 4
    if info == 27:
        if pos + 8 > len(buf):
            raise CborDecodeError("truncated 8-byte arg")
        return mt, int.from_bytes(buf[pos : pos + 8], "big"), pos + 8
    raise CborDecodeError(f"unsupported additional-info {info}")


def _decode_simple(buf: bytes, pos: int) -> tuple[Any, int]:
    """Handle major type 7 explicitly (true/false/null + f16/f32/f64)."""
    if pos >= len(buf):
        raise CborDecodeError("truncated simple head")
    b = buf[pos]
    info = b & 0x1F
    if info == 20:
        return False, pos + 1
    if info == 21:
        return True, pos + 1
    if info == 22:
        return None, pos + 1
    if info == 23:
        raise CborDecodeError("undefined not supported")
    if info == 25:
        if pos + 3 > len(buf):
            raise CborDecodeError("truncated f16")
        # f16 → float
        h = int.from_bytes(buf[pos + 1 : pos + 3], "big")
        return _half_to_float(h), pos + 3
    if info == 26:
        if pos + 5 > len(buf):
            raise CborDecodeError("truncated f32")
        return struct.unpack(">f", buf[pos + 1 : pos + 5])[0], pos + 5
    if info == 27:
        if pos + 9 > len(buf):
            raise CborDecodeError("truncated f64")
        return struct.unpack(">d", buf[pos + 1 : pos + 9])[0], pos + 9
    raise CborDecodeError(f"unsupported simple info {info}")


def _half_to_float(h: int) -> float:
    sign = (h >> 15) & 1
    exp = (h >> 10) & 0x1F
    frac = h & 0x3FF
    if exp == 0:
        val = (frac / 1024.0) * (2 ** -14)
    elif exp == 31:
        val = float("inf") if frac == 0 else float("nan")
    else:
        val = (1 + frac / 1024.0) * (2 ** (exp - 15))
    return -val if sign else val


def _decode(buf: bytes, pos: int) -> tuple[Any, int]:
    if pos >= len(buf):
        raise CborDecodeError(f"truncated value at offset {pos}")
    mt_byte = buf[pos]
    if (mt_byte >> 5) == _MT_SIMPLE:
        return _decode_simple(buf, pos)
    mt, arg, pos = _read_head(buf, pos)
    if mt == _MT_UINT:
        return arg, pos
    if mt == _MT_NINT:
        return -1 - arg, pos
    if mt == _MT_BSTR:
        end = pos + arg
        if end > len(buf):
            raise CborDecodeError("truncated byte string")
        return {"$bytes": buf[pos:end].hex()}, end
    if mt == _MT_TSTR:
        end = pos + arg
        if end > len(buf):
            raise CborDecodeError("truncated text string")
        return buf[pos:end].decode("utf-8"), end
    if mt == _MT_ARRAY:
        out: list[Any] = []
        for _ in range(arg):
            item, pos = _decode(buf, pos)
            out.append(item)
        return out, pos
    if mt == _MT_MAP:
        out_map: dict[Any, Any] = {}
        for _ in range(arg):
            k, pos = _decode(buf, pos)
            v, pos = _decode(buf, pos)
            if isinstance(k, dict):
                k = json.dumps(k, sort_keys=True)
            out_map[k] = v
        return out_map, pos
    if mt == _MT_TAG:
        # PagerOS uses no semantic tags (registry §2.4).
        raise CborDecodeError(f"semantic CBOR tag {arg} not allowed in PagerOS")
    raise CborDecodeError(f"unsupported major type {mt}")


def decode_cbor(buf: bytes) -> Any:
    if not buf:
        raise CborDecodeError("empty input")
    value, pos = _decode(buf, 0)
    if pos != len(buf):
        raise CborDecodeError(f"trailing bytes: consumed {pos} of {len(buf)}")
    return value


# ---------------------------------------------------------------------------
# Vector loader.
# ---------------------------------------------------------------------------


@dataclass
class Vector:
    name: str
    category: str
    kind: str
    tag_form: str | None
    description: str
    input: Any | None
    input_cbor_hex: str | None
    expected_cbor_hex: str | None
    expected_decoded: Any | None
    expect_error: str | None

    @classmethod
    def from_descriptor(cls, d: dict[str, Any]) -> "Vector":
        return cls(
            name=d["name"],
            category=d["category"],
            kind=d["kind"],
            tag_form=d.get("tag_form"),
            description=d.get("description", ""),
            input=d.get("input"),
            input_cbor_hex=d.get("input_cbor_hex"),
            expected_cbor_hex=d.get("expected_cbor_hex"),
            expected_decoded=d.get("expected_decoded"),
            expect_error=d.get("expect_error"),
        )


def load_vectors(root: Path) -> list[Vector]:
    index_path = root / "index.json"
    if not index_path.exists():
        raise FileNotFoundError(f"no index.json under {root}")
    index = json.loads(index_path.read_text())
    out: list[Vector] = []
    for entry in index["vectors"]:
        desc_path = root / entry["files"]["descriptor"]
        if not desc_path.exists():
            raise FileNotFoundError(f"missing descriptor {desc_path}")
        out.append(Vector.from_descriptor(json.loads(desc_path.read_text())))
    return out


# ---------------------------------------------------------------------------
# HTTP client (stdlib only).
# ---------------------------------------------------------------------------


class TransportError(RuntimeError):
    pass


def _post(url: str, body: bytes, content_type: str, timeout: float) -> tuple[int, dict[str, str], bytes]:
    req = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={"Content-Type": content_type, "Accept": "application/cbor, application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            headers = {k.lower(): v for k, v in resp.headers.items()}
            return resp.status, headers, resp.read()
    except urllib.error.HTTPError as e:
        headers = {k.lower(): v for k, v in e.headers.items()} if e.headers else {}
        return e.code, headers, e.read() or b""
    except urllib.error.URLError as e:
        raise TransportError(f"{url}: {e.reason}") from e


def _normalize_endpoint(endpoint: str) -> str:
    return endpoint.rstrip("/")


def _probe_endpoint(endpoint: str, timeout: float) -> None:
    """Pre-flight: confirm the endpoint host is reachable.

    A failed probe is the runner's "endpoint unreachable" signal (exit 2),
    distinct from per-vector transport noise (exit 1). The probe POSTs an
    intentionally invalid /conformance/encode payload and accepts any HTTP
    response status, including 4xx and 5xx — the SDK is up so long as it
    answers HTTP at all.
    """
    try:
        _post(f"{endpoint}/conformance/encode", b"{}", "application/json", timeout)
    except TransportError:
        raise


# ---------------------------------------------------------------------------
# Comparison helpers.
# ---------------------------------------------------------------------------


def _normalize_for_compare(node: Any) -> Any:
    """Coerce JSON-from-SDK forms into a comparable Python shape.

    - Treat list and tuple as the same sequence type.
    - Floats are compared bitwise via struct repr so 1.0 != 1 (int)
      mismatches don't sneak through.
    - Dict keys are coerced to str (CBOR allows non-string keys but JSON
      does not; the SDK's JSON form must already use string keys).
    """
    if isinstance(node, dict):
        return {str(k): _normalize_for_compare(v) for k, v in node.items()}
    if isinstance(node, (list, tuple)):
        return [_normalize_for_compare(v) for v in node]
    if isinstance(node, float):
        return ("float", struct.pack(">d", node))
    return node


def equal_structural(a: Any, b: Any) -> bool:
    return _normalize_for_compare(a) == _normalize_for_compare(b)


# ---------------------------------------------------------------------------
# Per-vector drivers.
# ---------------------------------------------------------------------------


@dataclass
class VectorResult:
    name: str
    category: str
    kind: str
    passed: bool
    skipped: bool = False
    detail: str = ""


def _run_encode(v: Vector, endpoint: str, timeout: float, require_canonical: bool) -> VectorResult:
    body = json.dumps({"name": v.name, "input": v.input}).encode("utf-8")
    try:
        status, headers, resp = _post(
            f"{endpoint}/conformance/encode", body, "application/json", timeout
        )
    except TransportError as exc:
        return VectorResult(v.name, v.category, v.kind, False, detail=f"transport: {exc}")

    if status != 200:
        msg = _safe_json_message(resp)
        return VectorResult(
            v.name,
            v.category,
            v.kind,
            False,
            detail=f"SDK refused encode (HTTP {status}: {msg})",
        )

    if v.expected_cbor_hex is None:
        return VectorResult(v.name, v.category, v.kind, False, detail="vector missing expected_cbor_hex")

    expected = bytes.fromhex(v.expected_cbor_hex)
    canonical = headers.get("x-pageros-canonical", "").lower() == "true"

    if canonical:
        if resp == expected:
            return VectorResult(v.name, v.category, v.kind, True)
        return VectorResult(
            v.name,
            v.category,
            v.kind,
            False,
            detail=(
                f"canonical CBOR mismatch: got {resp.hex()} ({len(resp)} B), "
                f"expected {v.expected_cbor_hex} ({len(expected)} B)"
            ),
        )

    if require_canonical:
        return VectorResult(
            v.name,
            v.category,
            v.kind,
            False,
            detail="SDK did not advertise X-PagerOS-Canonical: true and --allow-non-canonical not set",
        )

    try:
        got = decode_cbor(resp)
        want = decode_cbor(expected)
    except CborDecodeError as exc:
        return VectorResult(v.name, v.category, v.kind, False, detail=f"decode failed: {exc}")
    if equal_structural(got, want):
        return VectorResult(v.name, v.category, v.kind, True, detail="structural match (non-canonical encoder)")
    return VectorResult(
        v.name,
        v.category,
        v.kind,
        False,
        detail=f"structural mismatch: got {got!r}, expected {want!r}",
    )


def _run_decode_only(v: Vector, endpoint: str, timeout: float) -> VectorResult:
    raw = bytes.fromhex(v.input_cbor_hex or "")
    if not raw:
        return VectorResult(v.name, v.category, v.kind, False, detail="vector missing input_cbor_hex")
    try:
        status, headers, resp = _post(
            f"{endpoint}/conformance/decode", raw, "application/cbor", timeout
        )
    except TransportError as exc:
        return VectorResult(v.name, v.category, v.kind, False, detail=f"transport: {exc}")

    if status != 200:
        return VectorResult(
            v.name,
            v.category,
            v.kind,
            False,
            detail=f"SDK refused decode (HTTP {status}: {_safe_json_message(resp)})",
        )
    try:
        decoded = json.loads(resp.decode("utf-8"))
    except json.JSONDecodeError as exc:
        return VectorResult(v.name, v.category, v.kind, False, detail=f"non-JSON response: {exc}")
    if equal_structural(decoded, v.expected_decoded):
        return VectorResult(v.name, v.category, v.kind, True)
    return VectorResult(
        v.name,
        v.category,
        v.kind,
        False,
        detail=f"structural mismatch: got {decoded!r}, expected {v.expected_decoded!r}",
    )


def _run_negative(v: Vector, endpoint: str, timeout: float) -> VectorResult:
    raw = bytes.fromhex(v.input_cbor_hex or "")
    if not raw:
        return VectorResult(v.name, v.category, v.kind, False, detail="vector missing input_cbor_hex")
    try:
        status, headers, resp = _post(
            f"{endpoint}/conformance/decode", raw, "application/cbor", timeout
        )
    except TransportError as exc:
        return VectorResult(v.name, v.category, v.kind, False, detail=f"transport: {exc}")

    if 200 <= status < 300:
        return VectorResult(
            v.name,
            v.category,
            v.kind,
            False,
            detail=f"SDK accepted negative vector (HTTP {status}); expected refusal with error={v.expect_error}",
        )
    try:
        body = json.loads(resp.decode("utf-8"))
    except json.JSONDecodeError:
        return VectorResult(
            v.name,
            v.category,
            v.kind,
            False,
            detail=f"refusal body not JSON (HTTP {status}, body={resp[:64]!r})",
        )
    if body.get("error") == v.expect_error:
        return VectorResult(v.name, v.category, v.kind, True)
    return VectorResult(
        v.name,
        v.category,
        v.kind,
        False,
        detail=f"wrong error class: got {body.get('error')!r}, expected {v.expect_error!r}",
    )


def _safe_json_message(body: bytes) -> str:
    try:
        d = json.loads(body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return repr(body[:120])
    msg = d.get("message") or d.get("error") or json.dumps(d)
    return msg if isinstance(msg, str) else json.dumps(d)


# ---------------------------------------------------------------------------
# Orchestrator.
# ---------------------------------------------------------------------------


@dataclass
class RunSummary:
    total: int
    passed: int
    failed: int
    skipped: int
    results: list[VectorResult] = field(default_factory=list)


def run_vectors(
    vectors: Iterable[Vector],
    endpoint: str,
    *,
    timeout: float = 10.0,
    allow_non_canonical: bool = False,
) -> RunSummary:
    endpoint = _normalize_endpoint(endpoint)
    require_canonical = not allow_non_canonical
    results: list[VectorResult] = []
    for v in vectors:
        if v.kind == "encode":
            r = _run_encode(v, endpoint, timeout, require_canonical)
        elif v.kind == "decode_only":
            r = _run_decode_only(v, endpoint, timeout)
        elif v.kind == "negative":
            r = _run_negative(v, endpoint, timeout)
        else:
            r = VectorResult(v.name, v.category, v.kind, False, detail=f"unknown kind {v.kind}")
        results.append(r)

    total = len(results)
    passed = sum(1 for r in results if r.passed and not r.skipped)
    failed = sum(1 for r in results if not r.passed and not r.skipped)
    skipped = sum(1 for r in results if r.skipped)
    return RunSummary(total=total, passed=passed, failed=failed, skipped=skipped, results=results)


# ---------------------------------------------------------------------------
# CLI surface.
# ---------------------------------------------------------------------------


def _filter_vectors(vectors: list[Vector], globs: list[str]) -> list[Vector]:
    if not globs:
        return vectors
    out: list[Vector] = []
    for v in vectors:
        if any(fnmatch.fnmatchcase(v.name, g) for g in globs):
            out.append(v)
    return out


def _print_text_report(summary: RunSummary, verbose: bool) -> None:
    by_cat: dict[str, dict[str, int]] = {}
    for r in summary.results:
        slot = by_cat.setdefault(r.category, {"total": 0, "passed": 0, "failed": 0})
        slot["total"] += 1
        if r.passed:
            slot["passed"] += 1
        else:
            slot["failed"] += 1
    print(f"vectors: {summary.total}")
    for cat in sorted(by_cat):
        s = by_cat[cat]
        print(f"  {cat:15s} {s['passed']:4d}/{s['total']:<4d} passed  ({s['failed']} failed)")
    print(f"total: {summary.passed}/{summary.total} passed")
    if summary.failed == 0:
        print("PASS")
    else:
        print("FAIL")
    if verbose or summary.failed:
        for r in summary.results:
            if verbose or not r.passed:
                tag = "ok  " if r.passed else "FAIL"
                line = f"  {tag} {r.kind:11s} {r.name}"
                if r.detail and not r.passed:
                    line += f"   {r.detail}"
                elif r.detail and verbose:
                    line += f"   ({r.detail})"
                print(line)


def _print_json_report(summary: RunSummary) -> None:
    print(
        json.dumps(
            {
                "total": summary.total,
                "passed": summary.passed,
                "failed": summary.failed,
                "skipped": summary.skipped,
                "results": [
                    {
                        "name": r.name,
                        "category": r.category,
                        "kind": r.kind,
                        "passed": r.passed,
                        "skipped": r.skipped,
                        "detail": r.detail,
                    }
                    for r in summary.results
                ],
            },
            indent=2,
        )
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="proto-conformance",
        description="Drive PagerOS UI-protocol test vectors against an SDK HTTP endpoint.",
    )
    parser.add_argument("--endpoint", required=True, help="Base URL of the SDK under test.")
    parser.add_argument(
        "--vectors",
        default=str(Path(__file__).resolve().parents[1] / "test-vectors" / "ui"),
        help="Path to a vectors directory containing index.json.",
    )
    parser.add_argument(
        "--filter",
        action="append",
        default=[],
        metavar="GLOB",
        help="fnmatch glob over vector name. Repeatable; matches OR together.",
    )
    parser.add_argument("--report", choices=("text", "json"), default="text")
    parser.add_argument(
        "--allow-non-canonical",
        action="store_true",
        help="Accept structural-match on encode vectors when the SDK does not "
        "advertise X-PagerOS-Canonical: true.",
    )
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args(argv)

    try:
        vectors = load_vectors(Path(args.vectors))
    except FileNotFoundError as exc:
        print(f"runner error: {exc}", file=sys.stderr)
        return 2

    vectors = _filter_vectors(vectors, args.filter)
    if not vectors:
        print("runner error: filter excluded every vector", file=sys.stderr)
        return 2

    endpoint = _normalize_endpoint(args.endpoint)
    try:
        _probe_endpoint(endpoint, args.timeout)
    except TransportError as exc:
        print(f"runner error: endpoint unreachable: {exc}", file=sys.stderr)
        return 2

    summary = run_vectors(
        vectors,
        endpoint,
        timeout=args.timeout,
        allow_non_canonical=args.allow_non_canonical,
    )

    if args.report == "json":
        _print_json_report(summary)
    else:
        _print_text_report(summary, args.verbose)

    return 0 if summary.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
