#include "js_command_buffer.hpp"
#include <fxe/v8_literals.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace fxe::js {
  namespace {
    constexpr u32 k_initial_vertex_capacity = 8;
    constexpr u32 k_initial_index_capacity = 12;

    template <typename T> T* data_as(const std::shared_ptr<v8::BackingStore>& store) noexcept {
      return store ? static_cast<T*>(store->Data()) : nullptr;
    }

    template <typename T>
    const T* data_as_const(const std::shared_ptr<v8::BackingStore>& store) noexcept {
      return store ? static_cast<const T*>(store->Data()) : nullptr;
    }
  } // namespace

  js_command_buffer::js_command_buffer(v8::Isolate* isolate)
      : isolate_(isolate), verts_store_(make_store(0)) {
    for (auto& store : idx_stores_)
      store = make_store(0);
  }

  void js_command_buffer::sync_js_fields() const {
    if (!isolate_ || !self_)
      return;
    v8::HandleScope hs(isolate_);
    auto ctx = isolate_->GetCurrentContext();
    if (ctx.IsEmpty())
      return;
    auto self = self_->Get(isolate_);
    if (self.IsEmpty())
      return;
    (void)self->Set(ctx, "__fxe_v_len"_v8(isolate_), v8::Integer::NewFromUnsigned(isolate_, vlen_));
    (void)self->Set(ctx, "__fxe_tri_len"_v8(isolate_),
                    v8::Integer::NewFromUnsigned(
                        isolate_, ilen_[static_cast<usize>(vertex_topology::triangle)]));
    (void)self->Set(
        ctx, "__fxe_line_len"_v8(isolate_),
        v8::Integer::NewFromUnsigned(isolate_, ilen_[static_cast<usize>(vertex_topology::line)]));
    (void)self->Set(ctx, "__fxe_epoch"_v8(isolate_),
                    v8::Integer::NewFromUnsigned(isolate_, epoch_));
  }

  std::shared_ptr<v8::BackingStore> js_command_buffer::make_store(usize bytes) const {
    return std::shared_ptr<v8::BackingStore>(v8::ArrayBuffer::NewBackingStore(isolate_, bytes));
  }

  u32 js_command_buffer::grown_capacity(u32 current, usize required) {
    if (required > std::numeric_limits<u32>::max())
      throw std::length_error("CommandBuffer allocation exceeds u32 capacity");
    usize next = current == 0 ? 1 : current;
    while (next < required) {
      const usize growth = std::max<usize>(next / 2, 1);
      next += growth;
    }
    if (next > std::numeric_limits<u32>::max())
      throw std::length_error("CommandBuffer allocation exceeds u32 capacity");
    return static_cast<u32>(next);
  }

  vertex* js_command_buffer::vertex_data() noexcept {
    return data_as<vertex>(verts_store_);
  }

  const vertex* js_command_buffer::vertex_data() const noexcept {
    return data_as_const<vertex>(verts_store_);
  }

  u32* js_command_buffer::index_data(vertex_topology topology) noexcept {
    return data_as<u32>(idx_stores_[static_cast<usize>(topology)]);
  }

  const u32* js_command_buffer::index_data(vertex_topology topology) const noexcept {
    return data_as_const<u32>(idx_stores_[static_cast<usize>(topology)]);
  }

  std::span<const vertex> js_command_buffer::vertices() const noexcept {
    if (vlen_ == 0)
      return {};
    return {vertex_data(), vlen_};
  }

  std::span<const u32> js_command_buffer::indices(vertex_topology topology) const noexcept {
    const auto top = static_cast<usize>(topology);
    if (ilen_[top] == 0)
      return {};
    return {index_data(topology), ilen_[top]};
  }

  void js_command_buffer::ensure_vertex_capacity(usize required) {
    if (required <= cap_v_)
      return;
    const u32 next_cap = std::max(grown_capacity(cap_v_, required), k_initial_vertex_capacity);
    auto next = make_store(static_cast<usize>(next_cap) * sizeof(vertex));
    if (vlen_ != 0)
      std::memcpy(next->Data(), verts_store_->Data(), static_cast<usize>(vlen_) * sizeof(vertex));
    verts_store_ = std::move(next);
    cap_v_ = next_cap;
  }

  void js_command_buffer::ensure_index_capacity(vertex_topology topology, usize required) {
    const auto top = static_cast<usize>(topology);
    if (required <= cap_i_[top])
      return;
    const u32 next_cap = std::max(grown_capacity(cap_i_[top], required), k_initial_index_capacity);
    auto next = make_store(static_cast<usize>(next_cap) * sizeof(u32));
    if (ilen_[top] != 0) {
      std::memcpy(next->Data(), idx_stores_[top]->Data(),
                  static_cast<usize>(ilen_[top]) * sizeof(u32));
    }
    idx_stores_[top] = std::move(next);
    cap_i_[top] = next_cap;
  }

  write_args js_command_buffer::allocate(usize vtx, usize idx, vertex_topology topology) {
    const auto top = static_cast<usize>(topology);
    const usize next_vlen = static_cast<usize>(vlen_) + vtx;
    const usize next_ilen = static_cast<usize>(ilen_[top]) + idx;
    if (next_vlen > std::numeric_limits<u32>::max() || next_ilen > std::numeric_limits<u32>::max())
      throw std::length_error("CommandBuffer allocation exceeds u32 capacity");

    const u32 vertex_base = vlen_;
    const u32 index_base = ilen_[top];
    ensure_vertex_capacity(next_vlen);
    ensure_index_capacity(topology, next_ilen);

    auto* out_vertices = vtx == 0 ? nullptr : vertex_data() + vertex_base;
    auto* out_indices = idx == 0 ? nullptr : index_data(topology) + index_base;
    if (idx != 0)
      std::fill_n(out_indices, idx, vertex_base);

    vlen_ = static_cast<u32>(next_vlen);
    ilen_[top] = static_cast<u32>(next_ilen);
    bump_epoch();
    sync_js_fields();
    return {out_vertices, out_indices};
  }

  void js_command_buffer::clear() {
    vlen_ = 0;
    ilen_.fill(0);
    bump_epoch();
    sync_js_fields();
  }

  void js_command_buffer::transform(const math::mat4x4& tf, const std::optional<math::vec4>& tint) {
    auto* verts = vertex_data();
    if (tint) {
      for (u32 i = 0; i != vlen_; ++i)
        verts[i] = verts[i].transform(tf, *tint);
    } else {
      for (u32 i = 0; i != vlen_; ++i)
        verts[i] = verts[i].transform(tf);
    }
    bump_epoch();
    sync_js_fields();
  }

  js_command_buffer js_command_buffer::clone() const {
    js_command_buffer out(isolate_);
    out.ensure_vertex_capacity(vlen_);
    if (vlen_ != 0)
      std::memcpy(out.verts_store_->Data(), verts_store_->Data(),
                  static_cast<usize>(vlen_) * sizeof(vertex));
    out.vlen_ = vlen_;

    for (usize i = 0; i != ilen_.size(); ++i) {
      const auto top = static_cast<vertex_topology>(i);
      out.ensure_index_capacity(top, ilen_[i]);
      if (ilen_[i] != 0) {
        std::memcpy(out.idx_stores_[i]->Data(), idx_stores_[i]->Data(),
                    static_cast<usize>(ilen_[i]) * sizeof(u32));
      }
      out.ilen_[i] = ilen_[i];
    }
    out.epoch_ = epoch_;
    return out;
  }
} // namespace fxe::js
