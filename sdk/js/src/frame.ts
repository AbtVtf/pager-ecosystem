// Frame and widget shapes (SPEC §4). Producer-side types only — the
// renderer side ships in firmware. These are intentionally permissive
// (`[key: string]: unknown` escape hatches) so SDK users can extend
// widgets ahead of v1 widget catalog expansions without recompiling.
//
// `FrameWidget` describes the loose "any widget map on the wire" shape
// that the dispatcher accepts in a Frame body. The named builder
// classes in `widgets.ts` (Text, List, etc.) produce values conforming
// to this shape via their `toDict()` method.

export interface Frame {
  v: 1;
  id: string;
  ttl?: number;
  title?: string;
  body: FrameWidget[];
  actions?: Action[];
  subscribe?: Array<number | string>;
  subscribe_groups?: string[];
  meta?: Record<string, unknown>;
  [key: string]: unknown;
}

export interface FrameWidget {
  t: string | number;
  [key: string]: unknown;
}

export interface Action {
  // SPEC §5.12 — top-bar / soft-key actions. v1 leaves the shape open;
  // SDK users construct these as widget-style maps.
  [key: string]: unknown;
}
