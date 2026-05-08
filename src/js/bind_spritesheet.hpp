#pragma once

#include "weak_holder.hpp"

#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>

#include <memory>
#include <v8.h>
#include <vector>

namespace fxe::js {
  // Type tag stored in internal field 1 of a Spritesheet JS object.
  inline constexpr u32 TAG_SPRITESHEET = 0x53505348u; // 'SPSH'

  // Holder backing a JS Spritesheet. Owns one fxe::spritesheet plus shared
  // refs to the source images so disposing an Image after add() is safe.
  struct spritesheet_holder : weak_holder<spritesheet_holder> {
    fxe::spritesheet sheet;
    std::vector<std::shared_ptr<fxe::texture_data>> retained;
  };

  void install_spritesheet_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);

  spritesheet_holder* unwrap_spritesheet(v8::Local<v8::Value>);
} // namespace fxe::js
