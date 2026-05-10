// Opt-in layout instrumentation. When enabled, layout-aware components
// (View, Text, Button, …) push a structured record describing the rect
// they produced and the inputs that fed it. Off by default (one boolean
// check per component render), so it's safe to leave wired in.
//
// Designed to be driven from outside V8 — the debug-protocol Python SDK
// exposes `page.layout_trace_*` thin wrappers that toggle the flag and
// drain the buffer via `Runtime.evaluate`.
//
// The shape is deliberately small and JSON-serialisable so the wire
// payload stays cheap.

import type { LayoutResult } from '../layout/types.ts';
import type { Style } from '../style/types.ts';

export interface LayoutTraceEntry {
  /** Component or primitive name — `'View'`, `'Text'`, etc. */
  component: string;
  /** Final absolute rect (after parent positioning + own layout). */
  rect: LayoutResult;
  /** Whether the parent provided a precomputed `__layout`. */
  hasParentLayout: boolean;
  /** Raw style width/height as authored (number, `'100%'`, `undefined`). */
  styleWidth: Style['width'];
  styleHeight: Style['height'];
  /** Optional caller-supplied tag — useful for grouping multiple Views. */
  tag?: string;
}

interface State {
  enabled: boolean;
  buffer: LayoutTraceEntry[];
  limit: number;
}

function state(): State {
  let s = globalThis.__fxeLayoutTrace;
  if (!s) {
    s = { enabled: false, buffer: [], limit: 1000 };
    globalThis.__fxeLayoutTrace = s;
  }
  return s;
}

/** Toggle layout tracing. When disabled, `record()` is a no-op. */
export function setLayoutTraceEnabled(on: boolean, opts?: { limit?: number }): void {
  const s = state();
  s.enabled = on;
  if (opts?.limit !== undefined) s.limit = Math.max(1, opts.limit);
  if (on) s.buffer.length = 0;
}

/** Read accumulated entries; pass `clear: true` (default) to reset the buffer. */
export function drainLayoutTrace(clear = true): LayoutTraceEntry[] {
  const s = state();
  const out = s.buffer.slice();
  if (clear) s.buffer.length = 0;
  return out;
}

/** Push an entry. Cheap when disabled (single boolean check). */
export function recordLayout(entry: LayoutTraceEntry): void {
  const s = state();
  if (!s.enabled) return;
  if (s.buffer.length >= s.limit) s.buffer.shift();
  // Strip the recursive `children` tree — drain payloads explode otherwise,
  // and the caller only needs this node's own rect.
  const flat: LayoutResult = { ...entry.rect, children: [] };
  s.buffer.push({ ...entry, rect: flat });
}
