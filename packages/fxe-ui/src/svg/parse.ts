/**
 * Deterministic SVG parser supporting path/rect/circle/line/polyline/polygon geometry,
 * transforms, solid fills/strokes, and linear/radial gradient paints.
 * Text, images, filters, masks, clipping, and other unsupported elements are ignored.
 */
export type SvgAffine = readonly [number, number, number, number, number, number];

export interface SvgGradientStop {
  offset: number;
  color: number;
}

export type SvgPaint =
  | { kind: 'solid'; color: number }
  | {
      kind: 'linear-gradient';
      x1: number;
      y1: number;
      x2: number;
      y2: number;
      stops: SvgGradientStop[];
      spread: 'pad' | 'reflect' | 'repeat';
      gradientUnits: 'userSpaceOnUse' | 'objectBoundingBox';
      transform: SvgAffine;
    }
  | {
      kind: 'radial-gradient';
      cx: number;
      cy: number;
      r: number;
      fx: number;
      fy: number;
      stops: SvgGradientStop[];
      spread: 'pad' | 'reflect' | 'repeat';
      gradientUnits: 'userSpaceOnUse' | 'objectBoundingBox';
      transform: SvgAffine;
    };

export interface SvgShape {
  /** Filled subpath. */
  path: Path;
  /** Optional fill paint. Undefined = use caller default. */
  fill?: number | SvgPaint;
  /** Optional stroke paint. */
  stroke?: number | SvgPaint;
  /** Stroke width. */
  strokeWidth?: number;
  /** Fill rule. */
  fillRule?: 'nonzero' | 'evenodd';
}

export interface SvgDocument {
  /** Viewbox as [minX, minY, width, height]. Defaults to width/height when no viewBox. */
  viewBox: [number, number, number, number];
  width: number;
  height: number;
  shapes: SvgShape[];
}

type Affine = SvgAffine;

type StyleState = {
  fill?: number | SvgPaint;
  stroke?: number | SvgPaint;
  strokeWidth?: number;
  fillRule?: 'nonzero' | 'evenodd';
};

type Scope = {
  transform: Affine;
  style: StyleState;
};

type XmlToken =
  | { type: 'start'; name: string; attributes: Record<string, string>; selfClosing: boolean }
  | { type: 'end'; name: string };

type GradientSpread = 'pad' | 'reflect' | 'repeat';
type GradientUnits = 'userSpaceOnUse' | 'objectBoundingBox';
type GradientPaint =
  | Extract<SvgPaint, { kind: 'linear-gradient' }>
  | Extract<SvgPaint, { kind: 'radial-gradient' }>;

type BaseGradientDef = {
  id: string;
  href?: string;
  stops: SvgGradientStop[];
  spread?: GradientSpread;
  gradientUnits?: GradientUnits;
  transform?: Affine;
};

type LinearGradientDef = BaseGradientDef & {
  kind: 'linear-gradient';
  x1?: number;
  y1?: number;
  x2?: number;
  y2?: number;
};

type RadialGradientDef = BaseGradientDef & {
  kind: 'radial-gradient';
  cx?: number;
  cy?: number;
  r?: number;
  fx?: number;
  fy?: number;
};

type SvgGradientDef = LinearGradientDef | RadialGradientDef;

const IDENTITY: Affine = [1, 0, 0, 1, 0, 0];
const FLOAT_PATTERN = /[-+]?(?:\d+\.\d+|\d+\.?|\.\d+)(?:[eE][-+]?\d+)?/g;
const COLOR_NAMES: Record<string, number | undefined> = {
  black: 0x000000ff,
  white: 0xffffffff,
  red: 0xff0000ff,
  green: 0x00ff00ff,
  blue: 0x0000ffff,
  transparent: 0x00000000,
  none: undefined,
};

export function parseSvg(source: string): SvgDocument {
  const tokens = tokenizeXml(source);
  const gradients = collectGradients(tokens);
  const scopes: Scope[] = [{ transform: IDENTITY, style: {} }];
  const shapes: SvgShape[] = [];
  let width = 0;
  let height = 0;
  let viewBox: [number, number, number, number] | undefined;

  for (const token of tokens) {
    if (token.type === 'end') {
      if (scopes.length > 1) scopes.pop();
      continue;
    }

    const current = scopes[scopes.length - 1];
    const attrs = token.attributes;
    const transform = composeAffine(current.transform, parseTransform(attrs.transform));
    const style = mergeStyle(current.style, attrs, gradients);

    if (token.name === 'svg') {
      if (viewBox === undefined) {
        viewBox = parseViewBox(attrs.viewBox);
      }
      if (!Number.isFinite(width) || width === 0) width = parseNumber(attrs.width) ?? width;
      if (!Number.isFinite(height) || height === 0) height = parseNumber(attrs.height) ?? height;
      scopes.push({ transform, style });
      if (token.selfClosing) scopes.pop();
      continue;
    }

    if (token.name === 'g') {
      scopes.push({ transform, style });
      if (token.selfClosing) scopes.pop();
      continue;
    }

    const shape = buildShape(token.name, attrs, transform, style, gradients);
    if (shape !== undefined) {
      shapes.push(shape);
    }

    if (!token.selfClosing) {
      // Unsupported container elements are ignored; we still push scope so nested
      // supported nodes inherit transform/style.
      scopes.push({ transform, style });
    }
  }

  if (!Number.isFinite(width)) width = 0;
  if (!Number.isFinite(height)) height = 0;
  if (viewBox === undefined) {
    viewBox = [0, 0, width, height];
  }
  if (!width && viewBox[2]) width = viewBox[2];
  if (!height && viewBox[3]) height = viewBox[3];

  return {
    viewBox,
    width,
    height,
    shapes,
  };
}

function tokenizeXml(source: string): XmlToken[] {
  const out: XmlToken[] = [];
  let i = 0;
  while (i < source.length) {
    const lt = source.indexOf('<', i);
    if (lt < 0) break;

    if (source.startsWith('<!--', lt)) {
      const end = source.indexOf('-->', lt + 4);
      if (end < 0) break;
      i = end + 3;
      continue;
    }

    if (source.startsWith('<?', lt)) {
      const end = source.indexOf('?>', lt + 2);
      if (end < 0) break;
      i = end + 2;
      continue;
    }

    if (source.startsWith('<!', lt)) {
      const end = source.indexOf('>', lt + 2);
      if (end < 0) break;
      i = end + 1;
      continue;
    }

    const gt = source.indexOf('>', lt + 1);
    if (gt < 0) break;
    const raw = source.slice(lt + 1, gt).trim();
    if (!raw) {
      i = gt + 1;
      continue;
    }

    if (raw.startsWith('/')) {
      const name = raw.slice(1).trim().toLowerCase();
      if (name) out.push({ type: 'end', name });
      i = gt + 1;
      continue;
    }

    const selfClosing = raw.endsWith('/');
    const body = selfClosing ? raw.slice(0, -1).trim() : raw;
    const nameMatch = /^([A-Za-z_][A-Za-z0-9_.:-]*)/.exec(body);
    if (!nameMatch) {
      i = gt + 1;
      continue;
    }

    const name = nameMatch[1].toLowerCase();
    const attrSource = body.slice(nameMatch[0].length);
    const attributes = parseAttributes(attrSource);
    out.push({ type: 'start', name, attributes, selfClosing });
    i = gt + 1;
  }
  return out;
}

function parseAttributes(source: string): Record<string, string> {
  const attributes: Record<string, string> = {};
  const pattern = /([A-Za-z_][A-Za-z0-9_.:-]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))/g;
  let match: RegExpExecArray | null = pattern.exec(source);
  while (match !== null) {
    const value = match[2] ?? match[3] ?? match[4] ?? '';
    attributes[match[1]] = value;
    match = pattern.exec(source);
  }
  return attributes;
}
function collectGradients(tokens: XmlToken[]): Map<string, GradientPaint> {
  const defs = new Map<string, SvgGradientDef>();
  let current: SvgGradientDef | undefined;
  let depth = 0;

  for (const token of tokens) {
    if (token.type === 'start') {
      if (current === undefined) {
        const def = parseGradientDefinition(token);
        if (def === undefined) continue;
        if (token.selfClosing) {
          defs.set(def.id, def);
        } else {
          current = def;
          depth = 1;
        }
        continue;
      }

      if (token.name === 'stop') {
        const stop = parseGradientStop(token.attributes);
        if (stop !== undefined) current.stops.push(stop);
      }
      if (!token.selfClosing) {
        depth += 1;
      }
      continue;
    }

    if (current === undefined) continue;
    depth -= 1;
    if (depth === 0) {
      defs.set(current.id, current);
      current = undefined;
    }
  }

  const resolved = new Map<string, GradientPaint>();
  const visiting = new Set<string>();
  for (const id of defs.keys()) {
    const paint = resolveGradient(id, defs, resolved, visiting);
    if (paint !== undefined) {
      resolved.set(id, paint);
    }
  }
  return resolved;
}

function parseGradientDefinition(
  token: Extract<XmlToken, { type: 'start' }>,
): SvgGradientDef | undefined {
  const id = token.attributes.id?.trim();
  if (!id) return undefined;

  if (token.name === 'lineargradient') {
    return {
      kind: 'linear-gradient',
      id,
      href: parseGradientHref(token.attributes),
      stops: [],
      spread: parseGradientSpread(token.attributes.spreadMethod),
      gradientUnits: parseGradientUnits(token.attributes.gradientUnits),
      transform:
        token.attributes.gradientTransform !== undefined
          ? parseTransform(token.attributes.gradientTransform)
          : undefined,
      x1: parseSvgScalar(token.attributes.x1),
      y1: parseSvgScalar(token.attributes.y1),
      x2: parseSvgScalar(token.attributes.x2),
      y2: parseSvgScalar(token.attributes.y2),
    };
  }

  if (token.name === 'radialgradient') {
    return {
      kind: 'radial-gradient',
      id,
      href: parseGradientHref(token.attributes),
      stops: [],
      spread: parseGradientSpread(token.attributes.spreadMethod),
      gradientUnits: parseGradientUnits(token.attributes.gradientUnits),
      transform:
        token.attributes.gradientTransform !== undefined
          ? parseTransform(token.attributes.gradientTransform)
          : undefined,
      cx: parseSvgScalar(token.attributes.cx),
      cy: parseSvgScalar(token.attributes.cy),
      r: parseSvgScalar(token.attributes.r),
      fx: parseSvgScalar(token.attributes.fx),
      fy: parseSvgScalar(token.attributes.fy),
    };
  }

  return undefined;
}

function parseGradientHref(attrs: Record<string, string>): string | undefined {
  const raw = attrs.href ?? attrs['xlink:href'];
  if (!raw) return undefined;
  const value = raw.trim();
  return value.startsWith('#') && value.length > 1 ? value.slice(1) : undefined;
}

function parseGradientSpread(raw: string | undefined): GradientSpread | undefined {
  switch (raw?.trim()) {
    case 'reflect':
      return 'reflect';
    case 'repeat':
      return 'repeat';
    case 'pad':
      return 'pad';
    default:
      return undefined;
  }
}

function parseGradientUnits(raw: string | undefined): GradientUnits | undefined {
  switch (raw?.trim()) {
    case 'userSpaceOnUse':
      return 'userSpaceOnUse';
    case 'objectBoundingBox':
      return 'objectBoundingBox';
    default:
      return undefined;
  }
}

function parseGradientStop(attrs: Record<string, string>): SvgGradientStop | undefined {
  const offset = parseGradientOffset(attrs.offset);
  if (offset === undefined) return undefined;
  const color = parseStopColor(attrs['stop-color'], attrs['stop-opacity']);
  return { offset, color };
}

function resolveGradient(
  id: string,
  defs: ReadonlyMap<string, SvgGradientDef>,
  resolved: Map<string, GradientPaint>,
  visiting: Set<string>,
): GradientPaint | undefined {
  const cached = resolved.get(id);
  if (cached !== undefined) return cached;
  const def = defs.get(id);
  if (def === undefined || visiting.has(id)) return undefined;

  visiting.add(id);
  try {
    const base = def.href ? resolveGradient(def.href, defs, resolved, visiting) : undefined;
    let paint: GradientPaint;

    if (def.kind === 'linear-gradient') {
      const inherited = base?.kind === 'linear-gradient' ? base : undefined;
      paint = {
        kind: 'linear-gradient',
        x1: def.x1 ?? inherited?.x1 ?? 0,
        y1: def.y1 ?? inherited?.y1 ?? 0,
        x2: def.x2 ?? inherited?.x2 ?? 1,
        y2: def.y2 ?? inherited?.y2 ?? 0,
        stops: cloneGradientStops(def.stops.length > 0 ? def.stops : (inherited?.stops ?? [])),
        spread: def.spread ?? inherited?.spread ?? 'pad',
        gradientUnits: def.gradientUnits ?? inherited?.gradientUnits ?? 'objectBoundingBox',
        transform: copyAffine(def.transform ?? inherited?.transform ?? IDENTITY),
      };
    } else {
      const inherited = base?.kind === 'radial-gradient' ? base : undefined;
      const cx = def.cx ?? inherited?.cx ?? 0.5;
      const cy = def.cy ?? inherited?.cy ?? 0.5;
      const fx = def.fx ?? inherited?.fx ?? cx;
      const fy = def.fy ?? inherited?.fy ?? cy;
      paint = {
        kind: 'radial-gradient',
        cx,
        cy,
        r: def.r ?? inherited?.r ?? 0.5,
        fx,
        fy,
        stops: cloneGradientStops(def.stops.length > 0 ? def.stops : (inherited?.stops ?? [])),
        spread: def.spread ?? inherited?.spread ?? 'pad',
        gradientUnits: def.gradientUnits ?? inherited?.gradientUnits ?? 'objectBoundingBox',
        transform: copyAffine(def.transform ?? inherited?.transform ?? IDENTITY),
      };
    }

    resolved.set(id, paint);
    return paint;
  } finally {
    visiting.delete(id);
  }
}

function cloneGradientStops(stops: readonly SvgGradientStop[]): SvgGradientStop[] {
  return stops.map((stop) => ({ offset: stop.offset, color: stop.color }));
}

function copyAffine(matrix: Affine): Affine {
  return [matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]];
}

function buildShape(
  tag: string,
  attrs: Record<string, string>,
  transform: Affine,
  style: StyleState,
  gradients: ReadonlyMap<string, GradientPaint>,
): SvgShape | undefined {
  let path: Path | undefined;
  switch (tag) {
    case 'path':
      path = buildPathElement(attrs, transform);
      break;
    case 'rect':
      path = buildRect(attrs, transform);
      break;
    case 'circle':
      path = buildCircle(attrs, transform);
      break;
    case 'line':
      path = buildLine(attrs, transform);
      break;
    case 'polygon':
      path = buildPoly(attrs, transform, true);
      break;
    case 'polyline':
      path = buildPoly(attrs, transform, false);
      break;
    default:
      // Unsupported elements (e.g. text/image) are ignored.
      return undefined;
  }
  if (path === undefined) return undefined;

  const fill = attrs.fill !== undefined ? parsePaint(attrs.fill, gradients) : style.fill;
  const stroke = attrs.stroke !== undefined ? parsePaint(attrs.stroke, gradients) : style.stroke;
  const strokeWidth =
    attrs['stroke-width'] !== undefined ? parseNumber(attrs['stroke-width']) : style.strokeWidth;
  const fillRule =
    attrs['fill-rule'] !== undefined ? parseFillRule(attrs['fill-rule']) : style.fillRule;

  return {
    path,
    fill,
    stroke,
    strokeWidth,
    fillRule,
  };
}

function mergeStyle(
  base: StyleState,
  attrs: Record<string, string>,
  gradients: ReadonlyMap<string, GradientPaint>,
): StyleState {
  const next: StyleState = { ...base };
  if (attrs.fill !== undefined) next.fill = parsePaint(attrs.fill, gradients);
  if (attrs.stroke !== undefined) next.stroke = parsePaint(attrs.stroke, gradients);
  if (attrs['stroke-width'] !== undefined) {
    next.strokeWidth = parseNumber(attrs['stroke-width']);
  }
  if (attrs['fill-rule'] !== undefined) {
    next.fillRule = parseFillRule(attrs['fill-rule']);
  }
  return next;
}

function parseFillRule(raw: string): 'nonzero' | 'evenodd' | undefined {
  const value = raw.trim().toLowerCase();
  if (value === 'evenodd') return 'evenodd';
  if (value === 'nonzero') return 'nonzero';
  return undefined;
}

function buildPathElement(attrs: Record<string, string>, transform: Affine): Path | undefined {
  const d = attrs.d;
  if (!d) return undefined;
  const path = new Path();
  applyPathData(path, d, transform);
  return path;
}

function buildRect(attrs: Record<string, string>, transform: Affine): Path | undefined {
  const x = parseNumber(attrs.x) ?? 0;
  const y = parseNumber(attrs.y) ?? 0;
  const width = parseNumber(attrs.width) ?? 0;
  const height = parseNumber(attrs.height) ?? 0;
  if (width <= 0 || height <= 0) return undefined;

  const rx = Math.max(0, parseNumber(attrs.rx) ?? 0);
  const rySource = parseNumber(attrs.ry);
  const ry = Math.max(0, rySource ?? rx);

  const path = new Path();
  if (rx <= 0 && ry <= 0) {
    const p1 = applyPoint(transform, x, y);
    const p2 = applyPoint(transform, x + width, y);
    const p3 = applyPoint(transform, x + width, y + height);
    const p4 = applyPoint(transform, x, y + height);
    path.moveTo(p1.x, p1.y);
    path.lineTo(p2.x, p2.y);
    path.lineTo(p3.x, p3.y);
    path.lineTo(p4.x, p4.y);
    path.close();
    return path;
  }

  const clampedRx = Math.min(rx, width / 2);
  const clampedRy = Math.min(ry, height / 2);
  const kappa = 0.5522847498307936;
  const ox = clampedRx * kappa;
  const oy = clampedRy * kappa;

  moveTo(path, transform, x + clampedRx, y);
  lineTo(path, transform, x + width - clampedRx, y);
  cubicTo(
    path,
    transform,
    x + width - clampedRx + ox,
    y,
    x + width,
    y + clampedRy - oy,
    x + width,
    y + clampedRy,
  );
  lineTo(path, transform, x + width, y + height - clampedRy);
  cubicTo(
    path,
    transform,
    x + width,
    y + height - clampedRy + oy,
    x + width - clampedRx + ox,
    y + height,
    x + width - clampedRx,
    y + height,
  );
  lineTo(path, transform, x + clampedRx, y + height);
  cubicTo(
    path,
    transform,
    x + clampedRx - ox,
    y + height,
    x,
    y + height - clampedRy + oy,
    x,
    y + height - clampedRy,
  );
  lineTo(path, transform, x, y + clampedRy);
  cubicTo(path, transform, x, y + clampedRy - oy, x + clampedRx - ox, y, x + clampedRx, y);
  path.close();
  return path;
}

function buildCircle(attrs: Record<string, string>, transform: Affine): Path | undefined {
  const cx = parseNumber(attrs.cx) ?? 0;
  const cy = parseNumber(attrs.cy) ?? 0;
  const r = parseNumber(attrs.r) ?? 0;
  if (r <= 0) return undefined;

  if (isUniformScale(transform)) {
    const path = new Path();
    const center = applyPoint(transform, cx, cy);
    const sx = Math.hypot(transform[0], transform[1]);
    const sy = Math.hypot(transform[2], transform[3]);
    const rr = r * (sx + sy) * 0.5;
    path.moveTo(center.x + rr, center.y);
    path.arc(center.x, center.y, rr, 0, Math.PI * 2, false);
    path.close();
    return path;
  }

  const path = new Path();
  ellipseToCubics(path, transform, cx, cy, r, r, 0, 0, Math.PI * 2, false, true);
  return path;
}

function buildLine(attrs: Record<string, string>, transform: Affine): Path | undefined {
  const x1 = parseNumber(attrs.x1) ?? 0;
  const y1 = parseNumber(attrs.y1) ?? 0;
  const x2 = parseNumber(attrs.x2) ?? 0;
  const y2 = parseNumber(attrs.y2) ?? 0;

  const path = new Path();
  moveTo(path, transform, x1, y1);
  lineTo(path, transform, x2, y2);
  return path;
}

function buildPoly(
  attrs: Record<string, string>,
  transform: Affine,
  closePath: boolean,
): Path | undefined {
  const points = parsePoints(attrs.points);
  if (points.length < 2) return undefined;

  const path = new Path();
  moveTo(path, transform, points[0], points[1]);
  for (let i = 2; i + 1 < points.length; i += 2) {
    lineTo(path, transform, points[i], points[i + 1]);
  }
  if (closePath) path.close();
  return path;
}

function parsePoints(raw: string | undefined): number[] {
  if (!raw) return [];
  const values = parseNumberList(raw);
  if (values.length % 2 !== 0) values.pop();
  return values;
}

function applyPathData(path: Path, d: string, transform: Affine): void {
  const tokens = tokenizePathData(d);
  if (tokens.length === 0) return;

  let index = 0;
  let cmd = '';
  let cx = 0;
  let cy = 0;
  let sx = 0;
  let sy = 0;
  let lastCubic: { x: number; y: number } | undefined;
  let lastQuad: { x: number; y: number } | undefined;

  while (index < tokens.length) {
    const token = tokens[index];
    if (typeof token === 'string') {
      cmd = token;
      index += 1;
    } else if (!cmd) {
      index += 1;
      continue;
    }

    const abs = cmd === cmd.toUpperCase();
    switch (cmd.toUpperCase()) {
      case 'M': {
        if (!hasNumber(tokens, index + 1)) break;
        const first = readPoint(tokens, index, abs, cx, cy);
        if (!first) break;
        cx = first.x;
        cy = first.y;
        sx = cx;
        sy = cy;
        moveTo(path, transform, cx, cy);
        index = first.next;

        while (hasNumber(tokens, index + 1)) {
          const point = readPoint(tokens, index, abs, cx, cy);
          if (!point) break;
          cx = point.x;
          cy = point.y;
          lineTo(path, transform, cx, cy);
          index = point.next;
        }
        cmd = abs ? 'L' : 'l';
        lastCubic = undefined;
        lastQuad = undefined;
        break;
      }
      case 'L': {
        while (hasNumber(tokens, index + 1)) {
          const point = readPoint(tokens, index, abs, cx, cy);
          if (!point) break;
          cx = point.x;
          cy = point.y;
          lineTo(path, transform, cx, cy);
          index = point.next;
        }
        lastCubic = undefined;
        lastQuad = undefined;
        break;
      }
      case 'H': {
        while (hasNumber(tokens, index)) {
          const value = tokens[index] as number;
          cx = abs ? value : cx + value;
          lineTo(path, transform, cx, cy);
          index += 1;
        }
        lastCubic = undefined;
        lastQuad = undefined;
        break;
      }
      case 'V': {
        while (hasNumber(tokens, index)) {
          const value = tokens[index] as number;
          cy = abs ? value : cy + value;
          lineTo(path, transform, cx, cy);
          index += 1;
        }
        lastCubic = undefined;
        lastQuad = undefined;
        break;
      }
      case 'C': {
        while (hasNumber(tokens, index + 5)) {
          const c1 = readPoint(tokens, index, abs, cx, cy);
          if (!c1) break;
          const c2 = readPoint(tokens, c1.next, abs, cx, cy);
          if (!c2) break;
          const end = readPoint(tokens, c2.next, abs, cx, cy);
          if (!end) break;
          cubicTo(path, transform, c1.x, c1.y, c2.x, c2.y, end.x, end.y);
          cx = end.x;
          cy = end.y;
          lastCubic = { x: c2.x, y: c2.y };
          lastQuad = undefined;
          index = end.next;
        }
        break;
      }
      case 'S': {
        while (hasNumber(tokens, index + 3)) {
          const c1 =
            lastCubic === undefined
              ? { x: cx, y: cy }
              : { x: 2 * cx - lastCubic.x, y: 2 * cy - lastCubic.y };
          const c2 = readPoint(tokens, index, abs, cx, cy);
          if (!c2) break;
          const end = readPoint(tokens, c2.next, abs, cx, cy);
          if (!end) break;
          cubicTo(path, transform, c1.x, c1.y, c2.x, c2.y, end.x, end.y);
          cx = end.x;
          cy = end.y;
          lastCubic = { x: c2.x, y: c2.y };
          lastQuad = undefined;
          index = end.next;
        }
        break;
      }
      case 'Q': {
        while (hasNumber(tokens, index + 3)) {
          const control = readPoint(tokens, index, abs, cx, cy);
          if (!control) break;
          const end = readPoint(tokens, control.next, abs, cx, cy);
          if (!end) break;
          quadTo(path, transform, control.x, control.y, end.x, end.y);
          cx = end.x;
          cy = end.y;
          lastQuad = { x: control.x, y: control.y };
          lastCubic = undefined;
          index = end.next;
        }
        break;
      }
      case 'T': {
        while (hasNumber(tokens, index + 1)) {
          const control =
            lastQuad === undefined
              ? { x: cx, y: cy }
              : { x: 2 * cx - lastQuad.x, y: 2 * cy - lastQuad.y };
          const end = readPoint(tokens, index, abs, cx, cy);
          if (!end) break;
          quadTo(path, transform, control.x, control.y, end.x, end.y);
          cx = end.x;
          cy = end.y;
          lastQuad = { x: control.x, y: control.y };
          lastCubic = undefined;
          index = end.next;
        }
        break;
      }
      case 'A': {
        while (hasNumber(tokens, index + 6)) {
          const rxRaw = tokens[index] as number;
          const ryRaw = tokens[index + 1] as number;
          const angle = ((tokens[index + 2] as number) * Math.PI) / 180;
          const largeArc = (tokens[index + 3] as number) !== 0;
          const sweep = (tokens[index + 4] as number) !== 0;
          const target = readPoint(tokens, index + 5, abs, cx, cy);
          if (!target) break;
          arcToCubics(
            path,
            transform,
            cx,
            cy,
            Math.abs(rxRaw),
            Math.abs(ryRaw),
            angle,
            largeArc,
            sweep,
            target.x,
            target.y,
          );
          cx = target.x;
          cy = target.y;
          lastCubic = undefined;
          lastQuad = undefined;
          index = target.next;
        }
        break;
      }
      case 'Z': {
        path.close();
        cx = sx;
        cy = sy;
        lastCubic = undefined;
        lastQuad = undefined;
        break;
      }
      default:
        index += 1;
        break;
    }
  }
}

function tokenizePathData(data: string): Array<string | number> {
  const tokens: Array<string | number> = [];
  const pattern = /([AaCcHhLlMmQqSsTtVvZz])|([-+]?(?:\d+\.\d+|\d+\.?|\.\d+)(?:[eE][-+]?\d+)?)/g;
  let match: RegExpExecArray | null = pattern.exec(data);
  while (match !== null) {
    if (match[1]) {
      tokens.push(match[1]);
    } else if (match[2]) {
      tokens.push(Number.parseFloat(match[2]));
    }
    match = pattern.exec(data);
  }
  return tokens;
}

function hasNumber(tokens: Array<string | number>, index: number): boolean {
  return index < tokens.length && typeof tokens[index] === 'number';
}

function readPoint(
  tokens: Array<string | number>,
  index: number,
  absolute: boolean,
  cx: number,
  cy: number,
): { x: number; y: number; next: number } | undefined {
  if (!hasNumber(tokens, index) || !hasNumber(tokens, index + 1)) return undefined;
  const x = tokens[index] as number;
  const y = tokens[index + 1] as number;
  return {
    x: absolute ? x : cx + x,
    y: absolute ? y : cy + y,
    next: index + 2,
  };
}

function arcToCubics(
  path: Path,
  transform: Affine,
  x1: number,
  y1: number,
  rx: number,
  ry: number,
  phi: number,
  largeArc: boolean,
  sweep: boolean,
  x2: number,
  y2: number,
): void {
  if (rx === 0 || ry === 0 || (x1 === x2 && y1 === y2)) {
    lineTo(path, transform, x2, y2);
    return;
  }

  const cosPhi = Math.cos(phi);
  const sinPhi = Math.sin(phi);
  const dx = (x1 - x2) / 2;
  const dy = (y1 - y2) / 2;
  const x1p = cosPhi * dx + sinPhi * dy;
  const y1p = -sinPhi * dx + cosPhi * dy;

  let rrX = Math.abs(rx);
  let rrY = Math.abs(ry);
  const lambda = (x1p * x1p) / (rrX * rrX) + (y1p * y1p) / (rrY * rrY);
  if (lambda > 1) {
    const scale = Math.sqrt(lambda);
    rrX *= scale;
    rrY *= scale;
  }

  const num = rrX * rrX * rrY * rrY - rrX * rrX * y1p * y1p - rrY * rrY * x1p * x1p;
  const den = rrX * rrX * y1p * y1p + rrY * rrY * x1p * x1p;
  const sq = Math.sqrt(Math.max(0, num / den));
  const sign = largeArc === sweep ? -1 : 1;
  const cxp = (sign * sq * rrX * y1p) / rrY;
  const cyp = (sign * sq * -rrY * x1p) / rrX;

  const cx = cosPhi * cxp - sinPhi * cyp + (x1 + x2) / 2;
  const cy = sinPhi * cxp + cosPhi * cyp + (y1 + y2) / 2;

  const theta1 = angleBetween(1, 0, (x1p - cxp) / rrX, (y1p - cyp) / rrY);
  let delta = angleBetween(
    (x1p - cxp) / rrX,
    (y1p - cyp) / rrY,
    (-x1p - cxp) / rrX,
    (-y1p - cyp) / rrY,
  );

  if (!sweep && delta > 0) delta -= Math.PI * 2;
  if (sweep && delta < 0) delta += Math.PI * 2;

  ellipseToCubics(path, transform, cx, cy, rrX, rrY, phi, theta1, theta1 + delta, sweep, false);
}

function ellipseToCubics(
  path: Path,
  transform: Affine,
  cx: number,
  cy: number,
  rx: number,
  ry: number,
  phi: number,
  startAngle: number,
  endAngle: number,
  sweep: boolean,
  moveFirst: boolean,
): void {
  const total = endAngle - startAngle;
  const segmentCount = Math.max(1, Math.ceil(Math.abs(total) / (Math.PI / 2)));
  const step = total / segmentCount;

  if (moveFirst) {
    const start = ellipsePoint(cx, cy, rx, ry, phi, startAngle);
    moveTo(path, transform, start.x, start.y);
  }

  for (let i = 0; i < segmentCount; i += 1) {
    const t0 = startAngle + i * step;
    const t1 = t0 + step;
    const seg = cubicFromArc(cx, cy, rx, ry, phi, t0, t1);
    cubicTo(path, transform, seg.c1.x, seg.c1.y, seg.c2.x, seg.c2.y, seg.p.x, seg.p.y);
  }

  if (!sweep && Math.abs(total) >= Math.PI * 2 - 1e-6) {
    path.close();
  }
}

function cubicFromArc(
  cx: number,
  cy: number,
  rx: number,
  ry: number,
  phi: number,
  t0: number,
  t1: number,
): {
  c1: { x: number; y: number };
  c2: { x: number; y: number };
  p: { x: number; y: number };
} {
  const cosPhi = Math.cos(phi);
  const sinPhi = Math.sin(phi);
  const dt = t1 - t0;
  const alpha = (4 / 3) * Math.tan(dt / 4);

  const p0 = ellipsePoint(cx, cy, rx, ry, phi, t0);
  const p1 = ellipsePoint(cx, cy, rx, ry, phi, t1);

  const dx0 = -rx * Math.sin(t0);
  const dy0 = ry * Math.cos(t0);
  const dx1 = -rx * Math.sin(t1);
  const dy1 = ry * Math.cos(t1);

  const tx0 = cosPhi * dx0 - sinPhi * dy0;
  const ty0 = sinPhi * dx0 + cosPhi * dy0;
  const tx1 = cosPhi * dx1 - sinPhi * dy1;
  const ty1 = sinPhi * dx1 + cosPhi * dy1;

  return {
    c1: { x: p0.x + alpha * tx0, y: p0.y + alpha * ty0 },
    c2: { x: p1.x - alpha * tx1, y: p1.y - alpha * ty1 },
    p: p1,
  };
}

function ellipsePoint(
  cx: number,
  cy: number,
  rx: number,
  ry: number,
  phi: number,
  t: number,
): { x: number; y: number } {
  const cosPhi = Math.cos(phi);
  const sinPhi = Math.sin(phi);
  const x = rx * Math.cos(t);
  const y = ry * Math.sin(t);
  return {
    x: cx + cosPhi * x - sinPhi * y,
    y: cy + sinPhi * x + cosPhi * y,
  };
}

function angleBetween(ux: number, uy: number, vx: number, vy: number): number {
  const dot = ux * vx + uy * vy;
  const cross = ux * vy - uy * vx;
  return Math.atan2(cross, dot);
}

function parseTransform(raw: string | undefined): Affine {
  if (!raw) return IDENTITY;
  const pattern = /([A-Za-z]+)\s*\(([^)]*)\)/g;
  let out = IDENTITY;
  let match: RegExpExecArray | null = pattern.exec(raw);
  while (match !== null) {
    const op = match[1].toLowerCase();
    const values = parseNumberList(match[2]);
    const opMatrix = transformOp(op, values);
    out = composeAffine(out, opMatrix);
    match = pattern.exec(raw);
  }
  return out;
}

function transformOp(op: string, values: number[]): Affine {
  switch (op) {
    case 'matrix':
      if (values.length >= 6) {
        return [values[0], values[1], values[2], values[3], values[4], values[5]];
      }
      return IDENTITY;
    case 'translate': {
      const tx = values[0] ?? 0;
      const ty = values[1] ?? 0;
      return [1, 0, 0, 1, tx, ty];
    }
    case 'scale': {
      const sx = values[0] ?? 1;
      const sy = values[1] ?? sx;
      return [sx, 0, 0, sy, 0, 0];
    }
    case 'rotate': {
      const angle = ((values[0] ?? 0) * Math.PI) / 180;
      const cos = Math.cos(angle);
      const sin = Math.sin(angle);
      if (values.length >= 3) {
        const cx = values[1];
        const cy = values[2];
        return composeAffine(composeAffine([1, 0, 0, 1, cx, cy], [cos, sin, -sin, cos, 0, 0]), [
          1,
          0,
          0,
          1,
          -cx,
          -cy,
        ]);
      }
      return [cos, sin, -sin, cos, 0, 0];
    }
    case 'skewx': {
      const angle = ((values[0] ?? 0) * Math.PI) / 180;
      return [1, 0, Math.tan(angle), 1, 0, 0];
    }
    case 'skewy': {
      const angle = ((values[0] ?? 0) * Math.PI) / 180;
      return [1, Math.tan(angle), 0, 1, 0, 0];
    }
    default:
      return IDENTITY;
  }
}

function composeAffine(left: Affine, right: Affine): Affine {
  return [
    left[0] * right[0] + left[2] * right[1],
    left[1] * right[0] + left[3] * right[1],
    left[0] * right[2] + left[2] * right[3],
    left[1] * right[2] + left[3] * right[3],
    left[0] * right[4] + left[2] * right[5] + left[4],
    left[1] * right[4] + left[3] * right[5] + left[5],
  ];
}

function applyPoint(matrix: Affine, x: number, y: number): { x: number; y: number } {
  return {
    x: matrix[0] * x + matrix[2] * y + matrix[4],
    y: matrix[1] * x + matrix[3] * y + matrix[5],
  };
}

function moveTo(path: Path, matrix: Affine, x: number, y: number): void {
  const p = applyPoint(matrix, x, y);
  path.moveTo(p.x, p.y);
}

function lineTo(path: Path, matrix: Affine, x: number, y: number): void {
  const p = applyPoint(matrix, x, y);
  path.lineTo(p.x, p.y);
}

function quadTo(path: Path, matrix: Affine, cx: number, cy: number, x: number, y: number): void {
  const c = applyPoint(matrix, cx, cy);
  const p = applyPoint(matrix, x, y);
  path.quadTo(c.x, c.y, p.x, p.y);
}

function cubicTo(
  path: Path,
  matrix: Affine,
  c1x: number,
  c1y: number,
  c2x: number,
  c2y: number,
  x: number,
  y: number,
): void {
  const c1 = applyPoint(matrix, c1x, c1y);
  const c2 = applyPoint(matrix, c2x, c2y);
  const p = applyPoint(matrix, x, y);
  path.cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
}

function parseViewBox(raw: string | undefined): [number, number, number, number] | undefined {
  if (!raw) return undefined;
  const values = parseNumberList(raw);
  if (values.length < 4) return undefined;
  return [values[0], values[1], values[2], values[3]];
}

function parseNumber(raw: string | undefined): number | undefined {
  if (raw === undefined) return undefined;
  const match = /[-+]?(?:\d+\.\d+|\d+\.?|\.\d+)(?:[eE][-+]?\d+)?/.exec(raw);
  if (!match) return undefined;
  const value = Number.parseFloat(match[0]);
  return Number.isFinite(value) ? value : undefined;
}

function parseNumberList(raw: string): number[] {
  const out: number[] = [];
  FLOAT_PATTERN.lastIndex = 0;
  let match: RegExpExecArray | null = FLOAT_PATTERN.exec(raw);
  while (match !== null) {
    const value = Number.parseFloat(match[0]);
    if (Number.isFinite(value)) out.push(value);
    match = FLOAT_PATTERN.exec(raw);
  }
  return out;
}

function parsePaint(
  raw: string,
  gradients: ReadonlyMap<string, GradientPaint>,
): number | SvgPaint | undefined {
  const input = raw.trim();
  if (!input) return undefined;

  const gradientMatch = /^url\(#([^)]+)\)$/.exec(input);
  if (gradientMatch) {
    return gradients.get(gradientMatch[1]);
  }

  if (input.toLowerCase() === 'currentcolor') {
    return undefined;
  }

  return parseColor(input);
}

function parseColor(raw: string): number | undefined {
  const input = raw.trim().toLowerCase();
  if (!input) return undefined;
  if (input in COLOR_NAMES) return COLOR_NAMES[input];
  if (input.startsWith('#')) return parseHexColor(input);
  if (input.startsWith('rgb(') || input.startsWith('rgba(')) return parseRgbColor(input);
  return undefined;
}

function parseHexColor(input: string): number | undefined {
  const hex = input.slice(1);
  if (!/^[0-9a-f]+$/i.test(hex)) return undefined;
  if (hex.length === 3) {
    const rr = `${hex[0]}${hex[0]}`;
    const gg = `${hex[1]}${hex[1]}`;
    const bb = `${hex[2]}${hex[2]}`;
    return Number.parseInt(`${rr}${gg}${bb}ff`, 16) >>> 0;
  }
  if (hex.length === 6) return Number.parseInt(`${hex}ff`, 16) >>> 0;
  if (hex.length === 8) return Number.parseInt(hex, 16) >>> 0;
  return undefined;
}

function parseRgbColor(input: string): number | undefined {
  const open = input.indexOf('(');
  const close = input.lastIndexOf(')');
  if (open < 0 || close <= open) return undefined;
  const fields = input
    .slice(open + 1, close)
    .split(',')
    .map((part) => part.trim());

  if (fields.length !== 3 && fields.length !== 4) return undefined;
  const r = parseRgbChannel(fields[0]);
  const g = parseRgbChannel(fields[1]);
  const b = parseRgbChannel(fields[2]);
  if (r === undefined || g === undefined || b === undefined) return undefined;

  let a = 255;
  if (fields.length === 4) {
    const alpha = parseAlpha(fields[3]);
    if (alpha === undefined) return undefined;
    a = alpha;
  }

  return ((r << 24) | (g << 16) | (b << 8) | a) >>> 0;
}

function parseRgbChannel(raw: string): number | undefined {
  if (raw.endsWith('%')) {
    const pct = Number.parseFloat(raw.slice(0, -1));
    if (!Number.isFinite(pct)) return undefined;
    return clampByte((pct / 100) * 255);
  }
  const value = Number.parseFloat(raw);
  if (!Number.isFinite(value)) return undefined;
  return clampByte(value);
}

function parseAlpha(raw: string): number | undefined {
  if (raw.endsWith('%')) {
    const pct = Number.parseFloat(raw.slice(0, -1));
    if (!Number.isFinite(pct)) return undefined;
    return clampByte((pct / 100) * 255);
  }
  const value = Number.parseFloat(raw);
  if (!Number.isFinite(value)) return undefined;
  if (value <= 1) return clampByte(value * 255);
  return clampByte(value);
}

function parseSvgScalar(raw: string | undefined): number | undefined {
  if (raw === undefined) return undefined;
  const value = raw.trim();
  if (!value) return undefined;
  if (value.endsWith('%')) {
    const pct = Number.parseFloat(value.slice(0, -1));
    return Number.isFinite(pct) ? pct / 100 : undefined;
  }
  return parseNumber(value);
}

function parseGradientOffset(raw: string | undefined): number | undefined {
  const value = parseSvgScalar(raw);
  if (value === undefined) return undefined;
  return Math.max(0, Math.min(1, value));
}

function parseStopColor(colorRaw: string | undefined, opacityRaw: string | undefined): number {
  const color = parseColor(colorRaw ?? 'black') ?? 0x000000ff;
  if (opacityRaw === undefined) return color;
  const opacity = parseAlpha(opacityRaw);
  if (opacity === undefined) return color;
  return applyOpacity(color, opacity);
}

function applyOpacity(color: number, opacity: number): number {
  const alpha = clampByte((color & 0xff) * (opacity / 255));
  return ((color & 0xffffff00) | alpha) >>> 0;
}

function clampByte(value: number): number {
  return Math.max(0, Math.min(255, Math.round(value)));
}

function isUniformScale(matrix: Affine): boolean {
  const sx = Math.hypot(matrix[0], matrix[1]);
  const sy = Math.hypot(matrix[2], matrix[3]);
  if (sx === 0 || sy === 0) return false;
  const dot = matrix[0] * matrix[2] + matrix[1] * matrix[3];
  return Math.abs(sx - sy) < 1e-9 && Math.abs(dot) < 1e-9;
}
