#include "v8_code_cache.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <v8-script.h>
#include <v8-version-string.h>

namespace fxe::js::v8_code_cache {
  namespace {
    // Persistent file format (little-endian, host order assumed; the cache is
    // not portable across architectures, which is fine - V8 bytecode isn't
    // either):
    //   magic   4 bytes  "FX8C"
    //   version uint32   k_format_version
    //   src_h   uint64   FNV-1a 64 of `source`
    //   src_n   uint64   length of `source`
    //   id_n    uint32   length of `cache_id`
    //   id      bytes
    //   pay_n   uint32
    //   payload bytes    raw v8::ScriptCompiler::CachedData
    constexpr uint32_t k_format_version = 2;
    constexpr char k_magic[4] = {'F', 'X', '8', 'C'};

    struct dir_state {
      std::mutex mutex;
      bool resolved = false;
      std::filesystem::path dir; // empty => disabled
    };

    dir_state& state() {
      static dir_state s;
      return s;
    }

    bool ieq(std::string_view a, std::string_view b) {
      if (a.size() != b.size())
        return false;
      for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
          return false;
      return true;
    }

    bool is_disabled_token(std::string_view s) {
      return ieq(s, "0") || ieq(s, "off") || ieq(s, "no") || ieq(s, "none") || ieq(s, "false") ||
             ieq(s, "disable") || ieq(s, "disabled");
    }

    std::filesystem::path platform_default_dir() {
#if defined(__APPLE__)
      if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / "Library" / "Caches" / "fxe" / "v8";
#elif defined(_WIN32)
      if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
        return std::filesystem::path(la) / "fxe" / "v8";
      if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return std::filesystem::path(up) / "AppData" / "Local" / "fxe" / "v8";
#else
      if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "fxe" / "v8";
      if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache" / "fxe" / "v8";
#endif
      std::error_code ec;
      auto tmp = std::filesystem::temp_directory_path(ec);
      if (!ec)
        return tmp / "fxe-v8-cache";
      return {};
    }

    std::filesystem::path resolve_dir_locked(dir_state& s) {
      if (s.resolved)
        return s.dir;
      s.resolved = true;

      std::filesystem::path base;
      if (const char* env = std::getenv("FXE_V8_CACHE_DIR")) {
        std::string_view v(env);
        if (v.empty() || is_disabled_token(v)) {
          s.dir.clear();
          return s.dir;
        }
        base = std::filesystem::path(env);
      } else {
        base = platform_default_dir();
      }
      if (base.empty()) {
        s.dir.clear();
        return s.dir;
      }

      // Version-scope by V8 version so a v8 upgrade doesn't try to consume
      // incompatible bytecode.
      base /= std::string("v8-") + V8_VERSION_STRING;
      std::error_code ec;
      std::filesystem::create_directories(base, ec);
      if (ec) {
        s.dir.clear();
        return s.dir;
      }
      s.dir = std::move(base);
      return s.dir;
    }

    uint64_t fnv1a(std::string_view bytes) {
      uint64_t h = 0xcbf29ce484222325ULL;
      for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
      }
      return h;
    }

    std::string hex64(uint64_t v) {
      char buf[17];
      std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
      return std::string(buf, 16);
    }

    std::filesystem::path file_for(std::string_view subdir, std::string_view cache_id) {
      auto& s = state();
      std::lock_guard<std::mutex> lk(s.mutex);
      auto root = resolve_dir_locked(s);
      if (root.empty())
        return {};
      auto dir = root / std::string(subdir);
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec)
        return {};
      return dir / (hex64(fnv1a(cache_id)) + ".v8c");
    }

    // Read header + payload, verifying everything matches `cache_id` + `source`.
    bool load_blob(const std::filesystem::path& path, std::string_view cache_id,
                   std::string_view source, std::vector<uint8_t>& out) {
      std::ifstream f(path, std::ios::binary);
      if (!f)
        return false;

      auto rd = [&](void* dst, size_t n) -> bool {
        return static_cast<bool>(f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n)));
      };

      char magic[4];
      uint32_t version = 0;
      uint64_t src_hash = 0;
      uint64_t src_len = 0;
      uint32_t id_len = 0;
      uint32_t pay_len = 0;
      if (!rd(magic, 4) || std::memcmp(magic, k_magic, 4) != 0)
        return false;
      if (!rd(&version, 4) || version != k_format_version)
        return false;
      if (!rd(&src_hash, 8) || !rd(&src_len, 8))
        return false;
      if (src_len != source.size() || src_hash != fnv1a(source))
        return false;
      if (!rd(&id_len, 4) || id_len > (16u * 1024u))
        return false;
      std::string id(id_len, '\0');
      if (id_len && !rd(id.data(), id_len))
        return false;
      if (id != cache_id)
        return false;
      if (!rd(&pay_len, 4) || pay_len == 0 || pay_len > (256u * 1024u * 1024u))
        return false;
      out.assign(pay_len, 0);
      if (!rd(out.data(), pay_len))
        return false;
      return true;
    }

    void store_blob(const std::filesystem::path& path, std::string_view cache_id,
                    std::string_view source, const uint8_t* payload, size_t pay_len) {
      if (path.empty() || !payload || pay_len == 0)
        return;
      auto tmp = path;
      tmp += ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
          return;
        auto wr = [&](const void* p, size_t n) {
          f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
        };
        uint32_t version = k_format_version;
        uint64_t src_hash = fnv1a(source);
        uint64_t src_len = source.size();
        uint32_t id_len = static_cast<uint32_t>(cache_id.size());
        uint32_t pl = static_cast<uint32_t>(pay_len);
        wr(k_magic, 4);
        wr(&version, 4);
        wr(&src_hash, 8);
        wr(&src_len, 8);
        wr(&id_len, 4);
        wr(cache_id.data(), cache_id.size());
        wr(&pl, 4);
        wr(payload, pay_len);
        f.flush();
        if (!f) {
          std::error_code ec;
          std::filesystem::remove(tmp, ec);
          return;
        }
      }
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
      if (ec)
        std::filesystem::remove(tmp, ec);
    }

    void remove_quiet(const std::filesystem::path& path) {
      if (path.empty())
        return;
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }

    v8::Local<v8::String> make_string(v8::Isolate* iso, std::string_view source) {
      return v8::String::NewFromUtf8(iso, source.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(source.size()))
          .ToLocalChecked();
    }
  } // namespace

  std::filesystem::path cache_dir() {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    return resolve_dir_locked(s);
  }

  void clear() {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mutex);
    auto dir = resolve_dir_locked(s);
    if (dir.empty())
      return;
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec)
        break;
      std::error_code rec;
      std::filesystem::remove_all(entry.path(), rec);
    }
  }

  v8::MaybeLocal<v8::Script> compile_script(v8::Local<v8::Context> ctx, std::string_view cache_id,
                                            std::string_view source, v8::ScriptOrigin& origin) {
    auto* iso = v8::Isolate::GetCurrent();
    auto src_str = make_string(iso, source);
    auto path = cache_id.empty() ? std::filesystem::path{} : file_for("scripts", cache_id);

    v8::Local<v8::Script> script;
    if (!path.empty()) {
      std::vector<uint8_t> bytes;
      if (load_blob(path, cache_id, source, bytes)) {
        // V8 takes ownership of CachedData via ScriptCompiler::Source.
        auto* cached =
            new v8::ScriptCompiler::CachedData(bytes.data(), static_cast<int>(bytes.size()),
                                               v8::ScriptCompiler::CachedData::BufferNotOwned);
        v8::ScriptCompiler::Source v8src(src_str, origin, cached);
        bool compiled =
            v8::ScriptCompiler::Compile(ctx, &v8src, v8::ScriptCompiler::kConsumeCodeCache)
                .ToLocal(&script);
        const auto* post = v8src.GetCachedData();
        if (compiled && (!post || !post->rejected))
          return script;
        // Cache rejected (or compile failed via cached path): drop the bad
        // file and fall through to fresh compile.
        remove_quiet(path);
      }
    }

    v8::ScriptCompiler::Source fresh(src_str, origin);
    if (!v8::ScriptCompiler::Compile(ctx, &fresh, v8::ScriptCompiler::kNoCompileOptions)
             .ToLocal(&script)) {
      return v8::MaybeLocal<v8::Script>{};
    }
    if (!path.empty()) {
      std::unique_ptr<v8::ScriptCompiler::CachedData> produced(
          v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript()));
      if (produced && produced->data && produced->length > 0) {
        store_blob(path, cache_id, source, produced->data, static_cast<size_t>(produced->length));
      }
    }
    return script;
  }

  v8::MaybeLocal<v8::Module> compile_module(v8::Isolate* iso, std::string_view cache_id,
                                            std::string_view source, v8::ScriptOrigin& origin) {
    auto src_str = make_string(iso, source);
    auto path = cache_id.empty() ? std::filesystem::path{} : file_for("modules", cache_id);

    v8::Local<v8::Module> mod;
    if (!path.empty()) {
      std::vector<uint8_t> bytes;
      if (load_blob(path, cache_id, source, bytes)) {
        auto* cached =
            new v8::ScriptCompiler::CachedData(bytes.data(), static_cast<int>(bytes.size()),
                                               v8::ScriptCompiler::CachedData::BufferNotOwned);
        v8::ScriptCompiler::Source v8src(src_str, origin, cached);
        bool compiled =
            v8::ScriptCompiler::CompileModule(iso, &v8src, v8::ScriptCompiler::kConsumeCodeCache)
                .ToLocal(&mod);
        const auto* post = v8src.GetCachedData();
        if (compiled && (!post || !post->rejected))
          return mod;
        remove_quiet(path);
      }
    }

    v8::ScriptCompiler::Source fresh(src_str, origin);
    if (!v8::ScriptCompiler::CompileModule(iso, &fresh, v8::ScriptCompiler::kNoCompileOptions)
             .ToLocal(&mod)) {
      return v8::MaybeLocal<v8::Module>{};
    }
    if (!path.empty()) {
      std::unique_ptr<v8::ScriptCompiler::CachedData> produced(
          v8::ScriptCompiler::CreateCodeCache(mod->GetUnboundModuleScript()));
      if (produced && produced->data && produced->length > 0) {
        store_blob(path, cache_id, source, produced->data, static_cast<size_t>(produced->length));
      }
    }
    return mod;
  }
} // namespace fxe::js::v8_code_cache
