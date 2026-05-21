// Minimal CBOR encode/decode for PagerOS Frames (JS-001 stub).
//
// Full PROTO-003 vector conformance is JS-002's scope. This module only
// covers the value subset the routing layer needs to round-trip a
// hello-world Frame and parse a CBOR request body: positive integers
// (major 0), text strings (major 3), arrays (major 4), maps (major 5),
// and the small simple values false/true/null/undefined (major 7).
//
// Output is canonical per RFC 8949 §4.2.1:
//  - smallest length encoding for every header,
//  - map keys sorted by encoded byte sequence (length-then-lexicographic).
//
// The decoder is permissive (it accepts non-canonical input) so request
// bodies from a strict-encoder peer still parse. JS-002 will replace
// this file with a vector-driven implementation.

export class CborDecodeError extends Error {}
export class CborEncodeError extends Error {}

type CborValue =
  | null
  | undefined
  | boolean
  | number
  | bigint
  | string
  | Uint8Array
  | CborValue[]
  | { [k: string]: CborValue }
  | Map<CborValue, CborValue>;

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder("utf-8", { fatal: true });

// ---------------------------------------------------------------------------
// encode
// ---------------------------------------------------------------------------

export function encodeFrame(value: unknown): Uint8Array {
  const chunks: Uint8Array[] = [];
  encodeValue(value as CborValue, chunks);
  return concatBytes(chunks);
}

function encodeValue(value: CborValue, out: Uint8Array[]): void {
  if (value === null) {
    out.push(new Uint8Array([0xf6]));
    return;
  }
  if (value === undefined) {
    out.push(new Uint8Array([0xf7]));
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
  if (typeof value === "number") {
    encodeNumber(value, out);
    return;
  }
  if (typeof value === "bigint") {
    encodeBigInt(value, out);
    return;
  }
  if (typeof value === "string") {
    const bytes = textEncoder.encode(value);
    out.push(typeHeader(3, bytes.length));
    out.push(bytes);
    return;
  }
  if (value instanceof Uint8Array) {
    out.push(typeHeader(2, value.length));
    out.push(value);
    return;
  }
  if (Array.isArray(value)) {
    out.push(typeHeader(4, value.length));
    for (const item of value) encodeValue(item, out);
    return;
  }
  if (value instanceof Map) {
    encodeMap(value, out);
    return;
  }
  if (typeof value === "object") {
    encodePlainObject(value as { [k: string]: CborValue }, out);
    return;
  }
  throw new CborEncodeError(`unsupported value: ${typeof value}`);
}

function encodeNumber(value: number, out: Uint8Array[]): void {
  if (!Number.isFinite(value)) {
    throw new CborEncodeError(`non-finite numbers not supported in JS-001 stub`);
  }
  if (Number.isInteger(value)) {
    if (value >= 0) {
      out.push(typeHeader(0, value));
    } else {
      // Major type 1: argument = -1 - n
      out.push(typeHeader(1, -1 - value));
    }
    return;
  }
  // Float fallback: encode as float64 (major 7, 27). Full encoder will
  // pick the smallest float per canonical rules; the hello-world path
  // doesn't exercise floats, so we keep this simple for now.
  const buf = new ArrayBuffer(9);
  const view = new DataView(buf);
  view.setUint8(0, 0xfb);
  view.setFloat64(1, value, false);
  out.push(new Uint8Array(buf));
}

function encodeBigInt(value: bigint, out: Uint8Array[]): void {
  if (value >= 0n) {
    if (value > 0xffffffffffffffffn) {
      throw new CborEncodeError("bigint exceeds uint64 range");
    }
    out.push(typeHeaderBig(0, value));
  } else {
    const arg = -1n - value;
    if (arg > 0xffffffffffffffffn) {
      throw new CborEncodeError("bigint exceeds int64 range");
    }
    out.push(typeHeaderBig(1, arg));
  }
}

function encodePlainObject(
  obj: { [k: string]: CborValue },
  out: Uint8Array[],
): void {
  const entries: Array<[Uint8Array, Uint8Array]> = [];
  for (const key of Object.keys(obj)) {
    const keyChunks: Uint8Array[] = [];
    encodeValue(key, keyChunks);
    const valueChunks: Uint8Array[] = [];
    encodeValue(obj[key] as CborValue, valueChunks);
    entries.push([concatBytes(keyChunks), concatBytes(valueChunks)]);
  }
  entries.sort((a, b) => compareBytes(a[0], b[0]));
  out.push(typeHeader(5, entries.length));
  for (const [k, v] of entries) {
    out.push(k);
    out.push(v);
  }
}

function encodeMap(map: Map<CborValue, CborValue>, out: Uint8Array[]): void {
  const entries: Array<[Uint8Array, Uint8Array]> = [];
  for (const [k, v] of map.entries()) {
    const keyChunks: Uint8Array[] = [];
    encodeValue(k, keyChunks);
    const valueChunks: Uint8Array[] = [];
    encodeValue(v, valueChunks);
    entries.push([concatBytes(keyChunks), concatBytes(valueChunks)]);
  }
  entries.sort((a, b) => compareBytes(a[0], b[0]));
  out.push(typeHeader(5, entries.length));
  for (const [k, v] of entries) {
    out.push(k);
    out.push(v);
  }
}

function typeHeader(major: number, arg: number): Uint8Array {
  if (arg < 0) {
    throw new CborEncodeError(`negative argument to typeHeader: ${arg}`);
  }
  const head = major << 5;
  if (arg < 24) return new Uint8Array([head | arg]);
  if (arg < 0x100) return new Uint8Array([head | 24, arg]);
  if (arg < 0x10000) {
    const b = new Uint8Array(3);
    b[0] = head | 25;
    b[1] = (arg >>> 8) & 0xff;
    b[2] = arg & 0xff;
    return b;
  }
  if (arg <= 0xffffffff) {
    const b = new Uint8Array(5);
    b[0] = head | 26;
    new DataView(b.buffer).setUint32(1, arg >>> 0, false);
    return b;
  }
  return typeHeaderBig(major, BigInt(arg));
}

function typeHeaderBig(major: number, arg: bigint): Uint8Array {
  if (arg < 0n) {
    throw new CborEncodeError(`negative bigint argument: ${arg}`);
  }
  const head = major << 5;
  if (arg < 24n) return new Uint8Array([head | Number(arg)]);
  if (arg < 0x100n) return new Uint8Array([head | 24, Number(arg)]);
  if (arg < 0x10000n) {
    const n = Number(arg);
    return new Uint8Array([head | 25, (n >>> 8) & 0xff, n & 0xff]);
  }
  if (arg < 0x100000000n) {
    const b = new Uint8Array(5);
    b[0] = head | 26;
    new DataView(b.buffer).setUint32(1, Number(arg), false);
    return b;
  }
  if (arg < 0x10000000000000000n) {
    const b = new Uint8Array(9);
    b[0] = head | 27;
    new DataView(b.buffer).setBigUint64(1, arg, false);
    return b;
  }
  throw new CborEncodeError("argument exceeds uint64");
}

// ---------------------------------------------------------------------------
// decode
// ---------------------------------------------------------------------------

export function decodeFrame(bytes: Uint8Array): CborValue {
  const ctx = { bytes, pos: 0 };
  const value = readValue(ctx);
  if (ctx.pos !== bytes.length) {
    throw new CborDecodeError(
      `trailing bytes after CBOR value (consumed ${ctx.pos}/${bytes.length})`,
    );
  }
  return value;
}

interface DecodeCtx {
  bytes: Uint8Array;
  pos: number;
}

function readValue(ctx: DecodeCtx): CborValue {
  if (ctx.pos >= ctx.bytes.length) {
    throw new CborDecodeError("unexpected end of input");
  }
  const initial = ctx.bytes[ctx.pos]!;
  ctx.pos += 1;
  const major = initial >> 5;
  const minor = initial & 0x1f;

  switch (major) {
    case 0: {
      const arg = readArgument(ctx, minor);
      return arg;
    }
    case 1: {
      const arg = readArgument(ctx, minor);
      if (typeof arg === "bigint") {
        return -1n - arg;
      }
      return -1 - (arg as number);
    }
    case 2: {
      const len = readArgumentAsNumber(ctx, minor);
      return readBytes(ctx, len);
    }
    case 3: {
      const len = readArgumentAsNumber(ctx, minor);
      const buf = readBytes(ctx, len);
      try {
        return textDecoder.decode(buf);
      } catch (err) {
        throw new CborDecodeError(`invalid UTF-8 in text string: ${(err as Error).message}`);
      }
    }
    case 4: {
      const len = readArgumentAsNumber(ctx, minor);
      const arr: CborValue[] = [];
      for (let i = 0; i < len; i += 1) arr.push(readValue(ctx));
      return arr;
    }
    case 5: {
      const len = readArgumentAsNumber(ctx, minor);
      const out: { [k: string]: CborValue } = {};
      const fallback = new Map<CborValue, CborValue>();
      let useFallback = false;
      for (let i = 0; i < len; i += 1) {
        const key = readValue(ctx);
        const value = readValue(ctx);
        if (!useFallback && typeof key === "string") {
          if (Object.prototype.hasOwnProperty.call(out, key)) {
            throw new CborDecodeError(`duplicate map key: ${key}`);
          }
          out[key] = value;
        } else {
          if (!useFallback) {
            // promote: copy what we've collected so far
            for (const [k, v] of Object.entries(out)) fallback.set(k, v);
            useFallback = true;
          }
          if (fallback.has(key)) {
            throw new CborDecodeError(`duplicate map key in CBOR map`);
          }
          fallback.set(key, value);
        }
      }
      return useFallback ? fallback : out;
    }
    case 7: {
      if (minor === 20) return false;
      if (minor === 21) return true;
      if (minor === 22) return null;
      if (minor === 23) return undefined;
      if (minor === 27) {
        const buf = readBytes(ctx, 8);
        return new DataView(
          buf.buffer,
          buf.byteOffset,
          buf.byteLength,
        ).getFloat64(0, false);
      }
      throw new CborDecodeError(`unsupported simple value: minor=${minor}`);
    }
    default:
      throw new CborDecodeError(`unsupported major type ${major}`);
  }
}

function readArgument(ctx: DecodeCtx, minor: number): number | bigint {
  if (minor < 24) return minor;
  if (minor === 24) return readUint(ctx, 1);
  if (minor === 25) return readUint(ctx, 2);
  if (minor === 26) return readUint(ctx, 4);
  if (minor === 27) return readUint(ctx, 8);
  throw new CborDecodeError(`unsupported minor ${minor} (indefinite lengths not implemented in stub)`);
}

function readArgumentAsNumber(ctx: DecodeCtx, minor: number): number {
  const arg = readArgument(ctx, minor);
  if (typeof arg === "bigint") {
    if (arg > BigInt(Number.MAX_SAFE_INTEGER)) {
      throw new CborDecodeError(`length argument exceeds safe integer range`);
    }
    return Number(arg);
  }
  return arg;
}

function readUint(ctx: DecodeCtx, n: number): number | bigint {
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

function readBytes(ctx: DecodeCtx, len: number): Uint8Array {
  if (ctx.pos + len > ctx.bytes.length) {
    throw new CborDecodeError(`truncated byte run (need ${len} bytes)`);
  }
  const slice = ctx.bytes.slice(ctx.pos, ctx.pos + len);
  ctx.pos += len;
  return slice;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

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
  // RFC 8949 §4.2.1 — length-then-lexicographic (i.e., shorter first; ties
  // by byte order). Equivalent to comparing as if each encoding were a
  // bytestring with its length prepended.
  if (a.length !== b.length) return a.length - b.length;
  for (let i = 0; i < a.length; i += 1) {
    const diff = (a[i]! - b[i]!);
    if (diff !== 0) return diff;
  }
  return 0;
}
