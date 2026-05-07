#pragma once
// JS bindings for fetch / Headers / Request / Response / AbortController.
#include <v8.h>

namespace fxe::js {
  void install_fetch_globals(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js

namespace fxe::js::bind_fetch {
  // Drains finished fetches and resolves their JS promises. The integration
  // calls this once per app_run_loop iteration on the V8 thread.
  void pump(v8::Isolate*);
} // namespace fxe::js::bind_fetch
