#include "bind_blob.hpp"

#include <fxe/js_bindings.hpp>
#include <fxe/types.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    constexpr u32 TAG_BLOB = 0x424C4F42u;            // 'BLOB'
    constexpr u32 TAG_READABLE_STREAM = 0x52535452u; // 'RSTR'
    constexpr u32 TAG_STREAM_READER = 0x53524452u;   // 'SRDR'

    using TplGlobal = Global<FunctionTemplate>;

    std::unordered_map<Isolate*, TplGlobal>& blob_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& stream_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }
    std::unordered_map<Isolate*, TplGlobal>& reader_tpl_table() {
      static std::unordered_map<Isolate*, TplGlobal> t;
      return t;
    }

    void blob_reset_for_isolate(Isolate* iso) {
      auto reset = [iso](auto& table) {
        auto it = table.find(iso);
        if (it != table.end()) {
          it->second.Reset();
          table.erase(it);
        }
      };
      reset(blob_tpl_table());
      reset(stream_tpl_table());
      reset(reader_tpl_table());
    }

    struct blob_resetter_register {
      blob_resetter_register() {
        register_template_resetter(&blob_reset_for_isolate);
      }
    };
    static blob_resetter_register s_blob_resetter_register;

    Local<String> s8(Isolate* iso, const std::string& s) {
      return String::NewFromUtf8(iso, s.c_str(), NewStringType::kNormal, static_cast<int>(s.size()))
          .ToLocalChecked();
    }

    std::string to_str(Isolate* iso, Local<Value> v) {
      auto ctx = iso->GetCurrentContext();
      Local<String> str;
      if (!v->ToString(ctx).ToLocal(&str))
        return {};
      String::Utf8Value u(iso, str);
      return std::string(*u ? *u : "", *u ? u.length() : 0);
    }

    void throw_type(Isolate* iso, const char* m) {
      iso->ThrowException(Exception::TypeError(String::NewFromUtf8(iso, m).ToLocalChecked()));
    }

    std::string normalize_type(std::string type) {
      for (char ch : type) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c > 0x7e)
          return {};
      }
      std::transform(type.begin(), type.end(), type.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return type;
    }

    struct blob_holder {
      std::shared_ptr<std::vector<std::uint8_t>> bytes;
      std::size_t offset = 0;
      std::size_t length = 0;
      std::string type;
      Global<Object> self;
    };

    struct stream_holder {
      std::shared_ptr<std::vector<std::uint8_t>> bytes;
      std::size_t offset = 0;
      std::size_t length = 0;
      Global<Object> self;
    };

    struct reader_holder {
      std::shared_ptr<std::vector<std::uint8_t>> bytes;
      std::size_t offset = 0;
      std::size_t length = 0;
      bool consumed = false;
      Global<Object> self;
    };

    void blob_finalizer(const WeakCallbackInfo<blob_holder>& info) {
      auto* h = info.GetParameter();
      h->self.Reset();
      delete h;
    }
    void stream_finalizer(const WeakCallbackInfo<stream_holder>& info) {
      auto* h = info.GetParameter();
      h->self.Reset();
      delete h;
    }
    void reader_finalizer(const WeakCallbackInfo<reader_holder>& info) {
      auto* h = info.GetParameter();
      h->self.Reset();
      delete h;
    }

    blob_holder* unwrap_blob(Local<Value> v) {
      if (v.IsEmpty() || !v->IsObject())
        return nullptr;
      return static_cast<blob_holder*>(unwrap(v.As<Object>(), TAG_BLOB));
    }
    blob_holder* unwrap_blob_object(Local<Object> o) {
      return static_cast<blob_holder*>(unwrap(o, TAG_BLOB));
    }
    stream_holder* unwrap_stream_object(Local<Object> o) {
      return static_cast<stream_holder*>(unwrap(o, TAG_READABLE_STREAM));
    }
    reader_holder* unwrap_reader_object(Local<Object> o) {
      return static_cast<reader_holder*>(unwrap(o, TAG_STREAM_READER));
    }

    void init_blob_object(Isolate* iso, Local<Object> obj, blob_holder* h) {
      obj->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      obj->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_BLOB));
      h->self.Reset(iso, obj);
      h->self.SetWeak(h, blob_finalizer, WeakCallbackType::kParameter);
    }
    void init_stream_object(Isolate* iso, Local<Object> obj, stream_holder* h) {
      obj->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      obj->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_READABLE_STREAM));
      h->self.Reset(iso, obj);
      h->self.SetWeak(h, stream_finalizer, WeakCallbackType::kParameter);
    }
    void init_reader_object(Isolate* iso, Local<Object> obj, reader_holder* h) {
      obj->SetInternalField(0, External::New(iso, h, v8::kExternalPointerTypeTagDefault));
      obj->SetInternalField(1, Integer::NewFromUnsigned(iso, TAG_STREAM_READER));
      h->self.Reset(iso, obj);
      h->self.SetWeak(h, reader_finalizer, WeakCallbackType::kParameter);
    }

    Local<ArrayBuffer> copy_array_buffer(Isolate* iso, const std::uint8_t* data, std::size_t len) {
      auto store = ArrayBuffer::NewBackingStore(iso, len);
      if (len != 0)
        std::memcpy(store->Data(), data, len);
      return ArrayBuffer::New(iso, std::move(store));
    }

    struct blob_part {
      std::shared_ptr<std::vector<std::uint8_t>> bytes;
      std::size_t offset = 0;
      std::size_t length = 0;
    };

    blob_part part_from_owned(std::vector<std::uint8_t> bytes) {
      auto shared = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
      return blob_part{shared, 0, shared->size()};
    }

    blob_part part_from_string(std::string s) {
      std::vector<std::uint8_t> bytes(s.begin(), s.end());
      return part_from_owned(std::move(bytes));
    }

    bool extract_part(Isolate* iso, Local<Value> v, blob_part& out) {
      if (auto* b = unwrap_blob(v)) {
        out = blob_part{b->bytes, b->offset, b->length};
        return true;
      }
      if (v->IsArrayBuffer()) {
        auto ab = v.As<ArrayBuffer>();
        auto bs = ab->GetBackingStore();
        const auto* p = static_cast<const std::uint8_t*>(bs->Data());
        std::vector<std::uint8_t> bytes(p, p + bs->ByteLength());
        out = part_from_owned(std::move(bytes));
        return true;
      }
      if (v->IsArrayBufferView()) {
        auto view = v.As<ArrayBufferView>();
        auto ab = view->Buffer();
        auto bs = ab->GetBackingStore();
        const auto* p = static_cast<const std::uint8_t*>(bs->Data()) + view->ByteOffset();
        std::vector<std::uint8_t> bytes(p, p + view->ByteLength());
        out = part_from_owned(std::move(bytes));
        return true;
      }
      out = part_from_string(to_str(iso, v));
      return true;
    }

    std::shared_ptr<std::vector<std::uint8_t>>
    build_bytes_from_parts(const std::vector<blob_part>& parts, std::size_t total,
                           std::size_t& offset, std::size_t& length) {
      if (parts.empty()) {
        offset = 0;
        length = 0;
        return std::make_shared<std::vector<std::uint8_t>>();
      }
      if (parts.size() == 1) {
        offset = parts[0].offset;
        length = parts[0].length;
        return parts[0].bytes;
      }
      auto bytes = std::make_shared<std::vector<std::uint8_t>>();
      bytes->reserve(total);
      for (const auto& part : parts) {
        if (part.length == 0)
          continue;
        const auto* begin = part.bytes->data() + part.offset;
        bytes->insert(bytes->end(), begin, begin + part.length);
      }
      offset = 0;
      length = bytes->size();
      return bytes;
    }

    double number_arg(Local<Context> ctx, Local<Value> v, double fallback) {
      if (v.IsEmpty() || v->IsUndefined())
        return fallback;
      Maybe<double> maybe = v->NumberValue(ctx);
      return maybe.FromMaybe(fallback);
    }

    std::size_t normalize_slice_index(double value, std::size_t size) {
      if (std::isnan(value))
        value = 0;
      value = std::trunc(value);
      if (value < 0) {
        const double from_end = static_cast<double>(size) + value;
        return from_end <= 0 ? 0 : static_cast<std::size_t>(from_end);
      }
      if (value >= static_cast<double>(size))
        return size;
      return static_cast<std::size_t>(value);
    }

    std::string read_type_option(Isolate* iso, Local<Context> ctx, Local<Value> options) {
      if (options.IsEmpty() || !options->IsObject())
        return {};
      Local<Value> type_value;
      if (!options.As<Object>()
               ->Get(ctx, String::NewFromUtf8Literal(iso, "type"))
               .ToLocal(&type_value) ||
          type_value->IsUndefined())
        return {};
      return normalize_type(to_str(iso, type_value));
    }

    void blob_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      if (!info.IsConstructCall()) {
        throw_type(iso, "Blob must be called with new");
        return;
      }

      std::vector<blob_part> parts;
      std::size_t total = 0;
      if (info.Length() >= 1 && !info[0]->IsUndefined() && !info[0]->IsNull()) {
        if (info[0]->IsArray()) {
          auto arr = info[0].As<Array>();
          const u32 n = arr->Length();
          parts.reserve(n);
          for (u32 i = 0; i < n; ++i) {
            Local<Value> value;
            if (!arr->Get(ctx, i).ToLocal(&value))
              continue;
            blob_part part;
            if (extract_part(iso, value, part)) {
              total += part.length;
              parts.push_back(std::move(part));
            }
          }
        } else {
          blob_part part;
          if (extract_part(iso, info[0], part)) {
            total += part.length;
            parts.push_back(std::move(part));
          }
        }
      }

      std::size_t offset = 0;
      std::size_t length = 0;
      auto bytes = build_bytes_from_parts(parts, total, offset, length);
      auto* h =
          new blob_holder{std::move(bytes),
                          offset,
                          length,
                          info.Length() >= 2 ? read_type_option(iso, ctx, info[1]) : std::string{},
                          {}};
      init_blob_object(iso, info.This(), h);
      info.GetReturnValue().Set(info.This());
    }

    void blob_get_size(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* h = unwrap_blob_object(info.HolderV2());
      if (!h)
        return;
      info.GetReturnValue().Set(Number::New(info.GetIsolate(), static_cast<double>(h->length)));
    }

    void blob_get_type(Local<Name>, const PropertyCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto* h = unwrap_blob_object(info.HolderV2());
      if (!h)
        return;
      info.GetReturnValue().Set(s8(iso, h->type));
    }

    void blob_slice(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_blob_object(info.This());
      if (!h) {
        throw_type(iso, "Blob.slice: invalid this");
        return;
      }
      const auto start =
          normalize_slice_index(info.Length() >= 1 ? number_arg(ctx, info[0], 0) : 0, h->length);
      const auto end = normalize_slice_index(
          info.Length() >= 2 ? number_arg(ctx, info[1], static_cast<double>(h->length))
                             : static_cast<double>(h->length),
          h->length);
      const auto slice_length = start < end ? end - start : 0;
      auto* sliced =
          new blob_holder{h->bytes,
                          h->offset + start,
                          slice_length,
                          info.Length() >= 3 ? normalize_type(to_str(iso, info[2])) : std::string{},
                          {}};
      auto obj =
          blob_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      init_blob_object(iso, obj, sliced);
      info.GetReturnValue().Set(obj);
    }

    void blob_array_buffer(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_blob_object(info.This());
      if (!h) {
        throw_type(iso, "Blob.arrayBuffer: invalid this");
        return;
      }
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      const auto* data = h->length == 0 ? nullptr : h->bytes->data() + h->offset;
      resolver->Resolve(ctx, copy_array_buffer(iso, data, h->length)).Check();
      info.GetReturnValue().Set(resolver->GetPromise());
    }

    void blob_text(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_blob_object(info.This());
      if (!h) {
        throw_type(iso, "Blob.text: invalid this");
        return;
      }
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      const char* data =
          h->length == 0 ? "" : reinterpret_cast<const char*>(h->bytes->data() + h->offset);
      resolver
          ->Resolve(ctx, String::NewFromUtf8(iso, data, NewStringType::kNormal,
                                             static_cast<int>(h->length))
                             .ToLocalChecked())
          .Check();
      info.GetReturnValue().Set(resolver->GetPromise());
    }

    Local<Object> make_reader_object(Isolate* iso, Local<Context> ctx,
                                     std::shared_ptr<std::vector<std::uint8_t>> bytes,
                                     std::size_t offset, std::size_t length) {
      auto obj =
          reader_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      auto* h = new reader_holder{std::move(bytes), offset, length, false, {}};
      init_reader_object(iso, obj, h);
      return obj;
    }

    Local<Object> make_stream_object(Isolate* iso, Local<Context> ctx,
                                     std::shared_ptr<std::vector<std::uint8_t>> bytes,
                                     std::size_t offset, std::size_t length) {
      auto obj =
          stream_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
      auto* h = new stream_holder{std::move(bytes), offset, length, {}};
      init_stream_object(iso, obj, h);
      return obj;
    }

    void blob_stream(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_blob_object(info.This());
      if (!h) {
        throw_type(iso, "Blob.stream: invalid this");
        return;
      }
      info.GetReturnValue().Set(make_stream_object(iso, ctx, h->bytes, h->offset, h->length));
    }

    void stream_ctor(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (!info.IsConstructCall()) {
        throw_type(iso, "ReadableStream must be called with new");
        return;
      }
      auto empty = std::make_shared<std::vector<std::uint8_t>>();
      auto* h = new stream_holder{std::move(empty), 0, 0, {}};
      init_stream_object(iso, info.This(), h);
      info.GetReturnValue().Set(info.This());
    }

    void stream_get_reader(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_stream_object(info.This());
      if (!h) {
        throw_type(iso, "ReadableStream.getReader: invalid this");
        return;
      }
      info.GetReturnValue().Set(make_reader_object(iso, ctx, h->bytes, h->offset, h->length));
    }

    void reader_read(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      auto* h = unwrap_reader_object(info.This());
      if (!h) {
        throw_type(iso, "ReadableStreamDefaultReader.read: invalid this");
        return;
      }
      auto resolver = Promise::Resolver::New(ctx).ToLocalChecked();
      auto result = Object::New(iso);
      if (h->consumed) {
        result->Set(ctx, String::NewFromUtf8Literal(iso, "done"), Boolean::New(iso, true)).Check();
      } else {
        h->consumed = true;
        const auto* data = h->length == 0 ? nullptr : h->bytes->data() + h->offset;
        auto ab = copy_array_buffer(iso, data, h->length);
        result->Set(ctx, String::NewFromUtf8Literal(iso, "done"), Boolean::New(iso, false)).Check();
        result
            ->Set(ctx, String::NewFromUtf8Literal(iso, "value"), Uint8Array::New(ab, 0, h->length))
            .Check();
      }
      resolver->Resolve(ctx, result).Check();
      info.GetReturnValue().Set(resolver->GetPromise());
    }
  } // namespace

  Local<Object> make_blob_object(Isolate* iso, Local<Context> ctx,
                                 std::shared_ptr<std::vector<std::uint8_t>> bytes,
                                 std::string type) {
    if (!bytes)
      bytes = std::make_shared<std::vector<std::uint8_t>>();
    const auto length = bytes->size();
    auto obj =
        blob_tpl_table()[iso].Get(iso)->InstanceTemplate()->NewInstance(ctx).ToLocalChecked();
    auto* h = new blob_holder{std::move(bytes), 0, length, normalize_type(std::move(type)), {}};
    init_blob_object(iso, obj, h);
    return obj;
  }

  void install_blob_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);

    auto blob_tpl = FunctionTemplate::New(iso, blob_ctor);
    blob_tpl->SetClassName(String::NewFromUtf8Literal(iso, "Blob"));
    blob_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    blob_tpl->InstanceTemplate()->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "size"),
                                                        blob_get_size, nullptr);
    blob_tpl->InstanceTemplate()->SetNativeDataProperty(String::NewFromUtf8Literal(iso, "type"),
                                                        blob_get_type, nullptr);
    auto blob_proto = blob_tpl->PrototypeTemplate();
    blob_proto->Set(iso, "slice", FunctionTemplate::New(iso, blob_slice));
    blob_proto->Set(iso, "arrayBuffer", FunctionTemplate::New(iso, blob_array_buffer));
    blob_proto->Set(iso, "text", FunctionTemplate::New(iso, blob_text));
    blob_proto->Set(iso, "stream", FunctionTemplate::New(iso, blob_stream));

    auto stream_tpl = FunctionTemplate::New(iso, stream_ctor);
    stream_tpl->SetClassName(String::NewFromUtf8Literal(iso, "ReadableStream"));
    stream_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    stream_tpl->PrototypeTemplate()->Set(iso, "getReader",
                                         FunctionTemplate::New(iso, stream_get_reader));

    auto reader_tpl = FunctionTemplate::New(iso);
    reader_tpl->SetClassName(String::NewFromUtf8Literal(iso, "ReadableStreamDefaultReader"));
    reader_tpl->InstanceTemplate()->SetInternalFieldCount(2);
    reader_tpl->PrototypeTemplate()->Set(iso, "read", FunctionTemplate::New(iso, reader_read));

    global->Set(iso, "Blob", blob_tpl);
    global->Set(iso, "ReadableStream", stream_tpl);
    blob_tpl_table()[iso].Reset(iso, blob_tpl);
    stream_tpl_table()[iso].Reset(iso, stream_tpl);
    reader_tpl_table()[iso].Reset(iso, reader_tpl);
  }
} // namespace fxe::js
