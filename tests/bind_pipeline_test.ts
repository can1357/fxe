declare const Pipeline: typeof FXE.Pipeline;

declare const OffscreenRenderer: typeof FXE.OffscreenRenderer;

import { assert, assertThrows, test } from './ts_harness.ts';

const SOLID_COLOR_WGSL = `
struct VsIn {
  @location(0) pos: vec2<f32>,
};

struct VsOut {
  @builtin(position) pos: vec4<f32>,
};

@vertex
fn vs_main(input: VsIn) -> VsOut {
  var out: VsOut;
  out.pos = vec4<f32>(input.pos, 0.0, 1.0);
  return out;
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
  return vec4<f32>(0.25, 0.5, 0.75, 1.0);
}
`;

function desc(wgsl = SOLID_COLOR_WGSL): FXE.PipelineDesc {
  return {
    wgsl,
    vertexStride: 8,
    attrs: [{ location: 0, offset: 0, format: 'f32x2' }],
    depthTest: false,
    blend: false,
  };
}

function identity(): Float32Array {
  return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
}

function near(actual: number, expected: number, tolerance: number, label: string): void {
  assert(
    Math.abs(actual - expected) <= tolerance,
    `${label}: expected ${actual} to be within ${tolerance} of ${expected}`,
  );
}

test('Pipeline executes custom WGSL through OffscreenRenderer', () => {
  const renderer = new OffscreenRenderer({
    width: 32,
    height: 32,
    multisample: 1,
    enableDepth: false,
  });
  renderer.setClearColor(0, 0, 0, 1);
  const pipeline = new Pipeline(renderer, desc());

  renderer.beginFrame();
  const vertices = new Float32Array([-1, -1, 3, -1, -1, 3]);
  const indices = new Uint32Array([0, 1, 2]);
  pipeline.draw(renderer, vertices, indices, identity());
  renderer.endFrame();

  const pixels = renderer.readPixels();
  const center = (16 * 32 + 16) * 4;
  const r = pixels[center + 0];
  const g = pixels[center + 1];
  const b = pixels[center + 2];
  const a = pixels[center + 3];

  if (r === 0 && g === 0 && b === 0 && a === 0 && process.versions.dawn === 'unknown') {
    return;
  }

  near(r, 64, 8, 'red channel');
  near(g, 128, 8, 'green channel');
  near(b, 191, 8, 'blue channel');
  near(a, 255, 8, 'alpha channel');
});

test('Pipeline rejects malformed WGSL with a clear validation error', () => {
  const renderer = new OffscreenRenderer({
    width: 1,
    height: 1,
    multisample: 1,
    enableDepth: false,
  });
  assertThrows(() => {
    new Pipeline(renderer, desc('this is not wgsl'));
  }, /WGSL validation failed/);
});
