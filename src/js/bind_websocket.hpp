#pragma once
// JS bindings for the global WebSocket class.
#include <v8.h>

namespace fxe::js {
  void install_websocket_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js

namespace fxe::js::bind_websocket {
  // Drain incoming events on every WebSocket and dispatch to their JS event
  // handlers. Integration calls this once per app_run_loop iteration.
  void pump(v8::Isolate*);
} // namespace fxe::js::bind_websocket
