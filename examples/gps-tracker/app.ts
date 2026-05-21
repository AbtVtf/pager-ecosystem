// PagerOS gps-tracker example (DOCS-008, TS counterpart).
//
// Server-side renderer for the gps-tracker app — see app.py for the
// authoritative implementation. This file mirrors that surface against
// the @pageros/sdk JS API for parity with other examples once JS-008
// (ctx) and the widget builders land.

import { App, Map, MapMarker, Screen, Text, Button } from "@pageros/sdk";

const MIN_MOVE_METERS = 25;
type Point = { lat: number; lon: number; ts: number };
const tracks = new Map<string, Point[]>();

function haversineM(a: [number, number], b: [number, number]): number {
  const r = 6_371_000;
  const p1 = (a[0] * Math.PI) / 180;
  const p2 = (b[0] * Math.PI) / 180;
  const dp = ((b[0] - a[0]) * Math.PI) / 180;
  const dl = ((b[1] - a[1]) * Math.PI) / 180;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  return 2 * r * Math.asin(Math.sqrt(h));
}

function appendIfMoved(deviceId: string, lat: number, lon: number): Point[] {
  const trail = tracks.get(deviceId) ?? [];
  const last = trail.length ? ([trail[trail.length - 1].lat, trail[trail.length - 1].lon] as [number, number]) : null;
  if (!last || haversineM(last, [lat, lon]) >= MIN_MOVE_METERS) {
    trail.push({ lat, lon, ts: Date.now() / 1000 });
    tracks.set(deviceId, trail.slice(-200));
  }
  return tracks.get(deviceId) ?? [];
}

const app = new App({ name: "gps-tracker" });

app.screen("/", (req) => {
  const loc = req.ctx.location;
  if (!loc) {
    return Screen({
      id: "scr_nofix",
      title: "GPS tracker",
      body: [Text({ s: "Waiting for GPS fix…", style: "dim" })],
      extras: { subscribe: ["location"] },
    });
  }
  const trail = appendIfMoved(req.ctx.deviceId, loc.lat, loc.lon);
  return Screen({
    id: "scr_track",
    title: "GPS tracker",
    body: [
      Map({ lat: loc.lat, lon: loc.lon, zoom: 15, markers: trail.map((p) => MapMarker({ lat: p.lat, lon: p.lon })) }),
      Text({ s: `${trail.length} points · ±${Math.round(loc.accuracy ?? 0)} m`, style: "dim" }),
      Button({ label: "Clear trail", href: "/clear", method: "POST", confirm: "Erase trail?" }),
    ],
    extras: { subscribe: ["location"] },
  });
});

app.handler("/clear", { method: "POST" }, (req) => {
  tracks.delete(req.ctx.deviceId);
  return Screen({ id: "scr_cleared", body: [] }).redirect("/");
});

if (import.meta.url === `file://${process.argv[1]}`) {
  await app.run();
}
