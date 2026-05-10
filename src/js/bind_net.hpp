#pragma once
#include <v8.h>

namespace fxe::js {
  v8::MaybeLocal<v8::Module> build_net_module(v8::Isolate* iso, v8::Local<v8::Context> ctx);
} // namespace fxe::js
