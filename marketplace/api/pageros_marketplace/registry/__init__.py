"""PagerOS marketplace registry — REST API + in-memory store (MKT-002)."""

from .app import create_app
from .store import (
    AppEntry,
    DuplicateAppError,
    Registry,
    UnknownAppError,
    VersionNotIncreasedError,
)

__all__ = [
    "create_app",
    "AppEntry",
    "Registry",
    "DuplicateAppError",
    "UnknownAppError",
    "VersionNotIncreasedError",
]
