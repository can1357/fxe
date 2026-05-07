#pragma once

#include <v8.h>

namespace fxe::runtime {
  void install_native_http2(v8::Isolate* iso, v8::Local<v8::Context> ctx);
} // namespace fxe::runtime
