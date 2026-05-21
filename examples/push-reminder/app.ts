// PagerOS push-reminder example (DOCS-011, TS counterpart). See
// app.py for the authoritative implementation.

import {
  App, Button, Form, Input, List, ListItem, Notification, PushConfig, Screen, Text, sendPush,
  AppKeypair,
} from "@pageros/sdk";
import { generateKeyPair } from "node:crypto";
import { promisify } from "node:util";

const APP_ID = process.env.PAGEROS_REMINDER_APP_ID ?? "push-reminder.example";
const RELAY_URL = process.env.PAGEROS_RELAY_URL ?? "https://push.pageros.org";

// Dev-mode ephemeral keys; for real runs hand the marketplace-registered keys via env.
const genKey = promisify(generateKeyPair);
const { privateKey: signingKey } = await genKey("ed25519");
const appKeypair = await AppKeypair.generate();
const pushCfg: PushConfig = { appId: APP_ID, signingKey, keypair: appKeypair, relayUrl: RELAY_URL };

type Reminder = { deviceId: string; fireAt: number; body: string; tone: string; counter: number };
const pending = new Map<string, Reminder[]>();
let counter = 0;

function arm(r: Reminder): void {
  const delay = Math.max(0, (r.fireAt - Date.now() / 1000) * 1000);
  setTimeout(async () => {
    try {
      await sendPush(pushCfg, r.deviceId, { title: "Reminder", body: r.body, tone: r.tone }, { counter: r.counter });
    } catch (e) {
      console.warn("reminder failed", e);
    }
    const lst = pending.get(r.deviceId) ?? [];
    pending.set(r.deviceId, lst.filter((x) => x !== r));
    if ((pending.get(r.deviceId) ?? []).length === 0) pending.delete(r.deviceId);
  }, delay);
}

const app = new App({ name: "push-reminder" });

app.screen("/", (req) => {
  const mine = (pending.get(req.ctx.deviceId) ?? []).sort((a, b) => a.fireAt - b.fireAt);
  const body: unknown[] = mine.length === 0
    ? [Text({ s: "No reminders scheduled.", style: "dim" })]
    : [List({
        items: mine.map((r) =>
          ListItem({ label: r.body, sub: `in ${Math.round(r.fireAt - Date.now() / 1000)}s · tone=${r.tone}` }),
        ),
      })];
  body.push(Button({ label: "New reminder", href: "/new", key: "n" }));
  return Screen({ id: "scr_home", title: "Reminders", body });
});

app.screen("/new", () =>
  Screen({
    id: "scr_new", title: "New reminder",
    body: [
      Form({
        action: "/schedule", method: "POST",
        fields: [
          Input({ name: "body", label: "Message", max: 80 }),
          Input({ name: "delay_s", label: "Fire in (seconds)", type: "number", value: "60" }),
          Input({ name: "tone", label: "Tone", value: "default", max: 16 }),
        ],
        submit: "Schedule",
      }),
    ],
  }),
);

app.handler("/schedule", { method: "POST" }, (req, payload: { body?: string; delay_s?: string; tone?: string }) => {
  const body = (payload.body ?? "").trim();
  if (!body) {
    return Screen({
      id: "scr_new", title: "New reminder",
      body: [
        Notification({ s: "Body is required.", level: "error" }),
        Form({
          action: "/schedule", method: "POST",
          fields: [
            Input({ name: "body", label: "Message", max: 80 }),
            Input({ name: "delay_s", label: "Fire in (seconds)", type: "number", value: "60" }),
            Input({ name: "tone", label: "Tone", value: "default", max: 16 }),
          ],
          submit: "Schedule",
        }),
      ],
    });
  }
  const delay = Math.max(0, parseInt(payload.delay_s ?? "0", 10) || 60);
  const tone = (payload.tone ?? "default").trim().slice(0, 16) || "default";
  counter += 1;
  const r: Reminder = { deviceId: req.ctx.deviceId, fireAt: Date.now() / 1000 + delay, body: body.slice(0, 80), tone, counter };
  const lst = pending.get(req.ctx.deviceId) ?? [];
  lst.push(r);
  pending.set(req.ctx.deviceId, lst);
  arm(r);
  return Screen({ id: "scr_scheduled", body: [] }).redirect("/");
});

if (import.meta.url === `file://${process.argv[1]}`) {
  await app.run();
}
