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
from .moderation import LogEntry, Report, ReportStatus
from .store import ALLOWED_TAGS, AppEntry

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


# --- moderation wire models (MKT-006) -------------------------------------- #


_TAG_LITERALS = sorted(ALLOWED_TAGS)


class TagSetRequest(BaseModel):
    """Body of ``PUT /admin/apps/{app_id}/tags``."""

    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    tags: list[str] = Field(
        default_factory=list,
        description=f"Trust tags. Allowed: {_TAG_LITERALS}.",
    )


class ReportRequest(BaseModel):
    """Body of ``POST /reports``."""

    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    app_id: str = Field(min_length=3, max_length=253)
    reason: str = Field(min_length=3, max_length=1000)
    reporter_contact: str | None = Field(default=None, max_length=200)


class ReportResolutionRequest(BaseModel):
    """Body of ``POST /admin/reports/{id}/{resolve,dismiss}``."""

    model_config = ConfigDict(extra="forbid", str_strip_whitespace=True)

    note: str | None = Field(default=None, max_length=2000)


class ReportRecord(BaseModel):
    """Wire shape of a user-filed report."""

    model_config = ConfigDict(extra="forbid")

    id: str
    app_id: str
    reason: str
    reporter_contact: str | None
    status: ReportStatus
    filed_at: datetime
    resolved_at: datetime | None = None
    resolution_note: str | None = None
    resolved_by: str | None = None

    @classmethod
    def from_report(cls, r: Report) -> "ReportRecord":
        return cls(
            id=r.id,
            app_id=r.app_id,
            reason=r.reason,
            reporter_contact=r.reporter_contact,
            status=r.status,
            filed_at=r.filed_at,
            resolved_at=r.resolved_at,
            resolution_note=r.resolution_note,
            resolved_by=r.resolved_by,
        )


class LogRecord(BaseModel):
    """Wire shape of a moderation action log entry."""

    model_config = ConfigDict(extra="forbid")

    id: str
    action: str
    app_id: str | None
    actor: str
    occurred_at: datetime
    details: dict[str, object] = Field(default_factory=dict)

    @classmethod
    def from_entry(cls, e: LogEntry) -> "LogRecord":
        return cls(
            id=e.id,
            action=e.action,
            app_id=e.app_id,
            actor=e.actor,
            occurred_at=e.occurred_at,
            details=dict(e.details),
        )
