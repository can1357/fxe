#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <v8.h>

namespace fxe::js {
  void install_blob_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);

  v8::Local<v8::Object> make_blob_object(v8::Isolate*, v8::Local<v8::Context>,
                                         std::shared_ptr<std::vector<std::uint8_t>> bytes,
                                         std::string type = {});
} // namespace fxe::js
