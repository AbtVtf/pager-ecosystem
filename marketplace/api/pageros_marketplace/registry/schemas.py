"""Wire schemas for the marketplace REST API.

Requests reuse the canonical ``Manifest`` model from MKT-001. Responses wrap a
manifest with registry-side metadata (timestamps, trust tags).
"""

from __future__ import annotations

from datetime import datetime
from typing import Generic, TypeVar

from pydantic import BaseModel, ConfigDict, Field

from ..manifest import Manifest
from .store import AppEntry

T = TypeVar("T")


class AppRecord(BaseModel):
    """A registered app: the manifest plus marketplace metadata."""

    model_config = ConfigDict(extra="forbid")

    manifest: Manifest
    tags: list[str] = Field(default_factory=list)
    created_at: datetime
    updated_at: datetime

    @classmethod
    def from_entry(cls, entry: AppEntry) -> "AppRecord":
        return cls(
            manifest=entry.manifest,
            tags=list(entry.tags),
            created_at=entry.created_at,
            updated_at=entry.updated_at,
        )


class Page(BaseModel, Generic[T]):
    """Paginated envelope."""

    model_config = ConfigDict(extra="forbid")

    items: list[T]
    total: int = Field(ge=0)
    offset: int = Field(ge=0)
    limit: int = Field(ge=1)


class ErrorField(BaseModel):
    model_config = ConfigDict(extra="forbid")
    field: str
    message: str


class ErrorResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")
    error: str
    detail: str | None = None
    fields: list[ErrorField] | None = None
