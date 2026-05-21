// PagerOS chat example (DOCS-010, TS counterpart). See app.py for the
// authoritative implementation — this mirrors the surface for parity
// once the JS SDK widget builders (JS-003) and group helpers (JS-007)
// land.

import {
  App, Button, Chat, ChatCompose, ChatMessage,
  Form, Input, List, ListItem, Notification, PresenceList, PresenceMember, Screen, Text,
} from "@pageros/sdk";

const MAX_HISTORY = 200;
type Room = { members: string[]; history: { from: string; s: string; ts: number }[] };
const rooms = new Map<string, Room>();
const memberships = new Map<string, Set<string>>();
const online = new Map<string, Set<string>>();

const app = new App({ name: "chat" });

app.screen("/", (req) => {
  const mine = [...(memberships.get(req.ctx.deviceId) ?? new Set<string>())];
  if (mine.length === 0) {
    return Screen({
      id: "scr_no_rooms",
      title: "Chat",
      body: [
        Text({ s: "You aren't in any rooms yet.", style: "dim" }),
        Button({ label: "Create room", href: "/new", method: "POST" }),
        Button({ label: "Join by code", href: "/join" }),
      ],
    });
  }
  return Screen({
    id: "scr_rooms",
    title: "Chat",
    body: [
      List({
        items: mine.map((gid) =>
          ListItem({ label: gid, sub: `${rooms.get(gid)?.members.length ?? 0} members`, href: `/room/${gid}` }),
        ),
      }),
      Button({ label: "Create room", href: "/new", method: "POST" }),
      Button({ label: "Join by code", href: "/join" }),
    ],
  });
});

app.handler("/new", { method: "POST" }, (req) => {
  const code = Array.from(crypto.getRandomValues(new Uint8Array(3)))
    .map((b) => b.toString(16).padStart(2, "0")).join("").toUpperCase();
  rooms.set(code, { members: [req.ctx.deviceId], history: [] });
  const ms = memberships.get(req.ctx.deviceId) ?? new Set<string>();
  ms.add(code);
  memberships.set(req.ctx.deviceId, ms);
  return Screen({ id: "scr_rooms", body: [] }).redirect(`/room/${code}`);
});

app.screen("/join", () =>
  Screen({
    id: "scr_join",
    title: "Join a room",
    body: [
      Form({ action: "/join", method: "POST", fields: [Input({ name: "code", label: "Invite code", max: 6 })], submit: "Join" }),
    ],
  }),
);

app.handler("/join", { method: "POST" }, (req, payload: { code?: string }) => {
  const code = (payload.code ?? "").toUpperCase().trim();
  if (!rooms.has(code)) {
    return Screen({
      id: "scr_join", title: "Join a room",
      body: [
        Notification({ s: `No room with code "${code}"`, level: "error" }),
        Form({ action: "/join", method: "POST", fields: [Input({ name: "code", label: "Invite code", max: 6 })], submit: "Join" }),
      ],
    });
  }
  const room = rooms.get(code)!;
  if (!room.members.includes(req.ctx.deviceId)) {
    room.members.push(req.ctx.deviceId);
    const ms = memberships.get(req.ctx.deviceId) ?? new Set<string>();
    ms.add(code);
    memberships.set(req.ctx.deviceId, ms);
  }
  return Screen({ id: "scr_rooms", body: [] }).redirect(`/room/${code}`);
});

app.screen("/room/:group_id", (req, { group_id }) => {
  const room = rooms.get(group_id);
  if (!room || !room.members.includes(req.ctx.deviceId)) {
    return Screen({ id: "scr_404", title: "Chat", body: [Notification({ s: "Room not found.", level: "error" })] });
  }
  const here = online.get(group_id) ?? new Set<string>();
  here.add(req.ctx.deviceId);
  online.set(group_id, here);
  return Screen({
    id: `scr_room_${group_id}`,
    title: `#${group_id}`,
    body: [
      PresenceList({
        group_id,
        members: room.members.map((did) => PresenceMember({ id: did, name: did.slice(0, 8), online: here.has(did) })),
      }),
      Chat({
        group_id,
        messages: room.history.slice(-MAX_HISTORY).map((m) => ChatMessage({ from: m.from, s: m.s, ts: m.ts })),
        compose: ChatCompose({ name: "msg", submit: `/room/${group_id}/send` }),
      }),
    ],
    extras: { subscribe_groups: [group_id] },
  });
});

app.handler("/room/:group_id/send", { method: "POST" }, (req, payload: { msg?: string }, { group_id }) => {
  const text = (payload.msg ?? "").trim();
  if (!text) return Screen({ id: "scr_noop", body: [] }).redirect(`/room/${group_id}`);
  const room = rooms.get(group_id);
  if (!room || !room.members.includes(req.ctx.deviceId)) {
    return Screen({ id: "scr_403", title: "Chat", body: [Notification({ s: "Not a member.", level: "error" })] });
  }
  room.history.push({ from: req.ctx.deviceId.slice(0, 8), s: text, ts: Math.floor(Date.now() / 1000) });
  if (room.history.length > MAX_HISTORY) room.history = room.history.slice(-MAX_HISTORY);
  return Screen({ id: "scr_sent", body: [] }).redirect(`/room/${group_id}`);
});

if (import.meta.url === `file://${process.argv[1]}`) {
  await app.run();
}
