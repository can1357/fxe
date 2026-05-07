#pragma once
// JS bindings for the global URL and URLSearchParams classes.
#include <v8.h>

namespace fxe::js {
  void install_url_globals(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
