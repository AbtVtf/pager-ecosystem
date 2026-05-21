// Request signature verification + signing (JS-004).
//
// Implements the `PagerOS-Sig` request authentication scheme defined in
// SPEC.md §9.2:
//
//     Every request to an App Server includes ``PagerOS-Sig``: an Ed25519
//     signature over ``utf8(method) || utf8(url) || utf8(timestamp) || sha256(body)``.
//     Timestamp prevents replay (servers reject > 5 min skew).
//
// Headers (case-insensitive on input, canonical-cased on output):
//
// - ``PagerOS-Device``    — base64url (no padding) of the device Ed25519
//                           public key (32 bytes).
// - ``PagerOS-Sig``       — base64url (no padding) of the Ed25519 signature
//                           (64 bytes).
// - ``PagerOS-Timestamp`` — integer Unix seconds, as ASCII.
//
// Cryptography uses Node's built-in `node:crypto` Ed25519 support — no
// external deps. Ed25519 signatures are byte-identical to PyNaCl /
// libsodium output, which is what the firmware emits, so wire-level
// vectors round-trip across implementations.

import * as crypto from "node:crypto";

export const HEADER_DEVICE = "PagerOS-Device";
export const HEADER_SIG = "PagerOS-Sig";
export const HEADER_TIMESTAMP = "PagerOS-Timestamp";

export const DEFAULT_MAX_SKEW_SECONDS = 300; // SPEC §9.2

export const ED25519_PUBKEY_LEN = 32;
export const ED25519_SIG_LEN = 64;
export const ED25519_SEED_LEN = 32;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

export class SignatureError extends Error {
  override readonly name: string = "SignatureError";
}
export class MissingHeader extends SignatureError {
  override readonly name = "MissingHeader";
}
export class InvalidEncoding extends SignatureError {
  override readonly name = "InvalidEncoding";
}
export class TimestampSkew extends SignatureError {
  override readonly name = "TimestampSkew";
}
export class BadSignature extends SignatureError {
  override readonly name = "BadSignature";
}

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------

const SPKI_ED25519_PREFIX = Buffer.from("302a300506032b6570032100", "hex");
const PKCS8_ED25519_PREFIX = Buffer.from(
  "302e020100300506032b657004220420",
  "hex",
);

export interface VerifiedRequest {
  /** Raw 32-byte Ed25519 device public key (the PagerOS device identity). */
  readonly deviceId: Uint8Array;
  /** The Unix-seconds timestamp embedded in the request. */
  readonly timestamp: number;
}

export function computeBodyHash(body: Uint8Array | null | undefined): Uint8Array {
  const hash = crypto.createHash("sha256");
  if (body && body.length > 0) hash.update(body);
  return new Uint8Array(hash.digest());
}

export function buildSigningInput(
  method: string,
  url: string,
  timestamp: string | number,
  body: Uint8Array | null | undefined,
): Uint8Array {
  const ts = String(timestamp);
  const enc = new TextEncoder();
  const m = enc.encode(method);
  const u = enc.encode(url);
  const t = enc.encode(ts);
  const h = computeBodyHash(body);
  const out = new Uint8Array(m.length + u.length + t.length + h.length);
  out.set(m, 0);
  out.set(u, m.length);
  out.set(t, m.length + u.length);
  out.set(h, m.length + u.length + t.length);
  return out;
}

function base64UrlDecode(value: string, field: string, expectedLen: number): Uint8Array {
  if (typeof value !== "string") {
    throw new InvalidEncoding(`${field}: not a string`);
  }
  let buf: Buffer;
  try {
    buf = Buffer.from(value, "base64url");
  } catch (err) {
    throw new InvalidEncoding(`${field}: not valid base64url (${(err as Error).message})`);
  }
  if (buf.length !== expectedLen) {
    throw new InvalidEncoding(
      `${field}: expected ${expectedLen} bytes, got ${buf.length}`,
    );
  }
  return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
}

function base64UrlEncode(raw: Uint8Array): string {
  return Buffer.from(raw).toString("base64url");
}

function getCaseInsensitive(
  headers: Record<string, string>,
  key: string,
): string | undefined {
  if (key in headers) return headers[key];
  const lower = key.toLowerCase();
  for (const k of Object.keys(headers)) {
    if (k.toLowerCase() === lower) return headers[k];
  }
  return undefined;
}

function ed25519PublicKey(raw: Uint8Array): crypto.KeyObject {
  if (raw.length !== ED25519_PUBKEY_LEN) {
    throw new InvalidEncoding(
      `ed25519 public key: expected ${ED25519_PUBKEY_LEN} bytes, got ${raw.length}`,
    );
  }
  const der = Buffer.concat([SPKI_ED25519_PREFIX, Buffer.from(raw)]);
  return crypto.createPublicKey({ key: der, format: "der", type: "spki" });
}

function ed25519PrivateKey(seed: Uint8Array): crypto.KeyObject {
  if (seed.length !== ED25519_SEED_LEN) {
    throw new InvalidEncoding(
      `ed25519 seed: expected ${ED25519_SEED_LEN} bytes, got ${seed.length}`,
    );
  }
  const der = Buffer.concat([PKCS8_ED25519_PREFIX, Buffer.from(seed)]);
  return crypto.createPrivateKey({ key: der, format: "der", type: "pkcs8" });
}

/** Derive the 32-byte Ed25519 public key for a 32-byte seed. */
export function publicKeyFromSeed(seed: Uint8Array): Uint8Array {
  const priv = ed25519PrivateKey(seed);
  const pub = crypto.createPublicKey(priv);
  // Extract raw 32-byte key by exporting as JWK.
  const jwk = pub.export({ format: "jwk" }) as { x?: string };
  if (typeof jwk.x !== "string") {
    throw new Error("ed25519 public key export missing 'x'");
  }
  return new Uint8Array(Buffer.from(jwk.x, "base64url"));
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

export interface VerifyRequestOptions {
  method: string;
  url: string;
  headers: Record<string, string>;
  body?: Uint8Array | null;
  /**
   * Reject requests whose ``PagerOS-Timestamp`` is more than this many
   * seconds away from ``now()``. Set to ``null`` to disable the skew
   * check (e.g. in offline LoRa-path tests where the device clock is not
   * authoritative).
   */
  maxSkewSeconds?: number | null;
  now?: () => number; // returns Unix seconds (float OK)
}

export function verifyRequest(opts: VerifyRequestOptions): VerifiedRequest {
  const { method, url, headers, body = null } = opts;
  const maxSkew = opts.maxSkewSeconds === undefined
    ? DEFAULT_MAX_SKEW_SECONDS
    : opts.maxSkewSeconds;
  const now = opts.now ?? (() => Date.now() / 1000);

  const deviceHeader = getCaseInsensitive(headers, HEADER_DEVICE);
  const sigHeader = getCaseInsensitive(headers, HEADER_SIG);
  const tsHeader = getCaseInsensitive(headers, HEADER_TIMESTAMP);

  if (deviceHeader === undefined) throw new MissingHeader(`missing ${HEADER_DEVICE}`);
  if (sigHeader === undefined) throw new MissingHeader(`missing ${HEADER_SIG}`);
  if (tsHeader === undefined) throw new MissingHeader(`missing ${HEADER_TIMESTAMP}`);

  const pubkey = base64UrlDecode(deviceHeader, HEADER_DEVICE, ED25519_PUBKEY_LEN);
  const sig = base64UrlDecode(sigHeader, HEADER_SIG, ED25519_SIG_LEN);

  const tsInt = Number(tsHeader);
  if (!Number.isFinite(tsInt) || !Number.isInteger(tsInt) || !/^-?\d+$/.test(tsHeader)) {
    throw new InvalidEncoding(
      `${HEADER_TIMESTAMP}: expected integer Unix seconds, got ${JSON.stringify(tsHeader)}`,
    );
  }

  if (maxSkew !== null && maxSkew >= 0) {
    const current = Math.floor(now());
    if (Math.abs(current - tsInt) > maxSkew) {
      throw new TimestampSkew(
        `${HEADER_TIMESTAMP}: ${tsInt} outside ±${maxSkew}s of server time ${current}`,
      );
    }
  }

  const signingInput = buildSigningInput(method, url, tsHeader, body);
  const keyObj = ed25519PublicKey(pubkey);
  const ok = crypto.verify(null, signingInput, keyObj, sig);
  if (!ok) {
    throw new BadSignature("PagerOS-Sig did not verify");
  }
  return { deviceId: pubkey, timestamp: tsInt };
}

// ---------------------------------------------------------------------------
// Signing helper (clients, tests)
// ---------------------------------------------------------------------------

export interface SignRequestOptions {
  method: string;
  url: string;
  body?: Uint8Array | null;
  /** 32-byte Ed25519 seed (the same input PyNaCl / libsodium accept). */
  seed: Uint8Array;
  timestamp?: number;
}

export function signRequest(opts: SignRequestOptions): Record<string, string> {
  const { method, url, body = null, seed } = opts;
  const timestamp = opts.timestamp ?? Math.floor(Date.now() / 1000);

  const priv = ed25519PrivateKey(seed);
  const signingInput = buildSigningInput(method, url, timestamp, body);
  const sig = crypto.sign(null, signingInput, priv);
  const pub = publicKeyFromSeed(seed);
  return {
    [HEADER_DEVICE]: base64UrlEncode(pub),
    [HEADER_SIG]: base64UrlEncode(new Uint8Array(sig)),
    [HEADER_TIMESTAMP]: String(timestamp),
  };
}
