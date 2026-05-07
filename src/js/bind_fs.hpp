#pragma once
#include <v8.h>

namespace fxe::js {
  // Installs the `fs` global on `global`. Provides a Node.js-compatible subset:
  // sync variants (readFileSync/writeFileSync/...) and Promise-returning
  // async variants backed by libuv's worker pool.
  // Reads with no encoding return Uint8Array (raw bytes), not Node Buffer —
  // we have no Buffer in this runtime.
  void install_fs_global(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
