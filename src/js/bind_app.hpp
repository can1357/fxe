#pragma once
#include <v8.h>

namespace fxe::js {
  // Adds OS-extras (getName, getVersion, getPath, requestSingleInstanceLock,
  // setBadgeCount, whenReady, relaunch) to the existing `App` global produced
  // by install_window_template. Must run AFTER install_window_template, with
  // the live Context global object.
  void install_app_extras_to(v8::Isolate*, v8::Local<v8::Context>, v8::Local<v8::Object> appObj);
} // namespace fxe::js
