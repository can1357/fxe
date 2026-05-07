#pragma once

#include <v8.h>

namespace fxe::js {
  void install_crash_reporter_to(v8::Isolate*, v8::Local<v8::Context>,
                                 v8::Local<v8::Object> appObj);
} // namespace fxe::js
