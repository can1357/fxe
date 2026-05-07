import { assert, assertDeepEqual, assertEqual, assertThrows, test } from './ts_harness.ts';

const TRIANGLE = 0 as FXE.VertexTopology;
const LINE = 1 as FXE.VertexTopology;
const WHITE = 0xffffffff;
const RED = 0xff0000ff;

type PrimitiveExtras = FXE.PrimitivesNamespace & {
  drawSprite(
    cb: FXE.CommandBuffer | FXE.Renderer,
    spriteId: number,
    x: number,
    y: number,
    width: number,
    height: number,
    depth?: number,
    tint?: FXE.Color,
  ): void;
  drawText(
    cb: FXE.CommandBuffer | FXE.Renderer,
    at: FXE.Vec2,
    depth: number,
    text: string,
    opts?: { color?: FXE.Color; size?: number; pt?: number; fontId?: number },
  ): [number, number, number, number];
};

const PrimitivesEx = Primitives as PrimitiveExtras;

function mat4Identity(): Float32Array {
  return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
}

function v4(x: number, y: number, z = 0, w = 0): Float32Array {
  return new Float32Array([x, y, z, w]);
}

function counts(cb: FXE.CommandBuffer): { vertices: number; triangle: number; line: number } {
  return {
    vertices: cb.vertexCount(),
    triangle: cb.indexCount(TRIANGLE),
    line: cb.indexCount(LINE),
  };
}

function assertIncreases(
  name: string,
  draw: (cb: FXE.CommandBuffer) => void,
  expectedIndexTopology: FXE.VertexTopology,
): void {
  const cb = new CommandBuffer();
  const before = counts(cb);
  draw(cb);
  const after = counts(cb);
  assert(after.vertices > before.vertices, `${name} should add vertices`);
  if (expectedIndexTopology === TRIANGLE) {
    assert(after.triangle > before.triangle, `${name} should add triangle indices`);
  } else {
    assert(after.line > before.line, `${name} should add line indices`);
  }
  assert(after.triangle >= before.triangle, `${name} should not remove triangle indices`);
  assert(after.line >= before.line, `${name} should not remove line indices`);
}

test('Primitives rect, line, and triangle methods append CommandBuffer geometry', () => {
  assertIncreases(
    'fillRect scalar',
    (cb) => {
      Primitives.fillRect(cb, 0, 1, 8, 9, 0.25, RED);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillRect vector',
    (cb) => {
      Primitives.fillRect(cb, [0, 1], [8, 9], 0.25, [1, 0, 0, 1]);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawRect scalar',
    (cb) => {
      Primitives.drawRect(cb, 0, 1, 8, 9, 0.25, WHITE, 0);
    },
    LINE,
  );

  assertIncreases(
    'drawRect vector',
    (cb) => {
      Primitives.drawRect(cb, [0, 1], [8, 9], 0.25, [0, 1, 0, 1], 0);
    },
    LINE,
  );

  assertIncreases(
    'drawLine thin',
    (cb) => {
      Primitives.drawLine(cb, v4(0, 0), v4(10, 3), WHITE, 0);
    },
    LINE,
  );

  assertIncreases(
    'drawLine thick',
    (cb) => {
      Primitives.drawLine(cb, [0, 0, 0, 0], [10, 3, 0, 0], [0, 0, 1, 1], 2);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillTriangle',
    (cb) => {
      Primitives.fillTriangle(cb, v4(0, 0), v4(8, 0), v4(0, 8), WHITE);
    },
    TRIANGLE,
  );
});

test('Primitives text and calcText are deterministic and append glyph geometry', () => {
  Font.load('/System/Library/Fonts/Monaco.ttf', 32);
  const small = Primitives.calcText('A', 12);
  const large = Primitives.calcText('A', 24);
  assertEqual(small.length, 2);
  assertEqual(large.length, 2);
  assert(small[0] > 0, 'calcText should report positive text width');
  assert(large[0] > small[0], 'larger point size should increase width');

  const cb = new CommandBuffer();
  const before = counts(cb);
  const bounds = Primitives.drawText(cb, [3, 4], 0.5, 'ABC', { color: [1, 1, 1, 1], pt: 16 });
  const after = counts(cb);
  assertEqual(bounds.length, 4);
  assert(bounds[0] > 0, 'drawText should return positive row width');
  assertEqual(bounds[3], 3, 'drawText should return glyph count');
  assert(after.vertices > before.vertices, 'drawText should add vertices');
  assert(after.triangle > before.triangle, 'drawText should add triangle indices');

  const fontDefault = new CommandBuffer();
  const fontExplicit = new CommandBuffer();
  const defaultBounds = Primitives.drawText(fontDefault, [0, 0], 0, 'A', { pt: 16 });
  const explicitBounds = PrimitivesEx.drawText(fontExplicit, [0, 0], 0, 'A', { pt: 16, fontId: 0 });
  assertDeepEqual(explicitBounds, defaultBounds, 'fontId 0 should resolve to the default font');
  assertEqual(
    fontExplicit.vertexCount(),
    fontDefault.vertexCount(),
    'fontId 0 should emit the same vertex count',
  );
  assertEqual(
    fontExplicit.indexCount(TRIANGLE),
    fontDefault.indexCount(TRIANGLE),
    'fontId 0 should emit the same triangle index count',
  );
});

test('Primitives rounded and blur methods append CommandBuffer geometry', () => {
  const m = mat4Identity();
  const radii = new Float32Array([0.1, 0.2, 0.3, 0.4]);
  const p1 = v4(0, 0);
  const p2 = v4(1, 0);
  const p3 = v4(0, 1);
  const p4 = v4(1, 1);

  assertIncreases(
    'fillQuadRounded',
    (cb) => {
      Primitives.fillQuadRounded(cb, p1, p2, p3, p4, radii, WHITE);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawQuadRounded',
    (cb) => {
      Primitives.drawQuadRounded(cb, p1, p2, p3, p4, radii, [1, 0, 1, 1], 1);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillRectRounded',
    (cb) => {
      Primitives.fillRectRounded(cb, m, radii, 0.25, WHITE);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawRectRounded',
    (cb) => {
      Primitives.drawRectRounded(cb, m, radii, 0.25, WHITE, 1);
    },
    TRIANGLE,
  );

  assertIncreases(
    'blurRect',
    (cb) => {
      Primitives.blurRect(cb, 0, 0, 8, 8, 0, WHITE, 2, 64, 64);
    },
    TRIANGLE,
  );

  assertIncreases(
    'blurQuad',
    (cb) => {
      Primitives.blurQuad(cb, p1, p2, p3, p4, WHITE, 2, 64, 64);
    },
    TRIANGLE,
  );
});

test('Primitives 3D and matrix primitives append CommandBuffer geometry', () => {
  const m = mat4Identity();

  assertIncreases(
    'fillEllipse',
    (cb) => {
      Primitives.fillEllipse(cb, m, WHITE, 1, 8);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawEllipse thick',
    (cb) => {
      Primitives.drawEllipse(cb, m, WHITE, 1, 1, 8);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawEllipse thin',
    (cb) => {
      Primitives.drawEllipse(cb, m, WHITE, 0, 1, 8);
    },
    LINE,
  );

  assertIncreases(
    'fillBox',
    (cb) => {
      Primitives.fillBox(cb, m, WHITE);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawBox',
    (cb) => {
      Primitives.drawBox(cb, m, WHITE, 1);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillCbox',
    (cb) => {
      Primitives.fillCbox(cb, m, WHITE);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawCbox',
    (cb) => {
      Primitives.drawCbox(cb, m, WHITE, 1);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillSphere',
    (cb) => {
      Primitives.fillSphere(cb, m, WHITE, 1, 1, 8);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillCylinder',
    (cb) => {
      Primitives.fillCylinder(cb, m, WHITE, 1, 8);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillPyramid',
    (cb) => {
      Primitives.fillPyramid(cb, m, WHITE);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawPyramid',
    (cb) => {
      Primitives.drawPyramid(cb, m, WHITE, 1);
    },
    TRIANGLE,
  );

  assertIncreases(
    'fillQuad',
    (cb) => {
      Primitives.fillQuad(cb, m, WHITE);
    },
    TRIANGLE,
  );

  assertIncreases(
    'drawQuad',
    (cb) => {
      Primitives.drawQuad(cb, m, WHITE, 1);
    },
    TRIANGLE,
  );
});

test('Primitives drawSprite appends placeholder rect geometry for exposed runtime binding', () => {
  const cb = new CommandBuffer();
  const before = counts(cb);
  PrimitivesEx.drawSprite(cb, 123, 2, 3, 4, 5, 0.75, [0.5, 0.5, 0.5, 1]);
  const after = counts(cb);
  assert(after.vertices > before.vertices, 'drawSprite should add vertices');
  assert(after.triangle > before.triangle, 'drawSprite should add triangle indices');
});

test('Primitives.drain executes batches and validates malformed batches', () => {
  const cb = new CommandBuffer();
  const executed = Primitives.drain(
    cb,
    new Uint32Array([
      Primitives.OP_FILL_RECT,
      Primitives.OP_DRAW_RECT,
      Primitives.OP_FILL_TRIANGLE,
      Primitives.OP_DRAW_LINE,
      Primitives.OP_DRAW_TEXT,
    ]),
    new Float32Array([
      0, 0, 4, 4, 0, 1, 1, 1, 1, 0, 0, 4, 4, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 4, 0, 0,
      0, 1, 0, 1, 0, 0, 0, 0, 4, 4, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 16, 1, 1, 1, 1, 1, 65,
    ]),
  );
  assertEqual(executed, 5);
  assert(cb.vertexCount() > 0, 'drain should add vertices');
  assert(cb.indexCount(TRIANGLE) > 0, 'drain should add triangle indices');
  assert(cb.indexCount(LINE) > 0, 'drain should add line indices');

  assertThrows(
    () => Primitives.drain(new CommandBuffer(), new Uint32Array([999]), new Float32Array()),
    /unsupported opcode 999/,
  );
  assertThrows(
    () =>
      Primitives.drain(
        new CommandBuffer(),
        new Uint32Array([Primitives.OP_FILL_RECT]),
        new Float32Array([0]),
      ),
    /truncated params for fillRect/,
  );
  assertThrows(
    () => Primitives.drain(new CommandBuffer(), new Uint32Array(), new Float32Array([1])),
    /unused params/,
  );
  assertThrows(
    () =>
      Primitives.drain(
        new CommandBuffer(),
        new Uint32Array([Primitives.OP_DRAW_TEXT]),
        new Float32Array([0, 0, 0, 16, 1, 1, 1, 1, 1, 128]),
      ),
    /ASCII codepoints/,
  );
  assertThrows(
    () =>
      Primitives.drain(
        new CommandBuffer(),
        new Float32Array() as unknown as Uint32Array,
        new Float32Array(),
      ),
    /expected Uint32Array/,
  );
});

test('Primitives methods throw for invalid arguments where bindings implement validation', () => {
  const cb = new CommandBuffer();

  assertThrows(
    () => Primitives.fillRect({} as FXE.CommandBuffer, [0, 0], [1, 1]),
    /first arg must be CommandBuffer/,
  );
  assertThrows(
    () => Primitives.fillRect(cb, [0] as unknown as FXE.Vec2, [1, 1]),
    /fillRect: expected/,
  );
  assertThrows(
    () => Primitives.drawRect(cb, [0] as unknown as FXE.Vec2, [1, 1]),
    /drawRect: expected/,
  );
  assertThrows(
    () => Primitives.drawLine(cb, [0, 0, 0] as unknown as FXE.Vec4, [1, 1, 1, 1]),
    /drawLine: src\/dst/,
  );
  assertThrows(
    () => Primitives.fillTriangle(cb, [0, 0, 0, 0], [1, 0, 0, 0], [0, 1, 0] as unknown as FXE.Vec4),
    /fillTriangle: a\/b\/c/,
  );
  assertThrows(() => Primitives.fillEllipse(cb, new Float32Array(15)), /expected mat4/);
  assertThrows(() => Primitives.drawEllipse(cb, new Float32Array(15)), /expected mat4/);
  assertThrows(() => Primitives.fillBox(cb, new Float32Array(15)), /expected mat4/);
  assertThrows(() => Primitives.drawBox(cb, new Float32Array(15)), /expected mat4/);
  assertThrows(
    () => Primitives.drawText(cb, [0] as unknown as FXE.Vec2, 0, 'x'),
    /drawText: expected/,
  );
  assertThrows(
    () => (PrimitivesEx.drawSprite as (...args: unknown[]) => void)(cb, 1, 0, 0, 1),
    /drawSprite/,
  );
});
