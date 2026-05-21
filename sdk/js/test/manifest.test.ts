// JS-010: AppManifest generator.

import { test } from "node:test";
import { strict as assert } from "node:assert";

import { AppManifest } from "../src/index.js";

const BASE = {
  id: "notes.mafu.dev",
  name: "Notes",
  description: "A simple notepad.",
  icon: "https://example.com/notes/icon.png",
  url: "https://example.com/notes/",
  maintainer: { name: "Jane", contact: "jane@example.com" },
};

test("minimal manifest serialises with all required fields", () => {
  const m = new AppManifest(BASE);
  const obj = m.manifestObject();
  assert.equal(obj.id, "notes.mafu.dev");
  assert.equal(obj.name, "Notes");
  assert.equal(obj.url, "https://example.com/notes/");
  assert.equal(obj.version, 1);
  assert.deepEqual(obj.maintainer, BASE.maintainer);
});

test("default-off optional fields are omitted from the object output", () => {
  const obj = new AppManifest(BASE).manifestObject();
  assert.ok(!("lora_compatible" in obj));
  assert.ok(!("multi_device" in obj));
  assert.ok(!("donate_url" in obj));
  assert.ok(!("pubkey" in obj));
  assert.ok(!("permissions" in obj));
  // categories defaults to empty list → omitted to match Python canonical shape
  assert.ok(!("categories" in obj));
});

test("opt-in fields are included when set", () => {
  const obj = new AppManifest({
    ...BASE,
    loraCompatible: true,
    multiDevice: true,
    donateUrl: "https://give.example/notes",
    pubkey: "deadbeef",
    permissions: ["location", "nfc"],
    categories: ["productivity", "notes"],
  }).manifestObject();
  assert.equal(obj.lora_compatible, true);
  assert.equal(obj.multi_device, true);
  assert.equal(obj.donate_url, "https://give.example/notes");
  assert.equal(obj.pubkey, "deadbeef");
  assert.deepEqual(obj.permissions, ["location", "nfc"]);
  assert.deepEqual(obj.categories, ["productivity", "notes"]);
});

test("rejects required fields when missing", () => {
  assert.throws(() => new AppManifest({ ...BASE, id: "" }), RangeError);
  assert.throws(() => new AppManifest({ ...BASE, name: "" }), RangeError);
  assert.throws(() => new AppManifest({ ...BASE, url: "" }), RangeError);
  assert.throws(
    () => new AppManifest({ ...BASE, maintainer: { name: "", contact: "x" } }),
    RangeError,
  );
});

test("rejects non-positive version", () => {
  assert.throws(() => new AppManifest({ ...BASE, version: 0 }), RangeError);
  assert.throws(() => new AppManifest({ ...BASE, version: -1 }), RangeError);
  assert.throws(() => new AppManifest({ ...BASE, version: 1.5 }), RangeError);
});

test("YAML output has expected shape", () => {
  const yaml = new AppManifest({
    ...BASE,
    categories: ["productivity"],
    loraCompatible: true,
  }).manifestYaml();
  assert.match(yaml, /^id: notes\.mafu\.dev$/m);
  assert.match(yaml, /^name: Notes$/m);
  assert.match(yaml, /^url: "?https:\/\/example\.com\/notes\/"?$/m);
  assert.match(yaml, /^lora_compatible: true$/m);
  assert.match(yaml, /^categories: \[productivity\]$/m);
  assert.match(yaml, /^maintainer:$/m);
  assert.match(yaml, /^  name: Jane$/m);
  assert.match(yaml, /^version: 1$/m);
});

test("YAML quotes strings that look like other scalars", () => {
  const yaml = new AppManifest({
    ...BASE,
    name: "true", // would be a YAML bool unquoted
  }).manifestYaml();
  assert.match(yaml, /^name: "true"$/m);
});
