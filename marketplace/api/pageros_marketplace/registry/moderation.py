"""Moderation queue + admin action log (MKT-006).

Reports are filed by any (unauthenticated) user against an app; the moderation
queue is consumed by authenticated admins who tag the app or remove it. Every
admin action (tag-add, tag-remove, app-remove, report-resolve, report-dismiss)
appends an entry to the action log. MKT-008 (public moderation log) exposes a
read-only feed over this same log.
"""

from __future__ import annotations

import secrets
import threading
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
from enum import Enum
from typing import Iterable


class ReportStatus(str, Enum):
    OPEN = "open"
    RESOLVED = "resolved"
    DISMISSED = "dismissed"


class ModerationError(Exception):
    """Base class for moderation-layer errors."""


class UnknownReportError(ModerationError):
    def __init__(self, report_id: str) -> None:
        super().__init__(f"report not found: {report_id}")
        self.report_id = report_id


class ReportAlreadyClosedError(ModerationError):
    def __init__(self, report_id: str, status: ReportStatus) -> None:
        super().__init__(
            f"report {report_id} is already {status.value}; reopen is not supported"
        )
        self.report_id = report_id
        self.status = status


@dataclass(frozen=True)
class Report:
    """A user-filed report against an app."""

    id: str
    app_id: str
    reason: str
    reporter_contact: str | None
    status: ReportStatus
    filed_at: datetime
    resolved_at: datetime | None = None
    resolution_note: str | None = None
    resolved_by: str | None = None


@dataclass(frozen=True)
class LogEntry:
    """An immutable record of a single admin action.

    The log is append-only. MKT-008 publishes it read-only at
    ``GET /moderation/log``.
    """

    id: str
    action: str
    app_id: str | None
    actor: str
    occurred_at: datetime
    details: dict[str, object] = field(default_factory=dict)


class ModerationStore:
    """Thread-safe in-memory store for reports and the admin action log."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._reports: dict[str, Report] = {}
        self._log: list[LogEntry] = []

    # ----- reports --------------------------------------------------------- #

    def file_report(
        self,
        *,
        app_id: str,
        reason: str,
        reporter_contact: str | None = None,
    ) -> Report:
        report = Report(
            id=_new_id(),
            app_id=app_id,
            reason=reason.strip(),
            reporter_contact=(reporter_contact or None) and reporter_contact.strip(),
            status=ReportStatus.OPEN,
            filed_at=_utcnow(),
        )
        with self._lock:
            self._reports[report.id] = report
        return report

    def list_reports(
        self,
        *,
        status: ReportStatus | None = None,
        app_id: str | None = None,
        offset: int = 0,
        limit: int = 50,
    ) -> tuple[list[Report], int]:
        with self._lock:
            reports = list(self._reports.values())
        if status is not None:
            reports = [r for r in reports if r.status == status]
        if app_id is not None:
            reports = [r for r in reports if r.app_id == app_id]
        reports.sort(key=lambda r: r.filed_at, reverse=True)
        total = len(reports)
        return reports[offset : offset + limit], total

    def get_report(self, report_id: str) -> Report:
        with self._lock:
            try:
                return self._reports[report_id]
            except KeyError as exc:
                raise UnknownReportError(report_id) from exc

    def resolve_report(
        self,
        report_id: str,
        *,
        actor: str,
        note: str | None = None,
    ) -> Report:
        return self._close_report(
            report_id,
            new_status=ReportStatus.RESOLVED,
            actor=actor,
            note=note,
        )

    def dismiss_report(
        self,
        report_id: str,
        *,
        actor: str,
        note: str | None = None,
    ) -> Report:
        return self._close_report(
            report_id,
            new_status=ReportStatus.DISMISSED,
            actor=actor,
            note=note,
        )

    def _close_report(
        self,
        report_id: str,
        *,
        new_status: ReportStatus,
        actor: str,
        note: str | None,
    ) -> Report:
        with self._lock:
            current = self._reports.get(report_id)
            if current is None:
                raise UnknownReportError(report_id)
            if current.status != ReportStatus.OPEN:
                raise ReportAlreadyClosedError(report_id, current.status)
            updated = replace(
                current,
                status=new_status,
                resolved_at=_utcnow(),
                resolved_by=actor,
                resolution_note=(note or None) and note.strip(),
            )
            self._reports[report_id] = updated
        return updated

    # ----- action log ------------------------------------------------------ #

    def log(
        self,
        *,
        action: str,
        actor: str,
        app_id: str | None = None,
        details: dict[str, object] | None = None,
    ) -> LogEntry:
        entry = LogEntry(
            id=_new_id(),
            action=action,
            app_id=app_id,
            actor=actor,
            occurred_at=_utcnow(),
            details=dict(details or {}),
        )
        with self._lock:
            self._log.append(entry)
        return entry

    def list_log(
        self,
        *,
        app_id: str | None = None,
        action: str | None = None,
        offset: int = 0,
        limit: int = 100,
    ) -> tuple[list[LogEntry], int]:
        with self._lock:
            entries = list(self._log)
        if app_id is not None:
            entries = [e for e in entries if e.app_id == app_id]
        if action is not None:
            entries = [e for e in entries if e.action == action]
        # Newest first.
        entries.reverse()
        total = len(entries)
        return entries[offset : offset + limit], total

    # ----- introspection --------------------------------------------------- #

    def __len__(self) -> int:
        return len(self._reports)


# --- helpers --------------------------------------------------------------- #


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _new_id() -> str:
    return secrets.token_urlsafe(12)


def coerce_report_status(value: str | None) -> ReportStatus | None:
    if value is None:
        return None
    try:
        return ReportStatus(value)
    except ValueError as exc:
        valid = ", ".join(s.value for s in ReportStatus)
        raise ValueError(
            f"invalid report status {value!r}; valid: {valid}"
        ) from exc
