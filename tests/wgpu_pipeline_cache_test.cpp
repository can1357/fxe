#include "../src/wgpu/pipeline.hpp"

#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  template <typename Future> void wait_future(wgpu::Instance& instance, Future fut) {
    wgpu::FutureWaitInfo info{fut};
    auto status = instance.WaitAny(1, &info, UINT64_MAX);
    if (status != wgpu::WaitStatus::Success)
      throw std::runtime_error("wgpu::Instance::WaitAny failed");
  }

  wgpu::Adapter request_null_adapter(wgpu::Instance& instance) {
    wgpu::Adapter adapter;
    wgpu::RequestAdapterOptions opts{};
    opts.backendType = wgpu::BackendType::Null;
    auto fut = instance.RequestAdapter(
        &opts, wgpu::CallbackMode::WaitAnyOnly,
        [&adapter](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView msg) {
          if (status == wgpu::RequestAdapterStatus::Success) {
            adapter = std::move(a);
          } else {
            std::string m(msg.data, msg.length);
            std::fprintf(stderr, "RequestAdapter(null) failed: %s\n", m.c_str());
          }
        });
    wait_future(instance, fut);
    if (!adapter)
      throw std::runtime_error("RequestAdapter(null) returned null");
    return adapter;
  }

  wgpu::Device request_device(wgpu::Instance& instance, wgpu::Adapter& adapter) {
    wgpu::Device device;
    wgpu::DeviceDescriptor desc{};
    desc.label = "fxe-wgpu-pipeline-cache-test-device";
    auto fut = adapter.RequestDevice(
        &desc, wgpu::CallbackMode::WaitAnyOnly,
        [&device](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView msg) {
          if (status == wgpu::RequestDeviceStatus::Success) {
            device = std::move(d);
          } else {
            std::string m(msg.data, msg.length);
            std::fprintf(stderr, "RequestDevice failed: %s\n", m.c_str());
          }
        });
    wait_future(instance, fut);
    if (!device)
      throw std::runtime_error("RequestDevice returned null");
    return device;
  }

  int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

int main() {
  try {
    wgpu::InstanceDescriptor inst_desc{};
    static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    inst_desc.requiredFeatureCount = 1;
    inst_desc.requiredFeatures = &kTimedWaitAny;
    wgpu::Instance instance = wgpu::CreateInstance(&inst_desc);
    if (!instance)
      return fail("CreateInstance returned null");

    wgpu::Adapter adapter = request_null_adapter(instance);
    wgpu::Device device = request_device(instance, adapter);

    static constexpr const char* kShader = R"wgsl(
struct VertexIn {
  @location(0) pos: vec3<f32>,
  @location(1) is_world: f32,
  @location(2) color: vec4<f32>,
  @location(3) uv: vec2<f32>,
  @location(4) texture_id: u32,
};

struct VertexOut {
  @builtin(position) pos: vec4<f32>,
  @location(0) color: vec4<f32>,
};

@vertex
fn vs_transform(arg: VertexIn) -> VertexOut {
  var out: VertexOut;
  out.pos = vec4<f32>(arg.pos, 1.0);
  out.color = arg.color;
  return out;
}

@vertex
fn vs_transform_alt(arg: VertexIn) -> VertexOut {
  var out: VertexOut;
  out.pos = vec4<f32>(arg.pos.xy, arg.pos.z + 0.0, 1.0);
  out.color = arg.color;
  return out;
}

@fragment
fn ps_opaque(arg: VertexOut) -> @location(0) vec4<f32> {
  return arg.color;
}

@fragment
fn ps_alt(arg: VertexOut) -> @location(0) vec4<f32> {
  return vec4<f32>(arg.color.rgb, 1.0);
}
)wgsl";

    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    wgpu::ShaderModuleDescriptor shader_desc{};
    shader_desc.nextInChain = &wgsl;
    shader_desc.label = "fxe-wgpu-pipeline-cache-test.wgsl";
    wgpu::ShaderModule shader = device.CreateShaderModule(&shader_desc);
    if (!shader)
      return fail("CreateShaderModule returned null");

    wgpu::PipelineLayoutDescriptor layout_desc{};
    layout_desc.label = "fxe-wgpu-pipeline-cache-test-layout";
    wgpu::PipelineLayout layout = device.CreatePipelineLayout(&layout_desc);
    if (!layout)
      return fail("CreatePipelineLayout returned null");

    fxe::pipeline_cache cache;
    fxe::pipeline_key base{};
    base.vs_entry = "vs_transform";
    base.fs_entry = "ps_opaque";
    base.color_format = wgpu::TextureFormat::BGRA8Unorm;
    base.depth_format = wgpu::TextureFormat::Undefined;
    base.blend = fxe::blend_mode::alpha;
    base.topology = wgpu::PrimitiveTopology::TriangleList;
    base.sample_count = 1;

    wgpu::RenderPipeline first = cache.acquire(base, device, layout, shader);
    wgpu::RenderPipeline second = cache.acquire(base, device, layout, shader);
    if (!first || !second)
      return fail("pipeline_cache::acquire returned null");
    if (first.Get() != second.Get())
      return fail("pipeline_cache returned different pipeline handles for identical keys");
    if (cache.miss_count() != 1)
      return fail("identical key should cause exactly one cache miss");

    std::vector<fxe::pipeline_key> variants;
    auto vary = [&](auto mutate) {
      fxe::pipeline_key key = base;
      mutate(key);
      variants.push_back(std::move(key));
    };
    vary([](fxe::pipeline_key& key) { key.vs_entry = "vs_transform_alt"; });
    vary([](fxe::pipeline_key& key) { key.fs_entry = "ps_alt"; });
    vary([](fxe::pipeline_key& key) { key.color_format = wgpu::TextureFormat::RGBA8Unorm; });
    vary([](fxe::pipeline_key& key) { key.depth_format = wgpu::TextureFormat::Depth24Plus; });
    vary([](fxe::pipeline_key& key) { key.blend = fxe::blend_mode::none; });
    vary([](fxe::pipeline_key& key) { key.topology = wgpu::PrimitiveTopology::LineList; });
    vary([](fxe::pipeline_key& key) { key.sample_count = 4; });

    size_t expected_misses = 1;
    for (const auto& key : variants) {
      wgpu::RenderPipeline varied = cache.acquire(key, device, layout, shader);
      if (!varied)
        return fail("pipeline_cache returned null for varied key");
      if (varied.Get() == first.Get())
        return fail("varying one pipeline_key field unexpectedly reused the base handle");
      ++expected_misses;
      if (cache.miss_count() != expected_misses)
        return fail("varying one pipeline_key field did not cause exactly one cache miss");
      wgpu::RenderPipeline varied_again = cache.acquire(key, device, layout, shader);
      if (varied.Get() != varied_again.Get())
        return fail("varied key was not stable on repeated acquire");
      if (cache.miss_count() != expected_misses)
        return fail("repeated varied key acquired a new pipeline");
    }

    return 0;
  } catch (const std::exception& err) {
    std::fprintf(stderr, "unexpected exception: %s\n", err.what());
    return 1;
  }
}
