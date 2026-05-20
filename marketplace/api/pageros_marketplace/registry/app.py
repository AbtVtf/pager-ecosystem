"""FastAPI app factory for the marketplace registry (MKT-002 + MKT-003 + MKT-006 + MKT-008 + MKT-010)."""

from __future__ import annotations

import secrets

from fastapi import Depends, FastAPI, Header, HTTPException, Path, Query, Request, status
from fastapi.responses import HTMLResponse, JSONResponse

from ..manifest import Manifest, ManifestValidationError, validate_manifest
from .admin_ui import render_admin_index
from .challenges import (
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
from .dns_resolver import DnsLookupError, DnsResolver, DnspythonResolver
from .moderation import (
    ModerationStore,
    ReportAlreadyClosedError,
    ReportStatus,
    UnknownReportError,
    coerce_report_status,
)
from .schemas import (
    AppRecord,
    ChallengeRecord,
    ChallengeRequest,
    ErrorField,
    ErrorResponse,
    LogRecord,
    Page,
    ReportRecord,
    ReportRequest,
    ReportResolutionRequest,
    TagSetRequest,
)
from .store import (
    ALLOWED_TAGS,
    DuplicateAppError,
    InvalidTagError,
    Registry,
    UnknownAppError,
    VersionNotIncreasedError,
)

# Reverse-DNS id constraint mirrors ``Manifest.id``; documented in OpenAPI.
_APP_ID_PATTERN = r"^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$"

_ERROR_RESPONSES: dict[int | str, dict] = {
    400: {"model": ErrorResponse, "description": "Manifest validation failed"},
    401: {"model": ErrorResponse, "description": "Admin auth missing or invalid"},
    403: {"model": ErrorResponse, "description": "DNS TXT challenge not satisfied"},
    404: {"model": ErrorResponse, "description": "App, challenge, or report not found"},
    409: {"model": ErrorResponse, "description": "App already registered"},
    422: {"model": ErrorResponse, "description": "Request body could not be parsed"},
}

_CHALLENGE_HEADER = "X-Challenge-Token"
_ADMIN_AUTH_SCHEME = "Bearer"


def create_app(
    registry: Registry | None = None,
    *,
    challenges: ChallengeStore | None = None,
    dns_resolver: DnsResolver | None = None,
    challenge_required: bool = True,
    moderation: ModerationStore | None = None,
    admin_token: str | None = None,
) -> FastAPI:
    """Build a FastAPI app backed by ``registry``.

    Parameters
    ----------
    registry:
        Storage backend for app manifests. Defaults to a fresh in-memory
        :class:`Registry`.
    challenges:
        Store for publish-time DNS TXT challenges (MKT-003). Defaults to a
        fresh in-memory store backed by ``dns_resolver``.
    dns_resolver:
        Resolver used by the challenge store to look up TXT records. Defaults
        to :class:`DnspythonResolver` for live DNS lookups. Tests inject a
        :class:`FakeResolver` instead.
    challenge_required:
        When ``True`` (default), ``POST /apps`` requires an
        ``X-Challenge-Token`` header bound to a verified challenge for the
        manifest URL's host. Set to ``False`` in tests or local-dev setups
        that pre-date the challenge gate.
    moderation:
        Store for user reports + the admin action log (MKT-006 / MKT-008).
        Defaults to a fresh in-memory store.
    admin_token:
        Bearer token required by the ``/admin/...`` endpoints. When ``None``,
        the admin surface is unauthenticated (useful for tests and local
        development). Production deployments MUST set a non-empty token.
    """
    registry = registry if registry is not None else Registry()
    resolver = dns_resolver if dns_resolver is not None else DnspythonResolver()
    challenge_store = (
        challenges if challenges is not None else ChallengeStore(resolver)
    )
    moderation_store = moderation if moderation is not None else ModerationStore()

    app = FastAPI(
        title="PagerOS Marketplace Registry",
        version="0.3.0",
        summary="Registry REST API for PagerOS apps (SPEC §10).",
        description=(
            "Implements MKT-002 (registry CRUD + search), MKT-003 (DNS TXT "
            "publish-time challenge), MKT-006 (moderation queue + admin "
            "tooling), MKT-008 (public moderation log), and MKT-010 "
            "(featured curation tooling). The admin surface lives under "
            "`/admin` and is gated by an admin bearer token; the public "
            "moderation log at `GET /moderation/log` mirrors the admin "
            "action log read-only with no auth."
        ),
    )
    app.state.registry = registry
    app.state.challenges = challenge_store
    app.state.challenge_required = challenge_required
    app.state.moderation = moderation_store
    app.state.admin_token = admin_token or None

    _register_exception_handlers(app)
    _register_routes(app)
    return app


# --- dependencies ---------------------------------------------------------- #


def _get_registry(request: Request) -> Registry:
    return request.app.state.registry


def _get_challenges(request: Request) -> ChallengeStore:
    return request.app.state.challenges


def _challenge_required(request: Request) -> bool:
    return bool(request.app.state.challenge_required)


def _get_moderation(request: Request) -> ModerationStore:
    return request.app.state.moderation


def _require_admin(
    request: Request,
    authorization: str | None = Header(default=None, alias="Authorization"),
) -> str:
    """Verify the admin bearer token and return the caller's actor name.

    When ``admin_token`` is unset (test / local-dev mode), the admin surface
    is unauthenticated and the actor is reported as ``"local"``.
    """
    configured = request.app.state.admin_token
    if not configured:
        return "local"
    if not authorization:
        raise _admin_unauthorized("missing Authorization header")
    scheme, _, token = authorization.partition(" ")
    if scheme.lower() != _ADMIN_AUTH_SCHEME.lower() or not token:
        raise _admin_unauthorized(
            f"expected {_ADMIN_AUTH_SCHEME} authorization scheme"
        )
    if not secrets.compare_digest(token.strip(), configured):
        raise _admin_unauthorized("invalid admin token")
    return "admin"


def _admin_unauthorized(detail: str) -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_401_UNAUTHORIZED,
        detail={"error": "admin_unauthorized", "detail": detail},
        headers={"WWW-Authenticate": _ADMIN_AUTH_SCHEME},
    )


# --- routes ---------------------------------------------------------------- #


def _register_routes(app: FastAPI) -> None:
    @app.get("/healthz", tags=["meta"], summary="Liveness probe")
    def healthz() -> dict[str, str]:
        return {"status": "ok"}

    @app.post(
        "/apps/challenges",
        tags=["challenges"],
        status_code=status.HTTP_201_CREATED,
        response_model=ChallengeRecord,
        responses={400: _ERROR_RESPONSES[400], 422: _ERROR_RESPONSES[422]},
        summary="Start a publish-time DNS TXT challenge",
        description=(
            "Issues a single-use challenge for the given `app_id` + manifest "
            "`url`. The caller must publish a TXT record at "
            "`_pageros-challenge.<host>` containing the returned `txt_value` "
            "before calling `POST /apps/challenges/{id}/verify`."
        ),
    )
    def create_challenge(
        body: ChallengeRequest,
        challenges: ChallengeStore = Depends(_get_challenges),
    ) -> ChallengeRecord:
        try:
            ch = challenges.create(app_id=body.app_id, url=body.url)
        except ValueError as exc:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail={"error": "invalid_url", "detail": str(exc)},
            ) from exc
        return ChallengeRecord.from_challenge(ch)

    @app.post(
        "/apps/challenges/{challenge_id}/verify",
        tags=["challenges"],
        response_model=ChallengeRecord,
        responses={
            400: _ERROR_RESPONSES[400],
            403: _ERROR_RESPONSES[403],
            404: _ERROR_RESPONSES[404],
            409: {"model": ErrorResponse, "description": "Challenge expired or consumed"},
            502: {"model": ErrorResponse, "description": "DNS lookup failed"},
        },
        summary="Verify a DNS TXT challenge",
        description=(
            "Performs a TXT lookup for `_pageros-challenge.<host>` and marks "
            "the challenge verified iff the expected token value is present."
        ),
    )
    def verify_challenge(
        challenge_id: str = Path(min_length=1, max_length=64),
        challenges: ChallengeStore = Depends(_get_challenges),
    ) -> ChallengeRecord:
        try:
            ch = challenges.verify(challenge_id)
        except UnknownChallengeError as exc:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail={"error": "challenge_not_found", "detail": str(exc)},
            ) from exc
        except ChallengeExpiredError as exc:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail={"error": "challenge_expired", "detail": str(exc)},
            ) from exc
        except ChallengeAlreadyConsumedError as exc:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail={"error": "challenge_already_consumed", "detail": str(exc)},
            ) from exc
        except DnsTxtNotFoundError as exc:
            raise HTTPException(
                status_code=status.HTTP_403_FORBIDDEN,
                detail={
                    "error": "dns_txt_not_found",
                    "detail": str(exc),
                },
            ) from exc
        except DnsLookupError as exc:
            raise HTTPException(
                status_code=status.HTTP_502_BAD_GATEWAY,
                detail={"error": "dns_lookup_failed", "detail": str(exc)},
            ) from exc
        return ChallengeRecord.from_challenge(ch)

    @app.get(
        "/apps/challenges/{challenge_id}",
        tags=["challenges"],
        response_model=ChallengeRecord,
        responses={404: _ERROR_RESPONSES[404]},
        summary="Inspect a challenge",
    )
    def get_challenge(
        challenge_id: str = Path(min_length=1, max_length=64),
        challenges: ChallengeStore = Depends(_get_challenges),
    ) -> ChallengeRecord:
        try:
            return ChallengeRecord.from_challenge(challenges.get(challenge_id))
        except UnknownChallengeError as exc:
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail={"error": "challenge_not_found", "detail": str(exc)},
            ) from exc

    @app.post(
        "/apps",
        tags=["apps"],
        status_code=status.HTTP_201_CREATED,
        response_model=AppRecord,
        responses=_ERROR_RESPONSES,
        summary="Register a new app",
        description=(
            "Validates the submitted manifest against SPEC §10.2 and stores "
            "it. Requires `X-Challenge-Token` set to a verified DNS TXT "
            "challenge bound to the manifest's URL host (MKT-003). Apps land "
            "untagged (`tags=[]`) — moderation (MKT-006) flips trust tags "
            "after the fact."
        ),
    )
    def register_app(
        manifest: Manifest,
        challenge_token: str | None = Header(default=None, alias=_CHALLENGE_HEADER),
        registry: Registry = Depends(_get_registry),
        challenges: ChallengeStore = Depends(_get_challenges),
        gate: bool = Depends(_challenge_required),
    ) -> AppRecord:
        if gate:
            _consume_challenge(
                challenges,
                token=challenge_token,
                app_id=manifest.id,
                url=manifest.url,
            )
        try:
            entry = registry.register(manifest)
        except DuplicateAppError as exc:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail={"error": "duplicate_app", "detail": str(exc)},
            ) from exc
        return AppRecord.from_entry(entry)

    @app.get(
        "/apps",
        tags=["apps"],
        response_model=Page[AppRecord],
        summary="List registered apps",
    )
    def list_apps(
        registry: Registry = Depends(_get_registry),
        offset: int = Query(0, ge=0),
        limit: int = Query(50, ge=1, le=200),
        category: str | None = Query(
            None,
            min_length=1,
            description="Filter to apps that include this category slug.",
        ),
        featured_first: bool = Query(
            False,
            description=(
                "If true, sort featured apps first (most recently pinned "
                "first), then the rest of the catalog (id ASC). The "
                "in-device Apps home (MKT-005) sets this to surface "
                "curated picks at the top (MKT-010)."
            ),
        ),
    ) -> Page[AppRecord]:
        entries, total = registry.list(
            offset=offset,
            limit=limit,
            category=category,
            featured_first=featured_first,
        )
        return Page[AppRecord](
            items=[AppRecord.from_entry(e) for e in entries],
            total=total,
            offset=offset,
            limit=limit,
        )

    @app.get(
        "/apps/featured",
        tags=["apps"],
        response_model=Page[AppRecord],
        summary="List featured apps (MKT-010)",
        description=(
            "Returns the apps that admins have pinned with the `featured` "
            "trust tag, ordered by `featured_at` DESC (most recently "
            "pinned first), then `id` ASC. Public, unauthenticated. "
            "Feeds the in-device Apps home (MKT-005) and any external "
            "discovery surfaces that want to mirror the editorial picks."
        ),
    )
    def list_featured_apps(
        registry: Registry = Depends(_get_registry),
        offset: int = Query(0, ge=0),
        limit: int = Query(50, ge=1, le=200),
    ) -> Page[AppRecord]:
        entries, total = registry.list_featured(offset=offset, limit=limit)
        return Page[AppRecord](
            items=[AppRecord.from_entry(e) for e in entries],
            total=total,
            offset=offset,
            limit=limit,
        )

    @app.get(
        "/apps/search",
        tags=["apps"],
        response_model=Page[AppRecord],
        summary="Search apps by keyword",
        description=(
            "Case-insensitive substring search across `id`, `name`, "
            "`description`, and `categories`. Empty `q` returns the same shape "
            "as `GET /apps`."
        ),
    )
    def search_apps(
        q: str = Query("", description="Search keyword (case-insensitive)."),
        offset: int = Query(0, ge=0),
        limit: int = Query(50, ge=1, le=200),
        registry: Registry = Depends(_get_registry),
    ) -> Page[AppRecord]:
        entries, total = registry.search(q, offset=offset, limit=limit)
        return Page[AppRecord](
            items=[AppRecord.from_entry(e) for e in entries],
            total=total,
            offset=offset,
            limit=limit,
        )

    @app.get(
        "/apps/{app_id}",
        tags=["apps"],
        response_model=AppRecord,
        responses=_ERROR_RESPONSES,
        summary="Get an app by id",
    )
    def get_app(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
    ) -> AppRecord:
        try:
            entry = registry.get(app_id)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        return AppRecord.from_entry(entry)

    @app.put(
        "/apps/{app_id}",
        tags=["apps"],
        response_model=AppRecord,
        responses=_ERROR_RESPONSES,
        summary="Replace an existing app's manifest",
        description=(
            "Replaces the stored manifest. `manifest.id` must equal the path "
            "`{app_id}`, and `manifest.version` must strictly increase over "
            "the current stored version. Requires `X-Challenge-Token` (a "
            "fresh verified challenge for the new manifest's URL host) when "
            "the challenge gate is enabled."
        ),
    )
    def update_app(
        manifest: Manifest,
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        challenge_token: str | None = Header(default=None, alias=_CHALLENGE_HEADER),
        registry: Registry = Depends(_get_registry),
        challenges: ChallengeStore = Depends(_get_challenges),
        gate: bool = Depends(_challenge_required),
    ) -> AppRecord:
        if manifest.id != app_id:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail={
                    "error": "id_mismatch",
                    "detail": (
                        f"manifest.id ({manifest.id!r}) does not match path "
                        f"app_id ({app_id!r})"
                    ),
                },
            )
        if gate:
            _consume_challenge(
                challenges,
                token=challenge_token,
                app_id=manifest.id,
                url=manifest.url,
            )
        try:
            entry = registry.update(app_id, manifest)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        except VersionNotIncreasedError as exc:
            raise HTTPException(
                status_code=status.HTTP_409_CONFLICT,
                detail={"error": "version_not_increased", "detail": str(exc)},
            ) from exc
        return AppRecord.from_entry(entry)

    @app.delete(
        "/apps/{app_id}",
        tags=["apps"],
        status_code=status.HTTP_204_NO_CONTENT,
        responses={
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
        },
        summary="Delete an app (admin)",
        description=(
            "Removes an app from the registry. Requires admin bearer auth "
            "when an admin token is configured. Logs an `app.delete` action "
            "to the moderation log."
        ),
    )
    def delete_app(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> None:
        try:
            registry.delete(app_id)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        moderation.log(action="app.delete", actor=actor, app_id=app_id)

    # --- public reports ----------------------------------------------------- #

    @app.post(
        "/reports",
        tags=["moderation"],
        status_code=status.HTTP_201_CREATED,
        response_model=ReportRecord,
        responses={404: _ERROR_RESPONSES[404], 422: _ERROR_RESPONSES[422]},
        summary="File a report against an app",
        description=(
            "Anyone (no auth) may file a report. The reported `app_id` must "
            "already exist in the registry. Reports enter the Trust & Safety "
            "queue (`status=open`) until an admin resolves or dismisses them."
        ),
    )
    def file_report(
        body: ReportRequest,
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
    ) -> ReportRecord:
        try:
            registry.get(body.app_id)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        report = moderation.file_report(
            app_id=body.app_id,
            reason=body.reason,
            reporter_contact=body.reporter_contact,
        )
        moderation.log(
            action="report.file",
            actor="anonymous",
            app_id=body.app_id,
            details={"report_id": report.id},
        )
        return ReportRecord.from_report(report)

    # --- admin: reports ----------------------------------------------------- #

    @app.get(
        "/admin/reports",
        tags=["admin"],
        response_model=Page[ReportRecord],
        responses={401: _ERROR_RESPONSES[401]},
        summary="List reports (admin)",
    )
    def admin_list_reports(
        status_filter: str | None = Query(
            None,
            alias="status",
            description="Filter by report status: open, resolved, dismissed.",
        ),
        app_id: str | None = Query(None),
        offset: int = Query(0, ge=0),
        limit: int = Query(50, ge=1, le=200),
        moderation: ModerationStore = Depends(_get_moderation),
        _actor: str = Depends(_require_admin),
    ) -> Page[ReportRecord]:
        try:
            parsed_status = coerce_report_status(status_filter)
        except ValueError as exc:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail={"error": "invalid_status", "detail": str(exc)},
            ) from exc
        reports, total = moderation.list_reports(
            status=parsed_status,
            app_id=app_id,
            offset=offset,
            limit=limit,
        )
        return Page[ReportRecord](
            items=[ReportRecord.from_report(r) for r in reports],
            total=total,
            offset=offset,
            limit=limit,
        )

    @app.get(
        "/admin/reports/{report_id}",
        tags=["admin"],
        response_model=ReportRecord,
        responses={
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
        },
        summary="Get a report (admin)",
    )
    def admin_get_report(
        report_id: str = Path(min_length=1, max_length=64),
        moderation: ModerationStore = Depends(_get_moderation),
        _actor: str = Depends(_require_admin),
    ) -> ReportRecord:
        try:
            return ReportRecord.from_report(moderation.get_report(report_id))
        except UnknownReportError as exc:
            raise _report_not_found(exc) from exc

    @app.post(
        "/admin/reports/{report_id}/resolve",
        tags=["admin"],
        response_model=ReportRecord,
        responses={
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
            409: {"model": ErrorResponse, "description": "Report already closed"},
        },
        summary="Resolve a report (admin)",
        description=(
            "Marks the report `resolved`. Use this when the admin took an "
            "action on the reported app (tag change, removal) or otherwise "
            "agrees with the reporter."
        ),
    )
    def admin_resolve_report(
        body: ReportResolutionRequest,
        report_id: str = Path(min_length=1, max_length=64),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> ReportRecord:
        return _close_report_route(
            moderation,
            report_id=report_id,
            actor=actor,
            note=body.note,
            action="resolve",
        )

    @app.post(
        "/admin/reports/{report_id}/dismiss",
        tags=["admin"],
        response_model=ReportRecord,
        responses={
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
            409: {"model": ErrorResponse, "description": "Report already closed"},
        },
        summary="Dismiss a report (admin)",
        description=(
            "Marks the report `dismissed`. Use this when the report is not "
            "actionable (spam, no policy violation)."
        ),
    )
    def admin_dismiss_report(
        body: ReportResolutionRequest,
        report_id: str = Path(min_length=1, max_length=64),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> ReportRecord:
        return _close_report_route(
            moderation,
            report_id=report_id,
            actor=actor,
            note=body.note,
            action="dismiss",
        )

    # --- admin: trust tags -------------------------------------------------- #

    @app.put(
        "/admin/apps/{app_id}/tags",
        tags=["admin"],
        response_model=AppRecord,
        responses={
            400: _ERROR_RESPONSES[400],
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
        },
        summary="Replace trust tags (admin)",
        description=(
            "Replaces the full set of trust tags on an app. Allowed tags: "
            f"{sorted(ALLOWED_TAGS)}."
        ),
    )
    def admin_set_tags(
        body: TagSetRequest,
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> AppRecord:
        try:
            entry = registry.set_tags(app_id, body.tags)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        except InvalidTagError as exc:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail={"error": "invalid_tag", "detail": str(exc)},
            ) from exc
        moderation.log(
            action="app.tags.set",
            actor=actor,
            app_id=app_id,
            details={"tags": list(entry.tags)},
        )
        return AppRecord.from_entry(entry)

    @app.post(
        "/admin/apps/{app_id}/tags/{tag}",
        tags=["admin"],
        response_model=AppRecord,
        responses={
            400: _ERROR_RESPONSES[400],
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
        },
        summary="Add a single trust tag (admin)",
    )
    def admin_add_tag(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        tag: str = Path(min_length=1, max_length=32),
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> AppRecord:
        try:
            entry = registry.add_tag(app_id, tag)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        except InvalidTagError as exc:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail={"error": "invalid_tag", "detail": str(exc)},
            ) from exc
        moderation.log(
            action="app.tag.add",
            actor=actor,
            app_id=app_id,
            details={"tag": tag},
        )
        return AppRecord.from_entry(entry)

    @app.delete(
        "/admin/apps/{app_id}/tags/{tag}",
        tags=["admin"],
        response_model=AppRecord,
        responses={
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
        },
        summary="Remove a single trust tag (admin)",
    )
    def admin_remove_tag(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        tag: str = Path(min_length=1, max_length=32),
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> AppRecord:
        try:
            entry = registry.remove_tag(app_id, tag)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        moderation.log(
            action="app.tag.remove",
            actor=actor,
            app_id=app_id,
            details={"tag": tag},
        )
        return AppRecord.from_entry(entry)

    # --- public: moderation log (MKT-008) ----------------------------------- #

    @app.get(
        "/moderation/log",
        tags=["moderation"],
        response_model=Page[LogRecord],
        summary="Public moderation log (MKT-008)",
        description=(
            "Append-only, read-only public feed of policy decisions: app "
            "registrations are reviewed via reports, admins add/remove trust "
            "tags or delete apps, and reports are resolved or dismissed. "
            "Every such action is recorded as an entry here, newest first. "
            "No authentication required. Entries are stable: an entry is "
            "never modified or removed after it is appended. Per SPEC §10, "
            "this is the canonical public record of marketplace moderation."
        ),
    )
    def public_moderation_log(
        app_id: str | None = Query(
            None,
            description="Filter entries to a single app id.",
        ),
        action: str | None = Query(
            None,
            description=(
                "Filter to a single action key (e.g. `app.delete`, "
                "`app.tag.add`, `app.tag.remove`, `app.tags.set`, "
                "`report.file`, `report.resolve`, `report.dismiss`)."
            ),
        ),
        offset: int = Query(0, ge=0),
        limit: int = Query(100, ge=1, le=500),
        moderation: ModerationStore = Depends(_get_moderation),
    ) -> Page[LogRecord]:
        entries, total = moderation.list_log(
            app_id=app_id, action=action, offset=offset, limit=limit
        )
        return Page[LogRecord](
            items=[LogRecord.from_entry(e) for e in entries],
            total=total,
            offset=offset,
            limit=limit,
        )

    # --- admin: featured curation (MKT-010) -------------------------------- #

    @app.post(
        "/admin/apps/{app_id}/featured",
        tags=["admin"],
        response_model=AppRecord,
        responses={
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
        },
        summary="Pin app as featured (admin, MKT-010)",
        description=(
            "Idempotently sets the `featured` trust tag and bumps "
            "`featured_at` to now (a re-pin moves the app back to the "
            "top of the curated list). Logs an `app.featured.pin` action "
            "to the moderation log."
        ),
    )
    def admin_pin_featured(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> AppRecord:
        try:
            entry = registry.pin_featured(app_id)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        moderation.log(
            action="app.featured.pin",
            actor=actor,
            app_id=app_id,
            details={"featured_at": entry.featured_at.isoformat() if entry.featured_at else None},
        )
        return AppRecord.from_entry(entry)

    @app.delete(
        "/admin/apps/{app_id}/featured",
        tags=["admin"],
        response_model=AppRecord,
        responses={
            401: _ERROR_RESPONSES[401],
            404: _ERROR_RESPONSES[404],
        },
        summary="Unpin app from featured (admin, MKT-010)",
        description=(
            "Idempotently removes the `featured` trust tag and clears "
            "`featured_at`. Logs an `app.featured.unpin` action to the "
            "moderation log."
        ),
    )
    def admin_unpin_featured(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
        actor: str = Depends(_require_admin),
    ) -> AppRecord:
        try:
            entry = registry.unpin_featured(app_id)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc
        moderation.log(
            action="app.featured.unpin",
            actor=actor,
            app_id=app_id,
        )
        return AppRecord.from_entry(entry)

    # --- admin: action log -------------------------------------------------- #

    @app.get(
        "/admin/log",
        tags=["admin"],
        response_model=Page[LogRecord],
        responses={401: _ERROR_RESPONSES[401]},
        summary="Read the moderation action log (admin)",
        description=(
            "Append-only log of admin actions and report events. The same "
            "entries are also exposed unauthenticated at "
            "`GET /moderation/log` (MKT-008)."
        ),
    )
    def admin_list_log(
        app_id: str | None = Query(None),
        action: str | None = Query(None),
        offset: int = Query(0, ge=0),
        limit: int = Query(100, ge=1, le=500),
        moderation: ModerationStore = Depends(_get_moderation),
        _actor: str = Depends(_require_admin),
    ) -> Page[LogRecord]:
        entries, total = moderation.list_log(
            app_id=app_id, action=action, offset=offset, limit=limit
        )
        return Page[LogRecord](
            items=[LogRecord.from_entry(e) for e in entries],
            total=total,
            offset=offset,
            limit=limit,
        )

    # --- admin UI ----------------------------------------------------------- #

    @app.get(
        "/admin/ui",
        tags=["admin"],
        response_class=HTMLResponse,
        responses={401: _ERROR_RESPONSES[401]},
        summary="Admin moderation dashboard (HTML)",
        description=(
            "Single-page HTML view of the open reports queue + registered "
            "apps with their tags. Provides forms for tag mutation, app "
            "removal, and report resolution. Auth: admin bearer token."
        ),
        include_in_schema=False,
    )
    def admin_ui(
        registry: Registry = Depends(_get_registry),
        moderation: ModerationStore = Depends(_get_moderation),
        _actor: str = Depends(_require_admin),
    ) -> HTMLResponse:
        apps, _ = registry.list(offset=0, limit=200)
        open_reports, _ = moderation.list_reports(
            status=ReportStatus.OPEN, offset=0, limit=200
        )
        log_entries, _ = moderation.list_log(offset=0, limit=50)
        html = render_admin_index(
            apps=apps,
            open_reports=open_reports,
            recent_log=log_entries,
            allowed_tags=sorted(ALLOWED_TAGS),
        )
        return HTMLResponse(html)


def _close_report_route(
    moderation: ModerationStore,
    *,
    report_id: str,
    actor: str,
    note: str | None,
    action: str,
) -> ReportRecord:
    try:
        if action == "resolve":
            report = moderation.resolve_report(report_id, actor=actor, note=note)
        else:
            report = moderation.dismiss_report(report_id, actor=actor, note=note)
    except UnknownReportError as exc:
        raise _report_not_found(exc) from exc
    except ReportAlreadyClosedError as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail={"error": "report_already_closed", "detail": str(exc)},
        ) from exc
    moderation.log(
        action=f"report.{action}",
        actor=actor,
        app_id=report.app_id,
        details={"report_id": report.id, "note": note},
    )
    return ReportRecord.from_report(report)


def _report_not_found(exc: UnknownReportError) -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_404_NOT_FOUND,
        detail={"error": "report_not_found", "detail": str(exc)},
    )


def _consume_challenge(
    challenges: ChallengeStore,
    *,
    token: str | None,
    app_id: str,
    url: str,
) -> None:
    if not token:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={
                "error": "challenge_required",
                "detail": (
                    "missing X-Challenge-Token header; obtain one via POST "
                    "/apps/challenges and verify it before registering."
                ),
            },
        )
    try:
        challenges.consume(token=token, app_id=app_id, url=url)
    except UnknownChallengeError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"error": "challenge_not_found", "detail": str(exc)},
        ) from exc
    except ChallengeNotVerifiedError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"error": "challenge_not_verified", "detail": str(exc)},
        ) from exc
    except ChallengeExpiredError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"error": "challenge_expired", "detail": str(exc)},
        ) from exc
    except ChallengeAlreadyConsumedError as exc:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail={"error": "challenge_already_consumed", "detail": str(exc)},
        ) from exc
    except ChallengeAppIdMismatchError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"error": "challenge_app_id_mismatch", "detail": str(exc)},
        ) from exc
    except ChallengeHostMismatchError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"error": "challenge_host_mismatch", "detail": str(exc)},
        ) from exc
    except ChallengeError as exc:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail={"error": "challenge_invalid", "detail": str(exc)},
        ) from exc


def _not_found(exc: UnknownAppError) -> HTTPException:
    return HTTPException(
        status_code=status.HTTP_404_NOT_FOUND,
        detail={"error": "not_found", "detail": str(exc)},
    )


# --- exception handlers ---------------------------------------------------- #


def _register_exception_handlers(app: FastAPI) -> None:
    @app.exception_handler(ManifestValidationError)
    async def _manifest_validation_handler(
        _request: Request, exc: ManifestValidationError
    ) -> JSONResponse:
        return JSONResponse(
            status_code=status.HTTP_400_BAD_REQUEST,
            content=ErrorResponse(
                error="manifest_invalid",
                detail="manifest failed schema validation",
                fields=[ErrorField(field=e.field, message=e.message) for e in exc.errors],
            ).model_dump(),
        )

    @app.exception_handler(HTTPException)
    async def _http_exception_handler(
        _request: Request, exc: HTTPException
    ) -> JSONResponse:
        # Normalize structured error payloads to the ErrorResponse shape, but
        # preserve any caller-supplied headers (e.g. WWW-Authenticate).
        detail = exc.detail
        if isinstance(detail, dict):
            payload = ErrorResponse(
                error=str(detail.get("error", "http_error")),
                detail=detail.get("detail"),
            ).model_dump(exclude_none=True)
        else:
            payload = ErrorResponse(
                error="http_error",
                detail=str(detail) if detail is not None else None,
            ).model_dump(exclude_none=True)
        return JSONResponse(
            status_code=exc.status_code,
            content=payload,
            headers=dict(exc.headers) if exc.headers else None,
        )


# Re-export for convenience: ``from pageros_marketplace.registry.app import validate_manifest``
# isn't part of the public surface, but mypy users sometimes want it. Keep as
# a private alias to avoid a confusing public re-export.
_ = validate_manifest
