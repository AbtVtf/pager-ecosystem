"""Canonical CBOR codec for PagerOS Frames (PY-002).

Encoder produces RFC 8949 §4.2 canonical deterministic encoding:

- Smallest unsigned-integer head for every length / value.
- Definite-length arrays and maps.
- Map keys sorted lexicographically by their CBOR-encoded byte form
  (RFC 8949 §4.2.1).
- 32-bit floats when the value round-trips through binary32; otherwise
  64-bit. No half-precision on output.
- No semantic tags (PagerOS does not use them — registry §2.4).

Decoder accepts canonical input plus the relaxations the wire spec allows
(unordered maps, any minimal-or-not integer head, indefinite-length
strings/arrays/maps, half-precision floats). It returns plain Python
values: ``dict``, ``list``, ``str``, ``bytes``, ``int``, ``float``,
``bool``, ``None``. Byte-string fields written in JSON descriptors as
``{"$bytes": "<hex>"}`` are expanded to ``bytes`` on encode and surfaced
as ``bytes`` on decode.

Public API:

- :func:`encode_frame` — Frame dict → canonical CBOR bytes.
- :func:`decode_frame` — CBOR bytes → Python value.
- :class:`CborEncodeError` / :class:`CborDecodeError` — error classes.

Round-trip property: ``encode_frame(decode_frame(b)) == b`` for any
canonical ``b`` produced by this encoder.
"""

from __future__ import annotations

import math
import struct
from typing import Any

__all__ = [
    "CborDecodeError",
    "CborEncodeError",
    "decode_frame",
    "encode_frame",
]


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class CborEncodeError(TypeError):
    """Raised when a Python value cannot be encoded as canonical CBOR."""


class CborDecodeError(ValueError):
    """Raised when CBOR bytes are malformed or violate this codec's rules."""


# ---------------------------------------------------------------------------
# CBOR major types (RFC 8949 §3.1)
# ---------------------------------------------------------------------------

_MT_UINT = 0
_MT_NINT = 1
_MT_BSTR = 2
_MT_TSTR = 3
_MT_ARRAY = 4
_MT_MAP = 5
_MT_TAG = 6
_MT_SIMPLE = 7

_BREAK = 0xFF
_INDEFINITE = -1  # sentinel used in place of ``arg`` for info==31

_UINT_MAX = 0x1_0000_0000_0000_0000  # 2**64


def _head(mt: int, val: int) -> bytes:
    """Emit the smallest CBOR head that fits ``val`` (RFC 8949 §3 / §4.2)."""
    first = mt << 5
    if val < 24:
        return bytes([first | val])
    if val < 0x100:
        return bytes([first | 24, val])
    if val < 0x1_0000:
        return bytes([first | 25]) + val.to_bytes(2, "big")
    if val < 0x1_0000_0000:
        return bytes([first | 26]) + val.to_bytes(4, "big")
    if val < _UINT_MAX:
        return bytes([first | 27]) + val.to_bytes(8, "big")
    raise CborEncodeError(f"integer head value too large for CBOR: {val}")


# ---------------------------------------------------------------------------
# Encoder
# ---------------------------------------------------------------------------


def encode_frame(value: Any) -> bytes:
    """Encode ``value`` as a canonical CBOR Frame.

    ``value`` is typically the top-level Frame dict (see
    ``protocol/spec.md`` §4) but any CBOR-encodable Python value is
    accepted so callers can encode sub-trees, event payloads, or request
    bodies with the same routine.
    """
    return _encode(value)


def _encode(v: Any) -> bytes:
    # Marker expansion: descriptors carry byte-string fields as
    # {"$bytes": "<hex>"} so they stay JSON-serializable. Handle these
    # before generic dict handling.
    if (
        isinstance(v, dict)
        and len(v) == 1
        and "$bytes" in v
        and isinstance(v["$bytes"], str)
    ):
        try:
            raw = bytes.fromhex(v["$bytes"])
        except ValueError as exc:
            raise CborEncodeError(f"invalid $bytes hex: {v['$bytes']!r}") from exc
        return _head(_MT_BSTR, len(raw)) + raw

    if v is None:
        return b"\xf6"
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    # ``bool`` is a subclass of ``int`` — handled by the two checks above.
    if isinstance(v, int):
        if v >= 0:
            if v >= _UINT_MAX:
                raise CborEncodeError(f"integer out of CBOR range: {v}")
            return _head(_MT_UINT, v)
        if -1 - v >= _UINT_MAX:
            raise CborEncodeError(f"negative integer out of CBOR range: {v}")
        return _head(_MT_NINT, -1 - v)
    if isinstance(v, float):
        return _encode_float(v)
    if isinstance(v, (bytes, bytearray, memoryview)):
        raw = bytes(v)
        return _head(_MT_BSTR, len(raw)) + raw
    if isinstance(v, str):
        b = v.encode("utf-8")
        return _head(_MT_TSTR, len(b)) + b
    if isinstance(v, (list, tuple)):
        out = bytearray(_head(_MT_ARRAY, len(v)))
        for item in v:
            out += _encode(item)
        return bytes(out)
    if isinstance(v, dict):
        # Canonical key sort: encode each key, sort by encoded bytes,
        # then concatenate (RFC 8949 §4.2.1).
        pairs: list[tuple[bytes, bytes]] = []
        for k, val in v.items():
            pairs.append((_encode(k), _encode(val)))
        pairs.sort(key=lambda kv: kv[0])
        out = bytearray(_head(_MT_MAP, len(v)))
        for k_bytes, v_bytes in pairs:
            out += k_bytes
            out += v_bytes
        return bytes(out)
    raise CborEncodeError(
        f"unsupported value for CBOR encoding: {type(v).__name__}"
    )


def _encode_float(v: float) -> bytes:
    # NaN → canonical single-precision quiet NaN (RFC 8949 §4.2.2 says
    # all NaN payloads serialise identically; we pick the common form).
    if math.isnan(v):
        return b"\xfa\x7f\xc0\x00\x00"
    if math.isinf(v):
        return b"\xfa" + struct.pack(">f", v)
    # Prefer 32-bit when round-trippable; otherwise 64-bit. Half-
    # precision is intentionally not emitted (RFC 8949 §4.2.2 permits
    # it; we choose 32/64 only for simpler vector reproducibility).
    packed32 = struct.pack(">f", v)
    if struct.unpack(">f", packed32)[0] == v:
        return b"\xfa" + packed32
    return b"\xfb" + struct.pack(">d", v)


# ---------------------------------------------------------------------------
# Decoder
# ---------------------------------------------------------------------------


def decode_frame(data: bytes) -> Any:
    """Decode canonical CBOR ``data`` to its Python value.

    Trailing bytes after the top-level item raise :class:`CborDecodeError`.
    """
    if not isinstance(data, (bytes, bytearray, memoryview)):
        raise CborDecodeError(
            f"decode_frame expects bytes-like, got {type(data).__name__}"
        )
    buf = bytes(data)
    value, end = _decode(buf, 0)
    if end != len(buf):
        raise CborDecodeError(
            f"trailing bytes after top-level item ({len(buf) - end} extra)"
        )
    return value


def _read_head(buf: bytes, pos: int) -> tuple[int, int, int, int]:
    """Return ``(major_type, info, arg, next_pos)``.

    For indefinite-length items (``info == 31``), ``arg`` is set to
    :data:`_INDEFINITE`. For simple values and floats (major type 7),
    ``info`` lets the caller distinguish 16/32/64-bit floats from
    simple values.
    """
    if pos >= len(buf):
        raise CborDecodeError(f"unexpected EOF at {pos}")
    ib = buf[pos]
    mt = ib >> 5
    info = ib & 0x1F
    pos += 1
    if info < 24:
        return mt, info, info, pos
    if info == 24:
        if pos + 1 > len(buf):
            raise CborDecodeError("truncated 1-byte argument")
        return mt, info, buf[pos], pos + 1
    if info == 25:
        if pos + 2 > len(buf):
            raise CborDecodeError("truncated 2-byte argument")
        return mt, info, int.from_bytes(buf[pos : pos + 2], "big"), pos + 2
    if info == 26:
        if pos + 4 > len(buf):
            raise CborDecodeError("truncated 4-byte argument")
        return mt, info, int.from_bytes(buf[pos : pos + 4], "big"), pos + 4
    if info == 27:
        if pos + 8 > len(buf):
            raise CborDecodeError("truncated 8-byte argument")
        return mt, info, int.from_bytes(buf[pos : pos + 8], "big"), pos + 8
    if info == 31:
        return mt, info, _INDEFINITE, pos
    raise CborDecodeError(f"reserved additional info {info} at {pos - 1}")


def _decode(buf: bytes, pos: int) -> tuple[Any, int]:
    mt, info, arg, pos = _read_head(buf, pos)

    if mt == _MT_UINT:
        if arg == _INDEFINITE:
            raise CborDecodeError("indefinite length not allowed for integers")
        return arg, pos
    if mt == _MT_NINT:
        if arg == _INDEFINITE:
            raise CborDecodeError("indefinite length not allowed for integers")
        return -1 - arg, pos
    if mt == _MT_BSTR:
        return _decode_bytes(buf, pos, arg)
    if mt == _MT_TSTR:
        raw_bytes, end = _decode_bytes(buf, pos, arg, text=True)
        try:
            return raw_bytes.decode("utf-8"), end
        except UnicodeDecodeError as exc:
            raise CborDecodeError(f"invalid UTF-8 in text string: {exc}") from exc
    if mt == _MT_ARRAY:
        return _decode_array(buf, pos, arg)
    if mt == _MT_MAP:
        return _decode_map(buf, pos, arg)
    if mt == _MT_TAG:
        # PagerOS does not use semantic tags. Decode the tagged item but
        # surface the inner value transparently; the SDK keeps no record
        # of the tag number (registry §2.4 reserves no v1 tag use).
        if arg == _INDEFINITE:
            raise CborDecodeError("indefinite length not allowed for tags")
        inner, end = _decode(buf, pos)
        return inner, end
    if mt == _MT_SIMPLE:
        return _decode_mt7(info, arg, pos)
    raise CborDecodeError(f"unknown major type {mt}")  # pragma: no cover


def _decode_bytes(
    buf: bytes,
    pos: int,
    arg: int,
    *,
    text: bool = False,
) -> tuple[bytes, int]:
    if arg != _INDEFINITE:
        if pos + arg > len(buf):
            raise CborDecodeError("truncated byte/text string")
        return buf[pos : pos + arg], pos + arg
    # Indefinite-length string: concatenate definite-length chunks of
    # the matching major type until the break stop code.
    expected_mt = _MT_TSTR if text else _MT_BSTR
    out = bytearray()
    while True:
        if pos >= len(buf):
            raise CborDecodeError("unterminated indefinite-length string")
        if buf[pos] == _BREAK:
            return bytes(out), pos + 1
        chunk_mt, _info, chunk_arg, chunk_pos = _read_head(buf, pos)
        if chunk_mt != expected_mt or chunk_arg == _INDEFINITE:
            raise CborDecodeError(
                "indefinite-length string chunk has wrong shape"
            )
        if chunk_pos + chunk_arg > len(buf):
            raise CborDecodeError("truncated indefinite-length string chunk")
        out += buf[chunk_pos : chunk_pos + chunk_arg]
        pos = chunk_pos + chunk_arg


def _decode_array(buf: bytes, pos: int, arg: int) -> tuple[list[Any], int]:
    out: list[Any] = []
    if arg != _INDEFINITE:
        for _ in range(arg):
            item, pos = _decode(buf, pos)
            out.append(item)
        return out, pos
    while True:
        if pos >= len(buf):
            raise CborDecodeError("unterminated indefinite-length array")
        if buf[pos] == _BREAK:
            return out, pos + 1
        item, pos = _decode(buf, pos)
        out.append(item)


def _decode_map(buf: bytes, pos: int, arg: int) -> tuple[dict[Any, Any], int]:
    out: dict[Any, Any] = {}
    if arg != _INDEFINITE:
        for _ in range(arg):
            key, pos = _decode(buf, pos)
            if isinstance(key, (list, dict)):
                raise CborDecodeError(
                    "unhashable CBOR map key (composite types not supported)"
                )
            val, pos = _decode(buf, pos)
            out[key] = val
        return out, pos
    while True:
        if pos >= len(buf):
            raise CborDecodeError("unterminated indefinite-length map")
        if buf[pos] == _BREAK:
            return out, pos + 1
        key, pos = _decode(buf, pos)
        if isinstance(key, (list, dict)):
            raise CborDecodeError(
                "unhashable CBOR map key (composite types not supported)"
            )
        val, pos = _decode(buf, pos)
        out[key] = val


def _decode_mt7(info: int, arg: int, pos: int) -> tuple[Any, int]:
    """Decode a major-type-7 head whose argument is already extracted."""
    # Simple values (info < 24 share value with info; info==24 carries
    # an extended simple value in the argument byte).
    if info < 24:
        return _simple(info), pos
    if info == 24:
        # Extended simple value range — only 32-255 is legal; 24-31 in
        # this slot are reserved (RFC 8949 §3.3).
        if arg < 32:
            raise CborDecodeError(f"invalid simple value {arg}")
        raise CborDecodeError(f"unsupported simple value {arg}")
    if info == 25:
        return _half_to_float(arg), pos
    if info == 26:
        return struct.unpack(">f", arg.to_bytes(4, "big"))[0], pos
    if info == 27:
        return struct.unpack(">d", arg.to_bytes(8, "big"))[0], pos
    if info == 31:
        raise CborDecodeError(
            "unexpected break stop code outside indefinite-length item"
        )
    raise CborDecodeError(f"reserved mt7 info {info}")  # pragma: no cover


def _simple(value: int) -> Any:
    if value == 20:
        return False
    if value == 21:
        return True
    if value == 22:
        return None
    if value == 23:
        # "undefined" — surface as None (spec §3.1: treat null as
        # field-absent; undefined behaves the same to consumers).
        return None
    raise CborDecodeError(f"unsupported simple value {value}")


def _half_to_float(half: int) -> float:
    """Convert a 16-bit half-precision float (IEEE 754 binary16) to ``float``.

    Used only on decode; the encoder never emits half-precision.
    """
    sign = (half >> 15) & 0x1
    exp = (half >> 10) & 0x1F
    mant = half & 0x3FF
    if exp == 0:
        val = math.ldexp(mant, -24)
    elif exp == 31:
        if mant == 0:
            val = math.inf
        else:
            return math.nan
    else:
        val = math.ldexp(mant + 1024, exp - 25)
    return -val if sign else val
