// JS-001 acceptance tests: App routing, dispatch, error frames, hello example.

import { test } from "node:test";
import { strict as assert } from "node:assert";
import * as http from "node:http";
import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

import {
  App,
  Response,
  decodeFrame,
  encodeFrame,
  CBOR_CONTENT_TYPE,
} from "../src/index.js";

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

const REPO_ROOT = findRepoRoot(__dirname);
const HELLO_EXAMPLE = resolve(REPO_ROOT, "examples/hello/app.ts");

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

test("screen() registers a GET route", () => {
  const app = new App({ name: "t" });
  app.screen("/", () => ({ v: 1 as const, id: "scr_home", body: [] }));
  const routes = app.routeList();
  assert.deepEqual(routes, [{ method: "GET", path: "/" }]);
});

test("screen() supports curried decorator-like form", () => {
  const app = new App({ name: "t" });
  app.screen("/")(() => ({ v: 1 as const, id: "scr_home", body: [] }));
  assert.deepEqual(app.routeList(), [{ method: "GET", path: "/" }]);
});

test("handler() defaults to POST", () => {
  const app = new App({ name: "t" });
  app.handler("/save", undefined, () => null);
  assert.deepEqual(app.routeList(), [{ method: "POST", path: "/save" }]);
});

test("handler() normalises method to upper case", () => {
  const app = new App({ name: "t" });
  app.handler("/x", { method: "put" })(() => null);
  assert.deepEqual(app.routeList(), [{ method: "PUT", path: "/x" }]);
});

test("registration rejects paths without leading slash", () => {
  const app = new App({ name: "t" });
  assert.throws(() => app.handler("missing-slash", undefined, () => null), /must start with/);
});

test("duplicate route registration throws", () => {
  const app = new App({ name: "t" });
  app.screen("/", () => null);
  assert.throws(() => app.screen("/", () => null), /duplicate route/);
});

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------

test("GET / returns 200 + canonical CBOR Frame", async () => {
  const app = new App({ name: "t" });
  app.screen("/", () => ({
    v: 1 as const,
    id: "scr_home",
    body: [{ t: "text", s: "Hello, PagerOS!" }],
  }));

  const { status, headers, body } = await app.dispatch("GET", "/");
  assert.equal(status, 200);
  assert.equal(headers["Content-Type"], CBOR_CONTENT_TYPE);

  const decoded = decodeFrame(body) as Record<string, unknown>;
  assert.equal(decoded["v"], 1);
  assert.equal(decoded["id"], "scr_home");
  const widgets = decoded["body"] as Array<Record<string, unknown>>;
  assert.equal(widgets.length, 1);
  assert.equal(widgets[0]!["t"], "text");
  assert.equal(widgets[0]!["s"], "Hello, PagerOS!");
});

test("handler receives Request when it declares one arg", async () => {
  const app = new App({ name: "t" });
  app.handler("/echo", undefined, (req) => ({
    v: 1 as const,
    id: "scr_echo",
    body: [{ t: "text", s: `method=${req.method} path=${req.path}` }],
  }));
  const { status, body } = await app.dispatch("POST", "/echo");
  assert.equal(status, 200);
  const decoded = decodeFrame(body) as Record<string, unknown>;
  const widgets = decoded["body"] as Array<Record<string, unknown>>;
  assert.equal(widgets[0]!["s"], "method=POST path=/echo");
});

test("zero-arg handler is supported", async () => {
  const app = new App({ name: "t" });
  app.screen("/", () => ({ v: 1 as const, id: "scr_home", body: [] }));
  const { status, body } = await app.dispatch("GET", "/");
  assert.equal(status, 200);
  const decoded = decodeFrame(body) as Record<string, unknown>;
  assert.equal(decoded["id"], "scr_home");
});

test("returning null produces a 204 with empty body", async () => {
  const app = new App({ name: "t" });
  app.handler("/ack", undefined, () => null);
  const { status, body } = await app.dispatch("POST", "/ack");
  assert.equal(status, 204);
  assert.equal(body.length, 0);
});

test("Response wrapper controls status and headers", async () => {
  const app = new App({ name: "t" });
  app.handler("/created", undefined, () =>
    new Response({
      status: 201,
      body: { v: 1 as const, id: "scr_created", body: [] },
      headers: { "X-Resource": "abc" },
    }),
  );
  const { status, headers, body } = await app.dispatch("POST", "/created");
  assert.equal(status, 201);
  assert.equal(headers["X-Resource"], "abc");
  const decoded = decodeFrame(body) as Record<string, unknown>;
  assert.equal(decoded["id"], "scr_created");
});

test("Response with bytes body passes through untouched", async () => {
  const app = new App({ name: "t" });
  const raw = encodeFrame({ v: 1, id: "scr_raw", body: [] });
  app.handler("/raw", undefined, () => new Response({ body: raw }));
  const { body } = await app.dispatch("POST", "/raw");
  assert.deepEqual(body, raw);
});

// ---------------------------------------------------------------------------
// error envelopes
// ---------------------------------------------------------------------------

test("unknown path returns 404 with locally-generated error frame", async () => {
  const app = new App({ name: "t" });
  app.screen("/", () => ({ v: 1 as const, id: "scr_home", body: [] }));
  const { status, body } = await app.dispatch("GET", "/missing");
  assert.equal(status, 404);
  const decoded = decodeFrame(body) as Record<string, unknown>;
  assert.equal(decoded["id"], "err_local");
  assert.equal(decoded["title"], "Error");
});

test("wrong method returns 405 with Allow header", async () => {
  const app = new App({ name: "t" });
  app.handler("/save", undefined, () => null);
  const { status, headers } = await app.dispatch("GET", "/save");
  assert.equal(status, 405);
  assert.equal(headers["Allow"], "POST");
});

test("malformed CBOR body returns 400", async () => {
  const app = new App({ name: "t" });
  app.handler("/save", undefined, () => null);
  const garbage = new Uint8Array([0xff, 0xff, 0xff]);
  const { status, body } = await app.dispatch("POST", "/save", {
    body: garbage,
    headers: { "Content-Type": "application/cbor" },
  });
  assert.equal(status, 400);
  const decoded = decodeFrame(body) as Record<string, unknown>;
  assert.equal(decoded["id"], "err_local");
});

test("handler throw produces a 500 error frame", async () => {
  const app = new App({ name: "t" });
  app.handler("/boom", undefined, () => {
    throw new Error("nope");
  });
  // Swallow the noise from the error logger during this single test.
  const origError = console.error;
  console.error = () => {};
  try {
    const { status, body } = await app.dispatch("POST", "/boom");
    assert.equal(status, 500);
    const decoded = decodeFrame(body) as Record<string, unknown>;
    assert.equal(decoded["id"], "err_local");
  } finally {
    console.error = origError;
  }
});

// ---------------------------------------------------------------------------
// CBOR request body decoding
// ---------------------------------------------------------------------------

test("CBOR request body is decoded and reaches the handler", async () => {
  const app = new App({ name: "t" });
  let observed: unknown = undefined;
  app.handler("/save", undefined, (req) => {
    observed = req.body;
    return null;
  });
  const body = encodeFrame({ title: "Groceries", body: "Milk, eggs" });
  await app.dispatch("POST", "/save", {
    body,
    headers: { "content-type": "application/cbor" },
  });
  assert.deepEqual(observed, { title: "Groceries", body: "Milk, eggs" });
});

// ---------------------------------------------------------------------------
// hello example is ≤ 20 LOC and dispatches via App.dispatch
// ---------------------------------------------------------------------------

test("examples/hello/app.ts is ≤ 20 source lines", () => {
  const src = readFileSync(HELLO_EXAMPLE, "utf-8");
  const lines = src.split("\n");
  // Trailing empty newline is not a logical line; trim a single empty
  // line if present (every well-formed file ends in '\n').
  const last = lines[lines.length - 1];
  if (last === "") lines.pop();
  assert.ok(lines.length <= 20, `hello example has ${lines.length} lines (>20)`);
});

// ---------------------------------------------------------------------------
// live HTTP round-trip
// ---------------------------------------------------------------------------

test("a minimal http.Server wired to dispatch round-trips a Frame", async () => {
  const app = new App({ name: "hello" });
  app.screen("/", () => ({
    v: 1 as const,
    id: "scr_home",
    body: [{ t: "text", s: "Hello, PagerOS!" }],
  }));

  const server = http.createServer(async (req, res) => {
    const chunks: Buffer[] = [];
    for await (const c of req) chunks.push(c as Buffer);
    const rawBody = chunks.length > 0 ? Buffer.concat(chunks) : undefined;
    const headers: Record<string, string> = {};
    for (const [k, v] of Object.entries(req.headers)) {
      if (typeof v === "string") headers[k] = v;
    }
    const dispatchOpts: { body?: Uint8Array; headers: Record<string, string> } = { headers };
    if (rawBody !== undefined) {
      dispatchOpts.body = new Uint8Array(
        rawBody.buffer,
        rawBody.byteOffset,
        rawBody.byteLength,
      );
    }
    const result = await app.dispatch(req.method ?? "GET", req.url ?? "/", dispatchOpts);
    res.statusCode = result.status;
    for (const [k, v] of Object.entries(result.headers)) res.setHeader(k, v);
    res.end(Buffer.from(result.body));
  });

  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", () => resolve()));
  const addr = server.address();
  if (!addr || typeof addr === "string") {
    server.close();
    throw new Error("could not get bound port");
  }
  const { port } = addr;

  try {
    const res = await fetch(`http://127.0.0.1:${port}/`, {
      headers: { Accept: "application/cbor; pagerOS=1" },
    });
    assert.equal(res.status, 200);
    assert.equal(res.headers.get("content-type"), CBOR_CONTENT_TYPE);
    const buf = new Uint8Array(await res.arrayBuffer());
    const decoded = decodeFrame(buf) as Record<string, unknown>;
    assert.equal(decoded["id"], "scr_home");
    const widgets = decoded["body"] as Array<Record<string, unknown>>;
    assert.equal(widgets[0]!["s"], "Hello, PagerOS!");
  } finally {
    await new Promise<void>((resolve) => server.close(() => resolve()));
  }
});
