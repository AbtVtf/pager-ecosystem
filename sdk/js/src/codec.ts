// Canonical CBOR codec for PagerOS Frames (JS-002).
//
// Encoder produces RFC 8949 §4.2 canonical deterministic encoding:
//
// - smallest unsigned-integer head for every length / value,
// - definite-length arrays and maps,
// - map keys sorted lexicographically by their CBOR-encoded byte form
//   (RFC 8949 §4.2.1),
// - 32-bit floats when the value round-trips through binary32; otherwise
//   64-bit (no half-precision on output),
// - no semantic tags (PagerOS does not use them — registry §2.4).
//
// Decoder accepts canonical input plus the relaxations the wire spec
// allows (unordered maps, any minimal-or-not integer head,
// indefinite-length strings/arrays/maps, half-precision floats, semantic
// tags around an inner value).
//
// JS-specific gotchas vs the Python reference:
//
// - JS has a single ``number`` type, so the encoder cannot distinguish
//   ``12`` (uint) from ``12.0`` (float) when both arrive as a plain
//   ``Number``. The :class:`CborFloat` wrapper forces float encoding so
//   round-trips through descriptor JSON / decoded vectors preserve the
//   original major-7 form.
// - ``Number.MAX_SAFE_INTEGER`` is ``2**53 - 1``. The encoder accepts a
//   ``bigint`` up to the CBOR uint64 boundary; the decoder returns a
//   plain ``number`` when the value fits in the safe-integer range and a
//   ``bigint`` otherwise.
// - Byte strings are :class:`Uint8Array`. The encoder also recognises
//   the descriptor convention ``{"$bytes": "<hex>"}`` so vector JSON
//   round-trips through ``encodeFrame`` directly.

export class CborEncodeError extends Error {}
export class CborDecodeError extends Error {}

/**
 * Wrapper that forces float encoding for a JS Number, used to round-trip
 * canonical major-7 floats decoded from CBOR (and to encode descriptor
 * values that were `12.0` in source JSON but indistinguishable from
 * `12` once parsed by `JSON.parse`).
 *
 * The decoder always returns a `CborFloat` for major-7 floats so that
 * `encodeFrame(decodeFrame(b))` is byte-equal to `b` for any canonical
 * `b` this encoder could have produced.
 */
export class CborFloat {
  readonly value: number;
  constructor(value: number) {
    if (typeof value !== "number") {
      throw new CborEncodeError(`CborFloat expects number, got ${typeof value}`);
    }
    this.value = value;
  }
}

// ---------------------------------------------------------------------------
// CBOR major types (RFC 8949 §3.1)
// ---------------------------------------------------------------------------

const MT_UINT = 0;
const MT_NINT = 1;
const MT_BSTR = 2;
const MT_TSTR = 3;
const MT_ARRAY = 4;
const MT_MAP = 5;
const MT_TAG = 6;
const MT_SIMPLE = 7;
const BREAK = 0xff;
const INDEFINITE = -1;
const UINT_MAX = 0x10000000000000000n; // 2**64

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder("utf-8", { fatal: true });

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

export function encodeFrame(value: unknown): Uint8Array {
  const chunks: Uint8Array[] = [];
  encodeValue(value, chunks);
  return concatBytes(chunks);
}

function encodeValue(value: unknown, out: Uint8Array[]): void {
  // Descriptor byte-string marker expansion.
  if (
    value !== null
    && typeof value === "object"
    && !Array.isArray(value)
    && !(value instanceof Uint8Array)
    && !(value instanceof Map)
    && !(value instanceof CborFloat)
  ) {
    const obj = value as Record<string, unknown>;
    const keys = Object.keys(obj);
    if (keys.length === 1 && keys[0] === "$bytes" && typeof obj["$bytes"] === "string") {
      const raw = hexToBytes(obj["$bytes"] as string);
      out.push(head(MT_BSTR, raw.length));
      out.push(raw);
      return;
    }
  }

  if (value === null || value === undefined) {
    out.push(new Uint8Array([0xf6]));
    return;
  }
  if (value === true) {
    out.push(new Uint8Array([0xf5]));
    return;
  }
  if (value === false) {
    out.push(new Uint8Array([0xf4]));
    return;
  }
  if (value instanceof CborFloat) {
    encodeFloat(value.value, out);
    return;
  }
  if (typeof value === "number") {
    if (Number.isInteger(value) && Number.isFinite(value)) {
      encodeInteger(value, out);
    } else {
      encodeFloat(value, out);
    }
    return;
  }
  if (typeof value === "bigint") {
    encodeBigInt(value, out);
    return;
  }
  if (typeof value === "string") {
    const bytes = textEncoder.encode(value);
    out.push(head(MT_TSTR, bytes.length));
    out.push(bytes);
    return;
  }
  if (value instanceof Uint8Array) {
    out.push(head(MT_BSTR, value.length));
    out.push(value);
    return;
  }
  if (Array.isArray(value)) {
    out.push(head(MT_ARRAY, value.length));
    for (const item of value) encodeValue(item, out);
    return;
  }
  if (value instanceof Map) {
    encodeMapEntries(Array.from(value.entries()), out);
    return;
  }
  if (typeof value === "object") {
    encodeMapEntries(Object.entries(value as Record<string, unknown>), out);
    return;
  }
  throw new CborEncodeError(`unsupported value: ${typeof value}`);
}

function encodeInteger(n: number, out: Uint8Array[]): void {
  if (n >= 0) {
    out.push(head(MT_UINT, n));
  } else {
    out.push(head(MT_NINT, -1 - n));
  }
}

function encodeBigInt(n: bigint, out: Uint8Array[]): void {
  if (n >= 0n) {
    if (n >= UINT_MAX) throw new CborEncodeError(`integer out of CBOR range: ${n}`);
    out.push(headBig(MT_UINT, n));
  } else {
    const arg = -1n - n;
    if (arg >= UINT_MAX) {
      throw new CborEncodeError(`negative integer out of CBOR range: ${n}`);
    }
    out.push(headBig(MT_NINT, arg));
  }
}

function encodeFloat(value: number, out: Uint8Array[]): void {
  if (Number.isNaN(value)) {
    // Canonical f32 quiet NaN — matches PY-002 / RFC 8949 §4.2.2 advice.
    out.push(new Uint8Array([0xfa, 0x7f, 0xc0, 0x00, 0x00]));
    return;
  }
  // Try f32; if it round-trips (using bit-equality so -0 vs +0 is
  // preserved per IEEE 754), prefer it.
  const buf32 = new ArrayBuffer(4);
  new DataView(buf32).setFloat32(0, value, false);
  const back32 = new DataView(buf32).getFloat32(0, false);
  if (Object.is(back32, value)) {
    const head32 = new Uint8Array(5);
    head32[0] = 0xfa;
    head32.set(new Uint8Array(buf32), 1);
    out.push(head32);
    return;
  }
  const buf64 = new ArrayBuffer(8);
  new DataView(buf64).setFloat64(0, value, false);
  const head64 = new Uint8Array(9);
  head64[0] = 0xfb;
  head64.set(new Uint8Array(buf64), 1);
  out.push(head64);
}

function encodeMapEntries(
  entries: ReadonlyArray<readonly [unknown, unknown]>,
  out: Uint8Array[],
): void {
  const encoded: Array<[Uint8Array, Uint8Array]> = [];
  for (const [k, v] of entries) {
    const keyChunks: Uint8Array[] = [];
    encodeValue(k, keyChunks);
    const valueChunks: Uint8Array[] = [];
    encodeValue(v, valueChunks);
    encoded.push([concatBytes(keyChunks), concatBytes(valueChunks)]);
  }
  encoded.sort((a, b) => compareBytes(a[0], b[0]));
  for (let i = 1; i < encoded.length; i += 1) {
    if (compareBytes(encoded[i - 1]![0], encoded[i]![0]) === 0) {
      throw new CborEncodeError(
        `duplicate map key in canonical encoding: ${bytesToHex(encoded[i]![0])}`,
      );
    }
  }
  out.push(head(MT_MAP, encoded.length));
  for (const [k, v] of encoded) {
    out.push(k);
    out.push(v);
  }
}

function head(major: number, arg: number): Uint8Array {
  if (!Number.isInteger(arg) || arg < 0) {
    throw new CborEncodeError(`invalid head argument: ${arg}`);
  }
  if (arg <= Number.MAX_SAFE_INTEGER) {
    return headSafeInt(major, arg);
  }
  return headBig(major, BigInt(arg));
}

function headSafeInt(major: number, arg: number): Uint8Array {
  const first = major << 5;
  if (arg < 24) return new Uint8Array([first | arg]);
  if (arg < 0x100) return new Uint8Array([first | 24, arg]);
  if (arg < 0x10000) {
    return new Uint8Array([first | 25, (arg >>> 8) & 0xff, arg & 0xff]);
  }
  if (arg <= 0xffffffff) {
    const buf = new Uint8Array(5);
    buf[0] = first | 26;
    new DataView(buf.buffer).setUint32(1, arg >>> 0, false);
    return buf;
  }
  // 33..53-bit range: use 64-bit head.
  return headBig(major, BigInt(arg));
}

function headBig(major: number, arg: bigint): Uint8Array {
  if (arg < 0n) throw new CborEncodeError(`negative bigint head arg: ${arg}`);
  const first = major << 5;
  if (arg < 24n) return new Uint8Array([first | Number(arg)]);
  if (arg < 0x100n) return new Uint8Array([first | 24, Number(arg)]);
  if (arg < 0x10000n) {
    const n = Number(arg);
    return new Uint8Array([first | 25, (n >>> 8) & 0xff, n & 0xff]);
  }
  if (arg < 0x100000000n) {
    const buf = new Uint8Array(5);
    buf[0] = first | 26;
    new DataView(buf.buffer).setUint32(1, Number(arg), false);
    return buf;
  }
  if (arg < UINT_MAX) {
    const buf = new Uint8Array(9);
    buf[0] = first | 27;
    new DataView(buf.buffer).setBigUint64(1, arg, false);
    return buf;
  }
  throw new CborEncodeError(`head argument exceeds uint64: ${arg}`);
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

export function decodeFrame(bytes: Uint8Array): unknown {
  const ctx: DecodeCtx = { bytes, pos: 0 };
  const value = readValue(ctx);
  if (ctx.pos !== bytes.length) {
    throw new CborDecodeError(
      `trailing bytes after top-level item (consumed ${ctx.pos}/${bytes.length})`,
    );
  }
  return value;
}

interface DecodeCtx {
  bytes: Uint8Array;
  pos: number;
}

interface HeadInfo {
  major: number;
  info: number;
  arg: number | bigint;
}

function readHead(ctx: DecodeCtx): HeadInfo {
  if (ctx.pos >= ctx.bytes.length) {
    throw new CborDecodeError(`unexpected EOF at ${ctx.pos}`);
  }
  const ib = ctx.bytes[ctx.pos]!;
  ctx.pos += 1;
  const major = ib >> 5;
  const info = ib & 0x1f;
  if (info < 24) return { major, info, arg: info };
  if (info === 24) return { major, info, arg: readUintBytes(ctx, 1) };
  if (info === 25) return { major, info, arg: readUintBytes(ctx, 2) };
  if (info === 26) return { major, info, arg: readUintBytes(ctx, 4) };
  if (info === 27) return { major, info, arg: readUintBytes(ctx, 8) };
  if (info === 31) return { major, info, arg: INDEFINITE };
  throw new CborDecodeError(`reserved additional info ${info} at byte ${ctx.pos - 1}`);
}

function readUintBytes(ctx: DecodeCtx, n: number): number | bigint {
  if (ctx.pos + n > ctx.bytes.length) {
    throw new CborDecodeError(`truncated argument (need ${n} bytes)`);
  }
  let big = 0n;
  for (let i = 0; i < n; i += 1) {
    big = (big << 8n) | BigInt(ctx.bytes[ctx.pos + i]!);
  }
  ctx.pos += n;
  return big <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(big) : big;
}

function readValue(ctx: DecodeCtx): unknown {
  const h = readHead(ctx);
  switch (h.major) {
    case MT_UINT:
      if (h.arg === INDEFINITE) {
        throw new CborDecodeError("indefinite length not allowed for integers");
      }
      return h.arg;
    case MT_NINT: {
      if (h.arg === INDEFINITE) {
        throw new CborDecodeError("indefinite length not allowed for integers");
      }
      if (typeof h.arg === "bigint") {
        return -1n - h.arg;
      }
      return -1 - h.arg;
    }
    case MT_BSTR:
      return readByteString(ctx, h.arg, MT_BSTR);
    case MT_TSTR: {
      const raw = readByteString(ctx, h.arg, MT_TSTR);
      try {
        return textDecoder.decode(raw);
      } catch (err) {
        throw new CborDecodeError(`invalid UTF-8 in text string: ${(err as Error).message}`);
      }
    }
    case MT_ARRAY:
      return readArray(ctx, h.arg);
    case MT_MAP:
      return readMap(ctx, h.arg);
    case MT_TAG: {
      if (h.arg === INDEFINITE) {
        throw new CborDecodeError("indefinite length not allowed for tags");
      }
      // Surface the inner value transparently — PagerOS does not use
      // semantic tags (registry §2.4). The tag number itself is dropped.
      return readValue(ctx);
    }
    case MT_SIMPLE:
      return decodeMt7(ctx, h);
    default:
      throw new CborDecodeError(`unknown major type ${h.major}`);
  }
}

function readByteString(
  ctx: DecodeCtx,
  arg: number | bigint,
  expectedMt: number,
): Uint8Array {
  if (arg === INDEFINITE) {
    return readIndefiniteString(ctx, expectedMt);
  }
  const n = toLength(arg);
  if (ctx.pos + n > ctx.bytes.length) {
    throw new CborDecodeError(`truncated byte/text string (need ${n} bytes)`);
  }
  const slice = ctx.bytes.slice(ctx.pos, ctx.pos + n);
  ctx.pos += n;
  return slice;
}

function readIndefiniteString(ctx: DecodeCtx, expectedMt: number): Uint8Array {
  const chunks: Uint8Array[] = [];
  while (true) {
    if (ctx.pos >= ctx.bytes.length) {
      throw new CborDecodeError("unterminated indefinite-length string");
    }
    if (ctx.bytes[ctx.pos] === BREAK) {
      ctx.pos += 1;
      return concatBytes(chunks);
    }
    const h = readHead(ctx);
    if (h.major !== expectedMt || h.arg === INDEFINITE) {
      throw new CborDecodeError("indefinite-length string chunk has wrong shape");
    }
    chunks.push(readByteString(ctx, h.arg, expectedMt));
  }
}

function readArray(ctx: DecodeCtx, arg: number | bigint): unknown[] {
  const out: unknown[] = [];
  if (arg === INDEFINITE) {
    while (true) {
      if (ctx.pos >= ctx.bytes.length) {
        throw new CborDecodeError("unterminated indefinite-length array");
      }
      if (ctx.bytes[ctx.pos] === BREAK) {
        ctx.pos += 1;
        return out;
      }
      out.push(readValue(ctx));
    }
  }
  const n = toLength(arg);
  for (let i = 0; i < n; i += 1) out.push(readValue(ctx));
  return out;
}

function readMap(ctx: DecodeCtx, arg: number | bigint): unknown {
  const stringOnly: Record<string, unknown> = {};
  let composite: Map<unknown, unknown> | null = null;

  const addEntry = (key: unknown, value: unknown): void => {
    if (composite === null && typeof key === "string") {
      if (Object.prototype.hasOwnProperty.call(stringOnly, key)) {
        throw new CborDecodeError(`duplicate map key: ${key}`);
      }
      stringOnly[key] = value;
      return;
    }
    if (composite === null) {
      composite = new Map();
      for (const [k, v] of Object.entries(stringOnly)) composite.set(k, v);
    }
    if (composite.has(key)) {
      throw new CborDecodeError("duplicate map key in CBOR map");
    }
    composite.set(key, value);
  };

  if (arg === INDEFINITE) {
    while (true) {
      if (ctx.pos >= ctx.bytes.length) {
        throw new CborDecodeError("unterminated indefinite-length map");
      }
      if (ctx.bytes[ctx.pos] === BREAK) {
        ctx.pos += 1;
        return composite ?? stringOnly;
      }
      const key = readValue(ctx);
      const value = readValue(ctx);
      addEntry(key, value);
    }
  }
  const n = toLength(arg);
  for (let i = 0; i < n; i += 1) {
    const key = readValue(ctx);
    const value = readValue(ctx);
    addEntry(key, value);
  }
  return composite ?? stringOnly;
}

function decodeMt7(ctx: DecodeCtx, h: HeadInfo): unknown {
  const { info, arg } = h;
  if (info < 24) {
    return decodeSimple(info);
  }
  if (info === 24) {
    if (typeof arg !== "number") {
      throw new CborDecodeError(`mt7 extended simple value out of range: ${arg}`);
    }
    if (arg < 32) {
      throw new CborDecodeError(`invalid simple value ${arg}`);
    }
    throw new CborDecodeError(`unsupported simple value ${arg}`);
  }
  if (info === 25) {
    if (typeof arg !== "number") {
      throw new CborDecodeError(`mt7 half-float arg out of range`);
    }
    return new CborFloat(halfToFloat(arg));
  }
  if (info === 26) {
    if (typeof arg !== "number" && typeof arg !== "bigint") {
      throw new CborDecodeError("mt7 single-float arg missing");
    }
    const bits = typeof arg === "bigint" ? Number(arg) : arg;
    const buf = new ArrayBuffer(4);
    new DataView(buf).setUint32(0, bits >>> 0, false);
    return new CborFloat(new DataView(buf).getFloat32(0, false));
  }
  if (info === 27) {
    const bits = typeof arg === "bigint" ? arg : BigInt(arg as number);
    const buf = new ArrayBuffer(8);
    new DataView(buf).setBigUint64(0, bits, false);
    return new CborFloat(new DataView(buf).getFloat64(0, false));
  }
  if (info === 31) {
    throw new CborDecodeError("unexpected break stop code outside indefinite-length item");
  }
  void ctx;
  throw new CborDecodeError(`reserved mt7 info ${info}`);
}

function decodeSimple(value: number): unknown {
  if (value === 20) return false;
  if (value === 21) return true;
  if (value === 22) return null;
  if (value === 23) return null; // undefined → null (spec §3.1 advice)
  throw new CborDecodeError(`unsupported simple value ${value}`);
}

function halfToFloat(half: number): number {
  const sign = (half >> 15) & 0x1;
  const exp = (half >> 10) & 0x1f;
  const mant = half & 0x3ff;
  let val: number;
  if (exp === 0) {
    val = Math.pow(2, -24) * mant;
  } else if (exp === 31) {
    if (mant === 0) val = Infinity;
    else return Number.NaN;
  } else {
    val = Math.pow(2, exp - 25) * (mant + 1024);
  }
  return sign ? -val : val;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

function toLength(arg: number | bigint): number {
  if (typeof arg === "bigint") {
    if (arg > BigInt(Number.MAX_SAFE_INTEGER)) {
      throw new CborDecodeError("length argument exceeds safe integer range");
    }
    return Number(arg);
  }
  if (arg < 0) throw new CborDecodeError(`unexpected length sentinel ${arg}`);
  return arg;
}

function concatBytes(chunks: Uint8Array[]): Uint8Array {
  let total = 0;
  for (const c of chunks) total += c.length;
  const out = new Uint8Array(total);
  let offset = 0;
  for (const c of chunks) {
    out.set(c, offset);
    offset += c.length;
  }
  return out;
}

function compareBytes(a: Uint8Array, b: Uint8Array): number {
  // RFC 8949 §4.2.1: lex compare on the encoded byte sequence.
  // For canonical heads with smallest-encoding, shorter encodings come
  // first naturally (because the head's first byte is smaller when the
  // length is smaller), so straight bytewise compare is the right rule.
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i += 1) {
    const d = a[i]! - b[i]!;
    if (d !== 0) return d;
  }
  return a.length - b.length;
}

function hexToBytes(hex: string): Uint8Array {
  if (hex.length % 2 !== 0) {
    throw new CborEncodeError(`invalid $bytes hex (odd length): ${hex}`);
  }
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i += 1) {
    const byte = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
    if (Number.isNaN(byte)) {
      throw new CborEncodeError(`invalid $bytes hex: ${hex}`);
    }
    out[i] = byte;
  }
  return out;
}

export function bytesToHex(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i += 1) {
    s += bytes[i]!.toString(16).padStart(2, "0");
  }
  return s;
}
