"""FastAPI app factory for the marketplace registry (MKT-002 + MKT-003)."""

from __future__ import annotations

from fastapi import Depends, FastAPI, Header, HTTPException, Path, Query, Request, status
from fastapi.responses import JSONResponse

from ..manifest import Manifest, ManifestValidationError, validate_manifest
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
from .schemas import (
    AppRecord,
    ChallengeRecord,
    ChallengeRequest,
    ErrorField,
    ErrorResponse,
    Page,
)
from .store import (
    DuplicateAppError,
    Registry,
    UnknownAppError,
    VersionNotIncreasedError,
)

# Reverse-DNS id constraint mirrors ``Manifest.id``; documented in OpenAPI.
_APP_ID_PATTERN = r"^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$"

_ERROR_RESPONSES: dict[int | str, dict] = {
    400: {"model": ErrorResponse, "description": "Manifest validation failed"},
    403: {"model": ErrorResponse, "description": "DNS TXT challenge not satisfied"},
    404: {"model": ErrorResponse, "description": "App or challenge not found"},
    409: {"model": ErrorResponse, "description": "App already registered"},
    422: {"model": ErrorResponse, "description": "Request body could not be parsed"},
}

_CHALLENGE_HEADER = "X-Challenge-Token"


def create_app(
    registry: Registry | None = None,
    *,
    challenges: ChallengeStore | None = None,
    dns_resolver: DnsResolver | None = None,
    challenge_required: bool = True,
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
    """
    registry = registry if registry is not None else Registry()
    resolver = dns_resolver if dns_resolver is not None else DnspythonResolver()
    challenge_store = (
        challenges if challenges is not None else ChallengeStore(resolver)
    )

    app = FastAPI(
        title="PagerOS Marketplace Registry",
        version="0.2.0",
        summary="Registry REST API for PagerOS apps (SPEC §10).",
        description=(
            "Implements MKT-002 (registry CRUD + search) and MKT-003 (DNS TXT "
            "publish-time challenge). Moderation tagging (MKT-006) layers on "
            "top of this surface in later tasks."
        ),
    )
    app.state.registry = registry
    app.state.challenges = challenge_store
    app.state.challenge_required = challenge_required

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
    ) -> Page[AppRecord]:
        entries, total = registry.list(offset=offset, limit=limit, category=category)
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
        responses={404: _ERROR_RESPONSES[404]},
        summary="Delete an app",
    )
    def delete_app(
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
    ) -> None:
        try:
            registry.delete(app_id)
        except UnknownAppError as exc:
            raise _not_found(exc) from exc


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
        # Normalize structured error payloads to the ErrorResponse shape.
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
        return JSONResponse(status_code=exc.status_code, content=payload)


# Re-export for convenience: ``from pageros_marketplace.registry.app import validate_manifest``
# isn't part of the public surface, but mypy users sometimes want it. Keep as
# a private alias to avoid a confusing public re-export.
_ = validate_manifest
