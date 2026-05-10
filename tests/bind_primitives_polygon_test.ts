import { assert, assertEqual, test } from './ts_harness.ts';

const TRIANGLE = 0 as FXE.VertexTopology;
const WHITE = 0xffffffff;

function makePolygonPoints(): Float32Array {
  return new Float32Array([0, 0, 24, 0, 24, 18, 0, 18]);
}

function makeStrokePoints(): Float32Array {
  return new Float32Array([4, 4, 28, 10, 12, 24]);
}

function makePath(): FXE.Path {
  const path = new Path();
  path.moveTo(0, 0);
  path.lineTo(20, 0);
  path.lineTo(32, 12);
  path.lineTo(48, 12);
  return path;
}

function assertGeometry(name: string, cb: FXE.CommandBuffer): void {
  assert(cb.vertexCount() > 0, `${name} should emit vertices`);
  assert(cb.indexCount(TRIANGLE) > 0, `${name} should emit triangle indices`);
}

test('polygon primitives and text-on-path append command buffer geometry', () => {
  const fillCb = new CommandBuffer();
  Primitives.fillPolygon(fillCb, makePolygonPoints(), WHITE, 0.1);
  assertGeometry('fillPolygon', fillCb);

  const strokeCb = new CommandBuffer();
  Primitives.strokePolygon(
    strokeCb,
    makeStrokePoints(),
    [0, 1, 1, 1],
    2,
    true,
    'round',
    'round',
    0.2,
  );
  assertGeometry('strokePolygon', strokeCb);

  const dashCb = new CommandBuffer();
  Primitives.strokePath(
    dashCb,
    makePath(),
    WHITE,
    2,
    'miter',
    'round',
    0.3,
    new Float32Array([6, 4]),
    1,
  );
  assertGeometry('strokePath dashed', dashCb);

  const textCb = new CommandBuffer();
  const bounds = Primitives.drawTextOnPath(
    textCb,
    makePath(),
    'Path text',
    { fontId: Font.builtin('default') },
    { pt: 16, color: [1, 1, 1, 1] },
    2,
    0.4,
  );
  assertGeometry('drawTextOnPath', textCb);
  assertEqual(bounds.length, 4);
  assert(bounds[0] > 0, 'drawTextOnPath should report positive width');
  assert(bounds[3] > 0, 'drawTextOnPath should report emitted glyphs');
});
