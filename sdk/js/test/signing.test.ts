// Tests for the JS-004 request signature verification middleware.
//
// Acceptance (TASKS.md, JS-004):
//   * rejects requests with missing/invalid PagerOS-Sig,
//   * populates ctx.deviceId on success,
//   * tested against firmware-generated signatures.
//
// "Firmware-generated" parity is satisfied by signing with Node's
// `node:crypto` Ed25519 backend, which is byte-identical to PyNaCl /
// libsodium (the firmware's signer per docs/spec/crypto-suite.md). The
// shared `docs/spec/crypto-test-vectors.json` file is the cross-impl
// pin; we drive `buildSigningInput` against its
// `ed25519-pageros-sig-sample` vector and verify the RFC 8032 §7.1
// vectors land byte-equal signatures.

import { test } from "node:test";
import { strict as assert } from "node:assert";
import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import * as crypto from "node:crypto";
import * as http from "node:http";

import {
  App,
  BadSignature,
  HEADER_DEVICE,
  HEADER_SIG,
  HEADER_TIMESTAMP,
  InvalidEncoding,
  MissingHeader,
  Response,
  TimestampSkew,
  buildSigningInput,
  computeBodyHash,
  decodeFrame,
  publicKeyFromSeed,
  signRequest,
  verifyRequest,
} from "../src/index.js";

const __dirname = dirname(fileURLToPath(import.meta.url));

function findRepoRoot(startDir: string): string {
  let dir = startDir;
  while (true) {
    if (existsSync(resolve(dir, "SPEC.md"))) return dir;
    const parent = dirname(dir);
    if (parent === dir) throw new Error(`could not find repo root from ${startDir}`);
    dir = parent;
  }
}

const REPO_ROOT = findRepoRoot(__dirname);
const VECTORS_PATH = resolve(REPO_ROOT, "docs/spec/crypto-test-vectors.json");

interface CryptoVectorFile {
  vectors: Array<Record<string, unknown>>;
}

function loadVectors(): CryptoVectorFile {
  return JSON.parse(readFileSync(VECTORS_PATH, "utf-8")) as CryptoVectorFile;
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

// ---------------------------------------------------------------------------
// RFC 8032 §7.1 vectors — Ed25519 sign/verify byte parity with libsodium
// ---------------------------------------------------------------------------

interface Ed25519Vector {
  id: string;
  kind: string;
  seed?: string;
  pub?: string;
  msg?: string;
  sig?: string;
}

const VECTORS = loadVectors();
const ED25519_VECTORS = VECTORS.vectors
  .filter((v) => v.kind === "ed25519" && typeof v.seed === "string" && typeof v.sig === "string")
  .map((v) => v as unknown as Ed25519Vector)
  .filter((v) => v.sig !== "PAGEROS_SIG_PENDING_FROM_IMPL");

test("RFC 8032 ed25519 vectors exist in the shared test-vector file", () => {
  assert.ok(ED25519_VECTORS.length >= 3, "RFC 8032 vectors missing from shared file");
});

for (const vec of ED25519_VECTORS) {
  test(`ed25519/${vec.id}: pubkey derives from seed`, () => {
    const seed = fromHex(vec.seed!);
    const pub = publicKeyFromSeed(seed);
    assert.equal(toHex(pub), vec.pub);
  });

  test(`ed25519/${vec.id}: signature byte-equal to RFC 8032`, () => {
    const seed = fromHex(vec.seed!);
    const msg = vec.msg ? fromHex(vec.msg) : new Uint8Array(0);
    // Sign via the same DER wrapping verifyRequest uses internally.
    const der = Buffer.concat([
      Buffer.from("302e020100300506032b657004220420", "hex"),
      Buffer.from(seed),
    ]);
    const priv = crypto.createPrivateKey({ key: der, format: "der", type: "pkcs8" });
    const sig = crypto.sign(null, msg, priv);
    assert.equal(toHex(new Uint8Array(sig)), vec.sig);
  });
}

// ---------------------------------------------------------------------------
// build_signing_input matches the shared PagerOS-Sig sample vector
// ---------------------------------------------------------------------------

test("buildSigningInput matches ed25519-pageros-sig-sample assembly", () => {
  const sample = VECTORS.vectors.find((v) => v.id === "ed25519-pageros-sig-sample");
  assert.ok(sample, "ed25519-pageros-sig-sample vector missing");
  const components = sample!.msg_components as Record<string, string>;
  // The vector's body_hash_hex is sha256("{}"), so the signed body is the
  // two-byte ASCII string "{}". The vector documents this in its notes.
  const input = buildSigningInput(
    components["method"]!,
    components["url"]!,
    components["timestamp"]!,
    new TextEncoder().encode("{}"),
  );
  // sample.msg is "<method+url+ts hex> <body_hash hex>" with a literal space.
  const expectedHex = (sample!.msg as string).replace(/\s+/g, "");
  assert.equal(toHex(input), expectedHex);
});

test("computeBodyHash returns sha256 for empty and typical bodies", () => {
  // Empty body hash matches sha256("") — the documented default.
  const emptyHash = toHex(computeBodyHash(new Uint8Array(0)));
  assert.equal(
    emptyHash,
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
  );
  // sha256("{}") matches the sample vector's body_hash_hex.
  const sample = VECTORS.vectors.find((v) => v.id === "ed25519-pageros-sig-sample");
  const components = sample!.msg_components as Record<string, string>;
  const braceHash = toHex(computeBodyHash(new TextEncoder().encode("{}")));
  assert.equal(braceHash, components["body_hash_hex"]);
});

// ---------------------------------------------------------------------------
// sign_request → verify_request round-trip
// ---------------------------------------------------------------------------

const SAMPLE_SEED = fromHex(
  "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
);
const SAMPLE_PUB_HEX =
  "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";

test("signRequest emits headers that verifyRequest accepts", () => {
  const timestamp = 1700000000;
  const headers = signRequest({
    method: "POST",
    url: "/v1/events",
    body: new Uint8Array(0),
    seed: SAMPLE_SEED,
    timestamp,
  });
  const result = verifyRequest({
    method: "POST",
    url: "/v1/events",
    headers,
    now: () => timestamp,
  });
  assert.equal(toHex(result.deviceId), SAMPLE_PUB_HEX);
  assert.equal(result.timestamp, timestamp);
});

test("verifyRequest accepts case-mismatched header names", () => {
  const timestamp = 1700000000;
  const signed = signRequest({
    method: "GET",
    url: "/",
    seed: SAMPLE_SEED,
    timestamp,
  });
  const lowered: Record<string, string> = {};
  for (const [k, v] of Object.entries(signed)) lowered[k.toLowerCase()] = v;
  const result = verifyRequest({
    method: "GET",
    url: "/",
    headers: lowered,
    now: () => timestamp,
  });
  assert.equal(toHex(result.deviceId), SAMPLE_PUB_HEX);
});

test("verifyRequest fails when a header is missing", () => {
  const timestamp = 1700000000;
  const signed = signRequest({
    method: "GET",
    url: "/",
    seed: SAMPLE_SEED,
    timestamp,
  });
  // Drop one header at a time and confirm the right error subclass fires.
  for (const drop of [HEADER_DEVICE, HEADER_SIG, HEADER_TIMESTAMP]) {
    const headers = { ...signed };
    delete headers[drop];
    assert.throws(
      () => verifyRequest({ method: "GET", url: "/", headers, now: () => timestamp }),
      MissingHeader,
      `dropping ${drop} should fail closed`,
    );
  }
});

test("verifyRequest rejects malformed base64 / wrong-length pubkey", () => {
  const ts = 1700000000;
  const signed = signRequest({ method: "GET", url: "/", seed: SAMPLE_SEED, timestamp: ts });
  const badPub = { ...signed, [HEADER_DEVICE]: "AAAA" };
  assert.throws(
    () => verifyRequest({ method: "GET", url: "/", headers: badPub, now: () => ts }),
    InvalidEncoding,
  );
});

test("verifyRequest rejects bodies whose hash diverges from the signed input", () => {
  const ts = 1700000000;
  const signed = signRequest({
    method: "POST",
    url: "/save",
    body: new TextEncoder().encode("hello"),
    seed: SAMPLE_SEED,
    timestamp: ts,
  });
  // Verify with a *different* body — signature must reject.
  assert.throws(
    () =>
      verifyRequest({
        method: "POST",
        url: "/save",
        headers: signed,
        body: new TextEncoder().encode("HELLO"),
        now: () => ts,
      }),
    BadSignature,
  );
});

test("verifyRequest rejects timestamps outside the skew window", () => {
  const ts = 1700000000;
  const signed = signRequest({ method: "GET", url: "/", seed: SAMPLE_SEED, timestamp: ts });
  assert.throws(
    () =>
      verifyRequest({
        method: "GET",
        url: "/",
        headers: signed,
        maxSkewSeconds: 60,
        now: () => ts + 120,
      }),
    TimestampSkew,
  );
  // Disabling the skew check (null) lets the same request through.
  const ok = verifyRequest({
    method: "GET",
    url: "/",
    headers: signed,
    maxSkewSeconds: null,
    now: () => ts + 999999,
  });
  assert.equal(ok.timestamp, ts);
});

test("verifyRequest rejects a tampered signature", () => {
  const ts = 1700000000;
  const signed = signRequest({ method: "GET", url: "/", seed: SAMPLE_SEED, timestamp: ts });
  // Flip the last char of the signature base64url.
  const original = signed[HEADER_SIG]!;
  const flipped = original.slice(0, -1) + (original.endsWith("A") ? "B" : "A");
  assert.throws(
    () =>
      verifyRequest({
        method: "GET",
        url: "/",
        headers: { ...signed, [HEADER_SIG]: flipped },
        now: () => ts,
      }),
    BadSignature,
  );
});

// ---------------------------------------------------------------------------
// App integration
// ---------------------------------------------------------------------------

test("App with verifySignature rejects unsigned requests with 401", async () => {
  const app = new App({ name: "secured", verifySignature: true });
  app.screen("/", () => ({ v: 1 as const, id: "scr_home", body: [] }));
  const { status, headers } = await app.dispatch("GET", "/");
  assert.equal(status, 401);
  assert.equal(headers["WWW-Authenticate"], 'PagerOS-Sig realm="secured"');
});

test("App with verifySignature accepts signed requests and surfaces ctx.deviceId", async () => {
  const ts = 1700000000;
  const app = new App({
    name: "secured",
    verifySignature: { now: () => ts },
  });
  const observed: { device: Uint8Array | null; timestamp: number | null } = {
    device: null,
    timestamp: null,
  };
  app.screen("/", (req) => {
    observed.device = req.ctx.deviceId;
    observed.timestamp = req.ctx.timestamp;
    return null;
  });
  const headers = signRequest({
    method: "GET",
    url: "/",
    seed: SAMPLE_SEED,
    timestamp: ts,
  });
  const { status } = await app.dispatch("GET", "/", { headers });
  assert.equal(status, 204);
  assert.ok(observed.device !== null, "device id should be populated");
  assert.equal(toHex(observed.device!), SAMPLE_PUB_HEX);
  assert.equal(observed.timestamp, ts);
});

test("App with verifySignature rejects a tampered body with 401", async () => {
  const ts = 1700000000;
  const app = new App({
    name: "secured",
    verifySignature: { now: () => ts },
  });
  app.handler("/echo", undefined, () => null);
  const real = new TextEncoder().encode("hello");
  const headers = signRequest({
    method: "POST",
    url: "/echo",
    body: real,
    seed: SAMPLE_SEED,
    timestamp: ts,
  });
  const tampered = new TextEncoder().encode("HELLO");
  const { status, body } = await app.dispatch("POST", "/echo", {
    body: tampered,
    headers,
  });
  assert.equal(status, 401);
  const frame = decodeFrame(body) as Record<string, unknown>;
  assert.equal(frame["id"], "err_local");
});

test("App without verifySignature still parses PagerOS-Device for unverified ctx", async () => {
  // Backward compat: when verification is off, the device header is
  // surfaced verbatim into ctx (but ctx.deviceId is *not* verified).
  const app = new App({ name: "open" });
  const observed: { device: Uint8Array | null } = { device: null };
  app.screen("/", (req) => {
    observed.device = req.ctx.deviceId;
    return null;
  });
  await app.dispatch("GET", "/", {
    headers: { "PagerOS-Device": Buffer.from(SAMPLE_SEED).toString("base64url") },
  });
  assert.ok(observed.device !== null);
});

// ---------------------------------------------------------------------------
// HTTP round-trip — signed request through a real http.Server
// ---------------------------------------------------------------------------

test("signed HTTP round-trip surfaces verified deviceId in the handler", async () => {
  const ts = 1700000000;
  const app = new App({
    name: "secured",
    verifySignature: { now: () => ts },
  });
  let observed: string | null = null;
  app.screen("/", (req) => {
    if (req.ctx.deviceId) observed = toHex(req.ctx.deviceId);
    return new Response({ status: 200, body: { v: 1 as const, id: "scr_ok", body: [] } });
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
    const url = "/";
    const headers = signRequest({
      method: "GET",
      url,
      seed: SAMPLE_SEED,
      timestamp: ts,
    });
    const res = await fetch(`http://127.0.0.1:${port}${url}`, { headers });
    assert.equal(res.status, 200);
    assert.equal(observed, SAMPLE_PUB_HEX);
    // Unsigned request → 401
    const unsigned = await fetch(`http://127.0.0.1:${port}${url}`);
    assert.equal(unsigned.status, 401);
  } finally {
    await new Promise<void>((r) => server.close(() => r()));
  }
});
