#pragma once
#include <v8.h>

namespace fxe::js {
  // Installs the `path` global with pure-string path utilities (join, resolve,
  // dirname, basename, extname, relative, normalize, isAbsolute) and the
  // `sep` / `delimiter` properties. POSIX separators on non-Windows.
  void install_path_global(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);
} // namespace fxe::js
