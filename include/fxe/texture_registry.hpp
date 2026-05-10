#pragma once

#include <fxe/spritesheet.hpp>

#include <memory>

namespace fxe {
  [[nodiscard]] texture_id register_external_texture(const std::shared_ptr<texture_data>& tex);
  void refresh_external_texture(texture_id id, const std::shared_ptr<texture_data>& tex);
  [[nodiscard]] std::shared_ptr<texture_data> find_external_texture(texture_id id);
  void release_external_texture_if_unused(texture_id id);
} // namespace fxe
