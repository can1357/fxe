#pragma once

// Match brew V8 ABI: pointer compression + sandbox are enabled in libv8.dylib.
#define V8_COMPRESS_POINTERS 1

#include <v8.h>

namespace fxe::runtime {
  void install_fs_fd_native(v8::Isolate* iso, v8::Local<v8::Context> ctx,
                            v8::Local<v8::Object> native);
}
