"""Publish-time DNS TXT challenge service (MKT-003).

Publishing flow (SPEC §10.3):

1. Developer ``POST /apps/challenges`` with ``{app_id, url}``. Marketplace
   issues a single-use token and tells the caller which TXT record to create
   (``_pageros-challenge.<host>``) with the token value.
2. Developer adds the TXT record at their DNS provider and waits for it to
   propagate.
3. Developer ``POST /apps/challenges/{id}/verify``. Marketplace performs a
   live TXT lookup; on a match, the challenge is marked verified.
4. Developer ``POST /apps`` with the manifest, supplying the verified
   challenge token. Registration checks that the token matches the manifest's
   ``url`` host and consumes it.

The challenge store is in-memory; persistence is deferred to MKT-011 alongside
the registry's storage backend.
"""

from __future__ import annotations

import secrets
import threading
from dataclasses import dataclass, field, replace
from datetime import datetime, timedelta, timezone
from typing import Callable
from urllib.parse import urlparse

from .dns_resolver import DnsLookupError, DnsResolver

CHALLENGE_TXT_PREFIX = "_pageros-challenge"
CHALLENGE_TOKEN_PREFIX = "pageros-challenge="
DEFAULT_CHALLENGE_TTL = timedelta(hours=1)
DEFAULT_VERIFIED_TTL = timedelta(hours=24)


class ChallengeError(Exception):
    """Base class for challenge-service errors."""


class UnknownChallengeError(ChallengeError):
    def __init__(self, challenge_id: str) -> None:
        super().__init__(f"challenge not found: {challenge_id}")
        self.challenge_id = challenge_id


class ChallengeExpiredError(ChallengeError):
    def __init__(self, challenge_id: str) -> None:
        super().__init__(f"challenge expired: {challenge_id}")
        self.challenge_id = challenge_id


class ChallengeNotVerifiedError(ChallengeError):
    def __init__(self, challenge_id: str) -> None:
        super().__init__(f"challenge not yet verified: {challenge_id}")
        self.challenge_id = challenge_id


class ChallengeAlreadyConsumedError(ChallengeError):
    def __init__(self, challenge_id: str) -> None:
        super().__init__(f"challenge already consumed: {challenge_id}")
        self.challenge_id = challenge_id


class ChallengeHostMismatchError(ChallengeError):
    def __init__(self, expected_host: str, actual_host: str) -> None:
        super().__init__(
            f"challenge host mismatch: token is for {expected_host!r}, "
            f"manifest URL host is {actual_host!r}"
        )
        self.expected_host = expected_host
        self.actual_host = actual_host


class ChallengeAppIdMismatchError(ChallengeError):
    def __init__(self, expected_app_id: str, actual_app_id: str) -> None:
        super().__init__(
            f"challenge app_id mismatch: token issued for {expected_app_id!r}, "
            f"manifest id is {actual_app_id!r}"
        )
        self.expected_app_id = expected_app_id
        self.actual_app_id = actual_app_id


class DnsTxtNotFoundError(ChallengeError):
    """Raised on `verify` when the expected TXT value is absent from DNS."""

    def __init__(self, txt_name: str, expected_value: str, observed: list[str]) -> None:
        observed_repr = ", ".join(repr(s) for s in observed) or "(none)"
        super().__init__(
            f"expected TXT value {expected_value!r} not found at {txt_name!r}; "
            f"observed: {observed_repr}"
        )
        self.txt_name = txt_name
        self.expected_value = expected_value
        self.observed = list(observed)


@dataclass(frozen=True)
class Challenge:
    """A pending DNS TXT challenge for a publish flow.

    The challenge proves the caller controls ``host`` (derived from the
    manifest URL) by demonstrating they can set a TXT record at
    ``txt_name`` containing ``txt_value``.
    """

    id: str
    app_id: str
    host: str
    token: str
    created_at: datetime
    expires_at: datetime
    verified_at: datetime | None = None
    consumed_at: datetime | None = None
    last_lookup: tuple[datetime, list[str]] | None = field(default=None, repr=False)

    @property
    def txt_name(self) -> str:
        return f"{CHALLENGE_TXT_PREFIX}.{self.host}"

    @property
    def txt_value(self) -> str:
        return f"{CHALLENGE_TOKEN_PREFIX}{self.token}"

    @property
    def verified(self) -> bool:
        return self.verified_at is not None

    @property
    def consumed(self) -> bool:
        return self.consumed_at is not None

    def is_expired(self, now: datetime) -> bool:
        return now >= self.expires_at


class ChallengeStore:
    """Thread-safe in-memory store for DNS TXT challenges.

    Lookups by ``id`` for the verify endpoint and by ``token`` for the
    register endpoint are both O(1).
    """

    def __init__(
        self,
        resolver: DnsResolver,
        *,
        ttl: timedelta = DEFAULT_CHALLENGE_TTL,
        verified_ttl: timedelta = DEFAULT_VERIFIED_TTL,
        clock: Callable[[], datetime] | None = None,
    ) -> None:
        self._resolver = resolver
        self._ttl = ttl
        self._verified_ttl = verified_ttl
        self._now = clock or _utcnow
        self._lock = threading.Lock()
        self._by_id: dict[str, Challenge] = {}
        self._by_token: dict[str, str] = {}

    # ----- writes ---------------------------------------------------------- #

    def create(self, *, app_id: str, url: str) -> Challenge:
        host = _host_from_url(url)
        now = self._now()
        challenge = Challenge(
            id=_new_id(),
            app_id=app_id,
            host=host,
            token=_new_token(),
            created_at=now,
            expires_at=now + self._ttl,
        )
        with self._lock:
            self._by_id[challenge.id] = challenge
            self._by_token[challenge.token] = challenge.id
        return challenge

    def verify(self, challenge_id: str) -> Challenge:
        with self._lock:
            current = self._by_id.get(challenge_id)
            if current is None:
                raise UnknownChallengeError(challenge_id)
            now = self._now()
            if current.is_expired(now) and current.verified_at is None:
                raise ChallengeExpiredError(challenge_id)
            if current.consumed:
                raise ChallengeAlreadyConsumedError(challenge_id)

        # Run the DNS lookup outside the lock — it does I/O.
        try:
            records = self._resolver.resolve_txt(current.txt_name)
        except DnsLookupError:
            raise
        if current.txt_value not in records:
            with self._lock:
                self._by_id[challenge_id] = replace(
                    current,
                    last_lookup=(self._now(), records),
                )
            raise DnsTxtNotFoundError(current.txt_name, current.txt_value, records)

        with self._lock:
            now = self._now()
            updated = replace(
                self._by_id.get(challenge_id, current),
                verified_at=now,
                expires_at=now + self._verified_ttl,
                last_lookup=(now, records),
            )
            self._by_id[challenge_id] = updated
            return updated

    def consume(
        self,
        *,
        token: str,
        app_id: str,
        url: str,
    ) -> Challenge:
        """Spend a verified token at registration time.

        Raises if the token is unknown, unverified, expired, already used, or
        if the supplied ``app_id``/``url`` don't match what the token was
        issued for.
        """
        host = _host_from_url(url)
        with self._lock:
            challenge_id = self._by_token.get(token)
            if challenge_id is None:
                raise UnknownChallengeError(token)
            current = self._by_id[challenge_id]
            if current.consumed:
                raise ChallengeAlreadyConsumedError(challenge_id)
            if current.verified_at is None:
                raise ChallengeNotVerifiedError(challenge_id)
            now = self._now()
            if current.is_expired(now):
                raise ChallengeExpiredError(challenge_id)
            if current.app_id != app_id:
                raise ChallengeAppIdMismatchError(current.app_id, app_id)
            if current.host != host:
                raise ChallengeHostMismatchError(current.host, host)

            consumed = replace(current, consumed_at=now)
            self._by_id[challenge_id] = consumed
            # Token cannot be reused, but keep it indexed so a duplicate
            # consume returns a clear "already consumed" error rather than
            # "unknown".
            return consumed

    # ----- reads ----------------------------------------------------------- #

    def get(self, challenge_id: str) -> Challenge:
        with self._lock:
            try:
                return self._by_id[challenge_id]
            except KeyError as exc:
                raise UnknownChallengeError(challenge_id) from exc

    # ----- maintenance ----------------------------------------------------- #

    def purge_expired(self) -> int:
        """Drop unverified, expired challenges. Returns the count removed."""
        removed = 0
        now = self._now()
        with self._lock:
            for challenge_id, ch in list(self._by_id.items()):
                if ch.verified_at is None and ch.is_expired(now):
                    del self._by_id[challenge_id]
                    self._by_token.pop(ch.token, None)
                    removed += 1
        return removed

    def __len__(self) -> int:
        return len(self._by_id)


# --- helpers --------------------------------------------------------------- #


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _new_id() -> str:
    return secrets.token_urlsafe(12)


def _new_token() -> str:
    # 32 bytes ~= 43 url-safe base64 chars; >=256 bits of entropy.
    return secrets.token_urlsafe(32)


def _host_from_url(url: str) -> str:
    parsed = urlparse(url)
    if not parsed.scheme or not parsed.netloc:
        raise ValueError(f"manifest url must be an absolute http(s) URL: {url!r}")
    host = parsed.hostname
    if not host:
        raise ValueError(f"manifest url has no host: {url!r}")
    return host.lower()
