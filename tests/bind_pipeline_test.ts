declare const Pipeline: typeof FXE.Pipeline;

import { assert, assertEqual, assertThrows, test } from './ts_harness.ts';

const TRIANGLE = 0 as FXE.VertexTopology;

const PASSTHROUGH_WGSL = `
struct Uniforms { m: mat4x4<f32> };
@group(1) @binding(0) var<uniform> uniforms: Uniforms;

struct VsIn {
  @location(0) pos: vec3<f32>,
  @location(1) uv: vec2<f32>,
};

struct VsOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(input: VsIn) -> VsOut {
  var out: VsOut;
  out.pos = uniforms.m * vec4<f32>(input.pos, 1.0);
  out.uv = input.uv;
  return out;
}

@fragment
fn fs_main(input: VsOut) -> @location(0) vec4<f32> {
  return vec4<f32>(input.uv, 1.0, 1.0);
}
`;

function invisibleWindow(): FXE.Window {
  return new Window({
    width: 64,
    height: 48,
    visible: false,
    decorated: false,
    resizable: false,
    title: 'bind-pipeline-test',
  });
}

function desc(wgsl = PASSTHROUGH_WGSL): FXE.PipelineDesc {
  return {
    wgsl,
    vertexStride: 20,
    attrs: [
      { location: 0, offset: 0, format: 'f32x3' },
      { location: 1, offset: 12, format: 'f32x2' },
    ],
  };
}

function identity(): Float32Array {
  return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
}

test('Pipeline records a custom triangle draw into CommandBuffer', () => {
  const win = invisibleWindow();
  try {
    const renderer = new Renderer(win, { multisampleCount: 1, enableBloom: false, vsync: false });
    const pipeline = new Pipeline(renderer, desc());
    pipeline.updateUniforms(identity());

    const cb = new CommandBuffer();
    const vertices = new Float32Array([
      -0.5, -0.5, 0, 0, 0, 0.5, -0.5, 0, 1, 0, 0.0, 0.5, 0, 0.5, 1,
    ]);
    const indices = new Uint32Array([0, 1, 2]);
    pipeline.draw(cb, vertices, indices, identity());

    assertEqual(cb.vertexCount(), 3, 'pipeline draw records vertex count');
    assertEqual(cb.indexCount(TRIANGLE), 3, 'pipeline draw records index count');
    assert(!cb.isEmpty(), 'pipeline draw marks command buffer non-empty');
  } finally {
    win.close();
  }
});

test('Pipeline rejects malformed WGSL with a clear validation error', () => {
  const win = invisibleWindow();
  try {
    const renderer = new Renderer(win, { multisampleCount: 1, enableBloom: false, vsync: false });
    assertThrows(() => {
      new Pipeline(renderer, desc('this is not wgsl'));
    }, /WGSL validation failed/);
  } finally {
    win.close();
  }
});
