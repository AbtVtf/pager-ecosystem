"""LoRa size budget warning (PY-011).

SPEC §1 (Design principles) and §8.4 give Frames a hard ceiling of 200 B
encoded payload so a Frame fits in one LoRa packet. Apps that opt into
`lora_compatible=True` promise to stay under that ceiling, so the SDK
needs to tell developers when a response would violate it.

This module exposes a single helper, :func:`check_frame_size`, that
inspects an already-encoded Frame and logs a single warning per
oversized Frame. It does not raise — going over budget is a soft failure
that still works over Wi-Fi, and we don't want a misconfigured Frame to
crash an app server. Wiring this in at the Frame-encode boundary is left
to PY-002.
"""

from __future__ import annotations

import logging
from typing import Union

log = logging.getLogger("pageros.lora")

LORA_FRAME_BUDGET_BYTES = 200


def check_frame_size(
    encoded: Union[bytes, bytearray, memoryview, int],
    *,
    lora_compatible: bool,
    frame_label: str | None = None,
) -> bool:
    """Warn if ``encoded`` exceeds the LoRa Frame budget.

    ``encoded`` is either the CBOR-encoded Frame bytes or its length in
    bytes. ``lora_compatible`` mirrors the app's manifest flag — when
    False the budget does not apply and no warning is emitted.

    Returns ``True`` if a warning was logged, ``False`` otherwise.
    """
    if not lora_compatible:
        return False

    if isinstance(encoded, int):
        if encoded < 0:
            raise ValueError("encoded size must be non-negative")
        size = encoded
    else:
        size = len(encoded)

    if size <= LORA_FRAME_BUDGET_BYTES:
        return False

    overage = size - LORA_FRAME_BUDGET_BYTES
    label = f" ({frame_label})" if frame_label else ""
    log.warning(
        "Frame%s encoded size %d B exceeds LoRa budget of %d B by %d B; "
        "this app is lora_compatible=true and will fragment or be dropped over LoRa",
        label,
        size,
        LORA_FRAME_BUDGET_BYTES,
        overage,
    )
    return True
