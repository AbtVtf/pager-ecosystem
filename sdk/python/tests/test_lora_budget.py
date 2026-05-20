"""Tests for the PY-011 LoRa size budget warning."""

from __future__ import annotations

import logging

import pytest

from pageros.lora_budget import (
    LORA_FRAME_BUDGET_BYTES,
    check_frame_size,
)


def test_budget_constant_is_200_bytes() -> None:
    assert LORA_FRAME_BUDGET_BYTES == 200


def test_no_warning_when_app_is_not_lora_compatible(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.WARNING, logger="pageros.lora"):
        warned = check_frame_size(b"x" * 500, lora_compatible=False)
    assert warned is False
    assert caplog.records == []


def test_no_warning_at_exactly_the_budget(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.WARNING, logger="pageros.lora"):
        warned = check_frame_size(b"y" * LORA_FRAME_BUDGET_BYTES, lora_compatible=True)
    assert warned is False
    assert caplog.records == []


def test_warns_when_over_budget(caplog: pytest.LogCaptureFixture) -> None:
    encoded = b"z" * (LORA_FRAME_BUDGET_BYTES + 1)
    with caplog.at_level(logging.WARNING, logger="pageros.lora"):
        warned = check_frame_size(encoded, lora_compatible=True)
    assert warned is True
    assert len(caplog.records) == 1
    record = caplog.records[0]
    assert record.levelno == logging.WARNING
    assert "201" in record.getMessage()
    assert "200" in record.getMessage()


def test_warning_includes_frame_label_when_given(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.WARNING, logger="pageros.lora"):
        check_frame_size(
            b"q" * 300, lora_compatible=True, frame_label="GET /home"
        )
    assert any("GET /home" in r.getMessage() for r in caplog.records)


def test_accepts_size_as_integer(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level(logging.WARNING, logger="pageros.lora"):
        warned = check_frame_size(250, lora_compatible=True)
    assert warned is True
    assert "250" in caplog.records[0].getMessage()


def test_accepts_bytearray_and_memoryview() -> None:
    payload = bytearray(b"a" * 220)
    assert check_frame_size(payload, lora_compatible=True) is True
    assert check_frame_size(memoryview(payload), lora_compatible=True) is True


def test_rejects_negative_size() -> None:
    with pytest.raises(ValueError):
        check_frame_size(-1, lora_compatible=True)
