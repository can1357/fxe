#pragma once
#include <v8.h>

namespace fxe::js {
  void install_text_document_template(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
