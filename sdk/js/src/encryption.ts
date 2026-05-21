// X25519 + ChaCha20-Poly1305 inbound decryption (JS-005).
//
// Implements the device→app encrypted path described in SPEC §6.2.2 (LoRa
// inner envelope) and §6.6.3 (Push Relay payloads), keyed to the choices
// pinned in ``docs/spec/crypto-suite.md`` (SEC-001):
//
//  - X25519 ECDH between the device pubkey and the app's X25519 pubkey
//    produces a 32-byte raw shared secret;
//  - HKDF-SHA256 with a zero 32-byte salt and ``info = "pageros/v1/e2e"``
//    derives a 32-byte ChaCha20-Poly1305 key;
//  - AEAD is ChaCha20-Poly1305 *IETF* (12-byte nonce, 16-byte tag).
//
// Cryptography is `node:crypto` only (zero external deps). The same
// libsodium constructions used by the firmware are reachable through
// Node's stdlib via SPKI/PKCS8 DER wrapping for X25519 keys and the
// built-in `chacha20-poly1305` cipher.

import * as crypto from "node:crypto";

export const X25519_KEY_LEN = 32;
export const AEAD_KEY_LEN = 32;
export const AEAD_NONCE_LEN = 12;
export const AEAD_TAG_LEN = 16;

export const HKDF_INFO = new TextEncoder().encode("pageros/v1/e2e");
export const HKDF_SALT = new Uint8Array(32); // 32 zero bytes (crypto-suite.md §1.1)

export const HEADER_ENCRYPTED = "PagerOS-Encrypted";
export const HEADER_SENDER = "PagerOS-Sender";
export const HEADER_NONCE = "PagerOS-Nonce";

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

export class EncryptionError extends Error {
  override readonly name: string = "EncryptionError";
}
export class DecryptionError extends EncryptionError {
  override readonly name = "DecryptionError";
}
export class InvalidEncryptionHeader extends EncryptionError {
  override readonly name = "InvalidEncryptionHeader";
}
export class MissingEncryptionHeader extends EncryptionError {
  override readonly name = "MissingEncryptionHeader";
}

// ---------------------------------------------------------------------------
// DER wrapping helpers
// ---------------------------------------------------------------------------
//
// X25519 OID is 1.3.101.110 (`06 03 2b 65 6e`).
//   SPKI prefix:  302a 3005 06 03 2b 65 6e 03 21 00     (12 bytes)
//   PKCS8 prefix: 302e 0201 00 3005 06 03 2b 65 6e 04 22 0420 (16 bytes)

const SPKI_X25519_PREFIX = Buffer.from("302a300506032b656e032100", "hex");
const PKCS8_X25519_PREFIX = Buffer.from(
  "302e020100300506032b656e04220420",
  "hex",
);

function x25519PrivateKey(scalar: Uint8Array): crypto.KeyObject {
  if (scalar.length !== X25519_KEY_LEN) {
    throw new EncryptionError(
      `X25519 private key must be ${X25519_KEY_LEN} bytes, got ${scalar.length}`,
    );
  }
  const der = Buffer.concat([PKCS8_X25519_PREFIX, Buffer.from(scalar)]);
  return crypto.createPrivateKey({ key: der, format: "der", type: "pkcs8" });
}

function x25519PublicKey(raw: Uint8Array): crypto.KeyObject {
  if (raw.length !== X25519_KEY_LEN) {
    throw new EncryptionError(
      `X25519 public key must be ${X25519_KEY_LEN} bytes, got ${raw.length}`,
    );
  }
  const der = Buffer.concat([SPKI_X25519_PREFIX, Buffer.from(raw)]);
  return crypto.createPublicKey({ key: der, format: "der", type: "spki" });
}

function exportRawPublic(key: crypto.KeyObject): Uint8Array {
  const jwk = key.export({ format: "jwk" }) as { x?: string };
  if (typeof jwk.x !== "string") {
    throw new EncryptionError("X25519 public key export missing 'x'");
  }
  return new Uint8Array(Buffer.from(jwk.x, "base64url"));
}

// ---------------------------------------------------------------------------
// HKDF-SHA256
// ---------------------------------------------------------------------------

function hkdfSha256(
  ikm: Uint8Array,
  salt: Uint8Array,
  info: Uint8Array,
  length: number,
): Uint8Array {
  // node:crypto has built-in HKDF (Node 15+). Returns an ArrayBuffer.
  const buf = crypto.hkdfSync("sha256", ikm, salt, info, length);
  return new Uint8Array(buf);
}

// ---------------------------------------------------------------------------
// Session-key derivation
// ---------------------------------------------------------------------------

const ZERO32 = new Uint8Array(32);

function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let acc = 0;
  for (let i = 0; i < a.length; i += 1) acc |= a[i]! ^ b[i]!;
  return acc === 0;
}

export function x25519SharedSecret(
  privateScalar: Uint8Array,
  peerPublic: Uint8Array,
): Uint8Array {
  const priv = x25519PrivateKey(privateScalar);
  const pub = x25519PublicKey(peerPublic);
  const shared = crypto.diffieHellman({ privateKey: priv, publicKey: pub });
  return new Uint8Array(shared.buffer, shared.byteOffset, shared.byteLength);
}

export function deriveSessionKey(
  privateScalar: Uint8Array,
  peerPublic: Uint8Array,
): Uint8Array {
  const shared = x25519SharedSecret(privateScalar, peerPublic);
  if (bytesEqual(shared, ZERO32)) {
    // crypto-suite.md §1 footnote: contributory check.
    throw new EncryptionError("X25519 yielded an all-zero shared secret");
  }
  return hkdfSha256(shared, HKDF_SALT, HKDF_INFO, AEAD_KEY_LEN);
}

// ---------------------------------------------------------------------------
// AEAD primitives
// ---------------------------------------------------------------------------

export function encrypt(
  plaintext: Uint8Array,
  key: Uint8Array,
  nonce: Uint8Array,
  aad: Uint8Array = new Uint8Array(0),
): Uint8Array {
  if (key.length !== AEAD_KEY_LEN) {
    throw new EncryptionError(
      `AEAD key must be ${AEAD_KEY_LEN} bytes, got ${key.length}`,
    );
  }
  if (nonce.length !== AEAD_NONCE_LEN) {
    throw new EncryptionError(
      `AEAD nonce must be ${AEAD_NONCE_LEN} bytes, got ${nonce.length}`,
    );
  }
  const cipher = crypto.createCipheriv("chacha20-poly1305", key, nonce, {
    authTagLength: AEAD_TAG_LEN,
  });
  if (aad.length > 0) cipher.setAAD(Buffer.from(aad), { plaintextLength: plaintext.length });
  const ct1 = cipher.update(plaintext);
  const ct2 = cipher.final();
  const tag = cipher.getAuthTag();
  return new Uint8Array(Buffer.concat([ct1, ct2, tag]));
}

export function decrypt(
  ciphertext: Uint8Array,
  key: Uint8Array,
  nonce: Uint8Array,
  aad: Uint8Array = new Uint8Array(0),
): Uint8Array {
  if (key.length !== AEAD_KEY_LEN) {
    throw new EncryptionError(
      `AEAD key must be ${AEAD_KEY_LEN} bytes, got ${key.length}`,
    );
  }
  if (nonce.length !== AEAD_NONCE_LEN) {
    throw new EncryptionError(
      `AEAD nonce must be ${AEAD_NONCE_LEN} bytes, got ${nonce.length}`,
    );
  }
  if (ciphertext.length < AEAD_TAG_LEN) {
    throw new DecryptionError(
      `ciphertext shorter than ${AEAD_TAG_LEN}-byte AEAD tag`,
    );
  }
  const ctBody = ciphertext.subarray(0, ciphertext.length - AEAD_TAG_LEN);
  const tag = ciphertext.subarray(ciphertext.length - AEAD_TAG_LEN);
  const decipher = crypto.createDecipheriv("chacha20-poly1305", key, nonce, {
    authTagLength: AEAD_TAG_LEN,
  });
  if (aad.length > 0) decipher.setAAD(Buffer.from(aad), { plaintextLength: ctBody.length });
  decipher.setAuthTag(tag);
  try {
    const pt1 = decipher.update(ctBody);
    const pt2 = decipher.final();
    return new Uint8Array(Buffer.concat([pt1, pt2]));
  } catch {
    // node:crypto throws on authentication failure with the same error
    // class; surface as DecryptionError. The plaintext bytes are
    // discarded — node:crypto enforces "no partial output on auth fail".
    throw new DecryptionError("AEAD authentication failed");
  }
}

// ---------------------------------------------------------------------------
// AppKeypair
// ---------------------------------------------------------------------------

export class AppKeypair {
  /** Raw 32-byte X25519 private scalar. */
  readonly privateKey: Uint8Array;
  /** Raw 32-byte X25519 public key. */
  readonly publicKey: Uint8Array;

  private constructor(privateKey: Uint8Array, publicKey: Uint8Array) {
    if (privateKey.length !== X25519_KEY_LEN) {
      throw new EncryptionError(
        `X25519 private key must be ${X25519_KEY_LEN} bytes, got ${privateKey.length}`,
      );
    }
    if (publicKey.length !== X25519_KEY_LEN) {
      throw new EncryptionError(
        `X25519 public key must be ${X25519_KEY_LEN} bytes, got ${publicKey.length}`,
      );
    }
    this.privateKey = privateKey;
    this.publicKey = publicKey;
  }

  /** Generate a fresh X25519 keypair from the OS RNG. */
  static generate(): AppKeypair {
    const scalar = new Uint8Array(crypto.randomBytes(X25519_KEY_LEN));
    return AppKeypair.fromPrivateKey(scalar);
  }

  /** Import a 32-byte X25519 private scalar; derive the pubkey from it. */
  static fromPrivateKey(privateKey: Uint8Array): AppKeypair {
    if (privateKey.length !== X25519_KEY_LEN) {
      throw new EncryptionError(
        `X25519 private key must be ${X25519_KEY_LEN} bytes, got ${privateKey.length}`,
      );
    }
    const priv = x25519PrivateKey(privateKey);
    const pub = crypto.createPublicKey(priv);
    return new AppKeypair(privateKey, exportRawPublic(pub));
  }

  /** The pubkey as base64url-no-padding (manifest-friendly format). */
  get publicKeyB64(): string {
    return Buffer.from(this.publicKey).toString("base64url");
  }

  /** Derive the 32-byte AEAD key for a peer's X25519 pubkey. */
  sessionKey(peerPubkey: Uint8Array): Uint8Array {
    return deriveSessionKey(this.privateKey, peerPubkey);
  }

  /** Derive the session key and AEAD-decrypt in one call. */
  decryptFrom(
    peerPubkey: Uint8Array,
    ciphertext: Uint8Array,
    nonce: Uint8Array,
    aad: Uint8Array = new Uint8Array(0),
  ): Uint8Array {
    const key = this.sessionKey(peerPubkey);
    return decrypt(ciphertext, key, nonce, aad);
  }

  /** Companion to decryptFrom — used by tests, PUSH-002, etc. */
  encryptTo(
    peerPubkey: Uint8Array,
    plaintext: Uint8Array,
    nonce: Uint8Array,
    aad: Uint8Array = new Uint8Array(0),
  ): Uint8Array {
    const key = this.sessionKey(peerPubkey);
    return encrypt(plaintext, key, nonce, aad);
  }
}

// ---------------------------------------------------------------------------
// Spec-pinned nonce helper (crypto-suite.md §1.2)
// ---------------------------------------------------------------------------

/**
 * Build a 12-byte AEAD nonce per crypto-suite.md §1.2.
 *
 * Layout: ``sender_id_byte || u64_be(counter) || 3 zero bytes``.
 *
 * `senderIdByte`: `0x01` = app→device, `0x02` = device→app. Callers are
 * responsible for choosing the right value for their direction.
 */
export function buildNonce(senderIdByte: number, counter: number | bigint): Uint8Array {
  if (!Number.isInteger(senderIdByte) || senderIdByte < 0 || senderIdByte > 0xff) {
    throw new EncryptionError(`senderIdByte must fit in a byte, got ${senderIdByte}`);
  }
  const counterBig = typeof counter === "bigint" ? counter : BigInt(counter);
  if (counterBig < 0n || counterBig >= 0x10000000000000000n) {
    throw new EncryptionError(`counter must fit in u64, got ${counter}`);
  }
  const out = new Uint8Array(AEAD_NONCE_LEN);
  out[0] = senderIdByte;
  new DataView(out.buffer).setBigUint64(1, counterBig, false);
  return out;
}
