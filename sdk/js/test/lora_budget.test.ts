// JS-011: LoRa size budget warning.

import { test } from "node:test";
import { strict as assert } from "node:assert";

import {
  LORA_FRAME_BUDGET_BYTES,
  checkFrameSize,
} from "../src/index.js";

test("LORA_FRAME_BUDGET_BYTES matches SPEC §1 (200)", () => {
  assert.equal(LORA_FRAME_BUDGET_BYTES, 200);
});

test("no warning when loraCompatible is false (any size)", () => {
  const logs: string[] = [];
  const out = checkFrameSize(new Uint8Array(10_000), {
    loraCompatible: false,
    logger: (m) => logs.push(m),
  });
  assert.equal(out, false);
  assert.equal(logs.length, 0);
});

test("no warning when size is under the budget", () => {
  const logs: string[] = [];
  const out = checkFrameSize(new Uint8Array(LORA_FRAME_BUDGET_BYTES), {
    loraCompatible: true,
    logger: (m) => logs.push(m),
  });
  assert.equal(out, false);
  assert.equal(logs.length, 0);
});

test("warns when size exceeds the budget", () => {
  const logs: string[] = [];
  const out = checkFrameSize(new Uint8Array(LORA_FRAME_BUDGET_BYTES + 1), {
    loraCompatible: true,
    logger: (m) => logs.push(m),
  });
  assert.equal(out, true);
  assert.equal(logs.length, 1);
  assert.match(logs[0]!, /exceeds the LoRa budget/);
  assert.match(logs[0]!, /by 1 bytes/);
});

test("frameLabel is included in the warning text", () => {
  const logs: string[] = [];
  checkFrameSize(LORA_FRAME_BUDGET_BYTES + 50, {
    loraCompatible: true,
    frameLabel: "/notes",
    logger: (m) => logs.push(m),
  });
  assert.equal(logs.length, 1);
  assert.match(logs[0]!, /\(\/notes\)/);
});

test("accepts a numeric size in place of bytes", () => {
  const logs: string[] = [];
  const out = checkFrameSize(LORA_FRAME_BUDGET_BYTES + 5, {
    loraCompatible: true,
    logger: (m) => logs.push(m),
  });
  assert.equal(out, true);
  assert.equal(logs.length, 1);
});

test("negative size throws", () => {
  assert.throws(
    () => checkFrameSize(-1, { loraCompatible: true }),
    RangeError,
  );
});
