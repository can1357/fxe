#pragma once

#include <v8.h>

namespace fxe::runtime {
  void install_native_async_hooks(v8::Isolate* iso, v8::Local<v8::Context> ctx);
} // namespace fxe::runtime
