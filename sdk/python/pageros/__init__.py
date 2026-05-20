"""PagerOS Python SDK."""

from pageros.lora_budget import LORA_FRAME_BUDGET_BYTES, check_frame_size
from pageros.manifest import App, Maintainer
from pageros.signing import (
    DEFAULT_MAX_SKEW_SECONDS,
    HEADER_DEVICE,
    HEADER_SIG,
    HEADER_TIMESTAMP,
    BadSignature,
    InvalidEncoding,
    MissingHeader,
    SignatureError,
    SignatureVerifierMiddleware,
    TimestampSkew,
    VerifiedRequest,
    build_signing_input,
    compute_body_hash,
    sign_request,
    verify_request,
)

__version__ = "0.0.1"

__all__ = [
    "App",
    "BadSignature",
    "DEFAULT_MAX_SKEW_SECONDS",
    "HEADER_DEVICE",
    "HEADER_SIG",
    "HEADER_TIMESTAMP",
    "InvalidEncoding",
    "LORA_FRAME_BUDGET_BYTES",
    "Maintainer",
    "MissingHeader",
    "SignatureError",
    "SignatureVerifierMiddleware",
    "TimestampSkew",
    "VerifiedRequest",
    "__version__",
    "build_signing_input",
    "check_frame_size",
    "compute_body_hash",
    "sign_request",
    "verify_request",
]
