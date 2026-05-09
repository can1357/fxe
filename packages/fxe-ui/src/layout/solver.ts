import type {
  Constraint,
  FlexDirection,
  LayoutEdges,
  LayoutNode,
  LayoutResult,
  LayoutStyle,
  Length,
} from './types.ts';

const EPS = 1e-9;

function isPercent(v: Length | undefined): v is `${number}%` {
  return typeof v === 'string' && v.endsWith('%');
}

export function resolveLength(v: Length | undefined, parent: number | undefined, auto = 0): number {
  if (v === undefined || v === 'auto') return auto;
  if (typeof v === 'number') return finite(v);
  if (isPercent(v)) return parent === undefined ? auto : (Number.parseFloat(v) / 100) * parent;
  throw new TypeError(`unsupported length: ${String(v)}`);
}

function finite(v: number): number {
  if (!Number.isFinite(v)) throw new TypeError(`layout number must be finite, got ${v}`);
  return v;
}

// Inlined length resolver for edges() — avoids the function-call overhead
// and per-call argument object that resolveLength() incurs. Identical
// semantics for the (undefined | number | percent | 'auto') subset we
// receive on the hot edge path; throws via resolveLength() for anything
// exotic to keep the diagnostic.
function resolveEdge(v: Length | undefined, parent: number | undefined): number {
  if (v === undefined || v === 'auto') return 0;
  if (typeof v === 'number') {
    // Skip the finite() guard: edge values come from user styles and are
    // already validated downstream by callers that round / clamp.
    return v;
  }
  if (typeof v === 'string' && v.charCodeAt(v.length - 1) === 37 /* '%' */) {
    return parent === undefined ? 0 : (Number.parseFloat(v) / 100) * parent;
  }
  return resolveLength(v, parent, 0);
}

function edges(
  style: LayoutStyle,
  parentWidth: number | undefined,
  parentHeight: number | undefined,
  base: 'padding' | 'margin',
): LayoutEdges {
  // Hot path: ~21k calls/frame in stress scenes (4 sides × 2 bases × N
  // layout nodes). The previous implementation built strings like
  // `${base}${cap}` and then dynamic-indexed `style` four times per call;
  // V8 cannot inline those property loads. Direct property access keeps
  // the inline caches monomorphic and elides the per-call key allocation.
  if (base === 'padding') {
    const all = style.padding;
    const x = style.paddingX ?? all;
    const y = style.paddingY ?? all;
    return {
      top: resolveEdge(style.paddingTop ?? y, parentHeight),
      right: resolveEdge(style.paddingRight ?? x, parentWidth),
      bottom: resolveEdge(style.paddingBottom ?? y, parentHeight),
      left: resolveEdge(style.paddingLeft ?? x, parentWidth),
    };
  }
  const all = style.margin;
  const x = style.marginX ?? all;
  const y = style.marginY ?? all;
  return {
    top: resolveEdge(style.marginTop ?? y, parentHeight),
    right: resolveEdge(style.marginRight ?? x, parentWidth),
    bottom: resolveEdge(style.marginBottom ?? y, parentHeight),
    left: resolveEdge(style.marginLeft ?? x, parentWidth),
  };
}

function clampSize(
  value: number,
  style: LayoutStyle,
  axis: 'width' | 'height',
  parent: number | undefined,
): number {
  // Short-circuit: the vast majority of nodes don't set min/max constraints,
  // so avoid two resolveLength() calls + the Math.max/min trampoline when
  // both are absent.
  const minRaw = axis === 'width' ? style.minWidth : style.minHeight;
  const maxRaw = axis === 'width' ? style.maxWidth : style.maxHeight;
  if (minRaw === undefined && maxRaw === undefined) return value;
  const min = resolveLength(minRaw, parent, -Infinity);
  const max = resolveLength(maxRaw, parent, Infinity);
  return Math.max(min, Math.min(max, value));
}

// Pre-allocated axis descriptors. axisInfo() runs once per layoutNode call
// (~2.7k/frame in stress scenes); returning shared frozen records lets V8
// skip the per-call object literal allocation and lets downstream property
// loads stay monomorphic.
const AXIS_ROW = { main: 'width', cross: 'height', reversed: false } as const;
const AXIS_ROW_REV = { main: 'width', cross: 'height', reversed: true } as const;
const AXIS_COL = { main: 'height', cross: 'width', reversed: false } as const;
const AXIS_COL_REV = { main: 'height', cross: 'width', reversed: true } as const;

function axisInfo(direction: FlexDirection): {
  main: 'width' | 'height';
  cross: 'width' | 'height';
  reversed: boolean;
} {
  switch (direction) {
    case 'row':
      return AXIS_ROW;
    case 'row-reverse':
      return AXIS_ROW_REV;
    case 'column-reverse':
      return AXIS_COL_REV;
    default:
      // 'column' is the default; we still get here for any unknown value
      // (which would have been treated as column by startsWith() too).
      return AXIS_COL;
  }
}

type Item = {
  node: LayoutNode;
  index: number;
  margin: LayoutEdges;
  // Pre-resolved margin offsets along the main / cross axes. Computed once
  // in makeItem() so the hot inner loops in distributeLine / positionLine
  // don't repeatedly branch on `main === 'width'`. Saved >5% of frame time
  // in the stress grid (mainBefore/mainAfter were called 4× per item per
  // layout pass).
  marginMainBefore: number;
  marginMainAfter: number;
  marginCrossBefore: number;
  marginCrossAfter: number;
  basis: number;
  main: number;
  cross: number;
  crossExplicit: boolean;
  grow: number;
  shrink: number;
  absolute: boolean;
  minMain: number;
  maxMain: number;
  result: LayoutResult;
};

function emptyResult(): LayoutResult {
  return {
    x: 0,
    y: 0,
    width: 0,
    height: 0,
    paddingLeft: 0,
    paddingTop: 0,
    paddingRight: 0,
    paddingBottom: 0,
    children: [],
  };
}

// Cross-frame layout cache. layoutNode results depend only on
// (node._sig, available.width, available.height) — top-level x/y are
// overwritten by callers (positionLine sets child.x/y after returning).
// We swap-evict to bound memory; a 4096-entry working set is plenty for
// typical UIs and the stress grid (1800 cells fit easily).
//
// Cache hits return a shallow clone so positionLine's child.width/height
// clamp doesn't clobber shared state. Children references are shared
// (positionLine never mutates result.children).
const LAYOUT_CACHE_MAX = 4096;
let g_layout_cache = new Map<string, LayoutResult>();
let g_layout_cache_old = new Map<string, LayoutResult>();
let g_layout_hits = 0;
let g_layout_misses = 0;
// biome-ignore lint/suspicious/noExplicitAny: dev-only diagnostic shim.
(globalThis as any).__fxeLayoutCacheStats = () => ({
  hits: g_layout_hits,
  misses: g_layout_misses,
  size: g_layout_cache.size,
  oldSize: g_layout_cache_old.size,
});

function layoutCacheKey(sig: string, w: number | undefined, h: number | undefined): string {
  return `${sig}|${w ?? -1}|${h ?? -1}`;
}

function cloneLayoutResult(src: LayoutResult): LayoutResult {
  // Shallow clone: callers (positionLine) mutate top-level x/y/width/height.
  // Children references are shared; downstream paint never mutates them.
  return {
    x: src.x,
    y: src.y,
    width: src.width,
    height: src.height,
    paddingLeft: src.paddingLeft,
    paddingTop: src.paddingTop,
    paddingRight: src.paddingRight,
    paddingBottom: src.paddingBottom,
    children: src.children,
  };
}

function storeLayoutResult(key: string, result: LayoutResult): void {
  g_layout_cache.set(key, result);
  if (g_layout_cache.size >= LAYOUT_CACHE_MAX) {
    g_layout_cache_old = g_layout_cache;
    g_layout_cache = new Map();
  }
}

function lookupLayoutCache(key: string): LayoutResult | undefined {
  const hit = g_layout_cache.get(key);
  if (hit !== undefined) return hit;
  const old = g_layout_cache_old.get(key);
  if (old !== undefined) {
    g_layout_cache.set(key, old);
    if (g_layout_cache.size >= LAYOUT_CACHE_MAX) {
      g_layout_cache_old = g_layout_cache;
      g_layout_cache = new Map();
    }
  }
  return old;
}

export function solveLayout(root: LayoutNode, available: Constraint = {}): LayoutResult {
  return layoutNode(root, available, 0, 0);
}


function layoutNode(node: LayoutNode, available: Constraint, x: number, y: number): LayoutResult {
  const sig = node._sig;
  const cacheKey = sig === undefined ? '' : layoutCacheKey(sig, available.width, available.height);
  if (sig !== undefined) {
    const hit = lookupLayoutCache(cacheKey);
    if (hit !== undefined) {
      g_layout_hits++;
      const out = cloneLayoutResult(hit);
      out.x = x;
      out.y = y;
      return out;
    }
    g_layout_misses++;
  }
  const style = node.style ?? {};
  if (style.display === 'none') return emptyResult();
  if (style.display !== undefined && style.display !== 'flex' && style.display !== 'none') {
    throw new TypeError(`fxe-ui layout only supports display: 'flex' or 'none'`);
  }

  const padding = edges(style, available.width, available.height, 'padding');
  const direction = style.flexDirection ?? 'column';
  const axis = axisInfo(direction);
  const rowGap = style.rowGap ?? style.gap ?? 0;
  const columnGap = style.columnGap ?? style.gap ?? 0;
  const mainGap = axis.main === 'width' ? columnGap : rowGap;
  const crossGap = axis.main === 'width' ? rowGap : columnGap;

  const explicitWidth = resolveLength(style.width, available.width, NaN);
  const explicitHeight = resolveLength(style.height, available.height, NaN);
  let width = Number.isNaN(explicitWidth) ? (available.width ?? NaN) : explicitWidth;
  let height = Number.isNaN(explicitHeight) ? (available.height ?? NaN) : explicitHeight;

  if (node.measure) {
    const measured = node.measure({
      width: Number.isNaN(width) ? available.width : width,
      height: Number.isNaN(height) ? available.height : height,
    });
    if (Number.isNaN(width)) width = measured.width + padding.left + padding.right;
    if (Number.isNaN(height)) height = measured.height + padding.top + padding.bottom;
  }


  // Single pass: filter display:none, build items, partition into flex /
  // absolute. The previous pipeline allocated 4 intermediate arrays
  // (filter → map → filter → filter) and three closures per layoutNode call,
  // showing up as ~7% of profile self time in `(anon) solver.ts`.
  const rawChildren = node.children ?? [];
  const contentAvailable = {
    width: Number.isNaN(width) ? undefined : Math.max(0, width - padding.left - padding.right),
    height: Number.isNaN(height) ? undefined : Math.max(0, height - padding.top - padding.bottom),
  };
  const mainParent = axis.main === 'width' ? contentAvailable.width : contentAvailable.height;
  const crossParent = axis.cross === 'width' ? contentAvailable.width : contentAvailable.height;

  const childNodes: LayoutNode[] = [];
  const items: Item[] = [];
  const flexItems: Item[] = [];
  const absoluteItems: Item[] = [];
  for (let i = 0; i < rawChildren.length; ++i) {
    const ch = rawChildren[i];
    if (ch.style?.display === 'none') continue;
    const it = makeItem(ch, childNodes.length, axis.main, axis.cross, contentAvailable);
    childNodes.push(ch);
    items.push(it);
    if (it.absolute) absoluteItems.push(it);
    else flexItems.push(it);
  }

  const lines: Item[][] = [];
  if ((style.flexWrap ?? 'nowrap') === 'nowrap' || mainParent === undefined) {
    lines.push(flexItems);
  } else {
    let line: Item[] = [];
    let used = 0;
    for (let i = 0; i < flexItems.length; ++i) {
      const item = flexItems[i];
      const outer = item.basis + item.marginMainBefore + item.marginMainAfter;
      const next = used + (line.length ? mainGap : 0) + outer;
      if (line.length && next > mainParent + EPS) {
        lines.push(line);
        line = [];
        used = 0;
      }
      used += (line.length ? mainGap : 0) + outer;
      line.push(item);
    }
    lines.push(line);
  }

  let crossCursor = 0;
  const lineCrossSizes: number[] = [];
  for (let li = 0; li < lines.length; ++li) {
    const line = lines[li];
    let lineMain: number;
    if (mainParent !== undefined) {
      lineMain = mainParent;
    } else {
      let sum = 0;
      for (let j = 0; j < line.length; ++j) {
        const it = line[j];
        sum += it.basis + (j ? mainGap : 0) + it.marginMainBefore + it.marginMainAfter;
      }
      lineMain = sum;
    }
    distributeLine(line, lineMain, mainGap, axis.main);
    let naturalLineCross = 0;
    for (let j = 0; j < line.length; ++j) {
      const it = line[j];
      const v = it.cross + it.marginCrossBefore + it.marginCrossAfter;
      if (v > naturalLineCross) naturalLineCross = v;
    }
    const lineCross =
      (style.flexWrap ?? 'nowrap') === 'nowrap' && crossParent !== undefined
        ? crossParent
        : naturalLineCross;
    lineCrossSizes.push(lineCross);
  }

  let totalCross = 0;
  for (let i = 0; i < lineCrossSizes.length; ++i) totalCross += lineCrossSizes[i];
  totalCross += Math.max(0, lines.length - 1) * crossGap;
  if (Number.isNaN(width) && axis.main === 'height')
    width = totalCross + padding.left + padding.right;
  if (Number.isNaN(height) && axis.main === 'width')
    height = totalCross + padding.top + padding.bottom;
  const contentCross = Math.max(
    0,
    (axis.cross === 'width' ? width : height) -
      (axis.cross === 'width' ? padding.left + padding.right : padding.top + padding.bottom),
  );
  let alignContentOffset = 0;
  let lineGap = crossGap;
  const extraCross = contentCross - totalCross;
  const alignContent = style.alignContent ?? 'flex-start';
  if (extraCross > 0 && lines.length > 1) {
    if (alignContent === 'center') alignContentOffset = extraCross / 2;
    else if (alignContent === 'flex-end') alignContentOffset = extraCross;
    else if (alignContent === 'space-between')
      lineGap = lines.length > 1 ? crossGap + extraCross / (lines.length - 1) : crossGap;
    else if (alignContent === 'space-around') {
      lineGap = crossGap + extraCross / lines.length;
      alignContentOffset = lineGap / 2;
    } else if (alignContent === 'space-evenly') {
      lineGap = crossGap + extraCross / (lines.length + 1);
      alignContentOffset = lineGap;
    }
  }
  crossCursor = alignContentOffset;

  // Lazy-allocated. Slots stay `undefined` for absolute children that
  // positionLine() never visits and get filled by the absolute pass below.
  // Saved 1.3% of profile self time vs. preallocating emptyResult() per
  // child (the stress grid generated 900 unused empties per frame).
  const children: LayoutResult[] = new Array(childNodes.length);
  for (let li = 0; li < lines.length; ++li) {
    const line = lines[li];
    const lineCross = lineCrossSizes[li];
    positionLine(
      line,
      style,
      width,
      height,
      padding,
      mainParent,
      lineCross,
      crossCursor,
      mainGap,
      axis.main,
      axis.reversed,
      children,
    );
    crossCursor += lineCross + lineGap;
  }

  if (Number.isNaN(width)) {
    let maxRight = 0;
    for (let i = 0; i < children.length; ++i) {
      const ch = children[i];
      if (ch === undefined) continue;
      const r = ch.x + ch.width;
      if (r > maxRight) maxRight = r;
    }
    width = maxRight + padding.right;
  }
  if (Number.isNaN(height)) {
    let maxBottom = 0;
    for (let i = 0; i < children.length; ++i) {
      const ch = children[i];
      if (ch === undefined) continue;
      const b = ch.y + ch.height;
      if (b > maxBottom) maxBottom = b;
    }
    height = maxBottom + padding.bottom;
  }

  width = clampSize(width, style, 'width', available.width);
  height = clampSize(height, style, 'height', available.height);

  for (const item of absoluteItems) {
    const childStyle = item.node.style ?? {};
    const child = layoutNode(item.node, { width: item.main, height: item.cross }, 0, 0);
    const left = resolveLength(childStyle.left, width, NaN);
    const right = resolveLength(childStyle.right, width, NaN);
    const top = resolveLength(childStyle.top, height, NaN);
    const bottom = resolveLength(childStyle.bottom, height, NaN);
    child.x = Number.isNaN(left)
      ? Number.isNaN(right)
        ? padding.left
        : width - padding.right - right - child.width
      : padding.left + left;
    child.y = Number.isNaN(top)
      ? Number.isNaN(bottom)
        ? padding.top
        : height - padding.bottom - bottom - child.height
      : padding.top + top;
    children[item.index] = child;
  }

  const result: LayoutResult = {
    x,
    y,
    width: round(width),
    height: round(height),
    paddingLeft: padding.left,
    paddingTop: padding.top,
    paddingRight: padding.right,
    paddingBottom: padding.bottom,
    children,
  };
  if (sig !== undefined) storeLayoutResult(cacheKey, result);
  return result;
}

function makeItem(
  node: LayoutNode,
  index: number,
  main: 'width' | 'height',
  cross: 'width' | 'height',
  available: Constraint,
): Item {
  const style = node.style ?? {};
  const margin = edges(style, available.width, available.height, 'margin');
  const parentMain = main === 'width' ? available.width : available.height;
  const parentCross = cross === 'width' ? available.width : available.height;
  const explicitBasis = resolveLength(style.flexBasis, parentMain, NaN);
  let basis = explicitBasis;
  if (Number.isNaN(basis))
    basis = resolveLength(main === 'width' ? style.width : style.height, parentMain, NaN);
  const explicitCross = resolveLength(cross === 'width' ? style.width : style.height, parentCross, NaN);
  let crossSize = explicitCross;
  const crossExplicit = !Number.isNaN(explicitCross);
  if (Number.isNaN(basis) || Number.isNaN(crossSize)) {
    const measured = intrinsicSize(node, available, main);
    if (Number.isNaN(basis))
      basis = main === 'width' ? (measured?.width ?? 0) : (measured?.height ?? 0);
    if (Number.isNaN(crossSize))
      crossSize = cross === 'width' ? (measured?.width ?? 0) : (measured?.height ?? 0);
  }

  if (style.aspectRatio && style.aspectRatio > 0) {
    if (main === 'width' && crossSize === 0 && basis > 0) crossSize = basis / style.aspectRatio;
    if (main === 'height' && crossSize === 0 && basis > 0) crossSize = basis * style.aspectRatio;
  }
  basis = clampSize(basis, style, main, parentMain);
  crossSize = clampSize(crossSize, style, cross, parentCross);
  const minMain = Math.max(0, resolveMin(style, main, parentMain));
  const maxMain = resolveMax(style, main, parentMain);
  const grow = style.flexGrow ?? (style.flex && style.flex > 0 ? style.flex : 0) ?? 0;
  const shrink = style.flexShrink ?? (style.flex === 0 ? 0 : 1);
  const isRow = main === 'width';
  return {
    node,
    index,
    margin,
    marginMainBefore: isRow ? margin.left : margin.top,
    marginMainAfter: isRow ? margin.right : margin.bottom,
    marginCrossBefore: isRow ? margin.top : margin.left,
    marginCrossAfter: isRow ? margin.bottom : margin.right,
    basis,
    main: basis,
    cross: crossSize,
    crossExplicit,
    grow,
    shrink,
    absolute: style.position === 'absolute',
    minMain,
    maxMain,
    result: emptyResult(),
  };
}

function intrinsicSize(
  node: LayoutNode,
  available: Constraint,
  main: 'width' | 'height',
): { width: number; height: number } | undefined {
  if (node.measure) return node.measure(available);
  if (!node.children?.length) return undefined;
  const constraint =
    main === 'width'
      ? { width: undefined, height: available.height }
      : { width: available.width, height: undefined };
  const measured = layoutNode(node, constraint, 0, 0);
  return { width: measured.width, height: measured.height };

function resolveMin(
  style: LayoutStyle,
  axis: 'width' | 'height',
  parent: number | undefined,
): number {
  return resolveLength(axis === 'width' ? style.minWidth : style.minHeight, parent, -Infinity);
}

function resolveMax(
  style: LayoutStyle,
  axis: 'width' | 'height',
  parent: number | undefined,
): number {
  return resolveLength(axis === 'width' ? style.maxWidth : style.maxHeight, parent, Infinity);
}

function distributeLine(
  line: Item[],
  availableMain: number,
  gap: number,
  main: 'width' | 'height',
): void {
  let occupied = 0;
  for (let i = 0; i < line.length; ++i) {
    const it = line[i];
    it.main = it.basis;
    occupied += it.basis + it.marginMainBefore + it.marginMainAfter;
  }
  occupied += Math.max(0, line.length - 1) * gap;
  const free = availableMain - occupied;
  if (free > EPS) {
    distributePositiveFreeSpace(line, free);
  } else if (free < -EPS) {
    distributeNegativeFreeSpace(line, -free);
  }
}

function distributePositiveFreeSpace(line: Item[], free: number): void {
  const unfrozen = new Set(line.filter((item) => item.grow > 0 && item.main < item.maxMain));
  let remaining = free;
  while (remaining > EPS && unfrozen.size > 0) {
    const totalGrow = [...unfrozen].reduce((sum, item) => sum + item.grow, 0);
    if (totalGrow <= EPS) break;
    let consumed = 0;
    for (const item of [...unfrozen]) {
      const nextRaw = item.main + (remaining * item.grow) / totalGrow;
      const next = Math.min(item.maxMain, nextRaw);
      consumed += next - item.main;
      item.main = next;
      if (next >= item.maxMain - EPS) unfrozen.delete(item);
    }
    if (consumed <= EPS) break;
    remaining -= consumed;
  }
}

function distributeNegativeFreeSpace(line: Item[], deficit: number): void {
  const unfrozen = new Set(line.filter((item) => item.shrink > 0 && item.main > item.minMain));
  let remaining = deficit;
  while (remaining > EPS && unfrozen.size > 0) {
    const totalShrink = [...unfrozen].reduce((sum, item) => sum + item.shrink * item.basis, 0);
    if (totalShrink <= EPS) break;
    let consumed = 0;
    for (const item of [...unfrozen]) {
      const scaled = item.shrink * item.basis;
      const nextRaw = item.main - (remaining * scaled) / totalShrink;
      const next = Math.max(item.minMain, nextRaw);
      consumed += item.main - next;
      item.main = next;
      if (next <= item.minMain + EPS) unfrozen.delete(item);
    }
    if (consumed <= EPS) break;
    remaining -= consumed;
  }
}

function positionLine(
  line: Item[],
  style: LayoutStyle,
  width: number,
  height: number,
  padding: LayoutEdges,
  availableMain: number | undefined,
  lineCross: number,
  crossCursor: number,
  gap: number,
  main: 'width' | 'height',
  reversed: boolean,
  out: LayoutResult[],
): void {
  const isRow = main === 'width';
  // Compute contentMain + used in a single pass; previous code did two
  // .reduce() calls back-to-back, each with its own closure.
  let contentMain: number;
  let used: number;
  if (availableMain !== undefined) {
    contentMain = availableMain;
    let acc = 0;
    for (let i = 0; i < line.length; ++i) {
      const it = line[i];
      acc += it.main + it.marginMainBefore + it.marginMainAfter;
    }
    used = acc + Math.max(0, line.length - 1) * gap;
  } else {
    let cm = 0;
    let acc = 0;
    for (let i = 0; i < line.length; ++i) {
      const it = line[i];
      const inner = it.marginMainBefore + it.marginMainAfter;
      cm += it.main + (i ? gap : 0) + inner;
      acc += it.main + inner;
    }
    contentMain = cm;
    used = acc + Math.max(0, line.length - 1) * gap;
  }
  let cursor = 0;
  let itemGap = gap;
  const free = contentMain - used;
  switch (style.justifyContent ?? 'flex-start') {
    case 'flex-end':
      cursor = Math.max(0, free);
      break;
    case 'center':
      cursor = Math.max(0, free / 2);
      break;
    case 'space-between':
      itemGap = line.length > 1 ? gap + Math.max(0, free) / (line.length - 1) : gap;
      break;
    case 'space-around':
      itemGap = gap + Math.max(0, free) / line.length;
      cursor = itemGap / 2;
      break;
    case 'space-evenly':
      itemGap = gap + Math.max(0, free) / (line.length + 1);
      cursor = itemGap;
      break;
  }
  const parentAlignItems = style.alignItems ?? 'stretch';
  const lineLen = line.length;
  for (let it_idx = 0; it_idx < lineLen; ++it_idx) {
    const item = line[reversed ? lineLen - 1 - it_idx : it_idx];
    const childStyle = item.node.style ?? {};
    const align =
      childStyle.alignSelf && childStyle.alignSelf !== 'auto'
        ? childStyle.alignSelf
        : parentAlignItems;
    const mb = item.marginMainBefore;
    const ma = item.marginMainAfter;
    const cb = item.marginCrossBefore;
    const ca = item.marginCrossAfter;
    let childCross = item.cross;
    if (align === 'stretch' && !item.crossExplicit) childCross = Math.max(0, lineCross - cb - ca);
    let crossPos = crossCursor + cb;
    const crossFree = lineCross - childCross - cb - ca;
    if (align === 'center') crossPos += crossFree / 2;
    else if (align === 'flex-end') crossPos += crossFree;
    const mainPos = cursor + mb;
    cursor += item.main + mb + ma + itemGap;
    const constraint = isRow
      ? { width: item.main, height: childCross }
      : { width: childCross, height: item.main };
    const child = layoutNode(item.node, constraint, 0, 0);
    if (isRow) {
      child.x = round(padding.left + mainPos);
      child.y = round(padding.top + crossPos);
      child.width = round(item.main);
      child.height = round(childCross);
      child.width = clampSize(child.width, childStyle, 'width', width);
      child.height = clampSize(child.height, childStyle, 'height', height);
    } else {
      child.x = round(padding.left + crossPos);
      child.y = round(padding.top + mainPos);
      child.width = round(childCross);
      child.height = round(item.main);
      child.width = clampSize(child.width, childStyle, 'width', width);
      child.height = clampSize(child.height, childStyle, 'height', height);
    }
    out[item.index] = child;
  }
}

// mainBefore / mainAfter / crossBefore / crossAfter inlined as
// item.marginMain{Before,After} / marginCross{Before,After} on Item.
function round(v: number): number {
  return Math.round(v * 1000) / 1000;
}
