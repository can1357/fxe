#pragma once

#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <v8.h>

namespace fxe::js {
  // Install the `Font` global namespace on `global`. Font ids are opaque u32
  // values; id 0 is the engine's default (built-in) font. See bind_font.cpp
  // for the engine-gap notes around custom-size / per-id atlas building.
  void install_font_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);

  // Resolve a fontId to a const fxe::font_info*. Currently always returns the
  // default font; future engine work will populate a real registry.
  const fxe::font_info* resolve_font_id(u32 font_id);
} // namespace fxe::js
