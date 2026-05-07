#pragma once

#include <v8.h>

namespace fxe::js {
  void install_storage_globals(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
