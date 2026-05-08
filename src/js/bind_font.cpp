// JS bindings for the `Font` namespace.
//
// The engine uses one process-wide default font, but that font now owns a
// size-aware glyph registry and growable atlas. fontId remains a
// forward-compatible opaque handle (id 0 = default font), Font.load()
// replaces that default, and sizePx seeds the default variant used when
// callers omit an explicit text size.

#include "bind_font.hpp"

#include <fxe/font.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_strings.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr double kEngineMinFontSizePx = 1.0;

    void throw_type(Isolate* iso, const char* msg) {
      (void)throw_type_error(iso, msg);
    }

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    bool read_file_bytes(const std::string& path, std::vector<u8>& out) {
      std::ifstream f(path, std::ios::binary);
      if (!f)
        return false;
      f.seekg(0, std::ios::end);
      auto sz = f.tellg();
      if (sz < 0)
        return false;
      out.resize(static_cast<usize>(sz));
      f.seekg(0, std::ios::beg);
      f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
      return f.good() || f.eof();
    }

    void s_load(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsNumber())
        return throw_type(iso, "Font.load(path: string, sizePx: number)");
      auto ctx = iso->GetCurrentContext();
      const double size_px = info[1]->NumberValue(ctx).FromMaybe(0.0);
      if (!std::isfinite(size_px) || size_px < kEngineMinFontSizePx)
        return throw_type(iso, "Font.load: sizePx must be a finite number >= 1");
      std::vector<u8> bytes;
      if (!read_file_bytes(utf8(iso, info[0]), bytes))
        return throw_type(iso, "Font.load: failed to read TTF file");
      try {
        init_default_fonts(get_default_spritesheet(), std::span<const u8>(bytes),
                           static_cast<float>(size_px));
      } catch (const std::exception& e) {
        throw_type(iso, e.what());
        return;
      }
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, 0));
    }

    void s_builtin(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString() || utf8(iso, info[0]) != "default")
        return throw_type(iso, "Font.builtin(name): expected 'default'");
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, 0));
    }

    void s_dispose(const FunctionCallbackInfo<Value>& info) {
      // No-op until the per-id registry lands.
      (void)info;
    }

    void s_system(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString())
        return throw_type(iso, "Font.system(family: string, opts?)");
      auto ctx = iso->GetCurrentContext();
      const std::string family = utf8(iso, info[0]);
      double size_px = 16.0;
      fxe::font::Style want_style = fxe::font::Style::regular;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto o = info[1].As<Object>();
        Local<Value> field;
        if (o->Get(ctx, "sizePx"_v8(iso)).ToLocal(&field) && field->IsNumber()) {
          size_px = field->NumberValue(ctx).FromMaybe(16.0);
        }
        if (o->Get(ctx, "style"_v8(iso)).ToLocal(&field) && field->IsString()) {
          const std::string s = utf8(iso, field);
          if (s == "bold")
            want_style = fxe::font::Style::bold;
          else if (s == "italic")
            want_style = fxe::font::Style::italic;
          else if (s == "bold-italic")
            want_style = fxe::font::Style::bold_italic;
        }
      }
      if (!std::isfinite(size_px) || size_px < kEngineMinFontSizePx)
        return throw_type(iso, "Font.system: sizePx must be >= 1");
      auto disc = fxe::font::default_discover();
      if (!disc)
        return throw_type(iso, "Font.system: no font discovery backend available");
      fxe::font::Descriptor q;
      q.family = family;
      q.style = want_style;
      q.size_pt = static_cast<float>(size_px);
      auto results = disc->find(q);
      if (results.empty() || !results.front().path)
        return throw_type(iso, "Font.system: no matching font found");
      std::vector<u8> bytes;
      if (!read_file_bytes(*results.front().path, bytes))
        return throw_type(iso, "Font.system: failed to read discovered font");
      try {
        init_default_fonts(get_default_spritesheet(), std::span<const u8>(bytes),
                           static_cast<float>(size_px));
      } catch (const std::exception& e) {
        throw_type(iso, e.what());
        return;
      }
      info.GetReturnValue().Set(Integer::NewFromUnsigned(iso, 0));
    }
  } // namespace

  const fxe::font_info* resolve_font_id(u32 /*font_id*/) {
    // Always returns the engine default until the registry lands.
    return &fxe::get_font_info();
  }

  void install_font_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "load", FunctionTemplate::New(iso, s_load));
    ns->Set(iso, "builtin", FunctionTemplate::New(iso, s_builtin));
    ns->Set(iso, "dispose", FunctionTemplate::New(iso, s_dispose));
    ns->Set(iso, "system", FunctionTemplate::New(iso, s_system));
    global->Set(iso, "Font", ns);
  }
} // namespace fxe::js
