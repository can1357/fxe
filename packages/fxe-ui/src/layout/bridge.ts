// Bridge from fxe-ui's LayoutNode tree to the native fxe::layout solver.
// Builds a NodeDescriptor on each call (no caching — Yoga handles its own
// dirtying internally; if reconciler-level memo is later needed, layer it
// at the fiber boundary).

import type { TaggedMeasureFn } from './measure.ts';
import type { Constraint, LayoutNode, LayoutResult, LayoutStyle, Length } from './types.ts';

const NativeLayout = (
  globalThis as { Layout?: { solve: (root: unknown, constraint?: Constraint) => LayoutResult } }
).Layout;

if (NativeLayout === undefined) {
  throw new Error('fxe-ui: native Layout binding not available — link fxe_layout into fxe_js');
}

export function resolveLength(v: Length | undefined, parent: number | undefined, auto = 0): number {
  if (v === undefined || v === 'auto') return auto;
  if (typeof v === 'number') return v;
  if (typeof v === 'string' && v.endsWith('%')) {
    if (parent === undefined) return auto;
    return (Number.parseFloat(v) / 100) * parent;
  }
  throw new TypeError(`unsupported length: ${String(v)}`);
}

interface MeasureDescriptor {
  kind: 'text' | 'image' | 'js';
  text?: string;
  fontSize?: number;
  width?: number;
  height?: number;
  fn?: (c: Constraint) => { width: number; height: number };
}

interface NodeDescriptor {
  style?: LayoutStyle;
  children?: NodeDescriptor[];
  measure?: MeasureDescriptor;
}

function buildDescriptor(node: LayoutNode): NodeDescriptor {
  const out: NodeDescriptor = {};
  if (node.style !== undefined) out.style = node.style;
  const kids = node.children;
  if (kids !== undefined && kids.length > 0) {
    const arr: NodeDescriptor[] = new Array(kids.length);
    for (let i = 0; i < kids.length; ++i) arr[i] = buildDescriptor(kids[i]);
    out.children = arr;
  }
  const m = node.measure as TaggedMeasureFn | undefined;
  if (m !== undefined) {
    if (m.__fxeMeasureKind === 'text') {
      out.measure = {
        kind: 'text',
        text: m.__fxeMeasureText ?? '',
        fontSize: m.__fxeMeasureFontSize ?? 16,
      };
    } else if (m.__fxeMeasureKind === 'image') {
      out.measure = {
        kind: 'image',
        width: m.__fxeMeasureWidth ?? 0,
        height: m.__fxeMeasureHeight ?? 0,
      };
    } else {
      out.measure = { kind: 'js', fn: m };
    }
  }
  return out;
}

export function solveLayout(root: LayoutNode, available: Constraint = {}): LayoutResult {
  const desc = buildDescriptor(root);
  return NativeLayout.solve(desc, available);
}

export { solveLayout as layout };
