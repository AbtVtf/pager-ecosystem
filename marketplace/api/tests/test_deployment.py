"""Smoke tests for the deployment artifacts (MKT-011).

These tests run as part of normal ``pytest`` — they parse the Dockerfile and
docker-compose.yml as text, and they import + exercise the FastAPI factory
that the Docker CMD references. No Docker daemon is required.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml
from fastapi.testclient import TestClient

# marketplace/api/  ->  ../  from this file
API_ROOT = Path(__file__).resolve().parent.parent


# --- Dockerfile ------------------------------------------------------------ #


def _dockerfile() -> str:
    return (API_ROOT / "Dockerfile").read_text(encoding="utf-8")


def test_dockerfile_exists_and_uses_multistage_python_312():
    text = _dockerfile()
    assert "FROM python:3.12-slim-bookworm AS builder" in text
    assert "FROM python:3.12-slim-bookworm AS runtime" in text


def test_dockerfile_runs_as_non_root_user():
    text = _dockerfile()
    # Either we create a system user and switch to it, or USER is set.
    assert re.search(r"^USER\s+pageros\s*$", text, re.MULTILINE), text
    assert "useradd" in text


def test_dockerfile_exposes_8080_and_invokes_factory():
    text = _dockerfile()
    assert "EXPOSE 8080" in text
    # CMD must reference the create_app factory via uvicorn --factory.
    assert "pageros_marketplace.registry:create_app" in text
    assert "--factory" in text
    # Honor X-Forwarded-* headers from the reverse proxy.
    assert "--proxy-headers" in text


def test_dockerfile_declares_healthcheck_against_healthz():
    text = _dockerfile()
    assert "HEALTHCHECK" in text
    assert "/healthz" in text


# --- docker-compose.yml ---------------------------------------------------- #


def _compose() -> dict:
    return yaml.safe_load((API_ROOT / "docker-compose.yml").read_text(encoding="utf-8"))


def test_compose_is_valid_yaml_and_wires_three_services():
    spec = _compose()
    services = spec.get("services", {})
    assert {"api", "caddy", "db"}.issubset(services.keys()), services.keys()


def test_compose_api_builds_from_local_dockerfile():
    api = _compose()["services"]["api"]
    build = api["build"]
    # build can be string or mapping; we ship the mapping form
    assert isinstance(build, dict)
    assert build["dockerfile"] == "Dockerfile"
    assert "healthcheck" in api, "api service should have a container healthcheck"


def test_compose_caddy_publishes_80_and_443_and_depends_on_api():
    caddy = _compose()["services"]["caddy"]
    ports = set(caddy["ports"])
    assert "80:80" in ports
    assert "443:443" in ports
    depends = caddy["depends_on"]
    assert "api" in depends
    # service_healthy gating means caddy waits for the api healthcheck.
    assert depends["api"]["condition"] == "service_healthy"


def test_compose_db_uses_postgres_and_has_pgisready_healthcheck():
    db = _compose()["services"]["db"]
    assert db["image"].startswith("postgres:")
    assert any("pg_isready" in s for s in db["healthcheck"]["test"])


def test_compose_persists_named_volumes():
    spec = _compose()
    assert {"pg_data", "caddy_data", "caddy_config"}.issubset(set(spec["volumes"].keys()))


# --- entrypoint resolves + serves ----------------------------------------- #


def test_create_app_factory_resolves_and_serves_healthz():
    # The Docker CMD points at this exact target; if it stops resolving, the
    # container won't start. Exercise it the same way uvicorn --factory would.
    import importlib

    mod = importlib.import_module("pageros_marketplace.registry")
    factory = getattr(mod, "create_app")
    app = factory()
    with TestClient(app) as client:
        r = client.get("/healthz")
        assert r.status_code == 200
        assert r.json() == {"status": "ok"}


# --- Caddyfile sanity ------------------------------------------------------ #


def test_caddyfile_proxies_to_api_8080_and_sets_forwarded_headers():
    text = (API_ROOT / "deploy" / "Caddyfile").read_text(encoding="utf-8")
    assert "reverse_proxy api:8080" in text
    assert "X-Forwarded-For" in text
    assert "X-Forwarded-Proto" in text


# --- DEPLOYMENT.md covers acceptance criteria ----------------------------- #


@pytest.mark.parametrize(
    "section",
    [
        "Topology",
        "Production deployment",
        "Backups",
        "Monitoring",
        "Scaling plan",
        "Cutover punch list",
    ],
)
def test_deployment_doc_covers_acceptance_areas(section: str):
    text = (API_ROOT / "DEPLOYMENT.md").read_text(encoding="utf-8")
    assert section.lower() in text.lower(), f"DEPLOYMENT.md missing section: {section}"
