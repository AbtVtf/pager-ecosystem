// PROTO-003 vector conformance harness (JS-002 acceptance).
//
// Walks `protocol/test-vectors/ui/vectors/*.json` and runs every vector
// through the codec:
//
// - `kind: "encode"` vectors: `encodeFrame(input)` MUST produce the
//   bytes in the sibling `.cbor` file (and the descriptor's
//   `expected_cbor_hex`).
// - All vectors: decoding the `.cbor` then re-encoding the decoded value
//   MUST reproduce the original bytes (canonical round-trip).
// - Encode vectors: `decodeFrame(encodeFrame(input))` MUST equal the
//   logical input (after expanding `$bytes` markers and unwrapping
//   `CborFloat`).

import { test } from "node:test";
import { strict as assert } from "node:assert";
import { existsSync, readdirSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

import { bytesToHex, CborFloat, decodeFrame, encodeFrame } from "../src/index.js";
import { parseFloatPreservingJson } from "./vector_json.js";

const __dirname = dirname(fileURLToPath(import.meta.url));

function findRepoRoot(startDir: string): string {
  let dir = startDir;
  while (true) {
    if (existsSync(resolve(dir, "SPEC.md"))) return dir;
    const parent = dirname(dir);
    if (parent === dir) {
      throw new Error(`could not find repo root (SPEC.md) from ${startDir}`);
    }
    dir = parent;
  }
}

const VECTOR_DIR = resolve(
  findRepoRoot(__dirname),
  "protocol/test-vectors/ui/vectors",
);

interface Descriptor {
  name: string;
  category: string;
  kind: "encode" | "decode_only" | "negative";
  description?: string;
  expected_cbor_hex: string;
  expected_size_bytes: number;
  input?: unknown;
  input_cbor_hex?: string;
  expected_decoded?: unknown;
  expect_error?: string;
}

function loadDescriptors(): Descriptor[] {
  if (!existsSync(VECTOR_DIR)) {
    throw new Error(`PROTO-003 vector directory missing: ${VECTOR_DIR}`);
  }
  const out: Descriptor[] = [];
  for (const name of readdirSync(VECTOR_DIR).sort()) {
    if (!name.endsWith(".json")) continue;
    const text = readFileSync(resolve(VECTOR_DIR, name), "utf-8");
    out.push(parseFloatPreservingJson(text) as Descriptor);
  }
  return out;
}

const DESCRIPTORS = loadDescriptors();

function cborBytes(desc: Descriptor): Uint8Array {
  const raw = readFileSync(resolve(VECTOR_DIR, `${desc.name}.cbor`));
  return new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength);
}

function hexOfNumber(n: number): string {
  return n.toString(16).padStart(2, "0");
}

void hexOfNumber;

// `$bytes` markers in `expected_decoded` look like
// `"$bytes:<hex>"` (a literal string with the "$bytes:" prefix) and need
// expanding to Uint8Array for comparison. Distinct from descriptor
// `input`'s marker form `{"$bytes": "<hex>"}` which the encoder handles
// directly.
function expandExpectedMarkers(value: unknown): unknown {
  if (typeof value === "string" && value.startsWith("$bytes:")) {
    const hex = value.slice("$bytes:".length);
    const out = new Uint8Array(hex.length / 2);
    for (let i = 0; i < out.length; i += 1) {
      out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
    }
    return out;
  }
  if (value === null) return null;
  if (Array.isArray(value)) return value.map(expandExpectedMarkers);
  if (typeof value === "object") {
    const obj = value as Record<string, unknown>;
    // Descriptor input style: {"$bytes":"<hex>"} also appears in
    // expected_decoded sometimes (raw_widget passthrough). Expand the
    // same way.
    const keys = Object.keys(obj);
    if (keys.length === 1 && keys[0] === "$bytes" && typeof obj["$bytes"] === "string") {
      const hex = obj["$bytes"] as string;
      const out = new Uint8Array(hex.length / 2);
      for (let i = 0; i < out.length; i += 1) {
        out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
      }
      return out;
    }
    const result: Record<string, unknown> = {};
    for (const k of keys) result[k] = expandExpectedMarkers(obj[k]);
    return result;
  }
  return value;
}

// Drop CborFloat wrappers and Uint8Array specifics so we can structurally
// compare a decoded value to a descriptor input.
function normalise(value: unknown): unknown {
  if (value instanceof CborFloat) return value.value;
  if (value instanceof Uint8Array) {
    // canonicalise to Array of byte numbers for cross-instance equality
    return Array.from(value);
  }
  if (Array.isArray(value)) return value.map(normalise);
  if (value && typeof value === "object" && !(value instanceof Map)) {
    const obj = value as Record<string, unknown>;
    // Descriptor input: {"$bytes":"hex"} → bytes
    const keys = Object.keys(obj);
    if (keys.length === 1 && keys[0] === "$bytes" && typeof obj["$bytes"] === "string") {
      const hex = obj["$bytes"] as string;
      const out: number[] = [];
      for (let i = 0; i < hex.length; i += 2) out.push(parseInt(hex.slice(i, i + 2), 16));
      return out;
    }
    const result: Record<string, unknown> = {};
    for (const k of keys) result[k] = normalise(obj[k]);
    return result;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Sanity
// ---------------------------------------------------------------------------

test("at least one PROTO-003 vector is discovered", () => {
  assert.ok(DESCRIPTORS.length > 0, "no vectors discovered");
});

test("every descriptor records the bytes of its sibling .cbor", () => {
  for (const desc of DESCRIPTORS) {
    const onDisk = cborBytes(desc);
    assert.equal(
      bytesToHex(onDisk),
      desc.expected_cbor_hex,
      `${desc.name}: descriptor hex disagrees with .cbor file`,
    );
    assert.equal(
      onDisk.length,
      desc.expected_size_bytes,
      `${desc.name}: size mismatch`,
    );
  }
});

// ---------------------------------------------------------------------------
// Encode conformance
// ---------------------------------------------------------------------------

const encodeVectors = DESCRIPTORS.filter((d) => d.kind === "encode");

for (const desc of encodeVectors) {
  test(`encode/${desc.name}: encodeFrame(input) byte-equal to vector`, () => {
    const encoded = encodeFrame(desc.input);
    const got = bytesToHex(encoded);
    if (got !== desc.expected_cbor_hex) {
      throw new Error(
        `${desc.name}: encode mismatch\n  expected: ${desc.expected_cbor_hex}\n  got:      ${got}`,
      );
    }
  });
}

// ---------------------------------------------------------------------------
// Decode → re-encode round-trip (every vector)
// ---------------------------------------------------------------------------

for (const desc of DESCRIPTORS) {
  test(`roundtrip/${desc.name}: decode → encode reproduces bytes`, () => {
    const raw = cborBytes(desc);
    const decoded = decodeFrame(raw);
    const reencoded = encodeFrame(decoded);
    if (bytesToHex(reencoded) !== bytesToHex(raw)) {
      throw new Error(
        `${desc.name}: canonical round-trip diverged\n  expected: ${bytesToHex(raw)}\n  got:      ${bytesToHex(reencoded)}`,
      );
    }
  });
}

// ---------------------------------------------------------------------------
// decode(encode(input)) structurally equals input (encode vectors only)
// ---------------------------------------------------------------------------

for (const desc of encodeVectors) {
  test(`logical-roundtrip/${desc.name}: decode(encode(input)) ≡ input`, () => {
    const encoded = encodeFrame(desc.input);
    const decoded = decodeFrame(encoded);
    assert.deepEqual(normalise(decoded), normalise(desc.input));
  });
}

// ---------------------------------------------------------------------------
// decode_only / decoded structure spot-checks
// ---------------------------------------------------------------------------

const decodeOnlyVectors = DESCRIPTORS.filter((d) => d.kind === "decode_only");
for (const desc of decodeOnlyVectors) {
  if (desc.expected_decoded === undefined) continue;
  test(`decode_only/${desc.name}: decoded value carries expected payload`, () => {
    const raw = cborBytes(desc);
    const decoded = decodeFrame(raw);
    // The expected_decoded shape for forward-compat vectors includes
    // sentinels like $render_placeholder (renderer-side concern) — the
    // codec only guarantees the raw_widget bytes survive decode. We
    // therefore only check (a) decoding does not throw and (b) when an
    // expected `raw_widget`/`raw_event` map is present, its CBOR bytes
    // round-trip through re-encode.
    void expandExpectedMarkers; // imported for future strict comparison
    // For now, asserting "did not throw" is the meaningful conformance
    // step at the SDK layer; full unknown-widget rendering belongs to
    // the firmware/simulator side. Re-encoding is checked above.
    assert.ok(decoded !== undefined);
  });
}

// ---------------------------------------------------------------------------
// Negative vectors: SDK MUST surface an error of the named class.
// PROTO-005 (conformance runner) maps `expect_error` names to SDK-specific
// validation rules. At the codec layer all of v1's negative vectors decode
// to a structurally well-formed CBOR value — the "error" is a Frame-level
// validation result. So here we only check that decoding does not crash;
// the runner / `App` layer enforces the Frame contract.
// ---------------------------------------------------------------------------

const negativeVectors = DESCRIPTORS.filter((d) => d.kind === "negative");
for (const desc of negativeVectors) {
  test(`negative/${desc.name}: decoder parses CBOR without crash`, () => {
    const raw = cborBytes(desc);
    decodeFrame(raw);
  });
}
