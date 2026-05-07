#pragma once

#include <v8.h>

namespace fxe::js {

  // Audio binding tag. 'AUDS' = AUDio Sound.
  inline constexpr unsigned int TAG_AUDIO_SOUND = 0x41554453u;

  // Installs the global `Audio` namespace and the `Sound` constructor on the
  // supplied isolate-global template. Idempotent per isolate.
  //
  // NOTE: Must be invoked from v8_host.cpp's bootstrap alongside the other
  // install_*_template calls. Also requires linking fxe_audio (see notes in
  // the task summary for the cmake snippet).
  void install_audio_bindings(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);

} // namespace fxe::js
