# push.pageros.org — Service Level Objective

**Status:** v1 (PUSH-010)
**Service:** `https://push.pageros.org`
**Scope:** the project-operated PagerOS Push Relay (SPEC §6.6).
**Status page:** [`status.pageros.org`](https://status.pageros.org)

This document is the public commitment that the PagerOS project makes about
the relay's availability and behaviour. App developers and device owners can
rely on these numbers; the project commits to publishing changes here before
they take effect.

The SLO is intentionally conservative for v1. SPEC §17.2 explicitly notes
that "production numbers depend on observed app behavior; revisit after the
first 5 apps are in the wild" — once we have real traffic, these targets will
be tightened.

---

## 1. What the relay promises

### 1.1 Availability — `/push`, `/pull`, ack

| Window         | Target |
| -------------- | ------ |
| 28-day rolling | ≥ 99.5 % |
| Monthly (cal.) | ≥ 99.5 % |

**Definition.** A "good" minute is one in which the relay returned a non-5xx
response to at least one synthetic probe **and** in which Prometheus
recorded a non-zero `push_relay_storage_up`. "5xx" here means a response
in the 500-599 range or a connection failure. 4xx responses (rate limits,
auth failures, unknown apps) are client errors and do not count against the
SLO.

99.5 % over 28 days is 3.36 hours of allowed downtime. The project commits
to publishing an incident report within 48 hours of any single event that
exhausts more than 25 % of that budget (≥ 50 minutes of downtime in one
event).

### 1.2 Latency — `/push` and `/pull`

| Endpoint  | p50    | p95    | p99    |
| --------- | ------ | ------ | ------ |
| `/push`   | 200 ms | 800 ms | 1.5 s  |
| `/pull`   | 150 ms | 600 ms | 1.2 s  |

Measured from inside the same datacenter as the relay, excluding TLS
handshake. Devices on cellular networks should expect significantly higher
end-to-end numbers; the relay's contribution should match these targets.

### 1.3 Queue retention

The relay's per-device queue caps follow SPEC §6.6:

- **16** entries per device.
- **1 MiB** total stored bytes per device.
- **7 day** TTL per entry.

A notification accepted by `/push` will be delivered to a device on its next
`/pull`, **unless** any of the following occur, in which case the relay is
permitted to drop it:

1. The device does not pull within 7 days (TTL expiry).
2. The same device queues 16 newer notifications (count cap; oldest dropped).
3. The same device queues > 1 MiB of newer notifications (byte cap; oldest
   dropped).

Dropping under these conditions is **not** an SLO violation — they are
intentional caps. The relay does not commit to delivering past those
boundaries.

### 1.4 At-least-once delivery

When the device acks via `DELETE /pull/<device>/<id>`, the relay removes the
entry. If the ack is lost (mobile flap, NAT timeout), the device will see
the same entry on a subsequent `/pull`. SPEC §6.6.5 calls this out:

> At-least-once. Each notification has a server-assigned id; device dedupes
> on read.

Apps **must** treat notifications as idempotent. The relay does not promise
exactly-once delivery, ever.

### 1.5 Rate limits

| Bucket                       | Cap                              |
| ---------------------------- | -------------------------------- |
| `(app, device)` for `/push`  | 60 / hour, 1000 / day            |
| `(app, device)` for `/group_push` | 60 / hour, 1000 / day (separate bucket) |

Excess returns `429 Too Many Requests`. The relay does not commit to
"raising the cap on request"; quota changes ship as a SPEC §6.6.4 revision.

### 1.6 Marketplace dependency

`/push` calls the marketplace registry (MKT-002) to resolve the sender app's
signing pubkey. If the marketplace is unavailable, `/push` returns
`503 Service Unavailable` and the relay's SLO is paused for the affected
window — the project commits to publishing the marketplace's own SLO once
MKT-009 lands.

---

## 2. What the relay does **not** promise

- **TLS cert pinning.** The relay's cert may rotate at any time within a
  90-day window. Clients should rely on standard CA chain validation only.
- **Stable IP addresses.** The DNS records for `push.pageros.org` and
  `status.pageros.org` may change. Clients must resolve via DNS, not
  hardcode IPs.
- **Long-lived connections.** The relay closes idle connections after
  60 seconds. Push apps should treat each push as a fresh HTTP request.
- **Stable response timing.** Clients must not depend on a specific p99
  number — the SLO sets a ceiling, not a guarantee that the median will
  match any particular value.
- **Notification ordering.** Notifications are delivered in enqueue order
  *within a single `/pull`*. Across multiple `/pull`s, partial overlap is
  possible if a device acks some but not all entries from one batch and
  then re-pulls.

---

## 3. How the SLO is measured

### 3.1 Internal probes

- A synthetic prober inside the relay's Prometheus stack hits
  `/healthz` every 15 s. Each scrape is one sample.
- The `up{job="push-relay"}` and `push_relay_storage_up` gauges are
  combined into the per-minute "good" / "bad" classification described
  in §1.1.
- Burn-rate alerts fire at 2x and 14.4x the SLO budget consumption rate
  (industry-standard SRE multipliers), routed to the on-call rotation
  per `push-relay/deploy/alertmanager/alertmanager.yml`.

### 3.2 External probes

Once the relay is in production, an external uptime monitor (e.g. UptimeRobot,
StatusCake) will probe `https://push.pageros.org/healthz` from at least three
geographic regions. External probe results are the **primary** signal for the
public SLO; internal probes are secondary, because an internal probe cannot
detect a routing-layer outage between the internet and the relay.

The status page at `status.pageros.org` shows both signals.

### 3.3 Error budget policy

The 28-day error budget is 3.36 hours. When ≥ 75 % of the budget is consumed
in a rolling 7-day window:

1. Feature work on `push-relay/` pauses.
2. CEO + Pusher (this engineer) review the burn driver.
3. Reliability work is prioritised until the burn rate drops below 25 %
   over a 7-day window.

When ≥ 100 % is consumed in any 28-day window, a public post-mortem is
published within 72 hours, linked from the status page.

---

## 4. Changelog

| Date         | Change                          |
| ------------ | ------------------------------- |
| 2026-05-21   | v1 published with PUSH-010.     |

Any change to this document is published at least 7 days before it takes
effect, except for tightening (which can take effect immediately).
