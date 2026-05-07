#pragma once
// Wires V8's WebAssembly streaming hook so `WebAssembly.compileStreaming`
// and `WebAssembly.instantiateStreaming` work per the WebAssembly Web API
// spec. V8 already exposes the synchronous WebAssembly surface (compile,
// instantiate, validate, Module, Instance, Memory, Table, Global,
// CompileError, LinkError, RuntimeError); only the streaming entry points
// require an embedder callback.
#include <v8.h>

namespace fxe::js {
  // Installs the per-isolate WasmStreamingCallback. Call once per isolate,
  // after creation. Idempotent.
  void install_wasm_streaming(v8::Isolate*);
} // namespace fxe::js
