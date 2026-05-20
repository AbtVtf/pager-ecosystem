"""In-memory registry store for the marketplace REST API (MKT-002).

A durable backend (Postgres / SQLite) is intentionally deferred to MKT-011; the
public surface here is a small ``Registry`` class so swapping the backing
storage is a contained change.

Trust tagging (SPEC §10.5) lives on the entry. ``unverified`` is implicit (the
absence of ``verified``); ``verified`` / ``featured`` / ``flagged`` are
admin-applied tags driven by the moderation tooling in MKT-006.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
from typing import Iterable, Iterator

from ..manifest import Manifest


#: Admin-applied trust tags (SPEC §10.5). ``unverified`` is the implicit
#: default when no tag is applied.
ALLOWED_TAGS: frozenset[str] = frozenset({"verified", "featured", "flagged"})


class RegistryError(Exception):
    """Base class for registry-level errors."""


class DuplicateAppError(RegistryError):
    def __init__(self, app_id: str):
        super().__init__(f"app already registered: {app_id}")
        self.app_id = app_id


class InvalidTagError(RegistryError):
    def __init__(self, tag: str):
        super().__init__(
            f"invalid trust tag: {tag!r}. Allowed: "
            f"{', '.join(sorted(ALLOWED_TAGS))}"
        )
        self.tag = tag


class UnknownAppError(RegistryError):
    def __init__(self, app_id: str):
        super().__init__(f"app not found: {app_id}")
        self.app_id = app_id


class VersionNotIncreasedError(RegistryError):
    def __init__(self, app_id: str, current: int, submitted: int):
        super().__init__(
            f"manifest version must strictly increase for {app_id}: "
            f"current={current}, submitted={submitted}"
        )
        self.app_id = app_id
        self.current = current
        self.submitted = submitted


@dataclass(frozen=True)
class AppEntry:
    """A manifest stored in the registry, plus registry-side metadata."""

    manifest: Manifest
    tags: tuple[str, ...] = ()
    created_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    updated_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))

    @property
    def id(self) -> str:
        return self.manifest.id


class Registry:
    """Thread-safe in-memory registry keyed by manifest id (reverse-DNS)."""

    def __init__(self) -> None:
        self._entries: dict[str, AppEntry] = {}
        self._lock = threading.Lock()

    # ----- writes ---------------------------------------------------------- #

    def register(self, manifest: Manifest) -> AppEntry:
        with self._lock:
            if manifest.id in self._entries:
                raise DuplicateAppError(manifest.id)
            entry = AppEntry(manifest=manifest)
            self._entries[manifest.id] = entry
            return entry

    def update(self, app_id: str, manifest: Manifest) -> AppEntry:
        if manifest.id != app_id:
            # The path identifies the app; refuse silent renames.
            raise UnknownAppError(manifest.id)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            if manifest.version <= current.manifest.version:
                raise VersionNotIncreasedError(
                    app_id=app_id,
                    current=current.manifest.version,
                    submitted=manifest.version,
                )
            entry = replace(
                current,
                manifest=manifest,
                updated_at=datetime.now(timezone.utc),
            )
            self._entries[app_id] = entry
            return entry

    def delete(self, app_id: str) -> None:
        with self._lock:
            if self._entries.pop(app_id, None) is None:
                raise UnknownAppError(app_id)

    def set_tags(self, app_id: str, tags: Iterable[str]) -> AppEntry:
        """Replace the full trust-tag set for ``app_id``."""
        normalized = _normalize_tags(tags)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            entry = replace(
                current,
                tags=normalized,
                updated_at=datetime.now(timezone.utc),
            )
            self._entries[app_id] = entry
            return entry

    def add_tag(self, app_id: str, tag: str) -> AppEntry:
        """Idempotently add a single trust tag."""
        if tag not in ALLOWED_TAGS:
            raise InvalidTagError(tag)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            if tag in current.tags:
                return current
            entry = replace(
                current,
                tags=tuple(sorted({*current.tags, tag})),
                updated_at=datetime.now(timezone.utc),
            )
            self._entries[app_id] = entry
            return entry

    def remove_tag(self, app_id: str, tag: str) -> AppEntry:
        """Idempotently remove a single trust tag."""
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            if tag not in current.tags:
                return current
            entry = replace(
                current,
                tags=tuple(t for t in current.tags if t != tag),
                updated_at=datetime.now(timezone.utc),
            )
            self._entries[app_id] = entry
            return entry

    # ----- reads ----------------------------------------------------------- #

    def get(self, app_id: str) -> AppEntry:
        try:
            return self._entries[app_id]
        except KeyError:
            raise UnknownAppError(app_id) from None

    def list(
        self,
        *,
        offset: int = 0,
        limit: int = 50,
        category: str | None = None,
    ) -> tuple[list[AppEntry], int]:
        """Return ``(page, total)`` with deterministic ordering (id ASC)."""
        with self._lock:
            entries: Iterable[AppEntry] = self._entries.values()
            if category is not None:
                cat = category.lower()
                entries = (e for e in entries if cat in e.manifest.categories)
            ordered = sorted(entries, key=lambda e: e.manifest.id)
        total = len(ordered)
        return ordered[offset : offset + limit], total

    def search(
        self,
        query: str,
        *,
        offset: int = 0,
        limit: int = 50,
    ) -> tuple[list[AppEntry], int]:
        """Case-insensitive substring search across id, name, description, categories.

        Results are ordered by a simple priority: id/name exact-prefix match first,
        then any field match, broken by id ASC.
        """
        q = query.strip().lower()
        if not q:
            return self.list(offset=offset, limit=limit)
        with self._lock:
            snapshot = list(self._entries.values())

        scored: list[tuple[int, str, AppEntry]] = []
        for entry in snapshot:
            m = entry.manifest
            haystacks = [
                m.id.lower(),
                m.name.lower(),
                m.description.lower(),
                " ".join(c.lower() for c in m.categories),
            ]
            if not any(q in h for h in haystacks):
                continue
            if m.id.lower().startswith(q) or m.name.lower().startswith(q):
                rank = 0
            elif q in m.id.lower() or q in m.name.lower():
                rank = 1
            else:
                rank = 2
            scored.append((rank, m.id, entry))

        scored.sort(key=lambda t: (t[0], t[1]))
        total = len(scored)
        return [e for _, _, e in scored[offset : offset + limit]], total

    # ----- introspection --------------------------------------------------- #

    def __len__(self) -> int:
        return len(self._entries)

    def __iter__(self) -> Iterator[AppEntry]:
        return iter(list(self._entries.values()))

    def clear(self) -> None:
        with self._lock:
            self._entries.clear()


# --- helpers --------------------------------------------------------------- #


def _normalize_tags(tags: Iterable[str]) -> tuple[str, ...]:
    seen: set[str] = set()
    for tag in tags:
        if tag not in ALLOWED_TAGS:
            raise InvalidTagError(tag)
        seen.add(tag)
    return tuple(sorted(seen))
