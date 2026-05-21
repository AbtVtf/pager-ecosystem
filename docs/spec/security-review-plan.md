# PagerOS External Security Review — Plan

**Status:** v0.2 prep (SEC-003)
**Owner:** CEO (Ada). The review itself is performed by external reviewers; this document is the plan for *getting* that review done.
**Gate:** The review can be commissioned now (paid track lead time is long). The **sign-off** that closes SEC-003 cannot be granted until the v1 stack is substantially complete (M5 done, M6 in progress).

This plan exists so that when M5 closes, we are not starting from zero on procurement, scope, or materials. Everything the reviewer needs except the final code drop is pinned here.

## 1. Scope of the review (the four pillars)

These are the four areas the SEC-003 acceptance criterion names. Each has a primary document the reviewer should start from and a list of code surfaces that implement it.

| Pillar | Primary doc | Code surfaces |
|---|---|---|
| **Crypto choices** | [`docs/spec/crypto-suite.md`](./crypto-suite.md) (SEC-001) + [`docs/spec/crypto-test-vectors.json`](./crypto-test-vectors.json) | `firmware/components/pageros_crypto/`, `sdk/python/pageros/crypto/`, `sdk/js/src/crypto/`, `exit-node/internal/cryptotest/`, `simulator/crates/pageros-core/src/crypto/` |
| **Key management** | [`docs/spec/threat-model.md`](./threat-model.md) §3.4 (lost device), §4.2 (rotation), §4.4 (OTA) + `SPEC.md` §9.1, §13 | `firmware/components/pageros_identity/`, OTA pipeline (FW-036), Marketplace DNS TXT challenge (MKT-003) |
| **Transport security** | `SPEC.md` §6 (transport), §9.2 (request signing), §9.3 (E2E) + `protocol/spec.md` | `firmware/components/pageros_transport/`, `exit-node/internal/relay/`, `push-relay/internal/auth/`, LoRa codecs (`LORA-001..005`) |
| **Marketplace trust model** | `SPEC.md` §10, threat-model §3.5 + `marketplace/openapi.yaml` | `marketplace/internal/registry/`, `marketplace/internal/moderation/`, `cli/publish/` |

The review **does not** cover: anonymity properties (non-goal per `SPEC.md` §1.3), denial-of-service against the LoRa RF layer itself, supply-chain attacks against ESP-IDF or upstream libsodium (delegated to those projects' own review processes), hardware tamper attacks beyond what NVS flash encryption provides (out of v1 scope per SEC-001 §5).

## 2. Open questions the reviewer should answer

From the threat-model's §6 open items, surfaced here as concrete questions:

1. **Lost-device path.** Is shipping v1 without revocation / wipe / PIN acceptable? If not, propose the minimum viable scheme.
2. **Manifest mutation detection.** Should the device or `pagerctl` warn when an installed app's pubkey or URL changes in the marketplace registry?
3. **Permission prompt UX (FW-033).** Audit the dialog wording — it is the entire mitigation against permission overreach (threat-model §3.1).
4. **Replay window composition.** Confirm the 5-min timestamp + AEAD counter + push id dedupe behave correctly across reboot, clock skew, and exit-node retransmission.
5. **OTA signing key custody.** Recommend key-management procedures (HSM, threshold signing, rollback policy).
6. **Sideload boundary.** Confirm `PUSH-002` actually rejects unregistered (sideloaded) app signatures so sideloaded apps cannot use the relay.
7. **Rate-limit bucket isolation.** Validate the per-app/per-device and group-vs-notification bucket math against the realistic `chat` reference app worst case.

The reviewer is encouraged to surface additional findings; this list is a floor, not a ceiling.

## 3. Materials packet (what we send the reviewer)

When we commission the review, the packet contains:

- A snapshot tag in the monorepo (`release/v1-security-review-<date>`) pinning the exact tree under review.
- `SPEC.md` (current version, expected v0.3+ by M6).
- `protocol/spec.md` plus `protocol/test-vectors/` (PROTO-002, PROTO-003).
- `docs/spec/crypto-suite.md` + `docs/spec/crypto-test-vectors.json` (SEC-001).
- `docs/spec/threat-model.md` (SEC-002, this document's sibling).
- `docs/spec/security-review-plan.md` (this document).
- Build artifacts: firmware `.bin` for the review tag, Push Relay container image, Exit Node binary, Marketplace docker-compose.
- Conformance run output: PROTO-005 report against all SDKs (proves the test vectors do pass in every implementation).
- A short, one-page "what's not implemented yet" note pinned to known M6 gaps so the reviewer does not flag known-incomplete work as a finding.

## 4. Reviewer tracks

We commit to one of two tracks; the choice is the board's. Either is consistent with the SEC-003 acceptance criterion ("paid or community").

### 4.1 Paid track (faster, narrower, billable)

Candidates to solicit quotes from (alphabetical, all have prior LoRa / embedded / cryptographic-protocol work):

- **Trail of Bits** — strong on embedded + protocol review; familiar with libsodium.
- **NCC Group (Cryptography Services)** — strong on protocol audits and key management; published reports on similar IoT projects.
- **Cure53** — fast turnaround, strong on web-property (marketplace, push relay) plus protocol.
- **Atredis Partners** — embedded focus, public reports on radio + firmware stacks.

Typical engagement shape for a stack this size: 4–6 person-weeks, written report under NDA-then-public, one round of remediation review. Lead time to start: 6–12 weeks from contract signing. Budget order-of-magnitude: USD 60k–150k (board approves the actual quote).

**Decision needed from the board to start procurement:** preferred shortlist, budget ceiling, NDA posture (we strongly prefer public-after-fix to honor the open-ecosystem goal).

### 4.2 Community track (slower, broader, $0 cash cost)

Channels (in rough priority order):

- **`SPEC.md` + threat-model published on `spec.pageros.org`** with an explicit "We are looking for reviewers" banner and a `SECURITY-REVIEW.md` at the repo root that points to this plan.
- Targeted asks to authors of comparable prior work — Meshtastic security volunteers, Reticulum project, libsodium maintainers, NCC-published-protocol-review authors.
- A post on `oss-security` and `cryptography@lists.randombit.net` describing the scope and inviting reviewers.
- A LWN article (paid placement or pitched as story) once `spec.pageros.org` is live (DOCS-003).
- A `bountysource`-style program for specific findings (separate decision; not a precondition for SEC-003).

The community track requires accepting that sign-off comes from a *quorum* of named reviewers rather than a single audit firm. We define the quorum below.

### 4.3 What counts as sign-off

For SEC-003 to be marked done, **one** of the following must be true:

- A paid firm has delivered a final report under their normal review methodology, all "Critical" and "High" findings are either fixed or have a documented accepted-risk rationale signed by CEO, and the firm has confirmed in writing they have no remaining blocking findings.
- **OR** at least **three** independent community reviewers (no shared affiliation, no contributors to this project) have each examined all four pillars (§1) and posted a public review summary with their findings; CEO has dispositioned all "Critical" / "High"-equivalent findings as fixed or accepted-risk; no reviewer has an outstanding blocking concern.

Both bars demand the same artifact: a public `docs/spec/security-review-report.md` summarizing findings, dispositions, and reviewer attestations, before public launch.

## 5. Timeline / gates

| Gate | Condition | Action |
|---|---|---|
| **G1 — Plan published** | This document committed and linked from SEC-003 | Now (this heartbeat). |
| **G2 — Materials packet draft-ready** | All M5-prerequisite docs current; conformance run green | Track via the dep walker; revisit when SEC-001/SEC-002/PROTO-003/PROTO-005 are all done AND M4 is done. |
| **G3 — Procurement / outreach starts** | Board picks paid vs community track + budget (paid) or naming the first three reviewer asks (community) | Board decision; CEO posts comment when scheduled. |
| **G4 — Engagement under way** | Contract signed (paid) OR three reviewers have accepted scope (community) | CEO updates SEC-003 with engagement reference. |
| **G5 — Report received** | Final report draft delivered | CEO triages findings; opens SEC-* issues per finding. |
| **G6 — Sign-off** | All blocking findings closed; report published at `spec.pageros.org/security-review` | Close SEC-003. |

G2 is the most useful near-term milestone — it is what makes G3 a cheap decision instead of a months-long preamble.

## 6. Things this plan does **not** decide

These are deliberately left to the board / a subsequent decision:

- **Track choice.** Paid vs community, and within paid, which firm.
- **Budget.** No dollar figure is committed here.
- **Disclosure timing.** Coordinated-disclosure vs immediate-public is a board call; defaults to "public after fix" per our open-ecosystem goal.
- **Embargo on the report itself.** Default is "public on launch day"; the board may choose to publish earlier.

A board decision on the above is what flips this from `prep` to `executing`.
