#pragma once
#include <v8.h>

namespace fxe::js {
  // Per-isolate template setup. Call from the bootstrap path alongside other install_*.
  void install_ipc_bindings(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);
  // Build the synthetic `fxe:ipc` module. Call from per-context init.
  v8::MaybeLocal<v8::Module> build_ipc_module(v8::Isolate* iso, v8::Local<v8::Context> ctx);
} // namespace fxe::js
