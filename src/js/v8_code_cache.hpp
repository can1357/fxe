#pragma once

#include <filesystem>
#include <string_view>

#include <v8.h>

namespace fxe::js {
  // Persistent V8 code cache. Wraps v8::ScriptCompiler::{Compile,CompileModule}
  // with a disk-backed cache of v8::ScriptCompiler::CachedData blobs.
  //
  // cache_id is a stable identifier for the source unit (typically the
  // resolved file path, or a magic string for embedded scripts). The wrapper
  // hashes `source` and stores it next to the cached blob; on lookup the file
  // is rejected when the source bytes differ. V8's own consume-cache path
  // additionally rejects mismatched bytecode (different V8 build, mangled
  // file). Either failure transparently falls back to a fresh compile and
  // rewrites the cache.
  //
  // Cache directory resolution (first match wins):
  //   1. $FXE_V8_CACHE_DIR (special tokens "0","off","no","none","false",
  //      "disable","disabled" -> cache disabled).
  //   2. macOS: $HOME/Library/Caches/fxe/v8
  //      Linux: $XDG_CACHE_HOME/fxe/v8 or $HOME/.cache/fxe/v8
  //      Windows: %LOCALAPPDATA%/fxe/v8 or %USERPROFILE%/AppData/Local/fxe/v8
  //   3. <tempdir>/fxe-v8-cache
  // The resolved directory is suffixed with `/v8-<V8::GetVersion()>` so cache
  // entries never cross V8 builds.
  namespace v8_code_cache {
    v8::MaybeLocal<v8::Script> compile_script(v8::Local<v8::Context> ctx,
                                              std::string_view cache_id, std::string_view source,
                                              v8::ScriptOrigin& origin);

    v8::MaybeLocal<v8::Module> compile_module(v8::Isolate* iso, std::string_view cache_id,
                                              std::string_view source, v8::ScriptOrigin& origin);

    // Returns the resolved cache directory (creates it on first call). Empty
    // path means caching is disabled. Thread-safe; resolved once per process.
    std::filesystem::path cache_dir();

    // Best-effort wipe of all cache subdirectories under cache_dir().
    void clear();
  } // namespace v8_code_cache
} // namespace fxe::js
