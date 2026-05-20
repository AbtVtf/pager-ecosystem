"""FastAPI app factory for the marketplace registry (MKT-002)."""

from __future__ import annotations

from fastapi import Depends, FastAPI, HTTPException, Path, Query, Request, status
from fastapi.responses import JSONResponse

from ..manifest import Manifest, ManifestValidationError, validate_manifest
from .schemas import AppRecord, ErrorField, ErrorResponse, Page
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
    404: {"model": ErrorResponse, "description": "App not found"},
    409: {"model": ErrorResponse, "description": "App already registered"},
    422: {"model": ErrorResponse, "description": "Request body could not be parsed"},
}


def create_app(registry: Registry | None = None) -> FastAPI:
    """Build a FastAPI app backed by ``registry`` (a fresh in-memory one by default)."""
    registry = registry if registry is not None else Registry()

    app = FastAPI(
        title="PagerOS Marketplace Registry",
        version="0.1.0",
        summary="Registry REST API for PagerOS apps (SPEC §10).",
        description=(
            "Implements MKT-002: register, list, get-by-id, search-by-keyword "
            "over PagerOS app manifests (schema in SPEC §10.2). DNS TXT "
            "verification (MKT-003) and moderation tagging (MKT-006) are layered "
            "on top of this surface in later tasks."
        ),
    )
    app.state.registry = registry

    _register_exception_handlers(app)
    _register_routes(app)
    return app


# --- dependencies ---------------------------------------------------------- #


def _get_registry(request: Request) -> Registry:
    return request.app.state.registry


# --- routes ---------------------------------------------------------------- #


def _register_routes(app: FastAPI) -> None:
    @app.get("/healthz", tags=["meta"], summary="Liveness probe")
    def healthz() -> dict[str, str]:
        return {"status": "ok"}

    @app.post(
        "/apps",
        tags=["apps"],
        status_code=status.HTTP_201_CREATED,
        response_model=AppRecord,
        responses=_ERROR_RESPONSES,
        summary="Register a new app",
        description=(
            "Validates the submitted manifest against SPEC §10.2 and stores it. "
            "Rejects duplicates by `id`; DNS TXT verification (MKT-003) is not "
            "yet enforced — registered apps land untagged (`verified=false`)."
        ),
    )
    def register_app(
        manifest: Manifest,
        registry: Registry = Depends(_get_registry),
    ) -> AppRecord:
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
            "`{app_id}`, and `manifest.version` must strictly increase over the "
            "current stored version."
        ),
    )
    def update_app(
        manifest: Manifest,
        app_id: str = Path(pattern=_APP_ID_PATTERN, examples=["notes.mafu.dev"]),
        registry: Registry = Depends(_get_registry),
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
