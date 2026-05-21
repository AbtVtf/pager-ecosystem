// JS-008: extended Ctx fields populated from headers.

import { test } from "node:test";
import { strict as assert } from "node:assert";

import {
  App,
  HEADER_GRANTED,
  HEADER_GROUPS,
  HEADER_LOCATION,
  HEADER_TRANSPORT,
  type Ctx,
} from "../src/index.js";

async function captureCtx(headers: Record<string, string> = {}): Promise<Ctx> {
  const app = new App({ name: "ctx-test" });
  let observed: Ctx | undefined;
  app.screen("/", (req) => {
    observed = req.ctx;
    return { v: 1, id: "scr", body: [] };
  });
  const res = await app.dispatch("GET", "/", { headers });
  assert.equal(res.status, 200);
  if (!observed) throw new Error("handler did not run");
  return observed;
}

test("defaults: transport=wifi, empty granted/groups, null location", async () => {
  const ctx = await captureCtx();
  assert.equal(ctx.transport, "wifi");
  assert.deepEqual(ctx.granted, []);
  assert.deepEqual(ctx.groups, []);
  assert.equal(ctx.location, null);
});

test("PagerOS-Transport: lora is honoured; unknown values fall back to wifi", async () => {
  assert.equal((await captureCtx({ [HEADER_TRANSPORT]: "lora" })).transport, "lora");
  assert.equal((await captureCtx({ [HEADER_TRANSPORT]: "wifi" })).transport, "wifi");
  assert.equal((await captureCtx({ [HEADER_TRANSPORT]: "ble" })).transport, "wifi");
  assert.equal((await captureCtx({ [HEADER_TRANSPORT]: "" })).transport, "wifi");
});

test("PagerOS-Granted splits comma-separated permission names", async () => {
  const ctx = await captureCtx({ [HEADER_GRANTED]: "location, nfc , notifications" });
  assert.deepEqual(ctx.granted, ["location", "nfc", "notifications"]);
});

test("PagerOS-Groups splits comma-separated group ids and drops empties", async () => {
  const ctx = await captureCtx({ [HEADER_GROUPS]: "grp_a,,grp_b" });
  assert.deepEqual(ctx.groups, ["grp_a", "grp_b"]);
});

test("PagerOS-Location: well-formed value parses", async () => {
  const ctx = await captureCtx({ [HEADER_LOCATION]: "45.5,-73.6,1716200000" });
  assert.deepEqual(ctx.location, { lat: 45.5, lon: -73.6, ts: 1716200000 });
});

test("PagerOS-Location: malformed values yield null", async () => {
  assert.equal((await captureCtx({ [HEADER_LOCATION]: "x,y,z" })).location, null);
  assert.equal((await captureCtx({ [HEADER_LOCATION]: "45,73" })).location, null);
  assert.equal((await captureCtx({ [HEADER_LOCATION]: "100,0,1" })).location, null); // lat out of range
  assert.equal((await captureCtx({ [HEADER_LOCATION]: "0,-181,1" })).location, null); // lon out of range
  assert.equal((await captureCtx({ [HEADER_LOCATION]: "0,0,-1" })).location, null); // ts negative
});
