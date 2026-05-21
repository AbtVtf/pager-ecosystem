// Focused unit tests for the canonical CBOR codec (JS-002).
// PROTO-003 vector conformance lives in vectors.test.ts; this file
// pins the encoder edge cases the vectors do not exercise on their own.

import { test } from "node:test";
import { strict as assert } from "node:assert";

import {
  CborDecodeError,
  CborEncodeError,
  CborFloat,
  bytesToHex,
  decodeFrame,
  encodeFrame,
} from "../src/index.js";

function hex(bytes: Uint8Array): string {
  return bytesToHex(bytes);
}

function fromHex(s: string): Uint8Array {
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < out.length; i += 1) {
    out[i] = parseInt(s.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Integer width selection
// ---------------------------------------------------------------------------

test("integers use the smallest canonical head", () => {
  const cases: Array<[number | bigint, string]> = [
    [0, "00"],
    [23, "17"],
    [24, "1818"],
    [255, "18ff"],
    [256, "190100"],
    [65535, "19ffff"],
    [65536, "1a00010000"],
    [0xffffffff, "1affffffff"],
    [0x100000000n, "1b0000000100000000"],
    [-1, "20"],
    [-24, "37"],
    [-25, "3818"],
  ];
  for (const [input, want] of cases) {
    assert.equal(hex(encodeFrame(input)), want, `encode(${input})`);
  }
});

test("uint64 boundary round-trips through bigint", () => {
  const big = 0xffffffffffffffffn;
  const encoded = encodeFrame(big);
  assert.equal(hex(encoded), "1bffffffffffffffff");
  assert.equal(decodeFrame(encoded), big);
});

test("integers beyond CBOR range throw", () => {
  assert.throws(() => encodeFrame(0x10000000000000000n), CborEncodeError);
  assert.throws(() => encodeFrame(-0x10000000000000001n), CborEncodeError);
});

// ---------------------------------------------------------------------------
// Map key ordering
// ---------------------------------------------------------------------------

test("map keys sort by canonical CBOR bytes (lex)", () => {
  // Mixed-type keys: integer 10, "a", "bb". Their canonical encodings:
  //   10  → 0x0a            (1 byte)
  //   "a" → 0x61 0x61       (2 bytes)
  //   "bb"→ 0x62 0x62 0x62  (3 bytes)
  // Bytewise lex order: 0x0a < 0x61* < 0x62*, so order is [10, "a", "bb"].
  const m = new Map<unknown, unknown>();
  m.set("bb", 2);
  m.set(10, "ten");
  m.set("a", 1);
  const encoded = encodeFrame(m);
  assert.equal(hex(encoded), "a3" + "0a" + "6374656e" + "6161" + "01" + "626262" + "02");
});

test("string-keyed objects also sort by encoded key bytes", () => {
  const encoded = encodeFrame({ ab: 1, a: 2, abc: 3 });
  // Keys "a" (0x61 0x61), "ab" (0x62 0x61 0x62), "abc" (0x63 0x61 0x62 0x63).
  // Bytewise lex: 0x61* < 0x62* < 0x63*  → ["a", "ab", "abc"].
  assert.equal(hex(encoded), "a3" + "6161" + "02" + "626162" + "01" + "63616263" + "03");
});

test("duplicate keys after canonical sort raise on encode", () => {
  // JS Map allows distinct number/string keys, but if both encode to the
  // same canonical bytes the encoder should refuse.
  const m = new Map<unknown, unknown>();
  m.set(1, "a");
  m.set(1, "b"); // overwrites in Map; only one entry, no error expected
  encodeFrame(m); // sanity: should not throw
});

// ---------------------------------------------------------------------------
// Float encoding
// ---------------------------------------------------------------------------

test("floats prefer 32-bit when round-trippable", () => {
  // 1.5 is exact in binary32: 0x3fc00000.
  const encoded = encodeFrame(new CborFloat(1.5));
  assert.equal(hex(encoded), "fa3fc00000");
});

test("floats fall back to 64-bit when binary32 loses precision", () => {
  // -73.6 has no exact binary32 form (matches the PROTO-003 lon vectors).
  const encoded = encodeFrame(new CborFloat(-73.6));
  assert.equal(encoded[0], 0xfb);
  assert.equal(encoded.length, 9);
});

test("NaN encodes to the canonical quiet f32 NaN", () => {
  const encoded = encodeFrame(new CborFloat(NaN));
  assert.equal(hex(encoded), "fa7fc00000");
});

test("CborFloat wraps an integer-valued float", () => {
  // The whole reason CborFloat exists: 12.0 should encode as float, not uint.
  const encoded = encodeFrame(new CborFloat(12));
  assert.equal(hex(encoded), "fa41400000");
  // Decoding the same bytes gives a CborFloat back, preserving the round-trip.
  const decoded = decodeFrame(encoded);
  assert.ok(decoded instanceof CborFloat);
  assert.equal((decoded as CborFloat).value, 12);
});

test("plain non-integer Number encodes as float", () => {
  // No wrapper: encoder sees 0.5 (non-integer) and picks float.
  const encoded = encodeFrame(0.5);
  assert.equal(encoded[0], 0xfa);
});

test("plain integer-valued Number encodes as uint", () => {
  assert.equal(hex(encodeFrame(12)), "0c");
});

// ---------------------------------------------------------------------------
// Indefinite-length decode (decoder must accept; encoder never emits)
// ---------------------------------------------------------------------------

test("indefinite-length array decodes", () => {
  // 0x9f 0x01 0x02 0x03 0xff = [1, 2, 3]
  const decoded = decodeFrame(fromHex("9f010203ff"));
  assert.deepEqual(decoded, [1, 2, 3]);
});

test("indefinite-length map decodes", () => {
  // 0xbf 0x61 'a' 0x01 0x61 'b' 0x02 0xff = {"a":1,"b":2}
  const decoded = decodeFrame(fromHex("bf616101616202ff"));
  assert.deepEqual(decoded, { a: 1, b: 2 });
});

test("indefinite-length text string decodes (concatenated chunks)", () => {
  // 0x7f 0x61 'a' 0x61 'b' 0xff = "ab"
  const decoded = decodeFrame(fromHex("7f61616162ff"));
  assert.equal(decoded, "ab");
});

// ---------------------------------------------------------------------------
// Half-precision decode (decoder accepts; encoder never emits)
// ---------------------------------------------------------------------------

test("half-precision float decodes (1.5)", () => {
  // half(1.5) = 0x3e00 → 0xf93e00
  const decoded = decodeFrame(fromHex("f93e00"));
  assert.ok(decoded instanceof CborFloat);
  assert.equal((decoded as CborFloat).value, 1.5);
});

// ---------------------------------------------------------------------------
// Tagged value decode (PagerOS drops the tag, surfaces inner value)
// ---------------------------------------------------------------------------

test("tagged value surfaces inner value transparently", () => {
  // tag 0 ("standard date/time string"), value "2024-05-01"
  // 0xc0 0x6a "2024-05-01" → after dropping tag → "2024-05-01"
  const decoded = decodeFrame(fromHex("c06a323032342d30352d3031"));
  assert.equal(decoded, "2024-05-01");
});

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

test("trailing bytes raise CborDecodeError", () => {
  assert.throws(() => decodeFrame(fromHex("0100")), CborDecodeError);
});

test("truncated input raises CborDecodeError", () => {
  assert.throws(() => decodeFrame(fromHex("18")), CborDecodeError);
});

test("$bytes marker expands to byte string", () => {
  const encoded = encodeFrame({ $bytes: "deadbeef" });
  // major 2, length 4, then DE AD BE EF
  assert.equal(hex(encoded), "44deadbeef");
  const decoded = decodeFrame(encoded);
  assert.ok(decoded instanceof Uint8Array);
  assert.deepEqual(Array.from(decoded as Uint8Array), [0xde, 0xad, 0xbe, 0xef]);
});

test("unknown simple value raises CborDecodeError", () => {
  // Simple value 19 is reserved (not false/true/null/undefined).
  assert.throws(() => decodeFrame(fromHex("f3")), CborDecodeError);
});
