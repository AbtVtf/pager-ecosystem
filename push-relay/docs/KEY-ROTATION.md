# push-relay — Key Rotation Procedure (PUSH-009)

This document covers the credentials the relay holds at runtime and the
mechanical steps to rotate each without dropping traffic. SPEC §6.6.7 calls
key rotation out as part of the relay's operating burden — this is the
runbook that discharges that obligation.

> **Glossary of secrets.** The relay does not hold any app-level or
> device-level long-term key material itself. App and device Ed25519/X25519
> keys are owned by the SDK and the firmware respectively (see SPEC §9 and
> the marketplace registry). The relay's own secrets are operational only:
> TLS, the admin bearer token, and Redis ACL passwords.

---

## 1. TLS certificate

**Lifetime:** as issued; assume ≤ 90 days (Let's Encrypt) or as your CA
dictates. Set a calendar reminder for 14 days before expiry.

**Where it lives:**
- Files: `./certs/cert.pem` and `./certs/key.pem` (mounted read-only into
  the relay container at `/certs/`).
- Env vars: `PUSH_RELAY_TLS_CERT` and `PUSH_RELAY_TLS_KEY` (both must be set
  together — see `internal/config/config.go`).

**Rotation procedure.** The relay reads the cert+key once at boot and again
on graceful restart. A zero-downtime swap requires either (a) a TLS-terminating
load balancer in front of the relay or (b) acceptance of the ~1s gap during
container restart. The compose stack documented in `OPERATIONS.md` is (b).

For Let's Encrypt:

```sh
# 1. Renew via your usual ACME client. Example using certbot in standalone mode:
certbot certonly --standalone -d push.pageros.org

# 2. Copy the renewed pair into the compose mount point.
cp /etc/letsencrypt/live/push.pageros.org/fullchain.pem ./certs/cert.pem
cp /etc/letsencrypt/live/push.pageros.org/privkey.pem  ./certs/key.pem
chmod 600 ./certs/key.pem

# 3. Restart the relay container. SIGTERM is honoured; in-flight requests
#    drain via the 10s Shutdown timeout in cmd/push-relay/main.go.
docker compose -f docker-compose.prod.yml restart push-relay

# 4. Verify the new fingerprint.
echo | openssl s_client -connect push.pageros.org:8443 -servername push.pageros.org 2>/dev/null \
  | openssl x509 -noout -fingerprint -sha256
```

For a private CA: replace step 1 with your CA's issuance procedure. The rest
is identical.

**After rotation:**
- Confirm `PushRelayDown` does not fire (the `up` gauge stays at 1 across
  the restart, modulo a single scrape gap).
- Confirm `push_relay_build_info{tag="..."}` is still emitted with the
  expected tag (sanity check that the right container restarted).

**Rollback.** Keep the previous `cert.pem`/`key.pem` pair for 24h after
rotation. If something is wrong, `cp` the previous pair back in and restart.

---

## 2. Admin bearer token (`PUSH_RELAY_ADMIN_TOKEN`)

**Lifetime:** 90 days, or immediately on compromise / on operator turnover.

**Where it lives:**
- Env var: `PUSH_RELAY_ADMIN_TOKEN` on the `push-relay` container.
- Operator credential store: each operator who runs `curl /admin/...` holds
  a copy.

**Rotation procedure.** The admin dashboard accepts exactly one bearer token
at a time (see `internal/admin/http.go`). To rotate without a window where
admin access is broken:

```sh
# 1. Generate the new token. The relay refuses < 16 chars; 32 is the floor
#    we recommend.
NEW=$(openssl rand -hex 32)

# 2. Update the deployment env (your secret store, .env file, or systemd
#    drop-in). Example for the compose stack:
PUSH_RELAY_ADMIN_TOKEN="$NEW" docker compose -f docker-compose.prod.yml up -d push-relay

# 3. Distribute the new token to operators via the same channel you used
#    for the previous one. Use a sealed message — never paste tokens into
#    shared chat.

# 4. Invalidate the previous token by destroying every operator copy.
```

There is no "two tokens at once" mode — a misconfigured rotation that loses
the new token before distribution requires editing the env and restarting
again. Keep a sealed copy of the new token in a secret-recovery vault.

**Audit trail.** Every admin call logs at `INFO` with the action and app id
(see `internal/admin/http.go` — `slog.Info("admin ban added", ...)`). If you
suspect a token compromise, grep the relay container logs for
`admin ban (added|removed)` actions you don't recognize, then rotate.

---

## 3. Redis ACL passwords

**Lifetime:** 90 days, or on suspected lateral movement from another
container.

**Where it lives:**
- `PUSH_RELAY_REDIS_URL` env var on the relay (`relay` user password).
- Operator credential store (`ops` user password for `redis-cli`).
- Redis ACL list inside the redis container, persisted via `CONFIG REWRITE`
  (or `ACL SAVE` in newer Redis).

**Rotation procedure (relay user).** Redis ACL changes are atomic per-user,
so a careful sequence drops zero in-flight requests:

```sh
# 1. Generate the new password and load it into both Redis and the relay
#    env. The relay validates the URL prefix in config.FromEnv() — make
#    sure to keep `redis://` or `rediss://`.
NEW_RELAY=$(openssl rand -hex 24)

# 2. Tell Redis about the new password BEFORE the relay rolls. The relay
#    user keeps the old password too at this point; ACL SETUSER on an
#    existing user with `>password` ADDs a password by default.
docker compose -f docker-compose.prod.yml exec redis \
  redis-cli -a "$REDIS_OPS_PASSWORD" \
  ACL SETUSER relay ">$NEW_RELAY"

# 3. Restart the relay with the new password in PUSH_RELAY_REDIS_URL.
PUSH_RELAY_REDIS_URL="redis://relay:$NEW_RELAY@redis:6379/0" \
  docker compose -f docker-compose.prod.yml up -d push-relay

# 4. After verifying /healthz is green and `push_relay_storage_up == 1`,
#    drop the old password from the ACL.
docker compose -f docker-compose.prod.yml exec redis \
  redis-cli -a "$REDIS_OPS_PASSWORD" \
  ACL SETUSER relay "<$OLD_RELAY"

# 5. Persist the change so it survives a Redis restart.
docker compose -f docker-compose.prod.yml exec redis \
  redis-cli -a "$REDIS_OPS_PASSWORD" ACL SAVE
```

**Rotation procedure (ops user).** Same shape — `ACL SETUSER ops ">$NEW_OPS"`
to add, distribute the new password, then `<$OLD_OPS` to remove. Operators
update their `~/.redis-cli-rc` or the env var they use.

**If you only have the bootstrap `default` user.** This is the case
immediately after first boot; the compose-managed Redis starts with the
`default` user enabled and no password. Follow the one-time setup in
`redis/redis.conf` comments to create the `relay` and `ops` users, then
disable `default` (`ACL SETUSER default off`).

---

## 4. What is NOT in scope here

The following keys are owned by other subsystems. The relay never sees the
private halves and rotating them has no relay-side procedure:

| Key                           | Owner                              | Rotation reference                  |
| ----------------------------- | ---------------------------------- | ----------------------------------- |
| App signing keypair (Ed25519) | App developer + marketplace        | MKT-002, MKT-003                    |
| App E2E keypair (X25519)      | App developer + marketplace        | SPEC §9.4                           |
| Device identity (Ed25519)     | Device (`firmware/`, FW-014)       | FW-014 / FW-015                     |
| Device E2E key (X25519)       | Device                             | SPEC §9.4                           |
| Marketplace registry signing  | Marketplace (`marketplace/`)       | MKT-011                             |

If a private key in this table is compromised, escalate to CEO via an issue
on the owning subsystem. Relay-side mitigations are limited to: banning the
sender app id (`POST /admin/bans`) and waiting for the marketplace to publish
a key revocation.

---

## 5. Rotation calendar (suggested)

| Item                  | Cadence            | Triggered by                                |
| --------------------- | ------------------ | ------------------------------------------- |
| TLS cert              | 60 days            | LE cron job; manual on cert authority change|
| Admin bearer token    | 90 days            | Quarterly drill; immediately on op turnover |
| Redis `relay` user pw | 90 days            | Quarterly drill                             |
| Redis `ops` user pw   | 90 days            | Quarterly drill                             |

Track the next rotation date in the same place you track on-call (PagerDuty
override calendar, etc.). The relay does not enforce expiry, so calendar
discipline is the whole control.
