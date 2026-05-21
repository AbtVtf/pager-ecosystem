// App class and routing API (JS-001).
//
// JS/TS counterpart to sdk/python/pageros/app.py. The two implementations
// share semantics — routes keyed on (METHOD, path), GET / POST in v1,
// canonical CBOR bodies, locally-generated error frames per SPEC §7.4.1.
//
// JS doesn't have stable runtime decorators across all toolchains, so
// the "decorator-like" surface comes from a curried call style that
// mirrors Python's @app.screen("/") form:
//
//   app.screen("/")(home);        // curried — closest to Python's @decorator
//   app.screen("/", home);        // direct  — more idiomatic JS
//
// Both produce the same registration. Handlers may be sync or async,
// take zero or one arg (`Request`), and return a Frame, a Response,
// a Uint8Array (pre-encoded body), or null/undefined (204).

import * as http from "node:http";
import { URL } from "node:url";

import {
  CborDecodeError,
  decodeFrame,
  encodeFrame,
} from "./codec.js";
import type { Frame } from "./frame.js";
import {
  SignatureError,
  verifyRequest,
  type VerifyRequestOptions,
} from "./signing.js";
import {
  AppKeypair,
  AEAD_NONCE_LEN,
  EncryptionError,
  HEADER_ENCRYPTED,
  HEADER_NONCE,
  HEADER_SENDER,
  InvalidEncryptionHeader,
  MissingEncryptionHeader,
  X25519_KEY_LEN,
} from "./encryption.js";

export type { Frame, FrameWidget, Action } from "./frame.js";

export const CBOR_CONTENT_TYPE = "application/cbor";
export const CBOR_ACCEPT = "application/cbor; pagerOS=1";

export interface Request {
  method: string;
  path: string;
  query: Record<string, string[]>;
  headers: Record<string, string>;
  body: unknown;
  ctx: Ctx;
}

/**
 * Headers that carry per-request context fields. Mirrors the Python
 * SDK's `pageros.ctx` (JS-008) for cross-language consistency.
 */
export const HEADER_TRANSPORT = "PagerOS-Transport";
export const HEADER_GRANTED = "PagerOS-Granted";
export const HEADER_LOCATION = "PagerOS-Location";
export const HEADER_GROUPS = "PagerOS-Groups";

/** Transport string values (SPEC §8.3). */
export const TRANSPORT_WIFI = "wifi";
export const TRANSPORT_LORA = "lora";
export type Transport = "wifi" | "lora";

/** A device GPS fix. */
export interface Location {
  lat: number;
  lon: number;
  ts: number;
}

export interface Ctx {
  deviceId: Uint8Array | null;
  session: Record<string, unknown> | null;
  timestamp: number | null;
  /**
   * Verified X25519 pubkey of the request sender, populated when an
   * `AppKeypair` is configured on the App and the request arrives with
   * `PagerOS-Encrypted: 1`. `null` otherwise.
   */
  senderId: Uint8Array | null;
  /** Transport the request rode in on (SPEC §8.3, JS-008). */
  transport: Transport;
  /** Permissions the user has granted to this app (`PagerOS-Granted`). */
  granted: string[];
  /** Latest device GPS fix when subscribed to `location` events. */
  location: Location | null;
  /** Groups the device is currently subscribed to (`PagerOS-Groups`). */
  groups: string[];
}

export class Response {
  readonly status: number;
  readonly body: unknown;
  readonly headers: Record<string, string>;
  constructor(opts: {
    status?: number;
    body?: unknown;
    headers?: Record<string, string>;
  } = {}) {
    this.status = opts.status ?? 200;
    this.body = opts.body ?? null;
    this.headers = opts.headers ?? {};
  }
}

export type Handler = (req: Request) => unknown | Promise<unknown>;

/**
 * Per-app signature-verification policy.
 *
 * - ``false`` (the default) — skip verification entirely; handlers still
 *   see whatever ``PagerOS-Device`` header the client sent, but
 *   ``ctx.deviceId`` is unverified.
 * - ``true`` — require a valid ``PagerOS-Sig`` on every request, using
 *   the default 5-minute skew window.
 * - object — require verification with the given options (custom skew
 *   window, injected ``now()`` for tests).
 */
export type SignatureVerification =
  | boolean
  | Pick<VerifyRequestOptions, "maxSkewSeconds" | "now">;

export interface AppOptions {
  name?: string;
  loraCompatible?: boolean;
  verifySignature?: SignatureVerification;
  /**
   * X25519 keypair the app publishes for inbound encrypted requests
   * (SPEC §6.2.2 / §6.6.3). When set, requests with `PagerOS-Encrypted: 1`
   * are decrypted transparently before signature verification + CBOR
   * decode. Requests without the encryption flag pass through untouched.
   */
  keypair?: AppKeypair;
}

interface DispatchResult {
  status: number;
  headers: Record<string, string>;
  body: Uint8Array;
}

interface DispatchOptions {
  body?: Uint8Array;
  headers?: Record<string, string>;
  verifiedDeviceId?: Uint8Array;
  session?: Record<string, unknown>;
}

interface RunOptions {
  host?: string;
  port?: number;
  argv?: string[];
}

type RouteKey = `${string} ${string}`;

const ROUTE_SEP = " ";

export class App {
  readonly name: string;
  readonly loraCompatible: boolean;
  readonly verifySignature: SignatureVerification;
  readonly keypair: AppKeypair | null;
  private readonly routes = new Map<RouteKey, Handler>();

  constructor(opts: AppOptions = {}) {
    this.name = opts.name ?? "pageros-app";
    this.loraCompatible = opts.loraCompatible ?? false;
    this.verifySignature = opts.verifySignature ?? false;
    this.keypair = opts.keypair ?? null;
  }

  // -- registration ---------------------------------------------------------

  screen(path: string): (handler: Handler) => Handler;
  screen(path: string, handler: Handler): Handler;
  screen(path: string, handler?: Handler): Handler | ((h: Handler) => Handler) {
    if (handler !== undefined) {
      return this.register("GET", path, handler);
    }
    return (h: Handler) => this.register("GET", path, h);
  }

  handler(
    path: string,
    opts?: { method?: string },
  ): (handler: Handler) => Handler;
  handler(
    path: string,
    opts: { method?: string } | undefined,
    handler: Handler,
  ): Handler;
  handler(
    path: string,
    opts: { method?: string } = {},
    handler?: Handler,
  ): Handler | ((h: Handler) => Handler) {
    const method = (opts.method ?? "POST").toUpperCase();
    if (handler !== undefined) {
      return this.register(method, path, handler);
    }
    return (h: Handler) => this.register(method, path, h);
  }

  private register(method: string, path: string, handler: Handler): Handler {
    if (!path.startsWith("/")) {
      throw new Error(`route path must start with '/': ${JSON.stringify(path)}`);
    }
    const key = `${method}${ROUTE_SEP}${path}` as RouteKey;
    if (this.routes.has(key)) {
      throw new Error(`duplicate route registration for ${method} ${path}`);
    }
    this.routes.set(key, handler);
    return handler;
  }

  // -- introspection --------------------------------------------------------

  routeList(): Array<{ method: string; path: string }> {
    return Array.from(this.routes.keys()).map((key) => {
      const sep = key.indexOf(ROUTE_SEP);
      return { method: key.slice(0, sep), path: key.slice(sep + 1) };
    });
  }

  // -- dispatch -------------------------------------------------------------

  async dispatch(
    method: string,
    rawPath: string,
    opts: DispatchOptions = {},
  ): Promise<DispatchResult> {
    const methodNorm = method.toUpperCase();
    const headerMap = lowercaseHeaders(opts.headers ?? {});

    const url = new URL(rawPath, "http://localhost/");
    const routePath = url.pathname;
    const query = parseQuery(url.searchParams);

    const handler = this.routes.get(`${methodNorm}${ROUTE_SEP}${routePath}` as RouteKey);
    if (handler === undefined) {
      const otherMethods = Array.from(this.routes.keys())
        .filter((k) => k.endsWith(`${ROUTE_SEP}${routePath}`))
        .map((k) => k.slice(0, k.indexOf(ROUTE_SEP)))
        .sort();
      if (otherMethods.length > 0) {
        return this.errorResponse(405, `Method not allowed: ${methodNorm} ${routePath}`, {
          Allow: otherMethods.join(", "),
        });
      }
      return this.errorResponse(404, `Not found: ${methodNorm} ${routePath}`);
    }

    let rawBody = opts.body;
    let senderId: Uint8Array | null = null;

    // Transparent decryption: when the request advertises
    // `PagerOS-Encrypted: 1` and an `AppKeypair` is configured, swap the
    // ciphertext body for the decrypted plaintext before signature
    // verification (the device signs the *inner* plaintext via the
    // separate `PagerOS-Sig` header — SPEC §6.2.2 / §7.3.1).
    if (this.encryptionRequested(headerMap)) {
      try {
        const decryption = this.decryptIncoming(headerMap, rawBody);
        rawBody = decryption.plaintext;
        senderId = decryption.senderId;
      } catch (err) {
        if (err instanceof MissingEncryptionHeader || err instanceof InvalidEncryptionHeader) {
          return this.errorResponse(400, `${err.name}: ${err.message}`);
        }
        if (err instanceof EncryptionError) {
          // Bad ciphertext → 401, mirroring Python's behaviour.
          return this.errorResponse(401, `${err.name}: ${err.message}`, {
            "WWW-Authenticate": `PagerOS-Sig realm="${this.name}"`,
          });
        }
        throw err;
      }
    }

    // Signature verification short-circuits ahead of the CBOR body decode
    // and the handler invocation. The signing input includes the raw
    // body bytes (sha256), so we MUST verify before tampering with body
    // bytes — decoded shape diverges from the bytes the device signed.
    let verifiedDeviceId: Uint8Array | null = opts.verifiedDeviceId ?? null;
    let signedTimestamp: number | null = null;
    if (this.verifySignature !== false && opts.verifiedDeviceId === undefined) {
      const verifyOpts = this.verifySignature === true ? {} : this.verifySignature;
      try {
        const verified = verifyRequest({
          method: methodNorm,
          url: rawPath,
          headers: opts.headers ?? {},
          body: rawBody ?? null,
          ...verifyOpts,
        });
        verifiedDeviceId = verified.deviceId;
        signedTimestamp = verified.timestamp;
      } catch (err) {
        if (err instanceof SignatureError) {
          return this.unauthorised(err);
        }
        throw err;
      }
    }

    let decodedBody: unknown = null;
    if (rawBody && rawBody.length > 0) {
      const ctype = (headerMap["content-type"] ?? "").split(";", 1)[0]!.trim().toLowerCase();
      if (ctype === CBOR_CONTENT_TYPE) {
        try {
          decodedBody = decodeFrame(rawBody);
        } catch (err) {
          const message =
            err instanceof CborDecodeError ? err.message : String(err);
          return this.errorResponse(400, `Bad request: malformed CBOR body (${message})`);
        }
      } else {
        decodedBody = rawBody;
      }
    }

    const ctx: Ctx = {
      deviceId: verifiedDeviceId ?? parseDeviceHeader(headerMap),
      session: opts.session ?? null,
      timestamp: signedTimestamp ?? parseTimestampHeader(headerMap),
      senderId,
      transport: parseTransportHeader(headerMap),
      granted: parseCsvHeader(headerMap, HEADER_GRANTED),
      location: parseLocationHeader(headerMap),
      groups: parseCsvHeader(headerMap, HEADER_GROUPS),
    };

    const request: Request = {
      method: methodNorm,
      path: routePath,
      query,
      headers: headerMap,
      body: decodedBody,
      ctx,
    };

    let result: unknown;
    try {
      result = await this.invoke(handler, request);
    } catch (err) {
      // Surface as 500 — handler errors are not the device's problem to
      // troubleshoot beyond rendering the error frame. The original
      // stack is logged so the dev sees it during local runs.
      console.error(
        `[pageros] handler ${methodNorm} ${routePath} raised:`,
        err,
      );
      return this.errorResponse(500, "Server error");
    }

    return this.buildResponse(result);
  }

  // -- run ------------------------------------------------------------------

  async run(opts: RunOptions = {}): Promise<void> {
    const { host, port } = resolveAddress(opts);
    const server = http.createServer((req, res) => {
      this.handleHttpRequest(req, res).catch((err) => {
        console.error("[pageros] unexpected dispatch failure:", err);
        if (!res.headersSent) {
          res.statusCode = 500;
          res.end();
        }
      });
    });
    await new Promise<void>((resolve, reject) => {
      const onError = (err: Error) => reject(err);
      server.once("error", onError);
      server.listen(port, host, () => {
        server.off("error", onError);
        // eslint-disable-next-line no-console
        console.log(
          `[pageros] app "${this.name}" listening on http://${host}:${port}`,
        );
        resolve();
      });
    });
    // Keep the process alive until the server closes (SIGINT, etc.).
    await new Promise<void>((resolve) => {
      const shutdown = () => server.close(() => resolve());
      process.once("SIGINT", shutdown);
      process.once("SIGTERM", shutdown);
      server.once("close", () => resolve());
    });
  }

  private async handleHttpRequest(
    req: http.IncomingMessage,
    res: http.ServerResponse,
  ): Promise<void> {
    const chunks: Buffer[] = [];
    for await (const chunk of req) {
      chunks.push(chunk as Buffer);
    }
    const bodyBuf = chunks.length > 0 ? Buffer.concat(chunks) : undefined;
    const headers: Record<string, string> = {};
    for (const [k, v] of Object.entries(req.headers)) {
      if (typeof v === "string") headers[k] = v;
      else if (Array.isArray(v)) headers[k] = v.join(", ");
    }
    const dispatchOpts: DispatchOptions = { headers };
    if (bodyBuf !== undefined) {
      dispatchOpts.body = new Uint8Array(
        bodyBuf.buffer,
        bodyBuf.byteOffset,
        bodyBuf.byteLength,
      );
    }
    const result = await this.dispatch(
      req.method ?? "GET",
      req.url ?? "/",
      dispatchOpts,
    );
    res.statusCode = result.status;
    for (const [k, v] of Object.entries(result.headers)) {
      res.setHeader(k, v);
    }
    if (result.body.length > 0) {
      res.setHeader("Content-Length", String(result.body.length));
      res.end(Buffer.from(result.body));
    } else {
      res.end();
    }
  }

  // -- helpers --------------------------------------------------------------

  private async invoke(handler: Handler, request: Request): Promise<unknown> {
    if (handler.length === 0) {
      return await (handler as () => unknown | Promise<unknown>)();
    }
    return await handler(request);
  }

  private buildResponse(result: unknown): DispatchResult {
    let status: number;
    let body: unknown;
    let extraHeaders: Record<string, string>;

    if (result instanceof Response) {
      status = result.status;
      body = result.body;
      extraHeaders = { ...result.headers };
    } else {
      status = 200;
      body = result;
      extraHeaders = {};
    }

    if (body === null || body === undefined) {
      if (status === 200) status = 204;
      return { status, headers: extraHeaders, body: new Uint8Array(0) };
    }

    if (body instanceof Uint8Array) {
      if (extraHeaders["Content-Type"] === undefined) {
        extraHeaders["Content-Type"] = CBOR_CONTENT_TYPE;
      }
      return { status, headers: extraHeaders, body };
    }

    const encoded = encodeFrame(body);
    if (this.loraCompatible && encoded.length > 200) {
      console.warn(
        `[pageros] frame size ${encoded.length}B exceeds 200B LoRa budget for app "${this.name}"`,
      );
    }
    if (extraHeaders["Content-Type"] === undefined) {
      extraHeaders["Content-Type"] = CBOR_CONTENT_TYPE;
    }
    return { status, headers: extraHeaders, body: encoded };
  }

  private encryptionRequested(headers: Record<string, string>): boolean {
    const flag = headers[HEADER_ENCRYPTED.toLowerCase()];
    if (flag === undefined) return false;
    const v = flag.trim().toLowerCase();
    return v !== "" && v !== "0" && v !== "false" && v !== "no";
  }

  private decryptIncoming(
    headers: Record<string, string>,
    rawBody: Uint8Array | undefined,
  ): { plaintext: Uint8Array; senderId: Uint8Array } {
    if (this.keypair === null) {
      throw new InvalidEncryptionHeader(
        `${HEADER_ENCRYPTED}: app has no keypair configured`,
      );
    }
    const senderB64 = headers[HEADER_SENDER.toLowerCase()];
    const nonceB64 = headers[HEADER_NONCE.toLowerCase()];
    if (senderB64 === undefined) {
      throw new MissingEncryptionHeader(`missing ${HEADER_SENDER}`);
    }
    if (nonceB64 === undefined) {
      throw new MissingEncryptionHeader(`missing ${HEADER_NONCE}`);
    }
    const sender = decodeB64UrlExact(senderB64, HEADER_SENDER, X25519_KEY_LEN);
    const nonce = decodeB64UrlExact(nonceB64, HEADER_NONCE, AEAD_NONCE_LEN);
    const ciphertext = rawBody ?? new Uint8Array(0);
    const plaintext = this.keypair.decryptFrom(sender, ciphertext, nonce);
    return { plaintext, senderId: sender };
  }

  private unauthorised(error: SignatureError): DispatchResult {
    // Per SPEC §7.4 a 401 surfaces as "Sign-in required" on the device
    // after one retry. The error frame body identifies the failure
    // class for debugging (and matches Python's behaviour).
    return this.errorResponse(
      401,
      `${error.name}: ${error.message}`,
      { "WWW-Authenticate": `PagerOS-Sig realm="${this.name}"` },
    );
  }

  private errorResponse(
    status: number,
    message: string,
    extraHeaders: Record<string, string> = {},
  ): DispatchResult {
    const frame: Frame = {
      v: 1,
      id: "err_local",
      ttl: 0,
      title: "Error",
      body: [{ t: "text", s: message, style: "error" }],
    };
    return {
      status,
      headers: { "Content-Type": CBOR_CONTENT_TYPE, ...extraHeaders },
      body: encodeFrame(frame),
    };
  }
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

function lowercaseHeaders(h: Record<string, string>): Record<string, string> {
  const out: Record<string, string> = {};
  for (const [k, v] of Object.entries(h)) out[k.toLowerCase()] = v;
  return out;
}

function parseQuery(params: URLSearchParams): Record<string, string[]> {
  const out: Record<string, string[]> = {};
  for (const [key, value] of params.entries()) {
    const arr = out[key];
    if (arr === undefined) out[key] = [value];
    else arr.push(value);
  }
  return out;
}

function parseDeviceHeader(headers: Record<string, string>): Uint8Array | null {
  const raw = headers["pageros-device"];
  if (raw === undefined) return null;
  try {
    return Buffer.from(raw, "base64");
  } catch {
    return null;
  }
}

function decodeB64UrlExact(
  value: string,
  field: string,
  expectedLen: number,
): Uint8Array {
  let buf: Buffer;
  try {
    buf = Buffer.from(value, "base64url");
  } catch (err) {
    throw new InvalidEncryptionHeader(`${field}: not valid base64url (${(err as Error).message})`);
  }
  if (buf.length !== expectedLen) {
    throw new InvalidEncryptionHeader(
      `${field}: expected ${expectedLen} bytes, got ${buf.length}`,
    );
  }
  return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
}

function parseTimestampHeader(headers: Record<string, string>): number | null {
  const raw = headers["pageros-timestamp"];
  if (raw === undefined) return null;
  const n = Number(raw);
  return Number.isFinite(n) ? n : null;
}

// JS-008: ctx header parsers — case-insensitive (the dispatcher already
// lowercases keys before calling these). Malformed values fall back to
// safe defaults so a stale device header never crashes a handler.

function parseTransportHeader(headers: Record<string, string>): Transport {
  const raw = headers[HEADER_TRANSPORT.toLowerCase()];
  if (raw === undefined) return "wifi";
  const t = raw.trim().toLowerCase();
  return t === "lora" ? "lora" : "wifi";
}

function parseCsvHeader(headers: Record<string, string>, name: string): string[] {
  const raw = headers[name.toLowerCase()];
  if (raw === undefined) return [];
  return raw
    .split(",")
    .map((s) => s.trim())
    .filter((s) => s.length > 0);
}

function parseLocationHeader(headers: Record<string, string>): Location | null {
  const raw = headers[HEADER_LOCATION.toLowerCase()];
  if (raw === undefined) return null;
  const parts = raw.split(",").map((s) => s.trim());
  if (parts.length !== 3) return null;
  const lat = Number(parts[0]);
  const lon = Number(parts[1]);
  const ts = Number(parts[2]);
  if (
    !Number.isFinite(lat) || !Number.isFinite(lon) ||
    !Number.isInteger(ts) || ts < 0 ||
    lat < -90 || lat > 90 || lon < -180 || lon > 180
  ) {
    return null;
  }
  return { lat, lon, ts };
}

function resolveAddress(opts: RunOptions): { host: string; port: number } {
  if (opts.host !== undefined && opts.port !== undefined) {
    return { host: opts.host, port: opts.port };
  }
  const argv = opts.argv ?? process.argv.slice(2);
  let host = opts.host ?? "127.0.0.1";
  let port = opts.port ?? 8000;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i]!;
    if (arg === "--host" && i + 1 < argv.length) {
      host = argv[i + 1]!;
      i += 1;
    } else if (arg.startsWith("--host=")) {
      host = arg.slice("--host=".length);
    } else if (arg === "--port" && i + 1 < argv.length) {
      const next = argv[i + 1]!;
      port = Number(next);
      i += 1;
    } else if (arg.startsWith("--port=")) {
      port = Number(arg.slice("--port=".length));
    }
  }
  if (!Number.isFinite(port) || port < 0 || port > 65535) {
    throw new Error(`invalid port: ${port}`);
  }
  return { host, port };
}
