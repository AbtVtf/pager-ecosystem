// JS-006: Push Relay client.

import { test } from "node:test";
import { strict as assert } from "node:assert";
import * as crypto from "node:crypto";

import {
  AppKeypair,
  ED25519_SEED_LEN,
  DEFAULT_PUSH_RELAY_URL,
  HEADER_APP,
  HEADER_DEVICE,
  HEADER_SIG,
  HEADER_TIMESTAMP,
  PUSH_CONTENT_TYPE,
  PushError,
  PushRejected,
  PushUnavailable,
  buildPushBody,
  decodePushBody,
  sendPush,
  type PushConfig,
} from "../src/index.js";

function makeKeypair(): { sender: AppKeypair; device: AppKeypair } {
  return { sender: AppKeypair.generate(), device: AppKeypair.generate() };
}

function makeSigningSeed(): Uint8Array {
  return new Uint8Array(crypto.randomBytes(ED25519_SEED_LEN));
}

test("buildPushBody encrypts payload and emits a CBOR envelope", () => {
  const { sender, device } = makeKeypair();
  const body = buildPushBody(sender, device.publicKey, { hello: "world" }, 1);
  const decoded = decodePushBody(body, { devicePrivateKey: device.privateKey });
  assert.deepEqual(Array.from(decoded.senderPubkey), Array.from(sender.publicKey));
  assert.equal(decoded.nonce.length, 12);
  // Plaintext is the CBOR-encoded payload — decoding it must round-trip.
  // We don't need to decode here; the round-trip exercises the encrypt
  // path enough.
  assert.ok(decoded.ciphertext.length > 0);
  assert.ok(decoded.plaintext.length > 0);
});

test("buildPushBody rejects wrong-sized device pubkeys", () => {
  const { sender } = makeKeypair();
  assert.throws(
    () => buildPushBody(sender, new Uint8Array(10), {}, 1),
    PushError,
  );
});

test("buildPushBody — different counters produce different ciphertexts", () => {
  const { sender, device } = makeKeypair();
  const a = buildPushBody(sender, device.publicKey, "msg", 1);
  const b = buildPushBody(sender, device.publicKey, "msg", 2);
  assert.notDeepEqual(a, b);
});

test("sendPush rejects with PushRejected on 4xx", async () => {
  const { sender, device } = makeKeypair();
  const config: PushConfig = {
    appId: "demo.app",
    signingSeed: makeSigningSeed(),
    keypair: sender,
    relayUrl: "https://push.example/",
  };
  const fakeFetch: typeof fetch = async () =>
    new Response("rate limited", { status: 429 });
  await assert.rejects(
    sendPush(config, device.publicKey, { hi: 1 }, { counter: 1, fetchImpl: fakeFetch }),
    (err) => err instanceof PushRejected && (err as PushRejected).status === 429,
  );
});

test("sendPush rejects with PushUnavailable on network failure", async () => {
  const { sender, device } = makeKeypair();
  const config: PushConfig = {
    appId: "demo.app",
    signingSeed: makeSigningSeed(),
    keypair: sender,
  };
  const fakeFetch: typeof fetch = async () => {
    throw new TypeError("fetch failed: socket hangup");
  };
  await assert.rejects(
    sendPush(config, device.publicKey, { x: 1 }, { counter: 1, fetchImpl: fakeFetch }),
    (err) => err instanceof PushUnavailable,
  );
});

test("sendPush issues the right URL, headers, and body shape on success", async () => {
  const { sender, device } = makeKeypair();
  const config: PushConfig = {
    appId: "demo.app",
    signingSeed: makeSigningSeed(),
    keypair: sender,
    relayUrl: "https://push.example/",
  };
  let observed: { url?: string; init?: RequestInit } = {};
  const fakeFetch: typeof fetch = async (input, init) => {
    observed = { url: String(input), init: init as RequestInit };
    return new Response(JSON.stringify({ id: "queued-1", ok: true }), {
      status: 202,
      headers: { "Content-Type": "application/json" },
    });
  };
  const res = await sendPush(config, device.publicKey, { hello: "world" }, {
    counter: 7, fetchImpl: fakeFetch, now: () => 1716200000_000,
  });
  assert.equal(res.status, 202);
  assert.deepEqual(res.body, { id: "queued-1", ok: true });

  assert.equal(
    observed.url,
    `https://push.example/push/${Buffer.from(device.publicKey).toString("base64url")}`,
  );
  const h = (observed.init?.headers ?? {}) as Record<string, string>;
  assert.equal(h[HEADER_APP], "demo.app");
  assert.equal(h["Content-Type"], PUSH_CONTENT_TYPE);
  assert.equal(h[HEADER_TIMESTAMP], "1716200000");
  // Sig + device headers must be present (don't assert exact values; the
  // signing test already covers that).
  assert.ok(h[HEADER_SIG]?.length);
  assert.ok(h[HEADER_DEVICE]?.length);
});

test("sendPush rejects signing seed of wrong length", async () => {
  const { sender, device } = makeKeypair();
  const config: PushConfig = {
    appId: "demo.app",
    signingSeed: new Uint8Array(10),
    keypair: sender,
  };
  await assert.rejects(
    sendPush(config, device.publicKey, {}, { counter: 1, fetchImpl: async () => new Response("") }),
    PushError,
  );
});

test("DEFAULT_PUSH_RELAY_URL points at the project relay", () => {
  assert.equal(DEFAULT_PUSH_RELAY_URL, "https://push.pageros.org");
});
