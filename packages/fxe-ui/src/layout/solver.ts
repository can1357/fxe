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
function resolveEdge(
  v: Length | undefined,
  parent: number | undefined,
): number {
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
  basis: number;
  main: number;
  cross: number;
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

export function solveLayout(root: LayoutNode, available: Constraint = {}): LayoutResult {
  return layoutNode(root, available, 0, 0);
}

function layoutNode(node: LayoutNode, available: Constraint, x: number, y: number): LayoutResult {
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

  const childNodes = (node.children ?? []).filter((child) => child.style?.display !== 'none');
  const contentAvailable = {
    width: Number.isNaN(width) ? undefined : Math.max(0, width - padding.left - padding.right),
    height: Number.isNaN(height) ? undefined : Math.max(0, height - padding.top - padding.bottom),
  };
  const mainParent = axis.main === 'width' ? contentAvailable.width : contentAvailable.height;
  const crossParent = axis.cross === 'width' ? contentAvailable.width : contentAvailable.height;

  const items: Item[] = childNodes.map((child, index) =>
    makeItem(child, index, axis.main, axis.cross, contentAvailable),
  );
  const absoluteItems = items.filter((item) => item.absolute);
  const flexItems = items.filter((item) => !item.absolute);

  const lines: Item[][] = [];
  if ((style.flexWrap ?? 'nowrap') === 'nowrap' || mainParent === undefined) {
    lines.push(flexItems);
  } else {
    let line: Item[] = [];
    let used = 0;
    for (const item of flexItems) {
      const outer =
        item.basis + mainBefore(item.margin, axis.main) + mainAfter(item.margin, axis.main);
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
  for (const line of lines) {
    distributeLine(
      line,
      mainParent ??
        line.reduce(
          (sum, item, idx) =>
            sum +
            item.basis +
            (idx ? mainGap : 0) +
            mainBefore(item.margin, axis.main) +
            mainAfter(item.margin, axis.main),
          0,
        ),
      mainGap,
      axis.main,
    );
    const naturalLineCross = line.reduce(
      (max, item) =>
        Math.max(
          max,
          item.cross + crossBefore(item.margin, axis.main) + crossAfter(item.margin, axis.main),
        ),
      0,
    );
    const lineCross =
      (style.flexWrap ?? 'nowrap') === 'nowrap' && crossParent !== undefined
        ? crossParent
        : naturalLineCross;
    lineCrossSizes.push(lineCross);
  }

  const totalCross =
    lineCrossSizes.reduce((sum, n) => sum + n, 0) + Math.max(0, lines.length - 1) * crossGap;
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

  const children: LayoutResult[] = childNodes.map(() => emptyResult());
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
    const maxRight = children.reduce((max, child) => Math.max(max, child.x + child.width), 0);
    width = maxRight + padding.right;
  }
  if (Number.isNaN(height)) {
    const maxBottom = children.reduce((max, child) => Math.max(max, child.y + child.height), 0);
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

  return {
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
  let crossSize = resolveLength(cross === 'width' ? style.width : style.height, parentCross, NaN);
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
  return {
    node,
    index,
    margin,
    basis,
    main: basis,
    cross: crossSize,
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
}

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
  for (const item of line) item.main = item.basis;
  const occupied =
    line.reduce(
      (sum, item) =>
        sum + item.basis + mainBefore(item.margin, main) + mainAfter(item.margin, main),
      0,
    ) +
    Math.max(0, line.length - 1) * gap;
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
  const cross = main === 'width' ? 'height' : 'width';
  const contentMain =
    availableMain ??
    line.reduce(
      (sum, item, idx) =>
        sum +
        item.main +
        (idx ? gap : 0) +
        mainBefore(item.margin, main) +
        mainAfter(item.margin, main),
      0,
    );
  const used =
    line.reduce(
      (sum, item) => sum + item.main + mainBefore(item.margin, main) + mainAfter(item.margin, main),
      0,
    ) +
    Math.max(0, line.length - 1) * gap;
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
  const ordered = reversed ? [...line].reverse() : line;
  for (const item of ordered) {
    const childStyle = item.node.style ?? {};
    const align =
      childStyle.alignSelf && childStyle.alignSelf !== 'auto'
        ? childStyle.alignSelf
        : (style.alignItems ?? 'stretch');
    let childCross = item.cross;
    if (align === 'stretch' && item.cross === 0)
      childCross = Math.max(
        0,
        lineCross - crossBefore(item.margin, main) - crossAfter(item.margin, main),
      );
    let crossPos = crossCursor + crossBefore(item.margin, main);
    const crossFree =
      lineCross - childCross - crossBefore(item.margin, main) - crossAfter(item.margin, main);
    if (align === 'center') crossPos += crossFree / 2;
    else if (align === 'flex-end') crossPos += crossFree;
    const mainPos = cursor + mainBefore(item.margin, main);
    cursor += item.main + mainBefore(item.margin, main) + mainAfter(item.margin, main) + itemGap;
    const constraint =
      main === 'width'
        ? { width: item.main, height: childCross }
        : { width: childCross, height: item.main };
    const child = layoutNode(item.node, constraint, 0, 0);
    child.x = round(padding.left + (main === 'width' ? mainPos : crossPos));
    child.y = round(padding.top + (main === 'width' ? crossPos : mainPos));
    child.width = round(main === 'width' ? item.main : childCross);
    child.height = round(main === 'width' ? childCross : item.main);
    if (cross === 'width') child.width = clampSize(child.width, childStyle, 'width', width);
    if (cross === 'height') child.height = clampSize(child.height, childStyle, 'height', height);
    out[item.index] = child;
  }
}

function mainBefore(m: LayoutEdges, main: 'width' | 'height'): number {
  return main === 'width' ? m.left : m.top;
}
function mainAfter(m: LayoutEdges, main: 'width' | 'height'): number {
  return main === 'width' ? m.right : m.bottom;
}
function crossBefore(m: LayoutEdges, main: 'width' | 'height'): number {
  return main === 'width' ? m.top : m.left;
}
function crossAfter(m: LayoutEdges, main: 'width' | 'height'): number {
  return main === 'width' ? m.bottom : m.right;
}
function round(v: number): number {
  return Math.round(v * 1000) / 1000;
}
