// Editor-grade text paint primitives: drawTextSpans, drawSelectionRects,
// drawDecorationUnderline.
//
// Verifies command-buffer mutation per call. Span equivalence to N drawText
// calls is checked by comparing vertex-count growth on a single-character
// span vs a single drawText with the same text/color.

import { assert, assertEqual, test } from './ts_harness.ts';

const TRIANGLE = 0 as FXE.VertexTopology;
const LINE = 1 as FXE.VertexTopology;
const WHITE = 0xffffffff;
const RED = 0xff0000ff;
const BLUE = 0x0000ffff;

type Ext = FXE.PrimitivesNamespace & {
  drawTextSpans(
    cb: FXE.CommandBuffer | FXE.Renderer,
    x: number,
    y: number,
    depth: number,
    spans: ReadonlyArray<{
      text: string;
      color?: FXE.Color;
      size?: number;
      bold?: boolean;
      italic?: boolean;
      underline?: boolean;
      strikethrough?: boolean;
    }>,
    opts?: { tabSize?: number; tabOriginX?: number; size?: number; color?: FXE.Color },
  ): [number, number, number, number];
  drawSelectionRects(
    cb: FXE.CommandBuffer | FXE.Renderer,
    rects: Float32Array,
    color?: FXE.Color,
    depth?: number,
  ): void;
  drawDecorationUnderline(
    cb: FXE.CommandBuffer | FXE.Renderer,
    x1: number,
    x2: number,
    y: number,
    style: 'solid' | 'dashed' | 'dotted' | 'wavy',
    color?: FXE.Color,
    thickness?: number,
    depth?: number,
  ): void;
  drawText(
    cb: FXE.CommandBuffer | FXE.Renderer,
    at: FXE.Vec2,
    depth: number,
    text: string,
    opts?: { color?: FXE.Color; size?: number; tabSize?: number },
  ): [number, number, number, number];
};

const Pr = Primitives as Ext;

function counts(cb: FXE.CommandBuffer): { v: number; tri: number; line: number } {
  return {
    v: cb.vertexCount(),
    tri: cb.indexCount(TRIANGLE),
    line: cb.indexCount(LINE),
  };
}

test('drawTextSpans emits geometry for each non-empty span', () => {
  const cb = new CommandBuffer();
  const before = counts(cb);
  const ret = Pr.drawTextSpans(
    cb,
    10,
    10,
    0,
    [
      { text: 'hello ', color: WHITE, size: 16 },
      { text: 'world', color: RED, size: 16 },
    ],
    { size: 16 },
  );
  const after = counts(cb);
  assert(after.v > before.v, 'spans should append vertices');
  assert(after.tri > before.tri, 'spans should append triangle indices');
  assertEqual(ret.length, 4);
  assert(ret[0] > 0, 'width > 0');
  // glyphCount tracks total characters emitted across spans (>=11)
  assert(ret[3] >= 11, `glyph count >= 11, got ${ret[3]}`);
});

test('drawTextSpans skips empty span text', () => {
  const cb = new CommandBuffer();
  const before = counts(cb);
  Pr.drawTextSpans(cb, 0, 0, 0, [{ text: '' }, { text: '' }]);
  const after = counts(cb);
  assertEqual(after.v, before.v);
  assertEqual(after.tri, before.tri);
});

test('drawTextSpans equivalent vertex count to single drawText for one span', () => {
  const a = new CommandBuffer();
  const b = new CommandBuffer();
  Pr.drawText(a, [10, 10] as unknown as FXE.Vec2, 0, 'hello', { color: WHITE, size: 14 });
  Pr.drawTextSpans(b, 10, 10, 0, [{ text: 'hello', color: WHITE, size: 14 }]);
  const ca = counts(a);
  const cb = counts(b);
  assertEqual(ca.v, cb.v, `vertex counts must match (drawText=${ca.v}, drawTextSpans=${cb.v})`);
  assertEqual(ca.tri, cb.tri, 'triangle index counts must match');
});

test('drawTextSpans underline emits line topology', () => {
  const cb = new CommandBuffer();
  const before = counts(cb);
  Pr.drawTextSpans(cb, 0, 0, 0, [
    { text: 'underlined', size: 16, underline: true, color: BLUE },
  ]);
  const after = counts(cb);
  assert(after.line > before.line, 'underline should emit at least one line index');
});

test('drawTextSpans tabSize advances pen to tab stops', () => {
  const cb = new CommandBuffer();
  const noTabRet = Pr.drawTextSpans(cb, 0, 0, 0, [{ text: 'a', size: 16 }]);
  const cb2 = new CommandBuffer();
  const tabRet = Pr.drawTextSpans(
    cb2,
    0,
    0,
    0,
    [{ text: 'a\tb', size: 16 }],
    { tabSize: 64 },
  );
  // Pen with tab should land at >= tabSize after first 'a' (64) + 'b' width.
  assert(tabRet[0] >= 64, `tab span width ${tabRet[0]} should be >= 64`);
  assert(tabRet[0] > noTabRet[0], 'tab span wider than no-tab span');
});

test('drawSelectionRects appends one quad per rect', () => {
  const cb = new CommandBuffer();
  const before = counts(cb);
  const rects = new Float32Array([0, 0, 100, 18, 0, 18, 80, 18, 0, 36, 60, 18]);
  Pr.drawSelectionRects(cb, rects, 0xff8800aa);
  const after = counts(cb);
  // Each fill_rect in this codebase emits a 4-vertex strip → 6 triangle indices.
  assertEqual(after.v - before.v, 12, 'three rects → 12 vertices');
  assertEqual(after.tri - before.tri, 18, 'three rects → 18 triangle indices');
});

test('drawSelectionRects with empty array is a no-op', () => {
  const cb = new CommandBuffer();
  const before = counts(cb);
  Pr.drawSelectionRects(cb, new Float32Array([]), WHITE);
  const after = counts(cb);
  assertEqual(after.v, before.v);
  assertEqual(after.tri, before.tri);
});

test('drawSelectionRects skips zero-size rects', () => {
  const cb = new CommandBuffer();
  Pr.drawSelectionRects(cb, new Float32Array([0, 0, 0, 0]), WHITE);
  const after = counts(cb);
  assertEqual(after.tri, 0, 'zero rect should not emit triangles');
});

for (const style of ['solid', 'dashed', 'dotted', 'wavy'] as const) {
  test(`drawDecorationUnderline ${style} emits line geometry`, () => {
    const cb = new CommandBuffer();
    const before = counts(cb);
    Pr.drawDecorationUnderline(cb, 0, 100, 20, style, RED, 1.5);
    const after = counts(cb);
    assert(after.line > before.line, `${style} should emit line indices`);
  });
}

test('drawDecorationUnderline x2<=x1 is a no-op', () => {
  const cb = new CommandBuffer();
  const before = counts(cb);
  Pr.drawDecorationUnderline(cb, 50, 50, 20, 'solid', WHITE);
  Pr.drawDecorationUnderline(cb, 50, 0, 20, 'wavy', WHITE);
  const after = counts(cb);
  assertEqual(after.line, before.line);
});

test('drawText accepts tabSize without crashing', () => {
  const cb = new CommandBuffer();
  Pr.drawText(cb, [0, 0] as unknown as FXE.Vec2, 0, 'a\tb\tc', {
    color: WHITE,
    size: 16,
    tabSize: 32,
  });
  const after = counts(cb);
  assert(after.v > 0, 'tab text should still emit glyph quads');
});
