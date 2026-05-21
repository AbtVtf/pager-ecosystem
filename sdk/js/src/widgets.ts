// Idiomatic widget builders (JS-003).
//
// TypeScript/JS counterpart to sdk/python/pageros/widgets.py. Same
// validation rules, same wire shape, same numeric-vs-string tag
// switch. Constructors take a single options object — the most natural
// shape for TS given the absence of keyword arguments — and freeze the
// instance so a builder may be referenced from multiple handlers
// without aliasing risk.
//
// Tag encoding:
//
// - Default ``toDict()`` emits the **string** widget tag
//   (``{t: "text"}``). This is the canonical Wi-Fi-transport encoding
//   (``protocol/tag-registry.md`` §2.1) and matches every string-form
//   vector under ``protocol/test-vectors/ui/``.
// - ``toDict({numeric: true})`` emits the **numeric** tag
//   (``{t: 1}``). Apps with a tight LoRa budget prefer this form — it
//   shaves bytes per widget map (registry §2.3).
//
// Optional fields are omitted from the emitted dict when left
// unspecified (or set to the canonical default). This matches the
// minimal-vector shape (e.g. a default-level notification serialises
// *without* a ``level`` key).
//
// Handlers may return either a Screen (the typed path — best for
// editor help and refactor safety) or a plain object (the escape
// hatch). Widget instances may also appear nested inside plain
// objects; :func:`toFrameDict` flattens them recursively before CBOR
// encoding so either form works at every level.

import type { Action } from "./frame.js";

// ---------------------------------------------------------------------------
// Tag registries
// ---------------------------------------------------------------------------

// Widget tag registry (protocol/tag-registry.md §3). Pinned to v1.0 of
// the registry so the SDK and the conformance runner agree on numeric
// fallbacks for LoRa-bound Frames.
export const WIDGET_TAGS = {
  text: 1,
  list: 2,
  input: 3,
  form: 4,
  button: 5,
  image: 6,
  map: 7,
  notification: 8,
  presence_list: 9,
  chat: 10,
} as const;

export type WidgetTagName = keyof typeof WIDGET_TAGS;

// Event tag registry (§4). Kept here so callers building manual event
// payloads or `subscribe` arrays don't need to crack open the registry
// file at runtime.
export const EVENT_TAGS = {
  nfc_scan: 1,
  location: 2,
  back: 3,
  tick: 4,
  notification_action: 5,
  member_joined: 16,
  member_left: 17,
  presence_update: 18,
  group_message: 19,
} as const;

export type EventTagName = keyof typeof EVENT_TAGS;

// ---------------------------------------------------------------------------
// Enums + validation
// ---------------------------------------------------------------------------

export type TextStyle = "body" | "heading" | "dim" | "mono";
const TEXT_STYLES: ReadonlyArray<TextStyle> = ["body", "heading", "dim", "mono"];

export type InputType = "text" | "password" | "number" | "email";
const INPUT_TYPES: ReadonlyArray<InputType> = ["text", "password", "number", "email"];

export type NotificationLevel = "info" | "warn" | "error";
const NOTIFICATION_LEVELS: ReadonlyArray<NotificationLevel> = ["info", "warn", "error"];

export type HttpMethod = "GET" | "POST";
const HTTP_METHODS: ReadonlyArray<HttpMethod> = ["GET", "POST"];

const MAX_LATITUDE = 90;
const MAX_LONGITUDE = 180;

function validateNonEmpty(value: unknown, fieldName: string): asserts value is string {
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${fieldName} must be a non-empty string`);
  }
}

function validateEnum<T extends string>(
  value: string,
  allowed: ReadonlyArray<T>,
  fieldName: string,
): asserts value is T {
  if (!allowed.includes(value as T)) {
    throw new Error(
      `${fieldName} must be one of ${JSON.stringify(allowed)}, got ${JSON.stringify(value)}`,
    );
  }
}

function checkLatLon(lat: unknown, lon: unknown, owner: string): void {
  if (typeof lat !== "number" || !Number.isFinite(lat) || lat < -MAX_LATITUDE || lat > MAX_LATITUDE) {
    throw new Error(`${owner}.lat must be a number in [-90, 90]`);
  }
  if (typeof lon !== "number" || !Number.isFinite(lon) || lon < -MAX_LONGITUDE || lon > MAX_LONGITUDE) {
    throw new Error(`${owner}.lon must be a number in [-180, 180]`);
  }
}

function isPositiveInt(n: unknown): n is number {
  return typeof n === "number" && Number.isInteger(n) && n > 0;
}

// ---------------------------------------------------------------------------
// Widget base
// ---------------------------------------------------------------------------

export interface ToDictOptions {
  /** Emit numeric widget tags (`t: 1`) instead of string tags (`t: "text"`). */
  numeric?: boolean;
}

/**
 * Common base for every widget builder. Subclasses override `_tag()`
 * (the string id) and `_toPayload()` (the body sans the `t` field);
 * `toDict()` wraps both with string-or-numeric tag selection.
 */
export abstract class Widget {
  protected abstract _tag(): WidgetTagName;
  protected abstract _toPayload(numeric: boolean): Record<string, unknown>;

  toDict(opts: ToDictOptions = {}): Record<string, unknown> {
    const numeric = opts.numeric ?? false;
    const tag: string | number = numeric ? WIDGET_TAGS[this._tag()] : this._tag();
    return { t: tag, ...this._toPayload(numeric) };
  }
}

// ---------------------------------------------------------------------------
// Leaf widgets
// ---------------------------------------------------------------------------

export interface TextOptions {
  s: string;
  style?: TextStyle;
}

/** `text` widget (SPEC §5.3.1). */
export class Text extends Widget {
  readonly s: string;
  readonly style: TextStyle;

  constructor(opts: TextOptions) {
    super();
    validateNonEmpty(opts.s, "Text.s");
    const style = opts.style ?? "body";
    validateEnum(style, TEXT_STYLES, "Text.style");
    this.s = opts.s;
    this.style = style;
    Object.freeze(this);
  }

  protected _tag(): "text" {
    return "text";
  }

  protected _toPayload(): Record<string, unknown> {
    const out: Record<string, unknown> = { s: this.s };
    // `body` is the default; omit on the wire to match the canonical
    // widget_text_minimal_string vector.
    if (this.style !== "body") out.style = this.style;
    return out;
  }
}

export interface ListItemOptions {
  label: string;
  href?: string;
  sub?: string;
  /** `GET` (default) or `POST`. Case-insensitive on input, normalised on output. */
  method?: string;
}

/**
 * One row of a {@link List} widget. Not a {@link Widget} itself —
 * list items have no `t` discriminator on the wire.
 */
export class ListItem {
  readonly label: string;
  readonly href: string | null;
  readonly sub: string | null;
  readonly method: HttpMethod | null;

  constructor(opts: ListItemOptions) {
    validateNonEmpty(opts.label, "ListItem.label");
    this.label = opts.label;
    this.href = opts.href ?? null;
    this.sub = opts.sub ?? null;
    if (opts.method !== undefined) {
      const m = opts.method.toUpperCase();
      validateEnum(m, HTTP_METHODS, "ListItem.method");
      this.method = m;
    } else {
      this.method = null;
    }
    Object.freeze(this);
  }

  toDict(): Record<string, unknown> {
    const out: Record<string, unknown> = { label: this.label };
    if (this.href !== null) out.href = this.href;
    if (this.sub !== null) out.sub = this.sub;
    if (this.method !== null) out.method = this.method;
    return out;
  }
}

export type ListItemLike = ListItem | Record<string, unknown>;

export interface ListOptions {
  items?: ReadonlyArray<ListItemLike>;
}

/** `list` widget (SPEC §5.3.2). */
export class List extends Widget {
  readonly items: ReadonlyArray<ListItemLike>;

  constructor(opts: ListOptions = {}) {
    super();
    this.items = Object.freeze([...(opts.items ?? [])]);
    Object.freeze(this);
  }

  protected _tag(): "list" {
    return "list";
  }

  protected _toPayload(): Record<string, unknown> {
    const items: Array<Record<string, unknown>> = [];
    for (const item of this.items) {
      if (item instanceof ListItem) {
        items.push(item.toDict());
      } else if (isPlainObject(item)) {
        items.push(item);
      } else {
        throw new TypeError(
          `List.items entries must be ListItem or object, got ${typeOf(item)}`,
        );
      }
    }
    return { items };
  }
}

export interface InputOptions {
  name: string;
  label?: string;
  /** One of `text` (default), `password`, `number`, `email`. */
  type?: InputType;
  value?: unknown;
  max?: number;
}

/** `input` widget (SPEC §5.3.3). Used inside a {@link Form}; standalone use is rare. */
export class Input extends Widget {
  readonly name: string;
  readonly label: string | null;
  readonly type: InputType;
  readonly value: unknown;
  readonly max: number | null;

  constructor(opts: InputOptions) {
    super();
    validateNonEmpty(opts.name, "Input.name");
    const type = opts.type ?? "text";
    validateEnum(type, INPUT_TYPES, "Input.type");
    if (opts.max !== undefined && !isPositiveInt(opts.max)) {
      throw new Error("Input.max must be a positive integer");
    }
    this.name = opts.name;
    this.label = opts.label ?? null;
    this.type = type;
    this.value = opts.value ?? null;
    this.max = opts.max ?? null;
    Object.freeze(this);
  }

  protected _tag(): "input" {
    return "input";
  }

  protected _toPayload(): Record<string, unknown> {
    const out: Record<string, unknown> = { name: this.name, type: this.type };
    if (this.label !== null) out.label = this.label;
    // `null` here means "user did not pass a value"; an explicit `""`
    // is a meaningful pre-filled empty (matches Python behaviour).
    if (this.value !== null) out.value = this.value;
    if (this.max !== null) out.max = this.max;
    return out;
  }
}

export type FormField = Widget | Record<string, unknown>;

export interface FormOptions {
  action: string;
  fields?: ReadonlyArray<FormField>;
  /** `GET` or `POST` (default). Case-insensitive on input, normalised on output. */
  method?: string;
  submit?: string;
}

/** `form` widget (SPEC §5.3.4). */
export class Form extends Widget {
  readonly action: string;
  readonly fields: ReadonlyArray<FormField>;
  readonly method: HttpMethod;
  readonly submit: string | null;

  constructor(opts: FormOptions) {
    super();
    validateNonEmpty(opts.action, "Form.action");
    const rawMethod = (opts.method ?? "POST").toUpperCase();
    validateEnum(rawMethod, HTTP_METHODS, "Form.method");
    this.action = opts.action;
    this.method = rawMethod;
    this.fields = Object.freeze([...(opts.fields ?? [])]);
    this.submit = opts.submit ?? null;
    Object.freeze(this);
  }

  protected _tag(): "form" {
    return "form";
  }

  protected _toPayload(numeric: boolean): Record<string, unknown> {
    const fields: Array<Record<string, unknown>> = [];
    for (const f of this.fields) {
      if (f instanceof Widget) {
        fields.push(f.toDict({ numeric }));
      } else if (isPlainObject(f)) {
        fields.push(f);
      } else {
        throw new TypeError(
          `Form.fields entries must be Widget or object, got ${typeOf(f)}`,
        );
      }
    }
    const out: Record<string, unknown> = {
      action: this.action,
      method: this.method,
      fields,
    };
    if (this.submit !== null) out.submit = this.submit;
    return out;
  }
}

export interface ButtonOptions {
  label: string;
  href: string;
  /** `GET` (default) or `POST`. Case-insensitive on input, normalised on output. */
  method?: string;
  confirm?: string;
}

/** `button` widget (SPEC §5.3.5). */
export class Button extends Widget {
  readonly label: string;
  readonly href: string;
  readonly method: HttpMethod;
  readonly confirm: string | null;

  constructor(opts: ButtonOptions) {
    super();
    validateNonEmpty(opts.label, "Button.label");
    validateNonEmpty(opts.href, "Button.href");
    const rawMethod = (opts.method ?? "GET").toUpperCase();
    validateEnum(rawMethod, HTTP_METHODS, "Button.method");
    this.label = opts.label;
    this.href = opts.href;
    this.method = rawMethod;
    this.confirm = opts.confirm ?? null;
    Object.freeze(this);
  }

  protected _tag(): "button" {
    return "button";
  }

  protected _toPayload(): Record<string, unknown> {
    const out: Record<string, unknown> = {
      label: this.label,
      href: this.href,
      method: this.method,
    };
    if (this.confirm !== null) out.confirm = this.confirm;
    return out;
  }
}

export interface ImageOptions {
  /** Content-addressed reference: `img:<sha256-prefix>` (SPEC §5.6). */
  src: string;
  w?: number;
  h?: number;
  alt?: string;
}

/** `image` widget (SPEC §5.3.6). */
export class Image extends Widget {
  readonly src: string;
  readonly w: number | null;
  readonly h: number | null;
  readonly alt: string | null;

  constructor(opts: ImageOptions) {
    super();
    validateNonEmpty(opts.src, "Image.src");
    if (!opts.src.startsWith("img:")) {
      throw new Error("Image.src must be content-addressed: img:<sha256-prefix>");
    }
    for (const [name, val] of [["w", opts.w], ["h", opts.h]] as const) {
      if (val !== undefined && !isPositiveInt(val)) {
        throw new Error(`Image.${name} must be a positive integer when set`);
      }
    }
    this.src = opts.src;
    this.w = opts.w ?? null;
    this.h = opts.h ?? null;
    this.alt = opts.alt ?? null;
    Object.freeze(this);
  }

  protected _tag(): "image" {
    return "image";
  }

  protected _toPayload(): Record<string, unknown> {
    const out: Record<string, unknown> = { src: this.src };
    if (this.w !== null) out.w = this.w;
    if (this.h !== null) out.h = this.h;
    if (this.alt !== null) out.alt = this.alt;
    return out;
  }
}

export interface MapMarkerOptions {
  lat: number;
  lon: number;
  label?: string;
}

/** One pin on a {@link Map} widget. No `t` discriminator on the wire. */
export class MapMarker {
  readonly lat: number;
  readonly lon: number;
  readonly label: string | null;

  constructor(opts: MapMarkerOptions) {
    checkLatLon(opts.lat, opts.lon, "MapMarker");
    this.lat = opts.lat;
    this.lon = opts.lon;
    this.label = opts.label ?? null;
    Object.freeze(this);
  }

  toDict(): Record<string, unknown> {
    const out: Record<string, unknown> = { lat: this.lat, lon: this.lon };
    if (this.label !== null) out.label = this.label;
    return out;
  }
}

export type MapMarkerLike = MapMarker | Record<string, unknown>;

export interface MapOptions {
  lat: number;
  lon: number;
  /** OSM tile zoom 0..19 (SPEC §5.3.7). */
  zoom?: number;
  markers?: ReadonlyArray<MapMarkerLike>;
}

/** `map` widget (SPEC §5.3.7). */
export class Map_ extends Widget {
  readonly lat: number;
  readonly lon: number;
  readonly zoom: number | null;
  readonly markers: ReadonlyArray<MapMarkerLike>;

  constructor(opts: MapOptions) {
    super();
    checkLatLon(opts.lat, opts.lon, "Map");
    if (opts.zoom !== undefined) {
      if (!Number.isInteger(opts.zoom) || opts.zoom < 0 || opts.zoom > 19) {
        throw new Error("Map.zoom must be an integer in [0, 19]");
      }
    }
    this.lat = opts.lat;
    this.lon = opts.lon;
    this.zoom = opts.zoom ?? null;
    this.markers = Object.freeze([...(opts.markers ?? [])]);
    Object.freeze(this);
  }

  protected _tag(): "map" {
    return "map";
  }

  protected _toPayload(): Record<string, unknown> {
    const out: Record<string, unknown> = { lat: this.lat, lon: this.lon };
    if (this.zoom !== null) out.zoom = this.zoom;
    if (this.markers.length > 0) {
      const markers: Array<Record<string, unknown>> = [];
      for (const m of this.markers) {
        if (m instanceof MapMarker) {
          markers.push(m.toDict());
        } else if (isPlainObject(m)) {
          markers.push(m);
        } else {
          throw new TypeError(
            `Map.markers entries must be MapMarker or object, got ${typeOf(m)}`,
          );
        }
      }
      out.markers = markers;
    }
    return out;
  }
}

// `Map` is a JS built-in; expose the widget under both names so callers
// may choose whichever reads better at the import site.
export { Map_ as Map };

export interface NotificationOptions {
  s: string;
  /** One of `info` (default), `warn`, `error`. */
  level?: NotificationLevel;
}

/** `notification` widget (SPEC §5.3.8). */
export class Notification extends Widget {
  readonly s: string;
  readonly level: NotificationLevel;

  constructor(opts: NotificationOptions) {
    super();
    validateNonEmpty(opts.s, "Notification.s");
    const level = opts.level ?? "info";
    validateEnum(level, NOTIFICATION_LEVELS, "Notification.level");
    this.s = opts.s;
    this.level = level;
    Object.freeze(this);
  }

  protected _tag(): "notification" {
    return "notification";
  }

  protected _toPayload(): Record<string, unknown> {
    const out: Record<string, unknown> = { s: this.s };
    // Default-level omitted to match widget_notification_info_string.
    if (this.level !== "info") out.level = this.level;
    return out;
  }
}

export interface PresenceMemberOptions {
  id: string;
  name: string;
  online: boolean;
}

/** One member entry inside a {@link PresenceList}. */
export class PresenceMember {
  readonly id: string;
  readonly name: string;
  readonly online: boolean;

  constructor(opts: PresenceMemberOptions) {
    validateNonEmpty(opts.id, "PresenceMember.id");
    validateNonEmpty(opts.name, "PresenceMember.name");
    if (typeof opts.online !== "boolean") {
      throw new TypeError("PresenceMember.online must be boolean");
    }
    this.id = opts.id;
    this.name = opts.name;
    this.online = opts.online;
    Object.freeze(this);
  }

  toDict(): Record<string, unknown> {
    return { id: this.id, name: this.name, online: this.online };
  }
}

export type PresenceMemberLike = PresenceMember | Record<string, unknown>;

export interface PresenceListOptions {
  groupId: string;
  members?: ReadonlyArray<PresenceMemberLike>;
}

/** `presence_list` widget (SPEC §5.3.9). */
export class PresenceList extends Widget {
  readonly groupId: string;
  readonly members: ReadonlyArray<PresenceMemberLike>;

  constructor(opts: PresenceListOptions) {
    super();
    validateNonEmpty(opts.groupId, "PresenceList.groupId");
    this.groupId = opts.groupId;
    this.members = Object.freeze([...(opts.members ?? [])]);
    Object.freeze(this);
  }

  protected _tag(): "presence_list" {
    return "presence_list";
  }

  protected _toPayload(): Record<string, unknown> {
    const members: Array<Record<string, unknown>> = [];
    for (const m of this.members) {
      if (m instanceof PresenceMember) {
        members.push(m.toDict());
      } else if (isPlainObject(m)) {
        members.push(m);
      } else {
        throw new TypeError(
          `PresenceList.members entries must be PresenceMember or object, got ${typeOf(m)}`,
        );
      }
    }
    return { group_id: this.groupId, members };
  }
}

export interface ChatMessageOptions {
  /** Renamed to `from` on the wire (`from` is a JS reserved word). */
  from: string;
  s: string;
  ts: number;
}

/** One message bubble inside a {@link Chat} widget. */
export class ChatMessage {
  readonly from: string;
  readonly s: string;
  readonly ts: number;

  constructor(opts: ChatMessageOptions) {
    validateNonEmpty(opts.from, "ChatMessage.from");
    validateNonEmpty(opts.s, "ChatMessage.s");
    if (!Number.isInteger(opts.ts) || opts.ts < 0) {
      throw new Error("ChatMessage.ts must be a non-negative integer");
    }
    this.from = opts.from;
    this.s = opts.s;
    this.ts = opts.ts;
    Object.freeze(this);
  }

  toDict(): Record<string, unknown> {
    return { from: this.from, s: this.s, ts: this.ts };
  }
}

export interface ChatComposeOptions {
  name: string;
  submit: string;
}

/** The inline composer on a {@link Chat} widget. */
export class ChatCompose {
  readonly name: string;
  readonly submit: string;

  constructor(opts: ChatComposeOptions) {
    validateNonEmpty(opts.name, "ChatCompose.name");
    validateNonEmpty(opts.submit, "ChatCompose.submit");
    this.name = opts.name;
    this.submit = opts.submit;
    Object.freeze(this);
  }

  toDict(): Record<string, unknown> {
    return { name: this.name, submit: this.submit };
  }
}

export type ChatMessageLike = ChatMessage | Record<string, unknown>;
export type ChatComposeLike = ChatCompose | Record<string, unknown>;

export interface ChatOptions {
  groupId: string;
  messages?: ReadonlyArray<ChatMessageLike>;
  compose?: ChatComposeLike;
}

/** `chat` widget (SPEC §5.3.10). */
export class Chat extends Widget {
  readonly groupId: string;
  readonly messages: ReadonlyArray<ChatMessageLike>;
  readonly compose: ChatComposeLike | null;

  constructor(opts: ChatOptions) {
    super();
    validateNonEmpty(opts.groupId, "Chat.groupId");
    this.groupId = opts.groupId;
    this.messages = Object.freeze([...(opts.messages ?? [])]);
    this.compose = opts.compose ?? null;
    Object.freeze(this);
  }

  protected _tag(): "chat" {
    return "chat";
  }

  protected _toPayload(): Record<string, unknown> {
    const messages: Array<Record<string, unknown>> = [];
    for (const m of this.messages) {
      if (m instanceof ChatMessage) {
        messages.push(m.toDict());
      } else if (isPlainObject(m)) {
        messages.push(m);
      } else {
        throw new TypeError(
          `Chat.messages entries must be ChatMessage or object, got ${typeOf(m)}`,
        );
      }
    }
    const out: Record<string, unknown> = {
      group_id: this.groupId,
      messages,
    };
    if (this.compose !== null) {
      if (this.compose instanceof ChatCompose) {
        out.compose = this.compose.toDict();
      } else if (isPlainObject(this.compose)) {
        out.compose = this.compose;
      } else {
        throw new TypeError(
          `Chat.compose must be ChatCompose or object, got ${typeOf(this.compose)}`,
        );
      }
    }
    return out;
  }
}

// ---------------------------------------------------------------------------
// Screen / Frame builder
// ---------------------------------------------------------------------------

export type ScreenBodyEntry = Widget | Record<string, unknown>;

export interface ScreenOptions {
  id: string;
  body?: ReadonlyArray<ScreenBodyEntry>;
  title?: string;
  /** `0` is allowed and explicitly means "do not cache" (SPEC §5.5). */
  ttl?: number;
  actions?: ReadonlyArray<Action>;
  /**
   * Device-event subscriptions for this screen. Accepts canonical
   * string event names (`"nfc_scan"`) or numeric tag ids (`1`); both
   * are pinned by `protocol/tag-registry.md` §4.
   */
  subscribe?: ReadonlyArray<string | number>;
  subscribeGroups?: ReadonlyArray<string>;
  version?: number;
}

/**
 * Top-level Frame builder (SPEC §5.2). `id` is the only field a
 * typical screen handler must set — the others default to omitted to
 * match the canonical minimal Frame shape.
 */
export class Screen {
  readonly id: string;
  readonly body: ReadonlyArray<ScreenBodyEntry>;
  readonly title: string | null;
  readonly ttl: number | null;
  readonly actions: ReadonlyArray<Action>;
  readonly subscribe: ReadonlyArray<string | number>;
  readonly subscribeGroups: ReadonlyArray<string>;
  readonly version: number;

  constructor(opts: ScreenOptions) {
    validateNonEmpty(opts.id, "Screen.id");
    const version = opts.version ?? 1;
    if (!Number.isInteger(version) || version <= 0) {
      throw new Error("Screen.version must be a positive integer");
    }
    if (opts.ttl !== undefined) {
      if (!Number.isInteger(opts.ttl) || opts.ttl < 0) {
        throw new Error("Screen.ttl must be a non-negative integer");
      }
    }
    this.id = opts.id;
    this.body = Object.freeze([...(opts.body ?? [])]);
    this.title = opts.title ?? null;
    this.ttl = opts.ttl ?? null;
    this.actions = Object.freeze([...(opts.actions ?? [])]);
    this.subscribe = Object.freeze([...(opts.subscribe ?? [])]);
    this.subscribeGroups = Object.freeze([...(opts.subscribeGroups ?? [])]);
    this.version = version;
    Object.freeze(this);
  }

  toDict(opts: ToDictOptions = {}): Record<string, unknown> {
    const numeric = opts.numeric ?? false;
    const out: Record<string, unknown> = {
      v: this.version,
      id: this.id,
      body: this.body.map((b) => toFrameDict(b, { numeric })),
    };
    if (this.title !== null) out.title = this.title;
    if (this.ttl !== null) out.ttl = this.ttl;
    if (this.actions.length > 0) {
      out.actions = this.actions.map((a) => toFrameDict(a, { numeric }));
    }
    if (this.subscribe.length > 0) out.subscribe = [...this.subscribe];
    if (this.subscribeGroups.length > 0) out.subscribe_groups = [...this.subscribeGroups];
    return out;
  }
}

// ---------------------------------------------------------------------------
// Recursive flattener
// ---------------------------------------------------------------------------

/**
 * Recursively flatten Widget / Screen instances into plain JSON-shaped
 * values. Plain objects, arrays, and `Map` instances are walked so
 * widgets may appear at any nesting depth inside a handler return
 * value. Scalars and opaque values (`Uint8Array`, `bigint`, etc.) pass
 * through unchanged so canonical CBOR encoding still sees the original
 * type information.
 */
export function toFrameDict(value: unknown, opts: ToDictOptions = {}): unknown {
  const numeric = opts.numeric ?? false;
  if (value instanceof Widget) {
    return toFrameDict(value.toDict({ numeric }), { numeric });
  }
  if (value instanceof Screen) {
    return value.toDict({ numeric });
  }
  if (Array.isArray(value)) {
    return value.map((v) => toFrameDict(v, { numeric }));
  }
  if (isPlainObject(value)) {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value)) {
      out[k] = toFrameDict(v, { numeric });
    }
    return out;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

function isPlainObject(value: unknown): value is Record<string, unknown> {
  if (value === null || typeof value !== "object") return false;
  if (Array.isArray(value)) return false;
  if (value instanceof Widget) return false;
  if (value instanceof Screen) return false;
  if (value instanceof ListItem) return false;
  if (value instanceof MapMarker) return false;
  if (value instanceof PresenceMember) return false;
  if (value instanceof ChatMessage) return false;
  if (value instanceof ChatCompose) return false;
  if (value instanceof Uint8Array) return false;
  if (value instanceof globalThis.Map) return false;
  if (value instanceof Set) return false;
  const proto = Object.getPrototypeOf(value);
  return proto === null || proto === Object.prototype;
}

function typeOf(value: unknown): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return "array";
  return typeof value;
}
