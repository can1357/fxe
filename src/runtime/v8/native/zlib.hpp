#pragma once

#include <v8.h>

namespace fxe::runtime {
  void install_native_zlib(v8::Isolate* iso, v8::Local<v8::Context> ctx);
} // namespace fxe::runtime
