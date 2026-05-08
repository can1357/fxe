// WebGPU port of the gfw renderer shader entry points. The current backend
// wires the colour pipeline and keeps post-process entry points available so the
// Dawn module validates as one shader source while renderer passes are expanded.

// Mirrors the prefix of fxe::vshader_cbuf. The C++ side writes the full 256-byte
// vshader_cbuf into the UBO; the shader only needs the matrices + tint, so we
// declare fewer fields and let the shader read the prefix.
struct Constants {
  world_view_proj: mat4x4<f32>,  //   0..63
  screen_ndc:      mat4x4<f32>,  //  64..127
  tint:            vec4<f32>,    // 128..143
  capture_offset:  vec2<f32>,    // 144..151
  capture_scale:   vec2<f32>,    // 152..159
};

@group(0) @binding(0) var<uniform> cbuf: Constants;
@group(0) @binding(1) var atlas_sampler: sampler;
@group(0) @binding(2) var atlas_tex: texture_2d<f32>;
// Font mask page (R8Unorm). Sampled when FONT_MASK_FLAG is set on the
// vertex's texture id. The .r channel carries the alpha mask; vertex color
// multiplies in. Mirrors HarfBuzz/FreeType/CoreText alpha-bitmap output.
@group(0) @binding(3) var font_mask_tex: texture_2d<f32>;
// Font color emoji page (BGRA8Unorm with kCGBitmapByteOrder32Little →
// in-memory layout is BGRA which the GPU surfaces as RGBA when read with
// the device's native byte order). Sampled when FONT_COLOR_FLAG is set;
// vertex color is bypassed because emoji are pre-painted.
@group(0) @binding(4) var font_color_tex: texture_2d<f32>;
@group(0) @binding(5) var framebuffer_sampler: sampler;
@group(0) @binding(6) var framebuffer_texture: texture_2d<f32>;
// Nearest-filter sampler dedicated to the font mask page. Mask glyphs are
// rasterised at framebuffer resolution (logical pt × device_pixel_ratio) so
// any linear blend between adjacent texels softens otherwise crisp output.
// Color emoji and sprites stay on `atlas_sampler` for normal filtering.
@group(0) @binding(7) var mask_sampler: sampler;
// User texture slots for surface caching / external textures. Cap is 4 in
// the current bind group layout — bind each slot via
// `Renderer.bindUserTexture(slot, ...)` then issue draws with
// `Primitives.drawTextureQuad(cb, slot, ...)`. Vertex tx encodes
// `USER_TEX_FLAG | slot` so the shader knows to sample here. Unused
// slots stay bound to a 1×1 placeholder so the bind group stays valid.
@group(0) @binding(8) var user_tex_0: texture_2d<f32>;
@group(0) @binding(9) var user_tex_1: texture_2d<f32>;
@group(0) @binding(10) var user_tex_2: texture_2d<f32>;
@group(0) @binding(11) var user_tex_3: texture_2d<f32>;

struct VertexIn {
  @location(0) pos:      vec3<f32>,
  @location(1) is_world: f32,
  @location(2) color:    vec4<f32>,
  @location(3) uv:       vec2<f32>,
  @location(4) tx:       u32,
};

struct VertexOut {
  @builtin(position) pos:   vec4<f32>,
  @location(0)       color: vec4<f32>,
  @location(1)       uv:    vec2<f32>,
  @location(2) @interpolate(flat) tx: u32,
};

@vertex
fn vs_transform(arg: VertexIn) -> VertexOut {
  var out: VertexOut;
  let p = vec4<f32>(arg.pos, 1.0);
  // is_world > 0  → world-space vertex, run through projection.
  // is_world == 0 → screen-space vertex, premapped to NDC by screen_ndc.
  let world = arg.is_world > 0.0;
  out.pos = select(cbuf.screen_ndc * p, cbuf.world_view_proj * p, world);
  out.color = arg.color * cbuf.tint;
  out.uv = arg.uv;
  out.tx = arg.tx;
  return out;
}

// Texture id flags. Documented in include/gfx/spritesheet.hpp +
// include/gfx/font/glyph.hpp.
const MSPRITE_FLAG:    u32 = 0x100000u;  // sprite atlas alpha mask × color
const FONT_COLOR_FLAG: u32 = 0x080000u;  // font color emoji (BGRA, no tint)
const FONT_MASK_FLAG:  u32 = 0x040000u;  // font mask glyph (R8 alpha × color)
// User texture slot. Lower 2 bits of tx select user_tex_0..3. Vertex
// color is multiplied in (tint), like generic atlas sampling. Surface
// caching uses this slot to draw a quad sampling a previously-rendered
// offscreen texture.
const USER_TEX_FLAG:   u32 = 0x200000u;
const USER_TEX_SLOT_MASK: u32 = 0x3u;
const FRAMEBUFFER_TEXTURE_ID: u32 = 0x7ffffffeu;
const PAINT_LINEAR_TEXTURE_ID: u32 = 0x7ffffff0u;
const PAINT_RADIAL_TEXTURE_ID: u32 = 0x7ffffff1u;
const PAINT_CONIC_TEXTURE_ID: u32 = 0x7ffffff2u;


fn framebuffer_uv(frag_pos: vec4<f32>, offset: vec2<f32>) -> vec2<f32> {
  let dims = vec2<f32>(textureDimensions(framebuffer_texture));
  return clamp(frag_pos.xy / dims + offset, vec2<f32>(0.0), vec2<f32>(1.0));
}

fn sample_framebuffer(frag_pos: vec4<f32>, offset: vec2<f32>) -> vec4<f32> {
  return textureSampleLevel(framebuffer_texture, framebuffer_sampler, framebuffer_uv(frag_pos, offset), 0.0);
}
fn shade(arg: VertexOut) -> vec4<f32> {
  if (arg.tx == 0u) {
    return arg.color;
  }
  if (arg.tx == PAINT_LINEAR_TEXTURE_ID || arg.tx == PAINT_RADIAL_TEXTURE_ID || arg.tx == PAINT_CONIC_TEXTURE_ID) {
    // Gradient stops are CPU-evaluated into vertex colors for the fixed 32-byte
    // dynamic vertex layout; this branch prevents sentinel texture ids from
    // being interpreted as atlas pages while preserving a paint-kind tag in WGSL.
    return arg.color;
  }
  if ((arg.tx & FONT_MASK_FLAG) != 0u) {
    let s = textureSampleLevel(font_mask_tex, mask_sampler, arg.uv, 0.0);
    return vec4<f32>(arg.color.rgb, arg.color.a * s.r);
  }
  if ((arg.tx & FONT_COLOR_FLAG) != 0u) {
    let s = textureSampleLevel(font_color_tex, atlas_sampler, arg.uv, 0.0);
    return vec4<f32>(s.rgb, s.a * arg.color.a);
  }
  if ((arg.tx & USER_TEX_FLAG) != 0u) {
    let slot = arg.tx & USER_TEX_SLOT_MASK;
    var s: vec4<f32>;
    // WGSL has no array of texture_2d uniforms, so each slot is a
    // separate binding and we dispatch via switch. Cap is 4 slots; if you
    // need more, add bindings + switch arms here, in the bind group
    // layout, and in the bind group entries.
    switch (slot) {
      case 0u:      { s = textureSampleLevel(user_tex_0, atlas_sampler, arg.uv, 0.0); }
      case 1u:      { s = textureSampleLevel(user_tex_1, atlas_sampler, arg.uv, 0.0); }
      case 2u:      { s = textureSampleLevel(user_tex_2, atlas_sampler, arg.uv, 0.0); }
      default:      { s = textureSampleLevel(user_tex_3, atlas_sampler, arg.uv, 0.0); }
    }
    return arg.color * s;
  }
  if (arg.tx == FRAMEBUFFER_TEXTURE_ID) {
    return arg.color * sample_framebuffer(arg.pos, arg.uv);
  }
  let s = textureSampleLevel(atlas_tex, atlas_sampler, arg.uv, 0.0);
  if ((arg.tx & MSPRITE_FLAG) != 0u) {
    return vec4<f32>(arg.color.rgb, arg.color.a * s.a);
  }
  return arg.color * s;
}

fn shade_text_mask(arg: VertexOut) -> vec4<f32> {
  let s = textureSampleLevel(font_mask_tex, mask_sampler, arg.uv, 0.0);
  return vec4<f32>(arg.color.rgb, arg.color.a * s.r);
}

fn shade_text_color(arg: VertexOut) -> vec4<f32> {
  let s = textureSampleLevel(font_color_tex, atlas_sampler, arg.uv, 0.0);
  return vec4<f32>(s.rgb, s.a * arg.color.a);
}

fn shade_framebuffer_sample(arg: VertexOut) -> vec4<f32> {
  return arg.color * sample_framebuffer(arg.pos, arg.uv);
}


@fragment
fn ps_opaque(arg: VertexOut) -> @location(0) vec4<f32> {
  return shade(arg);
}

@fragment
fn ps_transparent(arg: VertexOut) -> @location(0) vec4<f32> {
  return shade(arg);
}

@fragment
fn ps_text_mask(arg: VertexOut) -> @location(0) vec4<f32> {
  return shade_text_mask(arg);
}

@fragment
fn ps_text_color(arg: VertexOut) -> @location(0) vec4<f32> {
  return shade_text_color(arg);
}

@fragment
fn ps_framebuffer_sample(arg: VertexOut) -> @location(0) vec4<f32> {
  return shade_framebuffer_sample(arg);
}


struct FullscreenOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) uv: vec2<f32>,
};

@vertex
fn vs_transform_uv(arg: VertexIn) -> FullscreenOut {
  var out: FullscreenOut;
  let p = vec4<f32>(arg.pos, 1.0);
  let world = arg.is_world > 0.0;
  out.pos = select(cbuf.screen_ndc * p, cbuf.world_view_proj * p, world);
  out.uv = arg.uv;
  return out;
}

@vertex
fn vs_fullscreen(@builtin(vertex_index) vertex_index: u32) -> FullscreenOut {
  var positions = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -1.0),
    vec2<f32>( 3.0, -1.0),
    vec2<f32>(-1.0,  3.0)
  );
  var uvs = array<vec2<f32>, 3>(
    vec2<f32>(0.0, 0.0),
    vec2<f32>(0.0, 0.0),
    vec2<f32>(0.0, 0.0)
  );
  var out: FullscreenOut;
  out.pos = vec4<f32>(positions[vertex_index], 0.0, 1.0);
  out.uv = uvs[vertex_index];
  return out;
}

@fragment
fn ps_sample(arg: FullscreenOut) -> @location(0) vec4<f32> {
  return sample_framebuffer(arg.pos, arg.uv);
}

@fragment
fn ps_lum_filter(arg: FullscreenOut) -> @location(0) vec4<f32> {
  let s = sample_framebuffer(arg.pos, arg.uv);
  let luma = dot(s.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
  let scale = max(luma - 1.0, 0.0) / max(luma, 0.0001);
  return vec4<f32>(s.rgb * scale, s.a);
}

fn blur9(pos: vec4<f32>, offset: vec2<f32>, direction: vec2<f32>) -> vec4<f32> {
  let dims = vec2<f32>(textureDimensions(framebuffer_texture));
  let texel = direction / dims;
  let uv = framebuffer_uv(pos, offset);
  var sum = textureSampleLevel(framebuffer_texture, framebuffer_sampler, uv, 0.0) * 0.2270270270;
  sum += textureSampleLevel(framebuffer_texture, framebuffer_sampler, uv + texel * 1.3846153846, 0.0) * 0.3162162162;
  sum += textureSampleLevel(framebuffer_texture, framebuffer_sampler, uv - texel * 1.3846153846, 0.0) * 0.3162162162;
  sum += textureSampleLevel(framebuffer_texture, framebuffer_sampler, uv + texel * 3.2307692308, 0.0) * 0.0702702703;
  sum += textureSampleLevel(framebuffer_texture, framebuffer_sampler, uv - texel * 3.2307692308, 0.0) * 0.0702702703;
  return sum;
}

@fragment
fn ps_vblur(arg: FullscreenOut) -> @location(0) vec4<f32> {
  return blur9(arg.pos, arg.uv, vec2<f32>(0.0, 1.0));
}

@fragment
fn ps_hblur(arg: FullscreenOut) -> @location(0) vec4<f32> {
  return blur9(arg.pos, arg.uv, vec2<f32>(1.0, 0.0));
}
