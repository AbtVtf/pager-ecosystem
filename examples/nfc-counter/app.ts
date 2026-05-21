// PagerOS nfc-counter example (DOCS-009, TS counterpart). See app.py
// for the authoritative implementation.

import { App, Button, List, ListItem, Notification, Screen, Text } from "@pageros/sdk";

const counts = new Map<string, Map<string, number>>();

const app = new App({ name: "nfc-counter" });

app.screen("/", (req) => {
  const deviceCounts = counts.get(req.ctx.deviceId) ?? new Map<string, number>();
  const ranked = [...deviceCounts.entries()].sort((a, b) => b[1] - a[1]);
  const body: unknown[] = [];
  const flash = req.query.flash as string | undefined;
  if (flash) body.push(Notification({ s: flash, level: "info" }));
  if (ranked.length) {
    body.push(
      List({
        items: ranked.map(([uid, n]) =>
          ListItem({ label: uid, sub: `${n} scan${n === 1 ? "" : "s"}`, href: `/tag/${uid}` }),
        ),
      }),
    );
  } else {
    body.push(Text({ s: "Tap any NFC tag to count it.", style: "dim" }));
  }
  body.push(Button({ label: "Reset all", href: "/reset", method: "POST", confirm: "Reset all counters?" }));
  return Screen({
    id: "scr_home",
    title: "NFC counter",
    body,
    extras: { subscribe: ["nfc_scan"] },
  });
});

app.handler("/event/nfc_scan", { method: "POST" }, (req, payload: { uid?: string }) => {
  const uid = (payload.uid ?? "").toUpperCase();
  if (!uid) return Screen({ id: "scr_home", body: [] }).redirect("/?flash=Empty+UID");
  const deviceCounts = counts.get(req.ctx.deviceId) ?? new Map<string, number>();
  const n = (deviceCounts.get(uid) ?? 0) + 1;
  deviceCounts.set(uid, n);
  counts.set(req.ctx.deviceId, deviceCounts);
  return Screen({ id: "scr_home", body: [] }).redirect(`/?flash=Scanned+${uid}+(x${n})`);
});

app.handler("/reset", { method: "POST" }, (req) => {
  counts.delete(req.ctx.deviceId);
  return Screen({ id: "scr_home", body: [] }).redirect("/");
});

if (import.meta.url === `file://${process.argv[1]}`) {
  await app.run();
}
