#pragma once

#include "weak_holder.hpp"

#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <memory>
#include <v8.h>

namespace fxe::js {
  // Type tag stored in internal field 1 of an ImageHandle JS object.
  inline constexpr u32 TAG_IMAGE = 0x494D4147u; // 'IMAG'

  // Holder backing a JS ImageHandle. The pixels live behind a shared_ptr so
  // multiple JS handles (and the bytes() Uint8Array view) can share storage
  // without double-free hazards. dispose() resets `tex` to release the JS-side
  // reference; remaining shared_ptr holders keep the pixels alive.
  struct image_holder : weak_holder<image_holder> {
    std::shared_ptr<fxe::texture_data> tex;
    fxe::texture_id texture = fxe::null_texture;

    void on_finalize(v8::Isolate*);
  };

  // Install the `Image` global namespace + ImageHandle constructor template
  // on `global`. Idempotent per isolate.
  void install_image_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);

  // Recover an image_holder* from a JS ImageHandle. Returns nullptr if the
  // value is not an ImageHandle or has been disposed.
  image_holder* unwrap_image(v8::Local<v8::Value>);
  [[nodiscard]] texture_id ensure_image_texture_id(image_holder* h);
} // namespace fxe::js
