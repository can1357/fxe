#pragma once
#include "../os/os.hpp"
#include <v8.h>
#include <vector>
namespace fxe::js {
  void install_menu_global(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
  // Shared by bind_tray.cpp.
  void parse_menu_items(v8::Isolate*, v8::Local<v8::Context>, v8::Local<v8::Value> arr,
                        std::vector<fxe::os::menu_item>& out);
} // namespace fxe::js
