"""DNS TXT lookup abstraction used by the publish-time challenge (MKT-003).

The publishing flow needs to read `_pageros-challenge.<host>` TXT records to
prove a developer controls the URL listed in their manifest. The real
implementation uses ``dnspython``; tests inject ``FakeResolver`` with a fixed
record table.
"""

from __future__ import annotations

from typing import Protocol


class DnsLookupError(Exception):
    """Raised when a TXT lookup fails (NXDOMAIN, timeout, network error)."""


class DnsResolver(Protocol):
    """Resolve TXT records for a fully-qualified DNS name."""

    def resolve_txt(self, name: str) -> list[str]:  # pragma: no cover - protocol
        ...


class DnspythonResolver:
    """Default resolver backed by ``dnspython``.

    Each strand of a TXT record is returned as a separate string. Callers
    looking for an exact match should compare against each returned value
    independently — DNS clients are allowed to split long TXT records into
    multiple strings within a single RDATA, and we don't try to merge them.
    """

    def __init__(self, *, timeout: float = 5.0) -> None:
        self._timeout = timeout

    def resolve_txt(self, name: str) -> list[str]:
        try:
            import dns.resolver
            import dns.exception
        except ImportError as exc:  # pragma: no cover - tested via missing install
            raise DnsLookupError(
                "dnspython is required for live DNS lookups; install the package "
                "or inject a custom DnsResolver"
            ) from exc

        resolver = dns.resolver.Resolver()
        resolver.lifetime = self._timeout
        resolver.timeout = self._timeout
        try:
            answer = resolver.resolve(name, "TXT")
        except dns.resolver.NXDOMAIN:
            return []
        except dns.resolver.NoAnswer:
            return []
        except dns.exception.DNSException as exc:
            raise DnsLookupError(f"DNS TXT lookup for {name!r} failed: {exc}") from exc

        records: list[str] = []
        for rdata in answer:
            for strand in rdata.strings:
                try:
                    records.append(strand.decode("utf-8"))
                except UnicodeDecodeError:
                    # Non-UTF-8 TXT strands cannot be challenge tokens; skip.
                    continue
        return records


class FakeResolver:
    """Test resolver with a pre-seeded TXT record table."""

    def __init__(self, records: dict[str, list[str]] | None = None) -> None:
        self._records: dict[str, list[str]] = dict(records or {})
        self.lookups: list[str] = []

    def set(self, name: str, values: list[str]) -> None:
        self._records[name] = list(values)

    def add(self, name: str, value: str) -> None:
        self._records.setdefault(name, []).append(value)

    def clear(self) -> None:
        self._records.clear()

    def resolve_txt(self, name: str) -> list[str]:
        self.lookups.append(name)
        return list(self._records.get(name, []))


class FailingResolver:
    """Resolver that raises ``DnsLookupError`` — handy for error-path tests."""

    def __init__(self, message: str = "simulated DNS failure") -> None:
        self._message = message

    def resolve_txt(self, name: str) -> list[str]:
        raise DnsLookupError(self._message)
