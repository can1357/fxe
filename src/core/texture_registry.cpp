#include <fxe/texture_registry.hpp>

#include <mutex>
#include <unordered_map>

namespace fxe {
  namespace {
    struct registry_state {
      std::mutex mutex;
      std::unordered_map<texture_id, std::weak_ptr<texture_data>> textures;
      u32 next_handle = 1;
    };

    registry_state& registry() {
      static registry_state state;
      return state;
    }

    [[nodiscard]] texture_id allocate_id(registry_state& state) {
      for (u32 attempt = 0; attempt < external_texture_handle_mask; ++attempt) {
        const u32 handle = state.next_handle;
        state.next_handle = (state.next_handle % external_texture_handle_mask) + 1u;
        const texture_id id = external_texture_flag | handle;
        auto it = state.textures.find(id);
        if (it == state.textures.end() || it->second.expired())
          return id;
      }
      return null_texture;
    }
  } // namespace

  texture_id register_external_texture(const std::shared_ptr<texture_data>& tex) {
    if (!tex)
      return null_texture;
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    const texture_id id = allocate_id(state);
    if (id == null_texture)
      return null_texture;
    state.textures[id] = tex;
    return id;
  }

  void refresh_external_texture(texture_id id, const std::shared_ptr<texture_data>& tex) {
    if ((id & external_texture_flag) == 0 || !tex)
      return;
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.textures[id] = tex;
  }

  std::shared_ptr<texture_data> find_external_texture(texture_id id) {
    if ((id & external_texture_flag) == 0)
      return {};
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.textures.find(id);
    if (it == state.textures.end())
      return {};
    auto tex = it->second.lock();
    if (!tex)
      state.textures.erase(it);
    return tex;
  }

  void release_external_texture_if_unused(texture_id id) {
    if ((id & external_texture_flag) == 0)
      return;
    auto& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto it = state.textures.find(id);
    if (it != state.textures.end() && it->second.expired())
      state.textures.erase(it);
  }
} // namespace fxe
