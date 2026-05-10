import { CommandBuffer, VertexTopology } from 'fxe';
import type { LayoutResult } from '../layout/types.ts';

const FLOATS_PER_VERTEX = 8;
const X = 0;
const Y = 1;
const Z = 2;
const IS_WORLD = 3;
const COLOR = 4;
const U = 5;
const V = 6;
const TEXTURE = 7;
const TRIANGLE = VertexTopology.Triangle;
const LINE = VertexTopology.Line;

interface ClipVertex {
  x: number;
  y: number;
  z: number;
  isWorld: number;
  color: number;
  u: number;
  v: number;
  texture: number;
}

interface ClipRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

// FXE CommandBuffer currently exposes raw vertices/indices but no renderer
// scissor, stencil, or clip-stack API. This fallback is therefore geometry
// clipping, not a real GPU clip state. It is exact for the screen-space
// triangle and line primitives emitted by fxe-ui painters (rects, glyph quads,
// image quads, zero-thickness outlines): x/y/z, uv, and RGBA vertex attributes
// are linearly interpolated at clip edges and the texture id is preserved when
// an edge belongs to a single textured primitive.
//
// Deliberate limits:
// - World-space vertices (`is_world != 0`) cannot be compared truthfully with a
//   screen-space layout rect without the renderer's view/projection matrices, so
//   clipped containers conservatively drop those primitives.
// - Curves and thick strokes are only as truthful as the triangles already
//   present in the CommandBuffer; there is no analytic re-stroking at the clip
//   boundary.
// - Custom buffers that vary texture id across one primitive are clipped with
//   the start vertex's texture id for generated edge vertices. FXE's built-in
//   primitives do not do this.
export function coarseClip(
  children: CommandBuffer,
  rect: LayoutResult,
  translate: { x: number; y: number } = { x: 0, y: 0 },
): CommandBuffer {
  const clip = normalizeRect(rect);
  const out = new CommandBuffer();
  if (clip === null || children.__fxe_v_len === 0) return out;

  const verts = children.vertexBuffer();
  const words = new Uint32Array(verts.buffer, verts.byteOffset, verts.length);
  const readVertex = (index: number): ClipVertex | null => {
    const base = index * FLOATS_PER_VERTEX;
    if (base < 0 || base + TEXTURE >= verts.length) return null;
    return {
      x: verts[base + X] + translate.x,
      y: verts[base + Y] + translate.y,
      z: verts[base + Z],
      isWorld: verts[base + IS_WORLD],
      color: words[base + COLOR],
      u: verts[base + U],
      v: verts[base + V],
      texture: words[base + TEXTURE],
    };
  };

  const triangles: ClipVertex[] = [];
  const triangleIndices = children.indexBuffer(TRIANGLE);
  for (let i = 0; i + 2 < triangleIndices.length; i += 3) {
    const a = readVertex(triangleIndices[i]);
    const b = readVertex(triangleIndices[i + 1]);
    const c = readVertex(triangleIndices[i + 2]);
    if (!a || !b || !c || !isScreenPrimitive(a, b, c)) continue;
    const minX = Math.min(a.x, b.x, c.x);
    const maxX = Math.max(a.x, b.x, c.x);
    const minY = Math.min(a.y, b.y, c.y);
    const maxY = Math.max(a.y, b.y, c.y);
    if (outsideRect(minX, minY, maxX, maxY, clip)) continue;
    if (insideRect(minX, minY, maxX, maxY, clip)) {
      triangles.push(a, b, c);
      continue;
    }
    const polygon = clipPolygon([a, b, c], clip);
    for (let j = 1; j + 1 < polygon.length; ++j) {
      triangles.push(polygon[0], polygon[j], polygon[j + 1]);
    }
  }
  writeTopology(out, triangles, TRIANGLE);

  const lines: ClipVertex[] = [];
  const lineIndices = children.indexBuffer(LINE);
  for (let i = 0; i + 1 < lineIndices.length; i += 2) {
    const a = readVertex(lineIndices[i]);
    const b = readVertex(lineIndices[i + 1]);
    if (!a || !b || !isScreenPrimitive(a, b)) continue;
    const minX = Math.min(a.x, b.x);
    const maxX = Math.max(a.x, b.x);
    const minY = Math.min(a.y, b.y);
    const maxY = Math.max(a.y, b.y);
    if (outsideRect(minX, minY, maxX, maxY, clip)) continue;
    if (insideRect(minX, minY, maxX, maxY, clip)) {
      lines.push(a, b);
      continue;
    }
    const segment = clipLine(a, b, clip);
    if (segment) lines.push(segment[0], segment[1]);
  }
  writeTopology(out, lines, LINE);

  return out;
}

function normalizeRect(rect: LayoutResult): ClipRect | null {
  const left = rect.x;
  const top = rect.y;
  const right = rect.x + rect.width;
  const bottom = rect.y + rect.height;
  if (
    !Number.isFinite(left) ||
    !Number.isFinite(top) ||
    !Number.isFinite(right) ||
    !Number.isFinite(bottom) ||
    right <= left ||
    bottom <= top
  ) {
    return null;
  }
  return { left, top, right, bottom };
}

function isScreenPrimitive(...vertices: ClipVertex[]): boolean {
  return vertices.every((vertex) => vertex.isWorld === 0);
}

function outsideRect(
  minX: number,
  minY: number,
  maxX: number,
  maxY: number,
  rect: ClipRect,
): boolean {
  return maxX < rect.left || minX > rect.right || maxY < rect.top || minY > rect.bottom;
}

function insideRect(
  minX: number,
  minY: number,
  maxX: number,
  maxY: number,
  rect: ClipRect,
): boolean {
  return minX >= rect.left && maxX <= rect.right && minY >= rect.top && maxY <= rect.bottom;
}

function clipPolygon(vertices: ClipVertex[], rect: ClipRect): ClipVertex[] {
  let out = clipPolygonEdge(
    vertices,
    (v) => v.x >= rect.left,
    (a, b) => intersectX(a, b, rect.left),
  );
  out = clipPolygonEdge(
    out,
    (v) => v.x <= rect.right,
    (a, b) => intersectX(a, b, rect.right),
  );
  out = clipPolygonEdge(
    out,
    (v) => v.y >= rect.top,
    (a, b) => intersectY(a, b, rect.top),
  );
  out = clipPolygonEdge(
    out,
    (v) => v.y <= rect.bottom,
    (a, b) => intersectY(a, b, rect.bottom),
  );
  return out;
}

function clipPolygonEdge(
  vertices: ClipVertex[],
  inside: (vertex: ClipVertex) => boolean,
  intersect: (a: ClipVertex, b: ClipVertex) => ClipVertex,
): ClipVertex[] {
  if (vertices.length === 0) return [];
  const out: ClipVertex[] = [];
  let prev = vertices[vertices.length - 1];
  let prevInside = inside(prev);
  for (const curr of vertices) {
    const currInside = inside(curr);
    if (currInside) {
      if (!prevInside) out.push(intersect(prev, curr));
      out.push(curr);
    } else if (prevInside) {
      out.push(intersect(prev, curr));
    }
    prev = curr;
    prevInside = currInside;
  }
  return out;
}

function clipLine(a: ClipVertex, b: ClipVertex, rect: ClipRect): [ClipVertex, ClipVertex] | null {
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  let t0 = 0;
  let t1 = 1;

  const accept = (p: number, q: number): boolean => {
    if (p === 0) return q >= 0;
    const r = q / p;
    if (p < 0) {
      if (r > t1) return false;
      if (r > t0) t0 = r;
    } else {
      if (r < t0) return false;
      if (r < t1) t1 = r;
    }
    return true;
  };

  if (
    !accept(-dx, a.x - rect.left) ||
    !accept(dx, rect.right - a.x) ||
    !accept(-dy, a.y - rect.top) ||
    !accept(dy, rect.bottom - a.y)
  ) {
    return null;
  }

  return [interpolateVertex(a, b, t0), interpolateVertex(a, b, t1)];
}

function intersectX(a: ClipVertex, b: ClipVertex, x: number): ClipVertex {
  const dx = b.x - a.x;
  return interpolateVertex(a, b, dx === 0 ? 0 : (x - a.x) / dx);
}

function intersectY(a: ClipVertex, b: ClipVertex, y: number): ClipVertex {
  const dy = b.y - a.y;
  return interpolateVertex(a, b, dy === 0 ? 0 : (y - a.y) / dy);
}

function interpolateVertex(a: ClipVertex, b: ClipVertex, t: number): ClipVertex {
  const clamped = Math.max(0, Math.min(1, t));
  return {
    x: lerp(a.x, b.x, clamped),
    y: lerp(a.y, b.y, clamped),
    z: lerp(a.z, b.z, clamped),
    isWorld: a.isWorld,
    color: interpolateColor(a.color, b.color, clamped),
    u: lerp(a.u, b.u, clamped),
    v: lerp(a.v, b.v, clamped),
    texture: a.texture,
  };
}

function lerp(a: number, b: number, t: number): number {
  return a + (b - a) * t;
}

function interpolateColor(a: number, b: number, t: number): number {
  if (a === b) return a;
  let out = 0;
  for (let shift = 0; shift < 32; shift += 8) {
    const ca = (a >>> shift) & 0xff;
    const cb = (b >>> shift) & 0xff;
    out |= Math.round(lerp(ca, cb, t)) << shift;
  }
  return out >>> 0;
}

function writeTopology(out: CommandBuffer, vertices: ClipVertex[], topology: VertexTopology): void {
  if (vertices.length === 0) return;
  const allocation = out.allocate(vertices.length, vertices.length, topology);
  const words = new Uint32Array(
    allocation.verts.buffer,
    allocation.verts.byteOffset,
    allocation.verts.length,
  );
  for (let i = 0; i < vertices.length; ++i) {
    const vertex = vertices[i];
    const base = i * FLOATS_PER_VERTEX;
    allocation.verts[base + X] = vertex.x;
    allocation.verts[base + Y] = vertex.y;
    allocation.verts[base + Z] = vertex.z;
    allocation.verts[base + IS_WORLD] = vertex.isWorld;
    words[base + COLOR] = vertex.color;
    allocation.verts[base + U] = vertex.u;
    allocation.verts[base + V] = vertex.v;
    words[base + TEXTURE] = vertex.texture;
    allocation.idxs[i] = allocation.base + i;
  }
}
