#pragma once

#include "weak_holder.hpp"

#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <memory>
#include <v8.h>
#include <vector>

namespace fxe::js {
  inline constexpr u32 TAG_IMAGE = 0x494D4147u;          // 'IMAG'
  inline constexpr u32 TAG_ANIMATED_IMAGE = 0x414e494du; // 'ANIM'

  struct image_holder : weak_holder<image_holder> {
    std::shared_ptr<fxe::texture_data> tex;
    fxe::texture_id texture = fxe::null_texture;

    void on_finalize(v8::Isolate*);
  };

  struct animated_image_holder : weak_holder<animated_image_holder> {
    std::vector<std::shared_ptr<fxe::texture_data>> frames;
    std::vector<fxe::texture_id> textures;
    std::vector<u32> delays_ms;
    u32 duration_ms = 0;

    void on_finalize(v8::Isolate*);
  };

  void install_image_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);

  image_holder* unwrap_image(v8::Local<v8::Value>);
  animated_image_holder* unwrap_animated_image(v8::Local<v8::Value>);
  [[nodiscard]] texture_id ensure_image_texture_id(image_holder* h);
} // namespace fxe::js
