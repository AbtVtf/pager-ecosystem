"""PagerOS marketplace registry — REST API + in-memory store (MKT-002, MKT-003, MKT-006)."""

from .app import create_app
from .challenges import (
    CHALLENGE_TOKEN_PREFIX,
    CHALLENGE_TXT_PREFIX,
    Challenge,
    ChallengeAlreadyConsumedError,
    ChallengeAppIdMismatchError,
    ChallengeError,
    ChallengeExpiredError,
    ChallengeHostMismatchError,
    ChallengeNotVerifiedError,
    ChallengeStore,
    DnsTxtNotFoundError,
    UnknownChallengeError,
)
from .dns_resolver import (
    DnsLookupError,
    DnsResolver,
    DnspythonResolver,
    FailingResolver,
    FakeResolver,
)
from .moderation import (
    LogEntry,
    ModerationError,
    ModerationStore,
    Report,
    ReportAlreadyClosedError,
    ReportStatus,
    UnknownReportError,
)
from .store import (
    ALLOWED_TAGS,
    AppEntry,
    DuplicateAppError,
    InvalidTagError,
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
    "InvalidTagError",
    "ALLOWED_TAGS",
    # Challenges (MKT-003)
    "Challenge",
    "ChallengeStore",
    "ChallengeError",
    "UnknownChallengeError",
    "ChallengeExpiredError",
    "ChallengeNotVerifiedError",
    "ChallengeAlreadyConsumedError",
    "ChallengeHostMismatchError",
    "ChallengeAppIdMismatchError",
    "DnsTxtNotFoundError",
    "CHALLENGE_TXT_PREFIX",
    "CHALLENGE_TOKEN_PREFIX",
    # DNS resolver
    "DnsResolver",
    "DnspythonResolver",
    "FakeResolver",
    "FailingResolver",
    "DnsLookupError",
    # Moderation (MKT-006)
    "ModerationStore",
    "ModerationError",
    "Report",
    "ReportStatus",
    "UnknownReportError",
    "ReportAlreadyClosedError",
    "LogEntry",
]
