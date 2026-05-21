// Tests for the JS-005 X25519 + ChaCha20-Poly1305 decryption stack.
//
// Acceptance (TASKS.md, JS-005):
//   * Apps can declare a keypair (`AppKeypair`),
//   * SDK transparently decrypts inbound encrypted requests.
//
// Pinned to the cross-impl vectors in
// `docs/spec/crypto-test-vectors.json` so any encoder/decoder drift
// versus libsodium / Python surfaces here.

import { test } from "node:test";
import { strict as assert } from "node:assert";
import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import * as http from "node:http";

import {
  AEAD_NONCE_LEN,
  App,
  AppKeypair,
  DecryptionError,
  HEADER_ENCRYPTED,
  HEADER_NONCE,
  HEADER_SENDER,
  InvalidEncryptionHeader,
  X25519_KEY_LEN,
  buildNonce,
  decrypt,
  deriveSessionKey,
  encrypt,
  encodeFrame,
  decodeFrame,
  signRequest,
  x25519SharedSecret,
} from "../src/index.js";

const __dirname = dirname(fileURLToPath(import.meta.url));

function findRepoRoot(startDir: string): string {
  let dir = startDir;
  while (true) {
    if (existsSync(resolve(dir, "SPEC.md"))) return dir;
    const parent = dirname(dir);
    if (parent === dir) throw new Error("repo root not found");
    dir = parent;
  }
}

const REPO_ROOT = findRepoRoot(__dirname);
const VECTORS_PATH = resolve(REPO_ROOT, "docs/spec/crypto-test-vectors.json");

interface VectorFile {
  vectors: Array<Record<string, unknown>>;
}

function loadVectors(): VectorFile {
  return JSON.parse(readFileSync(VECTORS_PATH, "utf-8")) as VectorFile;
}

function fromHex(s: string): Uint8Array {
  const clean = s.replace(/\s+/g, "");
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i += 1) {
    out[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

function toHex(b: Uint8Array): string {
  let s = "";
  for (let i = 0; i < b.length; i += 1) s += b[i]!.toString(16).padStart(2, "0");
  return s;
}

const VECTORS = loadVectors();

// ---------------------------------------------------------------------------
// X25519 — RFC 7748 §6.1
// ---------------------------------------------------------------------------

test("X25519: priv → pub matches RFC 7748 Alice/Bob", () => {
  const vec = VECTORS.vectors.find((v) => v.id === "x25519-rfc7748-6.1");
  assert.ok(vec, "x25519-rfc7748-6.1 vector missing");
  const priv = fromHex(vec!["priv"] as string);
  const expectedPub = vec!["pub_self"] as string;
  const kp = AppKeypair.fromPrivateKey(priv);
  // Node clamps the X25519 private scalar per RFC 7748 §5 (so does
  // libsodium). The pubkey *is* the clamped value's curve point.
  assert.equal(toHex(kp.publicKey), expectedPub);
});

test("X25519: shared secret matches RFC 7748 Alice/Bob", () => {
  const vec = VECTORS.vectors.find((v) => v.id === "x25519-rfc7748-6.1");
  const priv = fromHex(vec!["priv"] as string);
  const peer = fromHex(vec!["pub_peer"] as string);
  const expected = vec!["shared"] as string;
  const got = x25519SharedSecret(priv, peer);
  assert.equal(toHex(got), expected);
});

// ---------------------------------------------------------------------------
// ChaCha20-Poly1305 IETF — RFC 8439 §2.8.2
// ---------------------------------------------------------------------------

test("ChaCha20-Poly1305: RFC 8439 §2.8.2 encrypt matches vector bytes", () => {
  const vec = VECTORS.vectors.find((v) => v.id === "chacha20poly1305-rfc8439-2.8.2");
  assert.ok(vec, "chacha20poly1305-rfc8439-2.8.2 vector missing");
  const key = fromHex(vec!["key"] as string);
  const nonce = fromHex(vec!["nonce"] as string);
  const aad = fromHex(vec!["aad"] as string);
  const plaintext = fromHex(vec!["plaintext"] as string);
  const expectedCiphertext = vec!["ciphertext"] as string;
  const ct = encrypt(plaintext, key, nonce, aad);
  assert.equal(toHex(ct), expectedCiphertext);
});

test("ChaCha20-Poly1305: RFC 8439 §2.8.2 decrypt round-trips plaintext", () => {
  const vec = VECTORS.vectors.find((v) => v.id === "chacha20poly1305-rfc8439-2.8.2");
  const key = fromHex(vec!["key"] as string);
  const nonce = fromHex(vec!["nonce"] as string);
  const aad = fromHex(vec!["aad"] as string);
  const ciphertext = fromHex(vec!["ciphertext"] as string);
  const expectedPlaintext = vec!["plaintext"] as string;
  const pt = decrypt(ciphertext, key, nonce, aad);
  assert.equal(toHex(pt), expectedPlaintext);
});

test("ChaCha20-Poly1305: tag-mutated ciphertext fails decryption", () => {
  const vec = VECTORS.vectors.find((v) => v.id === "chacha20poly1305-rfc8439-tag-mutated");
  assert.ok(vec, "tag-mutated vector missing");
  const key = fromHex(vec!["key"] as string);
  const nonce = fromHex(vec!["nonce"] as string);
  const aad = fromHex(vec!["aad"] as string);
  const ciphertext = fromHex(vec!["ciphertext"] as string);
  assert.throws(() => decrypt(ciphertext, key, nonce, aad), DecryptionError);
});

// ---------------------------------------------------------------------------
// deriveSessionKey + encryptTo / decryptFrom round-trip
// ---------------------------------------------------------------------------

test("deriveSessionKey is symmetric across the X25519 ECDH pair", () => {
  const alice = AppKeypair.generate();
  const bob = AppKeypair.generate();
  const k_ab = deriveSessionKey(alice.privateKey, bob.publicKey);
  const k_ba = deriveSessionKey(bob.privateKey, alice.publicKey);
  assert.deepEqual(Array.from(k_ab), Array.from(k_ba));
  assert.equal(k_ab.length, 32);
});

test("AppKeypair.encryptTo / decryptFrom round-trips arbitrary payloads", () => {
  const alice = AppKeypair.generate();
  const bob = AppKeypair.generate();
  const nonce = buildNonce(0x02, 0n);
  const payload = new TextEncoder().encode("inner CBOR or whatever");
  const ciphertext = alice.encryptTo(bob.publicKey, payload, nonce);
  const recovered = bob.decryptFrom(alice.publicKey, ciphertext, nonce);
  assert.deepEqual(Array.from(recovered), Array.from(payload));
});

test("AppKeypair.decryptFrom raises DecryptionError on flipped tag", () => {
  const alice = AppKeypair.generate();
  const bob = AppKeypair.generate();
  const nonce = buildNonce(0x02, 1n);
  const ct = alice.encryptTo(bob.publicKey, new Uint8Array([1, 2, 3]), nonce);
  const lastIdx = ct.length - 1;
  ct[lastIdx] = (ct[lastIdx] ?? 0) ^ 1;
  assert.throws(
    () => bob.decryptFrom(alice.publicKey, ct, nonce),
    DecryptionError,
  );
});

test("buildNonce produces the spec-pinned 12-byte layout", () => {
  // sender_id 0x01 (app→device), counter 0 → 0x01 + 8 zeros + 3 zeros.
  assert.equal(toHex(buildNonce(0x01, 0)), "01" + "00".repeat(11));
  // sender_id 0x02 (device→app), counter 1 →  0x02 + u64be(1) + 3 zeros.
  assert.equal(toHex(buildNonce(0x02, 1)), "02" + "0000000000000001" + "000000");
});

// ---------------------------------------------------------------------------
// App integration — transparent decryption
// ---------------------------------------------------------------------------

function encodeBase64Url(b: Uint8Array): string {
  return Buffer.from(b).toString("base64url");
}

test("App with keypair transparently decrypts inbound encrypted requests", async () => {
  const appKp = AppKeypair.generate();
  const device = AppKeypair.generate();
  const app = new App({ name: "encrypted", keypair: appKp });

  const observed: { senderId: Uint8Array | null; body: unknown } = {
    senderId: null,
    body: null,
  };
  app.handler("/echo", undefined, (req) => {
    observed.senderId = req.ctx.senderId;
    observed.body = req.body;
    return null;
  });

  const plaintext = encodeFrame({ msg: "hi" });
  const nonce = buildNonce(0x02, 7n);
  const ciphertext = device.encryptTo(appKp.publicKey, plaintext, nonce);

  const { status } = await app.dispatch("POST", "/echo", {
    body: ciphertext,
    headers: {
      [HEADER_ENCRYPTED]: "1",
      [HEADER_SENDER]: encodeBase64Url(device.publicKey),
      [HEADER_NONCE]: encodeBase64Url(nonce),
      "Content-Type": "application/cbor",
    },
  });
  assert.equal(status, 204);
  assert.ok(observed.senderId !== null);
  assert.deepEqual(Array.from(observed.senderId!), Array.from(device.publicKey));
  assert.deepEqual(observed.body, { msg: "hi" });
});

test("App without keypair rejects encrypted requests with 400", async () => {
  const app = new App({ name: "no-keypair" });
  app.handler("/echo", undefined, () => null);
  const { status, body } = await app.dispatch("POST", "/echo", {
    body: new Uint8Array(20),
    headers: { [HEADER_ENCRYPTED]: "1" },
  });
  assert.equal(status, 400);
  const frame = decodeFrame(body) as Record<string, unknown>;
  assert.equal(frame["id"], "err_local");
});

test("App with keypair rejects encrypted requests missing sender/nonce headers (400)", async () => {
  const app = new App({ name: "encrypted", keypair: AppKeypair.generate() });
  app.handler("/echo", undefined, () => null);
  const { status } = await app.dispatch("POST", "/echo", {
    body: new Uint8Array(20),
    headers: { [HEADER_ENCRYPTED]: "1", [HEADER_SENDER]: "AAA" },
  });
  assert.equal(status, 400);
});

test("App with keypair rejects malformed ciphertext with 401", async () => {
  const appKp = AppKeypair.generate();
  const device = AppKeypair.generate();
  const app = new App({ name: "encrypted", keypair: appKp });
  app.handler("/echo", undefined, () => null);
  const nonce = buildNonce(0x02, 0n);
  const { status, headers } = await app.dispatch("POST", "/echo", {
    body: new Uint8Array(20), // random bytes, will fail auth
    headers: {
      [HEADER_ENCRYPTED]: "1",
      [HEADER_SENDER]: encodeBase64Url(device.publicKey),
      [HEADER_NONCE]: encodeBase64Url(nonce),
    },
  });
  assert.equal(status, 401);
  assert.match(headers["WWW-Authenticate"] ?? "", /PagerOS-Sig realm/);
});

test("App without PagerOS-Encrypted flag passes cleartext through unchanged", async () => {
  // Mixed-traffic scenario: app has a keypair but the request is cleartext.
  const app = new App({ name: "mixed", keypair: AppKeypair.generate() });
  let seen: unknown = undefined;
  app.handler("/save", undefined, (req) => {
    seen = req.body;
    return null;
  });
  const body = encodeFrame({ msg: "plain" });
  await app.dispatch("POST", "/save", {
    body,
    headers: { "Content-Type": "application/cbor" },
  });
  assert.deepEqual(seen, { msg: "plain" });
});

// ---------------------------------------------------------------------------
// Combined: encrypted + signed end-to-end
// ---------------------------------------------------------------------------

test("encrypted + signed: signature verifies on the decrypted plaintext", async () => {
  const appKp = AppKeypair.generate();
  const device = AppKeypair.generate();
  // Device's *signing* identity is an Ed25519 seed; its *encryption*
  // identity is an X25519 key. They're independent.
  const seed = new Uint8Array(32);
  // Fill with a known seed
  for (let i = 0; i < seed.length; i += 1) seed[i] = i + 1;

  const ts = 1700000000;
  const app = new App({
    name: "secure",
    keypair: appKp,
    verifySignature: { now: () => ts },
  });
  app.handler("/event", undefined, (req) => {
    if (!req.ctx.deviceId || !req.ctx.senderId) {
      throw new Error("missing identity");
    }
    return null;
  });

  // 1. Build the plaintext body the device signs.
  const plaintext = encodeFrame({ type: "nfc_scan", uid: "abcd" });

  // 2. Compute PagerOS-Sig over the PLAINTEXT body (per SPEC §7.3.1
  //    "Servers receiving via Exit Node see the rehydrated HTTP request
  //    and SHOULD NOT distinguish transport for application logic").
  const sigHeaders = signRequest({
    method: "POST",
    url: "/event",
    body: plaintext,
    seed,
    timestamp: ts,
  });

  // 3. Encrypt the plaintext for transport.
  const nonce = buildNonce(0x02, 0n);
  const ciphertext = device.encryptTo(appKp.publicKey, plaintext, nonce);

  const { status } = await app.dispatch("POST", "/event", {
    body: ciphertext,
    headers: {
      ...sigHeaders,
      [HEADER_ENCRYPTED]: "1",
      [HEADER_SENDER]: encodeBase64Url(device.publicKey),
      [HEADER_NONCE]: encodeBase64Url(nonce),
      "Content-Type": "application/cbor",
    },
  });
  assert.equal(status, 204);
});

// ---------------------------------------------------------------------------
// HTTP round-trip
// ---------------------------------------------------------------------------

test("encrypted HTTP round-trip surfaces verified sender to the handler", async () => {
  const appKp = AppKeypair.generate();
  const device = AppKeypair.generate();
  const app = new App({ name: "wire", keypair: appKp });

  let observedSender: string | null = null;
  app.handler("/event", undefined, (req) => {
    if (req.ctx.senderId) observedSender = toHex(req.ctx.senderId);
    return null;
  });

  const server = http.createServer(async (req, res) => {
    const chunks: Buffer[] = [];
    for await (const c of req) chunks.push(c as Buffer);
    const headers: Record<string, string> = {};
    for (const [k, v] of Object.entries(req.headers)) {
      if (typeof v === "string") headers[k] = v;
    }
    const dispatchOpts: { headers: Record<string, string>; body?: Uint8Array } = { headers };
    if (chunks.length > 0) {
      const b = Buffer.concat(chunks);
      dispatchOpts.body = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    }
    const result = await app.dispatch(req.method ?? "GET", req.url ?? "/", dispatchOpts);
    res.statusCode = result.status;
    for (const [k, v] of Object.entries(result.headers)) res.setHeader(k, v);
    res.end(Buffer.from(result.body));
  });

  await new Promise<void>((r) => server.listen(0, "127.0.0.1", () => r()));
  const addr = server.address();
  if (!addr || typeof addr === "string") {
    server.close();
    throw new Error("bind failed");
  }
  const port = addr.port;

  try {
    const plaintext = encodeFrame({ kind: "event" });
    const nonce = buildNonce(0x02, 42n);
    const ciphertext = device.encryptTo(appKp.publicKey, plaintext, nonce);
    const res = await fetch(`http://127.0.0.1:${port}/event`, {
      method: "POST",
      body: Buffer.from(ciphertext),
      headers: {
        [HEADER_ENCRYPTED]: "1",
        [HEADER_SENDER]: encodeBase64Url(device.publicKey),
        [HEADER_NONCE]: encodeBase64Url(nonce),
        "Content-Type": "application/cbor",
      },
    });
    assert.equal(res.status, 204);
    assert.equal(observedSender, toHex(device.publicKey));
  } finally {
    await new Promise<void>((r) => server.close(() => r()));
  }
});

// ---------------------------------------------------------------------------
// Lint nibbles — sanity for the constants we export
// ---------------------------------------------------------------------------

test("constants are pinned to crypto-suite.md", () => {
  assert.equal(X25519_KEY_LEN, 32);
  assert.equal(AEAD_NONCE_LEN, 12);
});

test("InvalidEncryptionHeader fires on garbage base64 in headers", async () => {
  const app = new App({ name: "guard", keypair: AppKeypair.generate() });
  app.handler("/x", undefined, () => null);
  const { status } = await app.dispatch("POST", "/x", {
    body: new Uint8Array(20),
    headers: {
      [HEADER_ENCRYPTED]: "1",
      [HEADER_SENDER]: "not_b64_url!!@!",
      [HEADER_NONCE]: encodeBase64Url(new Uint8Array(12)),
    },
  });
  assert.equal(status, 400);
  void InvalidEncryptionHeader; // typed import retained for completeness
});
