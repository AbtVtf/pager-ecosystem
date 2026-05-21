// Push Relay client (JS-006).
//
// Mirrors `sdk/python/pageros/push.py`. Apps encrypt a notification
// payload to a device's X25519 pubkey, sign the outer HTTPS request
// with the app's Ed25519 key, and POST to the Push Relay (SPEC §6.6).
// The relay verifies the app signature and forwards an opaque envelope
// the device decrypts.
//
// What this module owns:
//
//   - The opaque body envelope shape (CBOR map: from, nonce, ct).
//   - The HTTPS call (uses `fetch` so callers can swap it out for tests).
//   - Error types so callers can tell "the relay said no" from "the
//     relay couldn't be reached".
//
// What this module does NOT do:
//
//   - Persistent counter management. Callers MUST supply a monotonic
//     `counter` per (app keypair, device pubkey) pair — reusing one
//     breaks AEAD nonce uniqueness (SEC-001 §1.2). The Python SDK
//     leaves this to apps too; future helpers can layer on top.

import { encodeFrame, decodeFrame } from "./codec.js";
import {
  AppKeypair,
  EncryptionError,
  X25519_KEY_LEN,
  buildNonce,
} from "./encryption.js";
import {
  buildSigningInput,
  ED25519_SEED_LEN,
  HEADER_DEVICE,
  HEADER_SIG,
  HEADER_TIMESTAMP,
  publicKeyFromSeed,
} from "./signing.js";
import * as nodeCrypto from "node:crypto";

export const DEFAULT_PUSH_RELAY_URL = "https://push.pageros.org";
export const HEADER_APP = "PagerOS-App";
export const PUSH_CONTENT_TYPE = "application/cbor";

/** Domain of the SDK's push errors. */
export class PushError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PushError";
  }
}

/** Relay returned a 4xx/5xx — typically auth, rate-limit, or queue overflow. */
export class PushRejected extends PushError {
  readonly status: number;
  readonly body: string;
  constructor(status: number, body: string, message?: string) {
    super(message ?? `Push relay rejected request: HTTP ${status}`);
    this.name = "PushRejected";
    this.status = status;
    this.body = body;
  }
}

/** Couldn't reach the relay at all (DNS, TCP, TLS, timeout). */
export class PushUnavailable extends PushError {
  constructor(message: string) {
    super(message);
    this.name = "PushUnavailable";
  }
}

/** Configuration kept across many push calls. */
export interface PushConfig {
  /**
   * Reverse-DNS app id under which the app's Ed25519 pubkey is
   * registered with the Marketplace. Sent as `PagerOS-App`.
   */
  appId: string;
  /** The app's 32-byte Ed25519 seed (private key material). */
  signingSeed: Uint8Array;
  /** The app's X25519 keypair (encrypts to the device). */
  keypair: AppKeypair;
  /** Push relay base URL. Defaults to {@link DEFAULT_PUSH_RELAY_URL}. */
  relayUrl?: string;
}

/** Successful push delivery to the relay. */
export interface PushResult {
  status: number;
  /** The relay's optional JSON response body (id, queue position, …). */
  body: unknown;
}

interface SendPushOptions {
  /**
   * Per (app, device) monotonic counter. Reusing one breaks AEAD
   * nonce uniqueness. Treat callers' counter as opaque and persist it
   * with whatever durable storage the app already uses.
   */
  counter: number | bigint;
  /** Wall-clock function for the signed timestamp. Default `Date.now`. */
  now?: () => number;
  /** Request timeout in ms. Default 10_000. */
  timeoutMs?: number;
  /**
   * `fetch` implementation override. Use to inject a test transport;
   * defaults to `globalThis.fetch` (Node ≥18 has it built-in).
   */
  fetchImpl?: typeof fetch;
}

/**
 * Build the opaque encrypted body for `POST /push/<device>`.
 *
 * See SPEC §6.6.3 for the wire envelope. The relay treats the result
 * as opaque bytes; only the device's matching X25519 private key can
 * recover the plaintext.
 */
export function buildPushBody(
  keypair: AppKeypair,
  devicePubkey: Uint8Array,
  payload: unknown,
  counter: number | bigint,
): Uint8Array {
  if (devicePubkey.length !== X25519_KEY_LEN) {
    throw new PushError(
      `device pubkey must be ${X25519_KEY_LEN} bytes, got ${devicePubkey.length}`,
    );
  }
  const nonce = buildNonce(0x01, counter);
  const plaintext = encodePayload(payload);
  let ciphertext: Uint8Array;
  try {
    ciphertext = keypair.encryptTo(devicePubkey, plaintext, nonce);
  } catch (err) {
    if (err instanceof EncryptionError) {
      throw new PushError(`failed to encrypt push payload: ${err.message}`);
    }
    throw err;
  }
  return encodeFrame({
    from: keypair.publicKey,
    nonce,
    ct: ciphertext,
  });
}

/**
 * Decode an opaque push body produced by {@link buildPushBody}. Used by
 * tests and any device-side reference code. When `devicePrivateKey` is
 * supplied, the plaintext is decrypted; otherwise it's an empty Uint8Array.
 */
export function decodePushBody(
  body: Uint8Array,
  options: { devicePrivateKey?: Uint8Array } = {},
): {
  senderPubkey: Uint8Array;
  nonce: Uint8Array;
  ciphertext: Uint8Array;
  plaintext: Uint8Array;
} {
  const env = decodeFrame(body);
  if (typeof env !== "object" || env === null) {
    throw new PushError("push body must decode to a CBOR map");
  }
  const m = env as Record<string, unknown>;
  const senderPubkey = expectBytes(m, "from", X25519_KEY_LEN);
  const nonce = expectBytes(m, "nonce", 12);
  const ciphertext = expectBytes(m, "ct");
  let plaintext: Uint8Array = new Uint8Array(0);
  if (options.devicePrivateKey) {
    const peer = AppKeypair.fromPrivateKey(options.devicePrivateKey);
    const decoded = peer.decryptFrom(senderPubkey, ciphertext, nonce);
    plaintext = new Uint8Array(decoded);
  }
  return { senderPubkey, nonce, ciphertext, plaintext };
}

/**
 * Encrypt, sign, and POST a notification to the Push Relay.
 *
 * Rejects with {@link PushRejected} when the relay returns 4xx/5xx, and
 * {@link PushUnavailable} when the request can't reach the relay (DNS,
 * TCP, TLS, timeout). Anything else surfaces as {@link PushError}.
 */
export async function sendPush(
  config: PushConfig,
  deviceId: Uint8Array | string,
  payload: unknown,
  options: SendPushOptions,
): Promise<PushResult> {
  if (config.signingSeed.length !== ED25519_SEED_LEN) {
    throw new PushError(`signingSeed must be ${ED25519_SEED_LEN} bytes`);
  }
  // Touch the derived pubkey to fail fast on a malformed seed.
  publicKeyFromSeed(config.signingSeed);

  const [deviceRaw, deviceB64] = coerceDevicePubkey(deviceId);
  const body = buildPushBody(config.keypair, deviceRaw, payload, options.counter);

  const path = `/push/${deviceB64}`;
  const base = (config.relayUrl ?? DEFAULT_PUSH_RELAY_URL).replace(/\/+$/, "");
  const url = `${base}${path}`;

  const nowFn = options.now ?? Date.now;
  const ts = String(Math.floor(nowFn() / 1000));
  const signingInput = buildSigningInput("POST", path, ts, body);
  const sig = nodeCrypto.sign(null, signingInput, ed25519PrivateKey(config.signingSeed));

  const headers: Record<string, string> = {
    [HEADER_APP]: config.appId,
    [HEADER_DEVICE]: Buffer.from(publicKeyFromSeed(config.signingSeed)).toString("base64url"),
    [HEADER_SIG]: Buffer.from(sig).toString("base64url"),
    [HEADER_TIMESTAMP]: ts,
    "Content-Type": PUSH_CONTENT_TYPE,
  };

  const fetchImpl = options.fetchImpl ?? globalThis.fetch;
  if (typeof fetchImpl !== "function") {
    throw new PushError(
      "global fetch is unavailable; pass options.fetchImpl (Node ≥ 18 has fetch).",
    );
  }

  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), options.timeoutMs ?? 10_000);
  let response: Response;
  try {
    response = await fetchImpl(url, {
      method: "POST",
      headers,
      body,
      signal: ac.signal,
    });
  } catch (err) {
    throw new PushUnavailable(
      `push relay unreachable: ${(err as Error).message ?? String(err)}`,
    );
  } finally {
    clearTimeout(timer);
  }

  const responseText = await response.text();
  if (response.status >= 200 && response.status < 300) {
    let parsed: unknown = null;
    if (responseText.length > 0) {
      try {
        parsed = JSON.parse(responseText);
      } catch {
        parsed = responseText; // surface raw text if the relay didn't return JSON
      }
    }
    return { status: response.status, body: parsed };
  }
  throw new PushRejected(response.status, responseText);
}

// --- helpers ----------------------------------------------------------- //

function encodePayload(payload: unknown): Uint8Array {
  if (payload instanceof Uint8Array) return payload;
  if (typeof payload === "string") return new TextEncoder().encode(payload);
  // Default: CBOR-encode the payload (matches Python helper).
  return encodeFrame(payload);
}

function coerceDevicePubkey(id: Uint8Array | string): [Uint8Array, string] {
  if (id instanceof Uint8Array) {
    if (id.length !== X25519_KEY_LEN) {
      throw new PushError(`device pubkey must be ${X25519_KEY_LEN} bytes, got ${id.length}`);
    }
    return [id, Buffer.from(id).toString("base64url")];
  }
  let raw: Buffer;
  try {
    raw = Buffer.from(id, "base64url");
  } catch (err) {
    throw new PushError(`device pubkey not valid base64url: ${(err as Error).message}`);
  }
  if (raw.length !== X25519_KEY_LEN) {
    raw = Buffer.from(id, "base64");
    if (raw.length !== X25519_KEY_LEN) {
      throw new PushError(`device pubkey must decode to ${X25519_KEY_LEN} bytes`);
    }
  }
  return [new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength), id];
}

function expectBytes(m: Record<string, unknown>, key: string, expectedLen?: number): Uint8Array {
  const v = m[key];
  if (!(v instanceof Uint8Array)) {
    throw new PushError(`push envelope field ${key} must be bytes`);
  }
  if (expectedLen !== undefined && v.length !== expectedLen) {
    throw new PushError(`push envelope field ${key} must be ${expectedLen} bytes, got ${v.length}`);
  }
  return v;
}

function ed25519PrivateKey(seed: Uint8Array): import("node:crypto").KeyObject {
  // PKCS#8 wrapper around a raw 32-byte Ed25519 seed.
  const header = Buffer.from("302e020100300506032b657004220420", "hex");
  const der = Buffer.concat([header, Buffer.from(seed)]);
  return nodeCrypto.createPrivateKey({ key: der, format: "der", type: "pkcs8" });
}
