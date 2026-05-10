import { Renderer, Window } from 'fxe';

declare const Pipeline: typeof FXE.Pipeline;

const win = new Window({ width: 720, height: 420, title: 'fxe — custom pipeline' });
const renderer = new Renderer(win, { multisampleCount: 1 });

const wgsl = `
struct Uniforms { mvp: mat4x4<f32> };
@group(1) @binding(0) var<uniform> uniforms: Uniforms;
@group(1) @binding(1) var custom_sampler: sampler;
@group(1) @binding(2) var custom_texture: texture_2d<f32>;

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
  out.pos = uniforms.mvp * vec4<f32>(input.pos, 1.0);
  out.uv = input.uv;
  return out;
}

@fragment
fn fs_main(input: VsOut) -> @location(0) vec4<f32> {
  let texel = textureSample(custom_texture, custom_sampler, input.uv);
  return vec4<f32>(texel.rg * input.uv, texel.b, texel.a);
}
`;

const pipeline = new Pipeline(renderer, {
  wgsl,
  vertexStride: 20,
  attrs: [
    { location: 0, offset: 0, format: 'f32x3' },
    { location: 1, offset: 12, format: 'f32x2' },
  ],
});

const texture = Image.fromPixels(
  new Uint8Array([255, 64, 64, 255, 64, 255, 64, 255, 64, 64, 255, 255, 255, 255, 255, 255]),
  2,
  2,
);

const identity = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

const quad = new Float32Array([-1, -1, 0, 0, 1, 1, -1, 0, 1, 1, 1, 1, 0, 1, 0, -1, 1, 0, 0, 0]);
const indices = new Uint32Array([0, 1, 2, 0, 2, 3]);

pipeline.updateUniforms(identity);
pipeline.bindTexture(2, texture);

win.run(() => {
  renderer.beginFrame();
  renderer.setClearColor(0.03, 0.04, 0.08, 1);
  pipeline.draw(renderer, quad, indices, identity);
  renderer.endFrame();
});
