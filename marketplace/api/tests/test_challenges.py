"""Tests for the publish-time DNS TXT challenge service (MKT-003)."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

import pytest
from fastapi.testclient import TestClient

from pageros_marketplace import (
    CHALLENGE_TOKEN_PREFIX,
    CHALLENGE_TXT_PREFIX,
    ChallengeStore,
    DnsTxtNotFoundError,
    FailingResolver,
    FakeResolver,
    Registry,
    create_app,
)
from pageros_marketplace.registry.challenges import (
    ChallengeAlreadyConsumedError,
    ChallengeAppIdMismatchError,
    ChallengeExpiredError,
    ChallengeHostMismatchError,
    ChallengeNotVerifiedError,
    UnknownChallengeError,
)


APP_ID = "notes.mafu.dev"
APP_URL = "https://notes.app/"
APP_HOST = "notes.app"
TXT_NAME = f"{CHALLENGE_TXT_PREFIX}.{APP_HOST}"


def _manifest(
    *,
    id: str = APP_ID,
    url: str = APP_URL,
    name: str = "Notes",
    description: str = "A simple notepad.",
    version: int = 1,
    categories: list[str] | None = None,
) -> dict:
    return {
        "id": id,
        "name": name,
        "description": description,
        "icon": "https://notes.app/icon.png",
        "url": url,
        "categories": categories or ["productivity"],
        "maintainer": {"name": "Jane Doe", "contact": "jane@example.com"},
        "version": version,
    }


# --- store-level tests ----------------------------------------------------- #


class _ManualClock:
    """A deterministic clock for testing TTL behavior."""

    def __init__(self, start: datetime | None = None) -> None:
        self.now = start or datetime(2026, 1, 1, tzinfo=timezone.utc)

    def __call__(self) -> datetime:
        return self.now

    def advance(self, delta: timedelta) -> None:
        self.now += delta


def test_store_create_returns_challenge_with_txt_record():
    store = ChallengeStore(FakeResolver())
    ch = store.create(app_id=APP_ID, url=APP_URL)
    assert ch.host == APP_HOST
    assert ch.txt_name == TXT_NAME
    assert ch.txt_value.startswith(CHALLENGE_TOKEN_PREFIX)
    assert ch.token in ch.txt_value
    assert not ch.verified
    assert not ch.consumed
    # Tokens must be high-entropy: 32 bytes ≈ 43 base64url chars.
    assert len(ch.token) >= 40


def test_store_create_rejects_relative_url():
    store = ChallengeStore(FakeResolver())
    with pytest.raises(ValueError):
        store.create(app_id=APP_ID, url="/notes")


def test_store_verify_succeeds_when_txt_record_matches():
    resolver = FakeResolver()
    store = ChallengeStore(resolver)
    ch = store.create(app_id=APP_ID, url=APP_URL)
    resolver.set(TXT_NAME, [ch.txt_value])
    verified = store.verify(ch.id)
    assert verified.verified
    assert verified.verified_at is not None
    assert resolver.lookups == [TXT_NAME]


def test_store_verify_raises_when_txt_record_missing():
    resolver = FakeResolver()
    store = ChallengeStore(resolver)
    ch = store.create(app_id=APP_ID, url=APP_URL)
    resolver.set(TXT_NAME, ["pageros-challenge=wrong-token"])
    with pytest.raises(DnsTxtNotFoundError) as ei:
        store.verify(ch.id)
    assert ei.value.txt_name == TXT_NAME
    assert ei.value.observed == ["pageros-challenge=wrong-token"]
    # Unverified state is preserved on failure.
    assert not store.get(ch.id).verified


def test_store_verify_accepts_multiple_strands():
    resolver = FakeResolver()
    store = ChallengeStore(resolver)
    ch = store.create(app_id=APP_ID, url=APP_URL)
    resolver.set(TXT_NAME, ["v=spf1 -all", ch.txt_value, "google-site-verification=…"])
    assert store.verify(ch.id).verified


def test_store_verify_unknown_id():
    store = ChallengeStore(FakeResolver())
    with pytest.raises(UnknownChallengeError):
        store.verify("does-not-exist")


def test_store_verify_expired_unverified_challenge_raises():
    clock = _ManualClock()
    store = ChallengeStore(FakeResolver(), ttl=timedelta(minutes=5), clock=clock)
    ch = store.create(app_id=APP_ID, url=APP_URL)
    clock.advance(timedelta(minutes=10))
    with pytest.raises(ChallengeExpiredError):
        store.verify(ch.id)


def test_store_consume_requires_verified_challenge():
    store = ChallengeStore(FakeResolver())
    ch = store.create(app_id=APP_ID, url=APP_URL)
    with pytest.raises(ChallengeNotVerifiedError):
        store.consume(token=ch.token, app_id=APP_ID, url=APP_URL)


def test_store_consume_marks_challenge_consumed_and_is_single_use():
    resolver = FakeResolver()
    store = ChallengeStore(resolver)
    ch = store.create(app_id=APP_ID, url=APP_URL)
    resolver.set(TXT_NAME, [ch.txt_value])
    store.verify(ch.id)
    consumed = store.consume(token=ch.token, app_id=APP_ID, url=APP_URL)
    assert consumed.consumed
    with pytest.raises(ChallengeAlreadyConsumedError):
        store.consume(token=ch.token, app_id=APP_ID, url=APP_URL)


def test_store_consume_rejects_app_id_mismatch():
    resolver = FakeResolver()
    store = ChallengeStore(resolver)
    ch = store.create(app_id=APP_ID, url=APP_URL)
    resolver.set(TXT_NAME, [ch.txt_value])
    store.verify(ch.id)
    with pytest.raises(ChallengeAppIdMismatchError):
        store.consume(token=ch.token, app_id="other.example.dev", url=APP_URL)


def test_store_consume_rejects_host_mismatch():
    resolver = FakeResolver()
    store = ChallengeStore(resolver)
    ch = store.create(app_id=APP_ID, url=APP_URL)
    resolver.set(TXT_NAME, [ch.txt_value])
    store.verify(ch.id)
    with pytest.raises(ChallengeHostMismatchError):
        store.consume(token=ch.token, app_id=APP_ID, url="https://other.example.dev/")


def test_store_purge_drops_expired_unverified_only():
    clock = _ManualClock()
    resolver = FakeResolver()
    store = ChallengeStore(resolver, ttl=timedelta(minutes=5), clock=clock)
    pending = store.create(app_id=APP_ID, url=APP_URL)
    keep = store.create(app_id="other.example.dev", url="https://other.example.dev/")
    resolver.set(f"{CHALLENGE_TXT_PREFIX}.other.example.dev", [keep.txt_value])
    store.verify(keep.id)
    clock.advance(timedelta(minutes=10))
    removed = store.purge_expired()
    assert removed == 1
    with pytest.raises(UnknownChallengeError):
        store.get(pending.id)
    # Verified challenge is preserved past the unverified TTL.
    assert store.get(keep.id).verified


# --- HTTP-layer tests ------------------------------------------------------ #


@pytest.fixture
def resolver() -> FakeResolver:
    return FakeResolver()


@pytest.fixture
def store(resolver: FakeResolver) -> ChallengeStore:
    return ChallengeStore(resolver)


@pytest.fixture
def client(resolver: FakeResolver, store: ChallengeStore) -> TestClient:
    return TestClient(
        create_app(
            Registry(),
            challenges=store,
            dns_resolver=resolver,
            challenge_required=True,
        )
    )


def test_http_full_publish_flow(
    client: TestClient, resolver: FakeResolver, store: ChallengeStore
):
    # Step 1: create challenge.
    r = client.post("/apps/challenges", json={"app_id": APP_ID, "url": APP_URL})
    assert r.status_code == 201, r.text
    body = r.json()
    assert body["txt_name"] == TXT_NAME
    assert body["txt_value"].startswith(CHALLENGE_TOKEN_PREFIX)
    challenge_id = body["id"]
    token = body["token"]
    txt_value = body["txt_value"]
    assert body["verified_at"] is None
    assert body["consumed_at"] is None

    # Step 2: developer "creates" the TXT record.
    resolver.set(TXT_NAME, [txt_value])

    # Step 3: verify.
    r = client.post(f"/apps/challenges/{challenge_id}/verify")
    assert r.status_code == 200, r.text
    assert r.json()["verified_at"] is not None

    # Step 4: register the app using the verified token.
    r = client.post(
        "/apps",
        json=_manifest(),
        headers={"X-Challenge-Token": token},
    )
    assert r.status_code == 201, r.text
    assert r.json()["manifest"]["id"] == APP_ID

    # Token has been spent — a second registration attempt fails.
    r = client.post(
        "/apps",
        json=_manifest(id="other.notes.app", url=APP_URL),
        headers={"X-Challenge-Token": token},
    )
    assert r.status_code == 409
    assert r.json()["error"] == "challenge_already_consumed"


def test_http_register_without_challenge_token_is_403(client: TestClient):
    r = client.post("/apps", json=_manifest())
    assert r.status_code == 403
    assert r.json()["error"] == "challenge_required"


def test_http_register_with_unverified_token_is_403(
    client: TestClient, store: ChallengeStore
):
    ch = store.create(app_id=APP_ID, url=APP_URL)
    r = client.post(
        "/apps",
        json=_manifest(),
        headers={"X-Challenge-Token": ch.token},
    )
    assert r.status_code == 403
    assert r.json()["error"] == "challenge_not_verified"


def test_http_register_with_host_mismatch_is_403(
    client: TestClient, resolver: FakeResolver
):
    r = client.post(
        "/apps/challenges",
        json={"app_id": APP_ID, "url": "https://other.example.dev/"},
    )
    body = r.json()
    other_txt_name = f"{CHALLENGE_TXT_PREFIX}.other.example.dev"
    resolver.set(other_txt_name, [body["txt_value"]])
    assert (
        client.post(f"/apps/challenges/{body['id']}/verify").status_code == 200
    )

    # Manifest URL says notes.app — token was issued for other.example.dev.
    r = client.post(
        "/apps",
        json=_manifest(),
        headers={"X-Challenge-Token": body["token"]},
    )
    assert r.status_code == 403
    assert r.json()["error"] == "challenge_host_mismatch"


def test_http_verify_returns_403_when_txt_record_missing(
    client: TestClient, resolver: FakeResolver
):
    r = client.post("/apps/challenges", json={"app_id": APP_ID, "url": APP_URL})
    body = r.json()
    # No TXT record published.
    r2 = client.post(f"/apps/challenges/{body['id']}/verify")
    assert r2.status_code == 403
    assert r2.json()["error"] == "dns_txt_not_found"


def test_http_verify_returns_502_when_dns_lookup_fails():
    store = ChallengeStore(FailingResolver())
    client = TestClient(
        create_app(
            Registry(),
            challenges=store,
            challenge_required=True,
        )
    )
    r = client.post("/apps/challenges", json={"app_id": APP_ID, "url": APP_URL})
    body = r.json()
    r2 = client.post(f"/apps/challenges/{body['id']}/verify")
    assert r2.status_code == 502
    assert r2.json()["error"] == "dns_lookup_failed"


def test_http_verify_unknown_challenge_is_404(client: TestClient):
    r = client.post("/apps/challenges/not-a-real-id/verify")
    assert r.status_code == 404
    assert r.json()["error"] == "challenge_not_found"


def test_http_get_challenge_round_trip(client: TestClient):
    r = client.post("/apps/challenges", json={"app_id": APP_ID, "url": APP_URL})
    challenge_id = r.json()["id"]
    r2 = client.get(f"/apps/challenges/{challenge_id}")
    assert r2.status_code == 200
    assert r2.json()["id"] == challenge_id


def test_http_create_challenge_rejects_bad_url(client: TestClient):
    r = client.post("/apps/challenges", json={"app_id": APP_ID, "url": "/relative"})
    assert r.status_code == 400
    assert r.json()["error"] == "invalid_url"


def test_http_update_requires_fresh_challenge(
    client: TestClient, resolver: FakeResolver
):
    # Seed an app via the full flow.
    r = client.post("/apps/challenges", json={"app_id": APP_ID, "url": APP_URL})
    body = r.json()
    resolver.set(TXT_NAME, [body["txt_value"]])
    assert client.post(f"/apps/challenges/{body['id']}/verify").status_code == 200
    assert client.post(
        "/apps",
        json=_manifest(),
        headers={"X-Challenge-Token": body["token"]},
    ).status_code == 201

    # Attempting an update without a token fails.
    r = client.put(f"/apps/{APP_ID}", json=_manifest(name="Notes 2", version=2))
    assert r.status_code == 403
    assert r.json()["error"] == "challenge_required"

    # A fresh, verified challenge unlocks the update.
    r = client.post("/apps/challenges", json={"app_id": APP_ID, "url": APP_URL})
    body2 = r.json()
    resolver.set(TXT_NAME, [body2["txt_value"]])
    assert client.post(f"/apps/challenges/{body2['id']}/verify").status_code == 200
    r = client.put(
        f"/apps/{APP_ID}",
        json=_manifest(name="Notes 2", version=2),
        headers={"X-Challenge-Token": body2["token"]},
    )
    assert r.status_code == 200
    assert r.json()["manifest"]["name"] == "Notes 2"


def test_openapi_lists_challenge_endpoints(client: TestClient):
    schema = client.get("/openapi.json").json()
    paths = schema["paths"]
    assert "/apps/challenges" in paths
    assert "post" in paths["/apps/challenges"]
    assert "/apps/challenges/{challenge_id}" in paths
    assert "/apps/challenges/{challenge_id}/verify" in paths
