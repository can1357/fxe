// JS bindings for the `Font` namespace.
//
// The engine uses one process-wide default font, but that font now owns a
// size-aware glyph registry and growable atlas. fontId remains a
// forward-compatible opaque handle (id 0 = default font), Font.load()
// replaces that default, and sizePx seeds the default variant used when
// callers omit an explicit text size.

#include "bind_font.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <fxe/font.hpp>
#include <fxe/spritesheet.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <span>
#include <string>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr double kEngineMinFontSizePx = 1.0;

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
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsNumber()) {
        (void)throw_type_error(iso, "Font.load(path: string, sizePx: number)");
        return;
      }
      auto ctx = iso->GetCurrentContext();
      const double size_px = info[1]->NumberValue(ctx).FromMaybe(0.0);
      if (!std::isfinite(size_px) || size_px < kEngineMinFontSizePx) {
        (void)throw_type_error(iso, "Font.load: sizePx must be a finite number >= 1");
        return;
      }
      std::vector<u8> bytes;
      if (!read_file_bytes(to_std_string(iso, info[0]), bytes)) {
        (void)throw_type_error(iso, "Font.load: failed to read TTF file");
        return;
      }
      try {
        init_default_fonts(get_default_spritesheet(), std::span<const u8>(bytes),
                           static_cast<float>(size_px));
      } catch (const std::exception& e) {
        (void)throw_type_error(iso, e.what());
        return;
      }
      info.GetReturnValue().Set(0_v8(iso));
    }

    void s_builtin(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString() || to_std_string(iso, info[0]) != "default") {
        (void)throw_type_error(iso, "Font.builtin(name): expected 'default'");
        return;
      }
      info.GetReturnValue().Set(0_v8(iso));
    }

    void s_dispose(const FunctionCallbackInfo<Value>& info) {
      // No-op until the per-id registry lands.
      (void)info;
    }

    void s_system(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      if (info.Length() < 1 || !info[0]->IsString()) {
        (void)throw_type_error(iso, "Font.system(family: string, opts?)");
        return;
      }
      auto ctx = iso->GetCurrentContext();
      const std::string family = to_std_string(iso, info[0]);
      double size_px = 16.0;
      fxe::font::Style want_style = fxe::font::Style::regular;
      if (info.Length() >= 2 && info[1]->IsObject()) {
        auto o = info[1].As<Object>();
        if (auto field = get_prop<Local<Value>>(ctx, o, "sizePx"); field && (*field)->IsNumber()) {
          size_px = (*field)->NumberValue(ctx).FromMaybe(16.0);
        }
        if (auto field = get_prop<Local<Value>>(ctx, o, "style"); field && (*field)->IsString()) {
          auto s = field->As<String>();
          if (s == "bold"_v8)
            want_style = fxe::font::Style::bold;
          else if (s == "italic"_v8)
            want_style = fxe::font::Style::italic;
          else if (s == "bold-italic"_v8)
            want_style = fxe::font::Style::bold_italic;
        }
      }
      if (!std::isfinite(size_px) || size_px < kEngineMinFontSizePx) {
        (void)throw_type_error(iso, "Font.system: sizePx must be >= 1");
        return;
      }
      auto disc = fxe::font::default_discover();
      if (!disc) {
        (void)throw_type_error(iso, "Font.system: no font discovery backend available");
        return;
      }
      fxe::font::Descriptor q;
      q.family = family;
      q.style = want_style;
      q.size_pt = static_cast<float>(size_px);
      auto results = disc->find(q);
      if (results.empty() || !results.front().path) {
        (void)throw_type_error(iso, "Font.system: no matching font found");
        return;
      }
      std::vector<u8> bytes;
      if (!read_file_bytes(*results.front().path, bytes)) {
        (void)throw_type_error(iso, "Font.system: failed to read discovered font");
        return;
      }
      try {
        init_default_fonts(get_default_spritesheet(), std::span<const u8>(bytes),
                           static_cast<float>(size_px));
      } catch (const std::exception& e) {
        (void)throw_type_error(iso, e.what());
        return;
      }
      info.GetReturnValue().Set(0_v8(iso));
    }
    void font_namespace_getter(Local<Name> /*name*/, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto ns = Object::New(iso);
      (void)ns->Set(ctx, "load"_v8(iso), Function::New(ctx, s_load).ToLocalChecked());
      (void)ns->Set(ctx, "builtin"_v8(iso), Function::New(ctx, s_builtin).ToLocalChecked());
      (void)ns->Set(ctx, "dispose"_v8(iso), Function::New(ctx, s_dispose).ToLocalChecked());
      (void)ns->Set(ctx, "system"_v8(iso), Function::New(ctx, s_system).ToLocalChecked());
      info.GetReturnValue().Set(ns);
    }
  } // namespace

  const fxe::font_info* resolve_font_id(u32 /*font_id*/) {
    // Always returns the engine default until the registry lands.
    return &fxe::get_font_info();
  }

  void install_font_global(Isolate* iso, Local<ObjectTemplate> global) {
    global->SetLazyDataProperty("Font"_v8(iso), font_namespace_getter);
  }
} // namespace fxe::js
