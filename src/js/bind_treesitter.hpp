#pragma once

#ifdef FXE_HAS_TREESITTER

#include <v8.h>

namespace fxe::js {
  void install_treesitter_namespace(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
}

#endif
