#pragma once

#include <fxe/command_buffer.hpp>

#include <array>
#include <memory>
#include <v8.h>

namespace fxe::js {
  class js_command_buffer final : public command_sink {
  public:
    explicit js_command_buffer(v8::Isolate* isolate);

    [[nodiscard]] std::span<const vertex> vertices() const noexcept override;
    [[nodiscard]] std::span<const u32> indices(vertex_topology topology) const noexcept override;
    [[nodiscard]] u32 epoch_value() const noexcept override {
      return epoch_;
    }

    [[nodiscard]] u32 vertex_count() const noexcept {
      return vlen_;
    }
    [[nodiscard]] u32 index_count(vertex_topology topology) const noexcept {
      return ilen_[static_cast<usize>(topology)];
    }
    [[nodiscard]] u32 vertex_capacity() const noexcept {
      return cap_v_;
    }
    [[nodiscard]] u32 index_capacity(vertex_topology topology) const noexcept {
      return cap_i_[static_cast<usize>(topology)];
    }

    [[nodiscard]] std::shared_ptr<v8::BackingStore> vertex_store() const noexcept {
      return verts_store_;
    }
    [[nodiscard]] std::shared_ptr<v8::BackingStore>
    index_store(vertex_topology topology) const noexcept {
      return idx_stores_[static_cast<usize>(topology)];
    }

    [[nodiscard]] vertex* vertex_data() noexcept;
    [[nodiscard]] const vertex* vertex_data() const noexcept;
    [[nodiscard]] u32* index_data(vertex_topology topology) noexcept;
    [[nodiscard]] const u32* index_data(vertex_topology topology) const noexcept;

    write_args allocate(usize vtx, usize idx, vertex_topology topology) override;
    void clear() override;
    void transform(const math::mat4x4& tf,
                   const std::optional<math::vec4>& tint = std::nullopt) override;

    void set_js_object(v8::Global<v8::Object>* self) noexcept {
      self_ = self;
    }

    [[nodiscard]] js_command_buffer clone() const;

  private:
    std::shared_ptr<v8::BackingStore> make_store(usize bytes) const;
    static u32 grown_capacity(u32 current, usize required);

    void ensure_vertex_capacity(usize required);
    void ensure_index_capacity(vertex_topology topology, usize required);
    void bump_epoch() noexcept {
      ++epoch_;
    }

    v8::Isolate* isolate_ = nullptr;
    void sync_js_fields() const;

    std::shared_ptr<v8::BackingStore> verts_store_;
    v8::Global<v8::Object>* self_ = nullptr;

    std::array<std::shared_ptr<v8::BackingStore>, static_cast<usize>(vertex_topology::max)>
        idx_stores_{};
    u32 vlen_ = 0;
    std::array<u32, static_cast<usize>(vertex_topology::max)> ilen_{};
    u32 cap_v_ = 0;
    std::array<u32, static_cast<usize>(vertex_topology::max)> cap_i_{};
    u32 epoch_ = 1;
  };
} // namespace fxe::js
