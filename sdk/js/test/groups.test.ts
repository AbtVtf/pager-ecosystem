// JS-007: Group helpers.

import { test } from "node:test";
import { strict as assert } from "node:assert";
import * as crypto from "node:crypto";

import {
  AppKeypair,
  ED25519_SEED_LEN,
  GROUP_PUSH_PATH,
  GROUP_PUSH_CONTENT_TYPE,
  GROUP_RESULT_ACCEPTED,
  GroupBroadcastError,
  buildGroupPushBody,
  sendGroupPush,
  type PushConfig,
} from "../src/index.js";

function recipients(n: number): Array<[string, Uint8Array]> {
  const out: Array<[string, Uint8Array]> = [];
  for (let i = 0; i < n; i += 1) {
    const kp = AppKeypair.generate();
    out.push([Buffer.from(kp.publicKey).toString("base64url"), kp.publicKey]);
  }
  return out;
}

test("buildGroupPushBody emits one ciphertext per recipient", () => {
  const sender = AppKeypair.generate();
  const recs = recipients(3);
  const body = buildGroupPushBody(sender, recs, { kind: "group_message", body: "hi" });
  const parsed = JSON.parse(new TextDecoder().decode(body)) as {
    from: string; recipients: Array<{ device: string; nonce: string; ct: string }>;
  };
  assert.equal(parsed.from, Buffer.from(sender.publicKey).toString("base64url"));
  assert.equal(parsed.recipients.length, 3);
  const nonces = new Set(parsed.recipients.map((r) => r.nonce));
  // Per-recipient nonces are derived from a counter, so they must all differ.
  assert.equal(nonces.size, 3);
});

test("buildGroupPushBody rejects empty recipients list", () => {
  const sender = AppKeypair.generate();
  assert.throws(
    () => buildGroupPushBody(sender, [], {}),
    GroupBroadcastError,
  );
});

test("buildGroupPushBody rejects wrong-sized pubkey", () => {
  const sender = AppKeypair.generate();
  const bad: Array<[string, Uint8Array]> = [["abc", new Uint8Array(10)]];
  assert.throws(
    () => buildGroupPushBody(sender, bad, {}),
    GroupBroadcastError,
  );
});

test("sendGroupPush sends to GROUP_PUSH_PATH with the right content-type", async () => {
  const sender = AppKeypair.generate();
  const config: PushConfig = {
    appId: "chat.app",
    signingSeed: new Uint8Array(crypto.randomBytes(ED25519_SEED_LEN)),
    keypair: sender,
    relayUrl: "https://push.example/",
  };
  let observed: { url?: string; method?: string; headers?: Record<string, string> } = {};
  const fakeFetch: typeof fetch = async (input, init) => {
    observed = {
      url: String(input),
      method: init?.method ?? "GET",
      headers: (init?.headers ?? {}) as Record<string, string>,
    };
    return new Response(JSON.stringify({
      accepted: 2,
      rejected: 0,
      results: [
        { device: "aaa", result: GROUP_RESULT_ACCEPTED },
        { device: "bbb", result: GROUP_RESULT_ACCEPTED },
      ],
    }), { status: 202, headers: { "Content-Type": "application/json" } });
  };
  const res = await sendGroupPush(config, recipients(2), { kind: "group_message" }, {
    fetchImpl: fakeFetch,
  });
  assert.equal(observed.method, "POST");
  assert.equal(observed.url, "https://push.example" + GROUP_PUSH_PATH);
  assert.equal(observed.headers?.["Content-Type"], GROUP_PUSH_CONTENT_TYPE);
  assert.equal(res.status, 202);
  assert.equal(res.accepted, 2);
  assert.equal(res.rejected, 0);
  assert.equal(res.results.length, 2);
  assert.equal(res.results[0]!.result, GROUP_RESULT_ACCEPTED);
});

test("sendGroupPush surfaces relay 4xx as PushRejected via thrown error", async () => {
  const sender = AppKeypair.generate();
  const config: PushConfig = {
    appId: "chat.app",
    signingSeed: new Uint8Array(crypto.randomBytes(ED25519_SEED_LEN)),
    keypair: sender,
  };
  const fakeFetch: typeof fetch = async () => new Response("nope", { status: 401 });
  await assert.rejects(
    sendGroupPush(config, recipients(1), {}, { fetchImpl: fakeFetch }),
  );
});
