// Minimal PagerOS "hello world" app (≤ 20 LOC, JS-001 acceptance).

import { App } from "@pageros/sdk";

const app = new App({ name: "hello" });

app.screen("/", () => ({
  v: 1,
  id: "scr_home",
  body: [{ t: "text", s: "Hello, PagerOS!" }],
}));

if (import.meta.url === `file://${process.argv[1]}`) {
  await app.run();
}
