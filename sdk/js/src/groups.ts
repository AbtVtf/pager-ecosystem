// Group helpers (JS-007).
//
// Mirrors `sdk/python/pageros/groups.py`. Multi-device apps emit group
// events (member_joined/left, presence_update, group_message — SPEC
// §5.4.2). Online subscribers consume them via the app's own long-poll
// endpoint; offline subscribers receive them through the Push Relay's
// `/group_push` endpoint with a per-recipient X25519+ChaCha20 envelope.
//
// What this module owns:
//
//   - The wire path + content type for the group-push batch
//     (`POST /group_push`, application/json).
//   - The recipient-result enum so callers can branch on
//     "accepted" vs "rate_limited" vs "bad payload" etc.
//   - `sendGroupPush` — encrypt one payload per recipient, sign the
//     whole batch, POST. Returns the per-recipient result.
//
// What this module does NOT do:
//
//   - Membership / authorisation. The app owns those records
//     (SPEC §9.6); helpers only ferry events to known device pubkeys.

import { encodeFrame } from "./codec.js";
import {
  AppKeypair,
  X25519_KEY_LEN,
  buildNonce,
  EncryptionError,
} from "./encryption.js";
import {
  buildSigningInput,
  ED25519_SEED_LEN,
  HEADER_DEVICE,
  HEADER_SIG,
  HEADER_TIMESTAMP,
  publicKeyFromSeed,
} from "./signing.js";
import {
  DEFAULT_PUSH_RELAY_URL,
  HEADER_APP,
  PushError,
  PushRejected,
  PushUnavailable,
  type PushConfig,
} from "./push.js";
import * as nodeCrypto from "node:crypto";

export const GROUP_PUSH_PATH = "/group_push";
export const GROUP_PUSH_CONTENT_TYPE = "application/json";

/** Recipient outcomes — must match `sdk/python/pageros/groups.py`. */
export const GROUP_RESULT_ACCEPTED = "accepted";
export const GROUP_RESULT_RATE_LIMITED = "rate_limited";
export const GROUP_RESULT_PAYLOAD_EMPTY = "payload_empty";
export const GROUP_RESULT_PAYLOAD_LARGE = "payload_too_large";
export const GROUP_RESULT_BAD_PAYLOAD = "invalid_payload";
export const GROUP_RESULT_BAD_DEVICE = "invalid_device_pubkey";
export const GROUP_RESULT_STORAGE_ERROR = "storage_error";

export type GroupRecipientResultCode =
  | typeof GROUP_RESULT_ACCEPTED
  | typeof GROUP_RESULT_RATE_LIMITED
  | typeof GROUP_RESULT_PAYLOAD_EMPTY
  | typeof GROUP_RESULT_PAYLOAD_LARGE
  | typeof GROUP_RESULT_BAD_PAYLOAD
  | typeof GROUP_RESULT_BAD_DEVICE
  | typeof GROUP_RESULT_STORAGE_ERROR
  | string; // forward-compat for future codes

/** Per-recipient result returned by the relay. */
export interface GroupRecipientResult {
  device: string; // base64url device pubkey
  result: GroupRecipientResultCode;
  reason: string | undefined;
}

/** Aggregate result of a batch send. */
export interface GroupBroadcastResult {
  status: number;
  accepted: number;
  rejected: number;
  results: GroupRecipientResult[];
}

/** Domain error specific to group broadcast failures. */
export class GroupBroadcastError extends PushError {
  readonly status: number | undefined;
  constructor(message: string, status?: number) {
    super(message);
    this.name = "GroupBroadcastError";
    this.status = status;
  }
}

export interface SendGroupPushOptions {
  /** Wall-clock function. Default `Date.now`. */
  now?: () => number;
  /** Request timeout in ms. Default 10_000. */
  timeoutMs?: number;
  /** `fetch` override for tests. */
  fetchImpl?: typeof fetch;
}

/**
 * Encrypt `payload` once per recipient, sign the batch, and POST to
 * `/group_push` on the Push Relay. Returns the per-recipient verdicts
 * from the relay; throws on transport / signature failures.
 *
 * `recipients` is a list of `[deviceB64, devicePubkeyBytes]` tuples — the
 * b64 form is sent on the wire, the raw bytes feed the per-recipient
 * AEAD. Callers that only have the bytes can derive the b64 via
 * `Buffer.from(pubkey).toString("base64url")`.
 */
export async function sendGroupPush(
  config: PushConfig,
  recipients: Array<[string, Uint8Array]>,
  payload: unknown,
  options: SendGroupPushOptions = {},
): Promise<GroupBroadcastResult> {
  if (config.signingSeed.length !== ED25519_SEED_LEN) {
    throw new GroupBroadcastError(
      `signingSeed must be ${ED25519_SEED_LEN} bytes`,
    );
  }
  publicKeyFromSeed(config.signingSeed);

  const body = buildGroupPushBody(config.keypair, recipients, payload);
  const path = GROUP_PUSH_PATH;
  const base = (config.relayUrl ?? DEFAULT_PUSH_RELAY_URL).replace(/\/+$/, "");
  const url = `${base}${path}`;

  const nowFn = options.now ?? Date.now;
  const ts = String(Math.floor(nowFn() / 1000));
  const signingInput = buildSigningInput("POST", path, ts, body);
  const sig = nodeCrypto.sign(null, signingInput, ed25519PrivateKeyForSeed(config.signingSeed));

  const headers: Record<string, string> = {
    [HEADER_APP]: config.appId,
    [HEADER_DEVICE]: Buffer.from(publicKeyFromSeed(config.signingSeed)).toString("base64url"),
    [HEADER_SIG]: Buffer.from(sig).toString("base64url"),
    [HEADER_TIMESTAMP]: ts,
    "Content-Type": GROUP_PUSH_CONTENT_TYPE,
  };

  const fetchImpl = options.fetchImpl ?? globalThis.fetch;
  if (typeof fetchImpl !== "function") {
    throw new GroupBroadcastError(
      "global fetch is unavailable; pass options.fetchImpl",
    );
  }

  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), options.timeoutMs ?? 10_000);
  let response: Response;
  try {
    response = await fetchImpl(url, { method: "POST", headers, body, signal: ac.signal });
  } catch (err) {
    throw new PushUnavailable(
      `push relay unreachable: ${(err as Error).message ?? String(err)}`,
    );
  } finally {
    clearTimeout(timer);
  }

  const responseText = await response.text();
  if (response.status < 200 || response.status >= 300) {
    throw new PushRejected(response.status, responseText);
  }

  // The relay responds with `{accepted, rejected, results: [...]}`
  // (mirrors Python helper). Anything we can't parse is a hard fail.
  let parsed: { accepted?: number; rejected?: number; results?: unknown };
  try {
    parsed = JSON.parse(responseText);
  } catch (err) {
    throw new GroupBroadcastError(
      `push relay returned non-JSON body: ${(err as Error).message}`,
      response.status,
    );
  }

  const results: GroupRecipientResult[] = [];
  if (Array.isArray(parsed.results)) {
    for (const entry of parsed.results) {
      if (entry && typeof entry === "object") {
        const e = entry as Record<string, unknown>;
        results.push({
          device: String(e.device ?? ""),
          result: String(e.result ?? ""),
          reason: e.reason !== undefined ? String(e.reason) : undefined,
        });
      }
    }
  }
  return {
    status: response.status,
    accepted: Number(parsed.accepted ?? results.filter((r) => r.result === GROUP_RESULT_ACCEPTED).length),
    rejected: Number(parsed.rejected ?? results.filter((r) => r.result !== GROUP_RESULT_ACCEPTED).length),
    results,
  };
}

/**
 * Build the JSON body for a `/group_push` batch. Each recipient gets
 * an independent AEAD encryption of the same payload with a unique
 * nonce so devices that can't see each other's traffic still get a
 * fresh ciphertext.
 */
export function buildGroupPushBody(
  keypair: AppKeypair,
  recipients: Array<[string, Uint8Array]>,
  payload: unknown,
): Uint8Array {
  if (recipients.length === 0) {
    throw new GroupBroadcastError("recipients must be non-empty");
  }
  const plaintext = encodePayload(payload);
  const items: Record<string, string>[] = [];
  let counter = 1;
  for (const [b64, pubkey] of recipients) {
    if (pubkey.length !== X25519_KEY_LEN) {
      throw new GroupBroadcastError(
        `recipient ${b64}: pubkey must be ${X25519_KEY_LEN} bytes, got ${pubkey.length}`,
      );
    }
    const nonce = buildNonce(0x02, counter);
    counter += 1;
    let ct: Uint8Array;
    try {
      ct = keypair.encryptTo(pubkey, plaintext, nonce);
    } catch (err) {
      if (err instanceof EncryptionError) {
        throw new GroupBroadcastError(
          `recipient ${b64}: encrypt failed: ${err.message}`,
        );
      }
      throw err;
    }
    items.push({
      device: b64,
      nonce: Buffer.from(nonce).toString("base64url"),
      ct: Buffer.from(ct).toString("base64url"),
    });
  }
  const body = {
    from: Buffer.from(keypair.publicKey).toString("base64url"),
    recipients: items,
  };
  return new TextEncoder().encode(JSON.stringify(body));
}

function encodePayload(payload: unknown): Uint8Array {
  if (payload instanceof Uint8Array) return payload;
  if (typeof payload === "string") return new TextEncoder().encode(payload);
  return encodeFrame(payload);
}

function ed25519PrivateKeyForSeed(seed: Uint8Array): nodeCrypto.KeyObject {
  const header = Buffer.from("302e020100300506032b657004220420", "hex");
  const der = Buffer.concat([header, Buffer.from(seed)]);
  return nodeCrypto.createPrivateKey({ key: der, format: "der", type: "pkcs8" });
}
