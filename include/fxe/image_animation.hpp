#pragma once

#include <fxe/spritesheet.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace fxe {
  enum class animated_image_format {
    gif,
    apng,
    lottie,
  };

  struct animated_frame {
    u32 delay_ms = 0;
    texture_data image;
  };

  struct animated_image {
    animated_image_format format = animated_image_format::gif;
    std::vector<animated_frame> frames;
    u32 duration_ms = 0;

    [[nodiscard]] usize frame_index_at(double time_ms) const noexcept;
  };

  [[nodiscard]] animated_image load_animated_image(std::span<const u8> encoded,
                                                   std::string_view source_name = {});
  [[nodiscard]] animated_image load_lottie_placeholder(std::span<const u8> json,
                                                       std::string_view source_name = {});
} // namespace fxe
