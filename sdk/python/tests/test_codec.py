"""Conformance tests for the canonical CBOR codec (PY-002).

Walks every PROTO-003 vector under ``protocol/test-vectors/ui/`` and
asserts:

- For ``kind == "encode"`` vectors, ``encode_frame(input)`` produces the
  exact bytes in the sibling ``.cbor`` file and equals
  ``descriptor["expected_cbor_hex"]``.
- For every vector (encode / decode_only / negative), the decoder parses
  the ``.cbor`` bytes without error AND re-encoding the decoded value
  reproduces the original bytes byte-for-byte (canonical round-trip).
- A handful of focused unit tests cover encoder edge cases the vectors
  do not exercise on their own (deterministic key order, float width
  selection, error paths, indefinite-length input on decode).

The vector directory is the source of truth — the test only needs the
``.cbor`` bytes and the JSON descriptor; it does not depend on any
generator internals.
"""

from __future__ import annotations

import json
import math
import struct
from pathlib import Path
from typing import Any

import pytest

from pageros.codec import (
    CborDecodeError,
    CborEncodeError,
    decode_frame,
    encode_frame,
)

# ---------------------------------------------------------------------------
# Vector discovery
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parents[3]
VECTOR_DIR = REPO_ROOT / "protocol" / "test-vectors" / "ui" / "vectors"


def _load_descriptors() -> list[dict[str, Any]]:
    if not VECTOR_DIR.is_dir():
        pytest.skip(f"PROTO-003 vector dir not found at {VECTOR_DIR}")
    out: list[dict[str, Any]] = []
    for p in sorted(VECTOR_DIR.glob("*.json")):
        out.append(json.loads(p.read_text()))
    return out


DESCRIPTORS = _load_descriptors()


def _ids(d: dict[str, Any]) -> str:
    return d["name"]


def _expand_bytes_markers(value: Any) -> Any:
    """Mirror the descriptor convention: ``{"$bytes": "<hex>"}`` → bytes.

    Used when comparing a freshly-decoded value back against the
    original descriptor ``input`` for round-trip equality.
    """
    if isinstance(value, dict):
        if len(value) == 1 and "$bytes" in value and isinstance(value["$bytes"], str):
            return bytes.fromhex(value["$bytes"])
        return {k: _expand_bytes_markers(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand_bytes_markers(v) for v in value]
    return value


# ---------------------------------------------------------------------------
# Conformance against PROTO-003 vectors
# ---------------------------------------------------------------------------


def test_vector_dir_discovered() -> None:
    assert DESCRIPTORS, "no PROTO-003 vectors discovered"


@pytest.mark.parametrize("desc", DESCRIPTORS, ids=_ids)
def test_descriptor_matches_sibling_cbor_file(desc: dict[str, Any]) -> None:
    """Every descriptor records the same bytes as its sibling .cbor file."""
    cbor_path = VECTOR_DIR / f"{desc['name']}.cbor"
    on_disk = cbor_path.read_bytes()
    assert on_disk.hex() == desc["expected_cbor_hex"]
    assert len(on_disk) == desc["expected_size_bytes"]


@pytest.mark.parametrize(
    "desc",
    [d for d in DESCRIPTORS if d["kind"] == "encode"],
    ids=_ids,
)
def test_encode_matches_vector_bytes(desc: dict[str, Any]) -> None:
    """``encode_frame(input)`` reproduces the canonical bytes exactly."""
    encoded = encode_frame(desc["input"])
    assert encoded.hex() == desc["expected_cbor_hex"], (
        f"{desc['name']}: encode mismatch\n"
        f"  expected: {desc['expected_cbor_hex']}\n"
        f"  got:      {encoded.hex()}"
    )


@pytest.mark.parametrize(
    "desc",
    [d for d in DESCRIPTORS if d["kind"] == "encode"],
    ids=_ids,
)
def test_encode_roundtrip_equals_input(desc: dict[str, Any]) -> None:
    """``decode_frame(encode_frame(input))`` yields the logical input.

    The original descriptor input is normalised first by expanding any
    ``{"$bytes": "<hex>"}`` markers, since those are how JSON
    descriptors carry binary payloads.
    """
    encoded = encode_frame(desc["input"])
    decoded = decode_frame(encoded)
    expected = _expand_bytes_markers(desc["input"])
    assert decoded == expected


@pytest.mark.parametrize("desc", DESCRIPTORS, ids=_ids)
def test_canonical_roundtrip(desc: dict[str, Any]) -> None:
    """Every vector's bytes round-trip through decode → encode unchanged."""
    cbor_path = VECTOR_DIR / f"{desc['name']}.cbor"
    raw = cbor_path.read_bytes()
    value = decode_frame(raw)
    re_encoded = encode_frame(value)
    assert re_encoded == raw, (
        f"{desc['name']}: canonical round-trip diverged"
    )


# ---------------------------------------------------------------------------
# Focused encoder unit tests
# ---------------------------------------------------------------------------


def test_map_keys_sorted_by_canonical_cbor_bytes() -> None:
    # Map with three keys whose canonical CBOR-encoded forms have
    # different lengths. RFC 8949 §4.2.1 sorts by encoded bytes
    # (lexicographic), so shorter keys come first.
    frame: dict[Any, Any] = {"bb": 2, 10: "ten", "a": 1}
    encoded = encode_frame(frame)
    # Header is map-of-3 (0xa3). Then keys must appear in the order:
    #   0x0a ("10" as uint), 0x61 0x61 ("a"), 0x62 0x62 0x62 ("bb").
    assert encoded[0] == 0xA3
    body = encoded[1:]
    # Each (key, value) pair sequence, in canonical order:
    expected = b"\x0a"          # key: uint 10
    expected += b"\x63ten"      # value: tstr(3) "ten"
    expected += b"\x61a"        # key: tstr(1) "a"
    expected += b"\x01"         # value: uint 1
    expected += b"\x62bb"       # key: tstr(2) "bb"
    expected += b"\x02"         # value: uint 2
    assert body == expected


def test_integer_widths_use_smallest_head() -> None:
    # 0..23 → single-byte; 24..255 → 0x18 prefix; 256..65535 → 0x19; etc.
    assert encode_frame(0) == b"\x00"
    assert encode_frame(23) == b"\x17"
    assert encode_frame(24) == b"\x18\x18"
    assert encode_frame(255) == b"\x18\xff"
    assert encode_frame(256) == b"\x19\x01\x00"
    assert encode_frame(65535) == b"\x19\xff\xff"
    assert encode_frame(65536) == b"\x1a\x00\x01\x00\x00"
    assert encode_frame(2**32 - 1) == b"\x1a\xff\xff\xff\xff"
    assert encode_frame(2**32) == b"\x1b\x00\x00\x00\x01\x00\x00\x00\x00"
    # Negative integers use major type 1 with arg = -1 - n.
    assert encode_frame(-1) == b"\x20"
    assert encode_frame(-24) == b"\x37"
    assert encode_frame(-25) == b"\x38\x18"


def test_uint64_boundary_round_trips() -> None:
    big = 2**64 - 1
    encoded = encode_frame(big)
    assert encoded == b"\x1b" + (big).to_bytes(8, "big")
    assert decode_frame(encoded) == big


def test_integer_overflow_raises() -> None:
    with pytest.raises(CborEncodeError):
        encode_frame(2**64)
    with pytest.raises(CborEncodeError):
        encode_frame(-(2**64) - 1)


def test_float_prefers_32_bit_when_round_trippable() -> None:
    # 1.5 has an exact binary32 representation; expect single-precision.
    encoded = encode_frame(1.5)
    assert encoded[0] == 0xFA
    assert encoded == b"\xfa" + struct.pack(">f", 1.5)
    # A value with more precision than binary32 can carry → 64-bit.
    encoded = encode_frame(1.1)
    assert encoded[0] == 0xFB
    assert encoded == b"\xfb" + struct.pack(">d", 1.1)


def test_float_special_values_encode() -> None:
    inf = encode_frame(float("inf"))
    assert inf[0] == 0xFA
    assert decode_frame(inf) == float("inf")
    nan = encode_frame(float("nan"))
    assert nan[0] == 0xFA
    assert math.isnan(decode_frame(nan))


def test_dollar_bytes_marker_expands_to_byte_string() -> None:
    encoded = encode_frame({"$bytes": "04a1b2c3"})
    assert encoded == b"\x44\x04\xa1\xb2\xc3"
    assert decode_frame(encoded) == b"\x04\xa1\xb2\xc3"


def test_dollar_bytes_inside_nested_map() -> None:
    frame = {"uid": {"$bytes": "deadbeef"}, "n": 1}
    encoded = encode_frame(frame)
    decoded = decode_frame(encoded)
    assert decoded == {"uid": b"\xde\xad\xbe\xef", "n": 1}


def test_invalid_dollar_bytes_hex_raises() -> None:
    with pytest.raises(CborEncodeError):
        encode_frame({"$bytes": "not hex"})


def test_unsupported_type_raises_encode_error() -> None:
    with pytest.raises(CborEncodeError):
        encode_frame(object())
    with pytest.raises(CborEncodeError):
        encode_frame({1, 2, 3})


def test_bytes_input_encodes_as_byte_string() -> None:
    encoded = encode_frame(b"\x00\x01\x02")
    assert encoded == b"\x43\x00\x01\x02"
    encoded = encode_frame(bytearray(b"hi"))
    assert encoded == b"\x42hi"


def test_lists_and_tuples_encode_identically() -> None:
    assert encode_frame([1, 2, 3]) == encode_frame((1, 2, 3))


def test_none_true_false_encode_as_simple_values() -> None:
    assert encode_frame(None) == b"\xf6"
    assert encode_frame(True) == b"\xf5"
    assert encode_frame(False) == b"\xf4"


# ---------------------------------------------------------------------------
# Focused decoder unit tests
# ---------------------------------------------------------------------------


def test_decode_rejects_trailing_bytes() -> None:
    with pytest.raises(CborDecodeError):
        decode_frame(b"\x00\x00")


def test_decode_rejects_truncated_head() -> None:
    with pytest.raises(CborDecodeError):
        decode_frame(b"\x18")  # uint with 1-byte arg but no byte follows


def test_decode_rejects_truncated_payload() -> None:
    # bstr(4) header but only 2 payload bytes.
    with pytest.raises(CborDecodeError):
        decode_frame(b"\x44\x00\x01")


def test_decode_rejects_invalid_utf8_text_string() -> None:
    with pytest.raises(CborDecodeError):
        decode_frame(b"\x62\xff\xff")  # tstr(2) of 0xff 0xff


def test_decode_accepts_indefinite_length_array() -> None:
    # [_ 1, 2, 3 ]
    raw = b"\x9f\x01\x02\x03\xff"
    assert decode_frame(raw) == [1, 2, 3]


def test_decode_accepts_indefinite_length_map() -> None:
    # {_ "a": 1, "b": 2 }
    raw = b"\xbf\x61a\x01\x61b\x02\xff"
    assert decode_frame(raw) == {"a": 1, "b": 2}


def test_decode_accepts_indefinite_length_byte_string() -> None:
    # _( h'aabb', h'ccdd' )
    raw = b"\x5f\x42\xaa\xbb\x42\xcc\xdd\xff"
    assert decode_frame(raw) == b"\xaa\xbb\xcc\xdd"


def test_decode_accepts_half_precision_float() -> None:
    # 0xf9 0x3c 0x00 = half-precision 1.0
    assert decode_frame(b"\xf9\x3c\x00") == 1.0


def test_decode_handles_undefined_as_none() -> None:
    assert decode_frame(b"\xf7") is None


def test_decode_rejects_composite_map_key() -> None:
    # { [1]: 2 } — array key not allowed by this codec.
    raw = b"\xa1\x81\x01\x02"
    with pytest.raises(CborDecodeError):
        decode_frame(raw)


def test_decode_rejects_unterminated_indefinite_array() -> None:
    with pytest.raises(CborDecodeError):
        decode_frame(b"\x9f\x01\x02")  # no break


def test_decode_strips_semantic_tag_layer() -> None:
    # tag(0) "2020-01-01" — PagerOS doesn't use tags; surface the inner.
    raw = b"\xc0\x6a" + b"2020-01-01"
    assert decode_frame(raw) == "2020-01-01"


def test_break_at_top_level_is_an_error() -> None:
    with pytest.raises(CborDecodeError):
        decode_frame(b"\xff")
