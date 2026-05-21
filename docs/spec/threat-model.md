# PagerOS Threat Model

**Status:** v0.2 (SEC-002)
**Scope:** v1 stack as described in `SPEC.md` v0.2 and `docs/spec/crypto-suite.md` (SEC-001). Subject to external review under SEC-003 before public launch.

This document enumerates the adversaries we plan against, the attack scenarios each one enables, the mitigations the design relies on, and the residual risks we have explicitly accepted. It is meant to be read together with `SPEC.md` §9 (Identity, Security & Permissions), §6.6 (Push Relay), §11 (Exit Nodes), and §10 (Marketplace).

PagerOS is **not** an anonymity system (`SPEC.md` §1.3). The device has a stable Ed25519 identity that the marketplace, push relay, and every app the user interacts with see. Threats below are evaluated against confidentiality of payloads, integrity of requests, availability of service, and abuse of trust between subsystems — not against linkability of the device identity itself.

## 1. Assets

| Asset | Where it lives | Why it matters |
|---|---|---|
| Device Ed25519 private key | ESP32-S3 NVS (flash-encrypted) | Forging this lets an attacker impersonate the device to every app, every push, every group session. |
| Device X25519 private key | Same | Decrypts every push and every LoRa-bound payload addressed to the device. |
| App↔device session state | App server, device PSRAM/SD | User-visible content (notes, chat history, location traces) and any per-app secrets. |
| App manifest pubkey + URL binding | Marketplace registry | The trust root for "this app is the one that owns this URL." Compromise enables phishing of every device that installs the app. |
| Push Relay queue contents | `push.pageros.org` storage | Encrypted, but envelope metadata (sender app id, recipient device pubkey, size, timing) leaks. |
| Granted permissions (location, nfc, notifications, groups, lora_send, contacts) | Device NVS | Persistent decisions; misuse grants apps capabilities the user expected to be one-shot. |
| OTA signing keys | Project HSM / offline storage (out of v1 spec) | Compromise enables silent firmware replacement on the entire fleet. |

## 2. Adversaries

The four called out in SEC-002's acceptance criterion are the primary actors. Two supporting adversaries are included because the mitigations for the primary four assume them.

| # | Adversary | Position | Primary capability |
|---|---|---|---|
| A1 | **Malicious app** | Registered on the Marketplace, or sideloaded by URL | Serves arbitrary Frames, requests permissions, sends pushes, joins groups. |
| A2 | **Hostile Exit Node** | Listens to LoRa, bridges to HTTPS | Sees the LoRa envelope; chooses what to forward, drop, modify, or fabricate. |
| A3 | **Compromised Push Relay** | Operator-level access to `push.pageros.org` | Reads queues, drops messages, fabricates `pull` responses, observes timing/metadata. |
| A4 | **Lost / stolen device** | Physical possession of the pager | Whatever the device key allows, plus on-screen and on-SD content. |
| A5 | **Hostile Marketplace operator** | Operator-level access to `market.pageros.org` | Controls which apps are listed, what pubkey is bound to what URL, what tag (`verified`, `flagged`) each app carries. Included because A1 mitigations assume the marketplace is honest about the URL ↔ pubkey binding. |
| A6 | **LoRa-band passive eavesdropper** | RF listener in range | Captures every LoRa envelope. Included because A2 mitigations assume the on-air ciphertext is opaque. |

The on-wire attacker on the Wi-Fi path is **out of scope** for this document: that path is plain HTTPS to a developer-operated server, and the threat model there reduces to "trust the TLS PKI and the app's hosting." Wi-Fi attackers are handled by the same mitigations as any HTTPS client.

## 3. Adversary playbooks

For each adversary: capabilities, the concrete scenarios we defend against, mitigations the design relies on, and residual risk we accept.

### 3.1 A1 — Malicious app

**Capabilities.** Returns whatever Frame it likes. Asks for any subset of permissions. Sends pushes within rate limits. Joins groups it has been invited to. Receives signed requests carrying the device's stable pubkey (this *is* the linkage every app gets and is not considered a leak).

**Scenarios.**
- **S1.1 — Permission overreach.** App requests `location` for a feature that does not need it and exfiltrates GPS traces.
- **S1.2 — Phishing via render.** App renders a `Form` that mimics another app's chrome to harvest user-entered secrets.
- **S1.3 — Notification spam / harassment.** Sends maximum-rate pushes to annoy or DoS the user's attention.
- **S1.4 — Cross-app correlation.** Operator runs multiple apps, correlates the same device pubkey across them to build a behavior profile.
- **S1.5 — Sideload bait.** Hosts an app at a URL not in the Marketplace; convinces the user to add it via Settings → "Add app by URL," gaining all the above without moderation review.
- **S1.6 — Manifest squat.** Registers a name visually similar to a popular app (`n0tes`) to harvest installs.
- **S1.7 — Group flood.** Once admitted to a group, broadcasts at the per-pair push limit against every other member.

**Mitigations.**
- Per-app permission grants with prompts on first use; revocable from Settings; permission scope documented in the manifest (§9.4, FW-033).
- The renderer is a fixed set of widgets with no general scripting (`SPEC.md` §1.3, §5.3). There is no way for one app to draw outside its frame or read another app's state.
- Push rate limits are per `(app, device)` at the relay (PUSH-006: 60/hour, 1000/day) plus a per-device 1 MB queue cap with oldest-drop eviction. Group events use a **separate** rate-limit bucket so a misbehaving group does not starve user notifications.
- Marketplace moderation queue (MKT-006), trust tagging (`unverified` / `verified` / `featured` / `flagged`), and a public moderation log (MKT-008) give a paper trail; the Shell can filter or warn by tag (MKT-009).
- Sideload-by-URL is gated behind a Settings flow that tags the app as `sideloaded` in the drawer (FW-034) so the user can see what is not marketplace-vetted.
- Cross-app correlation is **explicitly accepted** as part of the "no anonymity guarantees" non-goal; the spec does not promise per-app key derivation in v1.

**Residual risk.**
- A user who clicks through permission prompts hands `location` / `nfc` data to a willing-to-exfiltrate app. We rely on the prompt UX (FW-033) and trust tagging (MKT-009) to discourage this; we cannot prevent it.
- The first-prompt UX matters: a poorly-worded dialog is the entire mitigation. SEC-003 should audit FW-033 wording.
- Manifest squatting is mitigated by moderation, not prevented. Initial publishes land as `unverified`; squatters survive until reported (MKT-008).
- Sideloaded apps bypass moderation by design; the trade-off (open ecosystem) is accepted in §1.2.

### 3.2 A2 — Hostile Exit Node

**Capabilities.** Receives every LoRa envelope from devices in range. Decides what to do with each: forward, drop, fabricate a response, replay an old response. Can also originate its own envelopes (it is just a device with an internet uplink). Cannot read the inner ciphertext.

**Scenarios.**
- **S2.1 — Drop or selectively forward.** Censors specific apps by destination URL hash (visible in the envelope) or specific devices by source pubkey.
- **S2.2 — Replay.** Resends an old, validly-signed device request to make a server believe the user did something twice.
- **S2.3 — Forged response.** Returns a fabricated CBOR Frame claiming to come from the target app.
- **S2.4 — Discovery spoofing.** Advertises with artificially strong RSSI / low load to attract traffic for traffic analysis.
- **S2.5 — Rate-limit denial.** Floods devices with bogus responses to exhaust per-device retry budgets.
- **S2.6 — Traffic analysis.** Logs envelope metadata (src pubkey, dst URL hash, size, timing) and correlates with public app behavior to learn what the user is doing.

**Mitigations.**
- Inner envelope is X25519+ChaCha20-Poly1305 to the app's published pubkey (SEC-001 §1, `SPEC.md` §9.3). Exit Node cannot read or modify payload without the app's key.
- Device requests are Ed25519-signed and include a timestamp; server rejects > 5 min skew (§9.2). PagerOS-Sig binds `(method, url, timestamp, body_hash)`, so an exit node cannot strip or swap fields.
- The crypto-suite nonce construction (SEC-001 §1.2) is a persisted per-pair counter; the receiver rejects replay below its current counter regardless of whether the exit node retransmits.
- App pubkey is fetched from the **Marketplace**, not from the exit node. Exit node cannot inject a key it controls.
- Exit Node ranking is by RSSI **plus** load (FW-027, LORA-005); devices keep a multi-entry list and retry against a different exit node on failure, so a single hostile node cannot fully censor unless it is the only one in range.
- Per-device rate limiting *at the exit node* (EXIT-004) gives operators a defense against abusive devices, but does not protect devices against exit-node abuse — that is what the multi-node ranked list does.

**Residual risk.**
- A region with only one in-range Exit Node has only that node's policy. We document this as an open ecosystem property (`SPEC.md` §11.3) — adding diversity is an operator/community concern, not a protocol fix.
- Envelope metadata (which device talks to which app, how often, how large) is visible to every exit node in range and is unavoidable without padding/cover traffic, both of which were rejected as out of scope for v1 (§1.3 — not Tor).
- A hostile exit node can drop *specific* traffic without the device knowing the difference between "censored" and "no response yet"; the device-side mitigation is a transport-selector retry timeout (`SPEC.md` §6.4) that falls back to push or surfaces an error to the user.

### 3.3 A3 — Compromised Push Relay

**Capabilities.** Reads stored queues. Can drop, reorder, replay, or fabricate `pull` responses. Sees envelope metadata for every push. Holds enough state (per-device queues, per-app rate counters) to enable correlation and selective denial.

**Scenarios.**
- **S3.1 — Plaintext disclosure.** Operator inspects queued payloads to read notification content.
- **S3.2 — Fabrication.** Operator inserts a queue entry that decrypts to a plausible-looking notification the user acts on.
- **S3.3 — Drop / suppress.** Operator silently drops messages from a specific app or to a specific device.
- **S3.4 — Replay.** Operator re-delivers an old payload after a long delay to confuse the user.
- **S3.5 — Metadata correlation.** Logs `(sender_app_id, recipient_pubkey, size, timestamp)` and correlates with public app activity.
- **S3.6 — Pull denial.** Returns 5xx or empty to a specific device to suppress notifications.

**Mitigations.**
- All push payloads are end-to-end encrypted X25519+ChaCha20-Poly1305 from app to device (`SPEC.md` §9.3, §6.6.3); relay sees only the envelope.
- Both directions are authenticated: app→relay is Ed25519-signed against the manifest pubkey on file (PUSH-002, `SPEC.md` §6.6.2); device→relay pulls and acks are Ed25519-signed (PUSH-003, PUSH-004). Fabricated payloads do not decrypt; fabricated `pull` responses do not authenticate either, because the relay cannot produce a valid app signature without the app's key.
- Nonce construction (SEC-001 §1.2) uses a counter persisted across reboots and rejects messages below the current counter, defeating replay regardless of how long the relay holds a copy.
- At-least-once delivery is documented (`SPEC.md` §6.6.5) with a server-assigned id; device dedupes on read. This makes "delivered twice" benign rather than a confusing user-visible event.
- Public uptime / SLO page (PUSH-010) gives a separate observability surface; selective drops are not detectable per-message, but aggregate suppression manifests as queue-depth or delivery-rate anomalies that operators and the community can watch for.

**Residual risk.**
- The relay sees envelope metadata. This is unavoidable for a centralized pull design; padding and cover traffic are out of scope (§1.3).
- The relay can suppress specific notifications. The user-side detection is "the app told you it pushed something and the device never showed it," which is a usability concern more than a protocol failure.
- The TTL plus per-device 1 MB cap (PUSH-005) means a hostile relay flooding bogus entries (which the device will reject on decrypt) can still evict legitimate older entries. The cap is small enough that the impact is bounded, but it is a real DoS vector.
- Key rotation for the relay's own TLS cert is documented as part of PUSH-009 deployment work; cert lifecycle bugs are a generic web-PKI risk shared with the marketplace.

### 3.4 A4 — Lost or stolen device

**Capabilities.** Full physical possession. Can attempt to extract NVS contents (requires defeating ESP32-S3 flash encryption), observe on-screen content, read the SD card, and use the device's signed identity until the rightful owner notices.

**Scenarios.**
- **S4.1 — Cached content disclosure.** Attacker pulls the SD card and reads `/notifications/inbox.cbor`, cached Frames, logs (FW-004), and bundled OTA images.
- **S4.2 — Identity continuation.** Attacker uses the device to keep signing requests as the legitimate user until the user revokes (or the device is wiped).
- **S4.3 — Push reading.** Future pushes addressed to the device's pubkey decrypt on the device the attacker now controls.
- **S4.4 — Group continuation.** Attacker continues to receive group events for groups the device was a member of.
- **S4.5 — Key extraction.** Attacker attempts to dump NVS, defeat flash encryption, and lift the Ed25519 / X25519 private keys.

**Mitigations.**
- Identity private keys live in NVS with flash encryption enabled (`SPEC.md` §9.1, FW-014); the eFuse-bound key for flash encryption is not user-extractable without invasive hardware attack. SEC-001 §5 explicitly tracks "hardware-backed key storage beyond NVS flash encryption" as a future concern.
- Permissions are persistent but revocable (§9.4); a user who recovers a device can revoke and re-grant, which is a weak mitigation post-loss but is the v1 design.
- Group leave is a signed `leave_group` request (§9.6); the legitimate user can leave from a backup device only if such backup exists, which v1 does not provide.
- Marketplace and push-relay subsystems have no concept of "device revocation list" in v1 — there is no list to ban a stolen device's pubkey from. Apps that care can implement their own revocation against the device pubkey.

**Residual risk.**
- v1 has **no key revocation, no device wipe-on-loss, no PIN, no remote lock**, and no backup of identity. This is the largest single residual risk in the v1 design and is called out explicitly in SEC-001 §5 and `SPEC.md` §1.3 (no anonymity / no recovery promises). SEC-003 should weigh whether v1 ships without an emergency-revocation path.
- An attacker with sufficient lab resources may eventually extract NVS contents from a lost device. The mitigation (flash encryption) raises the cost but does not make extraction infeasible.
- Cached Frames and notification inbox on SD are not encrypted at rest in v1. Removing the SD card discloses content directly. FW-003/004 do not mandate at-rest encryption.

### 3.5 A5 — Hostile Marketplace operator (supporting)

The marketplace is the trust root for the URL↔pubkey binding that the push-relay and the device's E2E encryption rely on. Mitigations for A1 and A3 assume an honest marketplace; this section lists what a hostile operator could do anyway and how we contain it.

**Scenarios.**
- **S5.1 — Bind a wrong pubkey to an app's manifest** so push payloads end up encrypted to a key the operator controls.
- **S5.2 — Silently swap an app's URL** so subsequent installs hit a hostile server.
- **S5.3 — Selectively list or delist apps** to favor or censor specific developers.

**Mitigations.**
- DNS TXT publishing challenge (MKT-003) at publish time ties an app id to control of its DNS zone. The marketplace cannot register an app without that challenge succeeding once, and the manifest's `url` field is verified against the challenged domain.
- Public moderation log (MKT-008) records every accept/tag/remove action; silent removal is not silent in this design.
- Open registration + post-hoc moderation (`SPEC.md` §10.5) limits the marketplace operator's ability to gatekeep — the spec choice was deliberate.

**Residual risk.**
- Post-publish silent pubkey/URL **rotation** in the registry is detectable only if devices cache and compare prior manifest values. v1 does **not** mandate this. SEC-003 should evaluate whether `pagerctl` or the Shell should warn on manifest mutations.
- A future federated marketplace is not in v1 (§10.1); for now the project operators are a single trust point.

### 3.6 A6 — Passive RF eavesdropper (supporting)

A device with a SX1262 in range hears every LoRa transmission. We do not protect against the *fact* of transmission, only the contents.

**Scenarios.**
- **S6.1 — Inner-payload disclosure.** Eavesdropper records envelopes and tries to read content.
- **S6.2 — Linkability over time.** Eavesdropper records every envelope's src pubkey and correlates location/activity across days.
- **S6.3 — Geolocation by triangulation.** Multiple receivers triangulate a sending device.

**Mitigations.**
- Inner envelope is encrypted to the destination app's X25519 key (`SPEC.md` §9.3); ciphertext is opaque without that key.
- The outer envelope (LORA-001 magic/version/type/msg_id) is intentionally minimal; the to/from inside (LORA-003) is part of the encrypted inner envelope, not the on-air header. This limits passive linkability to "an envelope was transmitted by *some* PagerOS device on this msg_id."

**Residual risk.**
- The device's RF signature *itself* is identifying (`SPEC.md` §1.3, no anonymity). We do not address this.
- Traffic analysis (frequency, size, timing) leaks behavior even with opaque payloads. This is the same trade-off as A2 and A3 envelope metadata.

## 4. Cross-cutting concerns

These do not belong to a single adversary because every adversary above touches them.

### 4.1 Replay protection

Replay is mitigated in three places, and SEC-003 should confirm they compose:

1. **PagerOS-Sig timestamp** (`SPEC.md` §9.2) — request must be within 5 min of server clock. This catches gross-scale replay by an exit node (A2.2) and by the push relay (A3.4).
2. **AEAD nonce counter** (SEC-001 §1.2) — receiver rejects below-current counters per `(src, dst)` pair, persisted across reboots. This catches subtle replays the timestamp window allows.
3. **Push notification id dedupe** (`SPEC.md` §6.6.5) — device dedupes on read; double-delivery is benign.

Composition issue to validate under SEC-003: if a device reboots and loses its receive counter (firmware bug), how quickly is divergence detected and what does recovery look like? FW-014 requires NVS persistence; the failure mode is firmware-bug-shaped, not protocol-shaped.

### 4.2 Key compromise & rotation

v1 does not specify routine key rotation for device identity. Rotation would require:

- A way to publish "device A's new pubkey is X" trustably (no PKI for device identities in v1).
- App-side acceptance of pubkey rotation events (no protocol message for this).
- Group migration (no membership-rotation message in §9.6).

This is **accepted** for v1 and is part of why SEC-003 is gated on M6, after the trust roots are stable.

### 4.3 Abuse and rate-limit composition

Rate limits live in three places, with the following intent:

- **Push Relay** (PUSH-006) — controls per-app abuse against a specific device.
- **Exit Node** (EXIT-004) — controls per-device abuse against the internet at large.
- **App-side** — apps may impose their own limits; SDK provides no enforcement helper in v1.

These bucket separately on purpose: a chatty group must not be able to suppress a critical notification (`SPEC.md` §6.6.6, separate bucket). SEC-003 should validate the per-bucket caps against the realistic worst case for the M5 reference `chat` app (DOCS-010).

### 4.4 OTA & firmware integrity

OTA updates (FW-036) are signed by a project key not in scope of this document. A compromise there bypasses every other mitigation in this doc. The OTA signing key custody, rotation, and emergency-revocation process are an SEC-003 input and a tracked dependency for the M6 launch.

### 4.5 Sideloaded apps

Sideload-by-URL (FW-034) is a deliberate escape hatch from marketplace moderation (`SPEC.md` §10.6). Sideloaded apps:

- Get the same permission prompts.
- Do not get a trust tag (rendered as `sideloaded` in the drawer).
- Cannot use push without being registered, because PUSH-002 verifies app signatures against marketplace-registered pubkeys.

The last point is non-obvious and is the actual security boundary on sideloads: a sideloaded app can render Frames and request permissions, but it cannot reach the relay. SEC-003 should sanity-check this is enforced (PUSH-002 acceptance criterion).

## 5. Things explicitly out of scope for v1

| Out-of-scope item | Why | Tracked where |
|---|---|---|
| Anonymity / unlinkability | Non-goal, `SPEC.md` §1.3 | n/a |
| Device PIN / lock screen | Not in v1 | Recommend SEC-003 reconsider |
| Remote wipe / revocation | No PKI for device identities | Recommend SEC-003 reconsider for "lost device" path |
| At-rest encryption of SD content | Hardware budget | SEC-001 §5 |
| Hardware-backed key storage beyond NVS flash encryption | Hardware budget | SEC-001 §5 |
| Cover traffic / message padding | Not Tor | n/a |
| On-device app code execution | Not in v1 (§1.3, §16.4) | n/a |
| Federated marketplace | Single operator in v1 | `SPEC.md` §10.1 |
| Pluggable / negotiable crypto | Pinned per SEC-001 | `crypto-suite.md` §2.1 |

## 6. Open items for SEC-003 review

The external review under SEC-003 should focus its time here. Each item is something this document has identified as a residual risk that warrants outside eyes:

1. **Lost-device path.** Is shipping v1 without a revocation/wipe mechanism acceptable? If not, propose minimum viable revocation (a PIN-gated identity slot, or a marketplace-side per-app revocation list).
2. **Manifest mutation detection.** Should `pagerctl` and/or the Shell record prior manifest pubkey/URL and warn on change? (Mitigates A5.S5.1 / A5.S5.2.)
3. **Permission prompt UX (FW-033) wording.** Audit the dialog text; the dialog is the entire mitigation for app overreach (A1.S1.1).
4. **Replay window composition.** Confirm the 5-min timestamp + nonce counter + push id dedupe behave correctly on reboot and on clock skew. Verify FW-014 persistence guarantees.
5. **OTA signing key custody.** Out of scope of this doc; ensure it is in scope of SEC-003's deliverable.
6. **Push relay sideload boundary.** Confirm PUSH-002 actually rejects unregistered (sideloaded) app signatures.
7. **Rate-limit bucket isolation.** Confirm group-event bucket cannot starve user-notification bucket under realistic chat load.

## 7. Change log

- **v0.2 (SEC-002, this document)** — Initial draft. Adversaries A1–A6; cross-cutting §4; open items §6. Grounded in `SPEC.md` v0.2 and `docs/spec/crypto-suite.md` v0.2.
