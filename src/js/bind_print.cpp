#include "bind_print.hpp"
#include "js_command_buffer.hpp"

#include <cstdio>
#include <filesystem>
#include <fxe/command_buffer.hpp>
#include <fxe/js_bindings.hpp>
#include <fxe/log.hpp>
#include <fxe/print_pdf.hpp>
#include <fxe/renderer.hpp>
#include <fxe/types.hpp>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <string>
#include <v8.h>
#include <vector>

namespace fxe::js {
  namespace {
    using namespace v8;

    std::string utf8(Isolate* iso, Local<Value> v) {
      String::Utf8Value u(iso, v);
      return *u ? std::string(*u, u.length()) : std::string{};
    }

    void throw_type(Isolate* iso, const char* msg) {
      (void)throw_type_error(iso, msg);
    }

    command_view* unwrap_cb(Local<Value> value) {
      if (value.IsEmpty() || !value->IsObject())
        return nullptr;
      auto obj = value.As<Object>();
      if (void* raw = unwrap(obj, TAG_COMMAND_BUFFER))
        return static_cast<js_command_buffer*>(raw);
      if (void* raw = unwrap(obj, TAG_RENDERER))
        return static_cast<command_view*>(static_cast<renderer*>(raw));
      return nullptr;
    }

    bool get_u32_prop(Local<Context> ctx, Local<Object> obj, Local<String> primary,
                      Local<String> fallback, u32& out) {
      Local<Value> v;
      if (obj->Get(ctx, primary).ToLocal(&v) && !v->IsUndefined()) {
        out = v->Uint32Value(ctx).FromMaybe(0);
        return out > 0;
      }
      if (obj->Get(ctx, fallback).ToLocal(&v) && !v->IsUndefined()) {
        out = v->Uint32Value(ctx).FromMaybe(0);
        return out > 0;
      }
      return false;
    }

    void print_to_pdf(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 2 || !info[0]->IsString() || !info[1]->IsArray()) {
        throw_type(iso, "Print.toPdf(path, pages)");
        return;
      }

      auto pages_arg = info[1].As<Array>();
      std::vector<pdf_page> pages;
      pages.reserve(pages_arg->Length());
      for (u32 i = 0; i != pages_arg->Length(); ++i) {
        Local<Value> page_value;
        if (!pages_arg->Get(ctx, i).ToLocal(&page_value) || !page_value->IsObject()) {
          throw_type(iso, "Print.toPdf: pages must be objects");
          return;
        }
        auto page_obj = page_value.As<Object>();
        pdf_page page;
        if (!get_u32_prop(ctx, page_obj, "widthPt"_v8(iso), "width"_v8(iso), page.width_pt) ||
            !get_u32_prop(ctx, page_obj, "heightPt"_v8(iso), "height"_v8(iso), page.height_pt)) {
          throw_type(iso, "Print.toPdf: page requires widthPt/heightPt");
          return;
        }
        Local<Value> cb_value;
        if (!page_obj->Get(ctx, "commandBuffer"_v8(iso)).ToLocal(&cb_value)) {
          throw_type(iso, "Print.toPdf: page requires commandBuffer");
          return;
        }
        page.cb = unwrap_cb(cb_value);
        if (!page.cb) {
          throw_type(iso, "Print.toPdf: commandBuffer must be CommandBuffer or Renderer");
          return;
        }
        pages.push_back(page);
      }

      std::string err;
      const bool ok = emit_pdf(std::filesystem::path(utf8(iso, info[0])), pages, &err);
      if (!ok && !err.empty())
        FXE_ERROR("js.print", "Print.toPdf failed: {}", err);
      info.GetReturnValue().Set(Boolean::New(iso, ok));
    }
  } // namespace

  void install_print_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "toPdf", FunctionTemplate::New(iso, print_to_pdf));
    global->Set(iso, "Print", ns);
  }
} // namespace fxe::js
