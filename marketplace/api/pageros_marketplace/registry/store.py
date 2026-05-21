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
    #: When the ``featured`` trust tag was most recently applied. ``None``
    #: when the app is not currently featured. Used by featured curation
    #: tooling (MKT-010) to surface recently-pinned apps at the top of the
    #: in-device "Apps" home screen.
    featured_at: datetime | None = None

    @property
    def id(self) -> str:
        return self.manifest.id

    @property
    def is_featured(self) -> bool:
        return "featured" in self.tags


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
        now = datetime.now(timezone.utc)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            featured_at = _next_featured_at(
                current_tags=current.tags,
                new_tags=normalized,
                current_featured_at=current.featured_at,
                now=now,
            )
            entry = replace(
                current,
                tags=normalized,
                updated_at=now,
                featured_at=featured_at,
            )
            self._entries[app_id] = entry
            return entry

    def add_tag(self, app_id: str, tag: str) -> AppEntry:
        """Idempotently add a single trust tag."""
        if tag not in ALLOWED_TAGS:
            raise InvalidTagError(tag)
        now = datetime.now(timezone.utc)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            if tag in current.tags:
                return current
            new_tags = tuple(sorted({*current.tags, tag}))
            featured_at = _next_featured_at(
                current_tags=current.tags,
                new_tags=new_tags,
                current_featured_at=current.featured_at,
                now=now,
            )
            entry = replace(
                current,
                tags=new_tags,
                updated_at=now,
                featured_at=featured_at,
            )
            self._entries[app_id] = entry
            return entry

    def pin_featured(self, app_id: str) -> AppEntry:
        """Pin (or re-pin) an app as featured.

        Idempotently adds the ``featured`` tag and always bumps
        ``featured_at`` to ``now``. Re-pinning is useful when admins want
        to promote an app back to the top of the in-device Apps home
        (MKT-005 / MKT-010) without juggling other trust tags.
        """
        now = datetime.now(timezone.utc)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            new_tags = tuple(sorted({*current.tags, "featured"}))
            entry = replace(
                current,
                tags=new_tags,
                updated_at=now,
                featured_at=now,
            )
            self._entries[app_id] = entry
            return entry

    def unpin_featured(self, app_id: str) -> AppEntry:
        """Unpin an app from featured (idempotent)."""
        now = datetime.now(timezone.utc)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            if "featured" not in current.tags:
                return current
            new_tags = tuple(t for t in current.tags if t != "featured")
            entry = replace(
                current,
                tags=new_tags,
                updated_at=now,
                featured_at=None,
            )
            self._entries[app_id] = entry
            return entry

    def remove_tag(self, app_id: str, tag: str) -> AppEntry:
        """Idempotently remove a single trust tag."""
        now = datetime.now(timezone.utc)
        with self._lock:
            current = self._entries.get(app_id)
            if current is None:
                raise UnknownAppError(app_id)
            if tag not in current.tags:
                return current
            new_tags = tuple(t for t in current.tags if t != tag)
            featured_at = _next_featured_at(
                current_tags=current.tags,
                new_tags=new_tags,
                current_featured_at=current.featured_at,
                now=now,
            )
            entry = replace(
                current,
                tags=new_tags,
                updated_at=now,
                featured_at=featured_at,
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
        tag: str | None = None,
        featured_first: bool = False,
    ) -> tuple[list[AppEntry], int]:
        """Return ``(page, total)`` with deterministic ordering.

        Default ordering is ``id`` ASC. When ``featured_first=True`` the
        featured-tagged apps come first, ordered by ``featured_at`` DESC
        (most recently pinned first), tie-broken by ``id`` ASC; then the
        rest of the catalog in ``id`` ASC. The Shell's in-device "Apps"
        home (MKT-005) uses ``featured_first=True`` to surface curated
        picks at the top (MKT-010).

        ``tag`` filters to entries that carry that trust tag (MKT-009).
        Pass ``"verified"`` for the Shell's verified-only view; the
        sentinel ``"unverified"`` matches entries with no admin tags
        applied (the implicit default per SPEC §10.5).
        """
        with self._lock:
            entries: Iterable[AppEntry] = self._entries.values()
            if category is not None:
                cat = category.lower()
                entries = (e for e in entries if cat in e.manifest.categories)
            if tag is not None:
                tag_norm = tag.lower()
                if tag_norm == "unverified":
                    entries = (e for e in entries if not e.tags)
                elif tag_norm in ALLOWED_TAGS:
                    entries = (e for e in entries if tag_norm in e.tags)
                else:
                    raise InvalidTagError(tag)
            if featured_first:
                ordered = sorted(entries, key=_featured_first_key)
            else:
                ordered = sorted(entries, key=lambda e: e.manifest.id)
        total = len(ordered)
        return ordered[offset : offset + limit], total

    def list_featured(
        self,
        *,
        offset: int = 0,
        limit: int = 50,
    ) -> tuple[list[AppEntry], int]:
        """Return ``(page, total)`` of currently-featured apps.

        Ordered by ``featured_at`` DESC (most recently pinned first), with
        ``id`` ASC as the tie-breaker. Feeds the MKT-010 curation surface
        and the MKT-005 Shell home "Featured" rail.
        """
        with self._lock:
            featured = [e for e in self._entries.values() if e.is_featured]
        featured.sort(key=_featured_first_key)
        total = len(featured)
        return featured[offset : offset + limit], total

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


def _next_featured_at(
    *,
    current_tags: tuple[str, ...],
    new_tags: tuple[str, ...],
    current_featured_at: datetime | None,
    now: datetime,
) -> datetime | None:
    """Decide the ``featured_at`` timestamp for the next entry state.

    - ``featured`` newly added → set to ``now`` (new pin).
    - ``featured`` newly removed → clear to ``None``.
    - ``featured`` unchanged → keep the existing value (preserve pin order).
    """
    was_featured = "featured" in current_tags
    is_featured = "featured" in new_tags
    if is_featured and not was_featured:
        return now
    if was_featured and not is_featured:
        return None
    return current_featured_at


def _featured_first_key(entry: "AppEntry") -> tuple[int, float, str]:
    """Sort key that puts featured entries (newest pin first) ahead of the rest."""
    if entry.is_featured and entry.featured_at is not None:
        # Negative timestamp → DESC order on featured_at.
        return (0, -entry.featured_at.timestamp(), entry.manifest.id)
    return (1, 0.0, entry.manifest.id)
