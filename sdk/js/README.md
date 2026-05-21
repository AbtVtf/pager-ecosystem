# @pageros/sdk

JavaScript / TypeScript SDK for [PagerOS](../../SPEC.md) apps.

Status: scaffolding for `JS-001..JS-013`. Each task lands an isolated
slice (App routing, CBOR codec, signing middleware, dev server,
manifest, npm publish, …).

## Quick example

```ts
import { App } from "@pageros/sdk";

const app = new App({ name: "hello" });

app.screen("/", () => ({
  v: 1,
  id: "scr_home",
  body: [{ t: "text", s: "Hello, PagerOS!" }],
}));

if (import.meta.url === `file://${process.argv[1]}`) await app.run();
```

## Build & test

```bash
cd sdk/js
npm install
npm run build
npm test
```
