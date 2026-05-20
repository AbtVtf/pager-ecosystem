"""Wire schemas for the marketplace REST API.

Requests reuse the canonical ``Manifest`` model from MKT-001. Responses wrap a
manifest with registry-side metadata (timestamps, trust tags).
"""

from __future__ import annotations

from datetime import datetime
from typing import Generic, TypeVar

from pydantic import BaseModel, ConfigDict, Field

from ..manifest import Manifest
from .challenges import Challenge
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


class ChallengeRequest(BaseModel):
    """Body of ``POST /apps/challenges``."""

    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    app_id: str = Field(min_length=3, max_length=253)
    url: str = Field(min_length=1)


class ChallengeRecord(BaseModel):
    """Wire shape of a publish-time DNS TXT challenge."""

    model_config = ConfigDict(extra="forbid")

    id: str
    app_id: str
    host: str
    token: str
    txt_name: str
    txt_value: str
    created_at: datetime
    expires_at: datetime
    verified_at: datetime | None = None
    consumed_at: datetime | None = None

    @classmethod
    def from_challenge(cls, ch: Challenge) -> "ChallengeRecord":
        return cls(
            id=ch.id,
            app_id=ch.app_id,
            host=ch.host,
            token=ch.token,
            txt_name=ch.txt_name,
            txt_value=ch.txt_value,
            created_at=ch.created_at,
            expires_at=ch.expires_at,
            verified_at=ch.verified_at,
            consumed_at=ch.consumed_at,
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
