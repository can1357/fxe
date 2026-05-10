#include "../src/js/js_command_buffer.hpp"

#include <fxe/command_buffer.hpp>
#include <fxe/math.hpp>
#include <fxe/primitives.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_host.hpp>
#include <v8.h>

#include <cstdio>
#include <memory>

#ifndef FXE_V8_ICUDTL_PATH
#define FXE_V8_ICUDTL_PATH ""
#endif

namespace {
  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
    } else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  fxe::math::mat4x4 translation(float x, float y, float z) {
    auto m = fxe::math::identity();
    m[3] = fxe::math::vec4{x, y, z, 1.0f};
    return m;
  }

  void test_allocate_grow_and_clear(v8::Isolate* isolate) {
    fxe::js::js_command_buffer cb(isolate);
    CHECK(cb.epoch_value() == 1);
    CHECK(cb.is_empty());

    auto [verts, idx] = cb.allocate(3, 3, fxe::vertex_topology::triangle);
    CHECK(verts != nullptr);
    CHECK(idx != nullptr);
    CHECK(cb.epoch_value() == 2);
    CHECK(cb.vertex_count() == 3);
    CHECK(cb.index_count(fxe::vertex_topology::triangle) == 3);
    CHECK(cb.index_count(fxe::vertex_topology::line) == 0);
    CHECK(idx[0] == 0 && idx[1] == 0 && idx[2] == 0);
    verts[0] = fxe::make_vertex({1, 2}, 3, {}, fxe::null_texture, fxe::white);
    verts[1] = fxe::make_vertex({4, 5}, 6, {}, fxe::null_texture, fxe::red);
    verts[2] = fxe::make_vertex({7, 8}, 9, {}, fxe::null_texture, fxe::green);
    fxe::fill_indices_list(idx, 3, fxe::vertex_topology::triangle);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2);

    auto old_store = cb.vertex_store();
    void* old_data = old_store->Data();
    const u32 old_cap = cb.vertex_capacity();
    auto [more, no_indices] =
        cb.allocate(static_cast<usize>(old_cap) + 1, 0, fxe::vertex_topology::triangle);
    CHECK(more != nullptr);
    CHECK(no_indices == nullptr);
    CHECK(cb.vertex_store().get() != old_store.get());
    CHECK(old_store->Data() == old_data);
    CHECK(cb.vertices()[0].pos.x == 1.0f);
    CHECK(cb.vertex_count() == old_cap + 4);

    cb.clear();
    CHECK(cb.vertex_count() == 0);
    CHECK(cb.index_count(fxe::vertex_topology::triangle) == 0);
    CHECK(cb.is_empty());
  }

  void test_queue_clone_and_transform(v8::Isolate* isolate) {
    fxe::command_buffer src;
    fxe::primitives::fill_rect(src, {0, 0}, {10, 10}, 0.0f, fxe::white);

    fxe::js::js_command_buffer dst(isolate);
    auto before_queue = dst.epoch_value();
    dst.queue(src, translation(5, 6, 7));
    CHECK(dst.epoch_value() == before_queue + 1);
    CHECK(dst.vertex_count() == src.vertex_buffer.size());
    CHECK(dst.index_count(fxe::vertex_topology::triangle) == src.index_buffers[0].size());
    CHECK(dst.vertices()[0].pos.x == src.vertex_buffer[0].pos.x + 5.0f);
    CHECK(dst.vertices()[0].pos.y == src.vertex_buffer[0].pos.y + 6.0f);
    CHECK(dst.vertices()[0].pos.z == src.vertex_buffer[0].pos.z + 7.0f);

    auto cloned = dst.clone();
    CHECK(cloned.epoch_value() == dst.epoch_value());
    CHECK(cloned.vertex_count() == dst.vertex_count());
    CHECK(cloned.index_count(fxe::vertex_topology::triangle) ==
          dst.index_count(fxe::vertex_topology::triangle));
    CHECK(cloned.vertices()[0].pos.x == dst.vertices()[0].pos.x);

    const auto before_transform = cloned.epoch_value();
    cloned.transform(translation(1, 2, 3));
    CHECK(cloned.epoch_value() == before_transform + 1);
    CHECK(cloned.vertices()[0].pos.x == dst.vertices()[0].pos.x + 1.0f);
    CHECK(dst.vertices()[0].pos.x != cloned.vertices()[0].pos.x);
  }
} // namespace

int main(int argc, char** argv) {
  fxe::js::initialize(argc > 0 ? argv[0] : "", FXE_V8_ICUDTL_PATH);
  auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  auto* isolate = v8::Isolate::New(params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    test_allocate_grow_and_clear(isolate);
    test_queue_clone_and_transform(isolate);
  }
  isolate->Dispose();
  fxe::js::shutdown();

  if (g_fail != 0) {
    std::fprintf(stderr, "js_command_buffer tests: %d passed, %d failed\n", g_pass, g_fail);
    return 1;
  }
  std::printf("js_command_buffer tests: %d passed, 0 failed\n", g_pass);
  return 0;
}
