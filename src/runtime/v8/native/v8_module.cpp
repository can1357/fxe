#include "runtime/v8/native/v8_module.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fxe/v8_helpers.hpp>
#include <fxe/v8_literals.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <v8-profiler.h>

namespace fxe::runtime {
  namespace {
    using namespace v8;
    using namespace fxe::js;

    struct byte_span {
      const uint8_t* data = nullptr;
      size_t size = 0;
    };

    std::string string_arg(Isolate* iso, Local<Value> value) {
      String::Utf8Value utf8(iso, value);
      if (*utf8 == nullptr)
        return {};
      return std::string(*utf8, static_cast<size_t>(utf8.length()));
    }

    bool bytes_arg(Local<Value> value, byte_span& out) {
      if (value->IsArrayBufferView()) {
        auto view = value.As<ArrayBufferView>();
        auto backing = view->Buffer()->GetBackingStore();
        out.data = static_cast<const uint8_t*>(backing->Data()) + view->ByteOffset();
        out.size = view->ByteLength();
        return true;
      }
      if (value->IsArrayBuffer()) {
        auto buffer = value.As<ArrayBuffer>();
        auto backing = buffer->GetBackingStore();
        out.data = static_cast<const uint8_t*>(backing->Data());
        out.size = backing->ByteLength();
        return true;
      }
      return false;
    }

    Local<Uint8Array> uint8_array_from_bytes(Isolate* iso, const uint8_t* data, size_t size) {
      auto backing = ArrayBuffer::NewBackingStore(iso, size);
      if (size > 0)
        std::memcpy(backing->Data(), data, size);
      auto buffer = ArrayBuffer::New(iso, std::move(backing));
      return Uint8Array::New(buffer, 0, size);
    }

    void add_function(Isolate* iso, Local<Context> ctx, Local<Object> obj, const char* name,
                      FunctionCallback fn) {
      auto maybe = Function::New(ctx, fn);
      if (!maybe.IsEmpty())
        (void)obj->Set(ctx, to_v8_string(iso, name), maybe.ToLocalChecked());
    }

    class file_output_stream final : public OutputStream {
    public:
      explicit file_output_stream(FILE* f) : f_(f) {}

      int GetChunkSize() override {
        return 64 * 1024;
      }

      WriteResult WriteAsciiChunk(char* data, int size) override {
        if (!f_ || size <= 0)
          return f_ ? kContinue : kAbort;
        const auto written = std::fwrite(data, 1, static_cast<size_t>(size), f_);
        if (written != static_cast<size_t>(size)) {
          ok_ = false;
          return kAbort;
        }
        return kContinue;
      }

      void EndOfStream() override {}

      bool ok() const {
        return ok_;
      }

    private:
      FILE* f_ = nullptr;
      bool ok_ = true;
    };

    void get_heap_statistics(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      HeapStatistics hs;
      iso->GetHeapStatistics(&hs);
      auto out = Object::New(iso);
      set_prop(ctx, out, "total_heap_size", static_cast<double>(hs.total_heap_size()));
      set_prop(ctx, out, "total_heap_size_executable",
               static_cast<double>(hs.total_heap_size_executable()));
      set_prop(ctx, out, "total_physical_size", static_cast<double>(hs.total_physical_size()));
      set_prop(ctx, out, "total_available_size", static_cast<double>(hs.total_available_size()));
      set_prop(ctx, out, "used_heap_size", static_cast<double>(hs.used_heap_size()));
      set_prop(ctx, out, "heap_size_limit", static_cast<double>(hs.heap_size_limit()));
      set_prop(ctx, out, "malloced_memory", static_cast<double>(hs.malloced_memory()));
      set_prop(ctx, out, "peak_malloced_memory", static_cast<double>(hs.peak_malloced_memory()));
      set_prop(ctx, out, "does_zap_garbage", static_cast<double>(hs.does_zap_garbage()));
      set_prop(ctx, out, "number_of_native_contexts",
               static_cast<double>(hs.number_of_native_contexts()));
      set_prop(ctx, out, "number_of_detached_contexts",
               static_cast<double>(hs.number_of_detached_contexts()));
      set_prop(ctx, out, "total_global_handles_size",
               static_cast<double>(hs.total_global_handles_size()));
      set_prop(ctx, out, "used_global_handles_size",
               static_cast<double>(hs.used_global_handles_size()));
      set_prop(ctx, out, "external_memory", static_cast<double>(hs.external_memory()));
      info.GetReturnValue().Set(out);
    }

    void get_heap_space_statistics(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      const size_t count = iso->NumberOfHeapSpaces();
      auto out = Array::New(iso, static_cast<int>(count));
      for (size_t i = 0; i < count; ++i) {
        HeapSpaceStatistics stats;
        if (!iso->GetHeapSpaceStatistics(&stats, i))
          continue;
        auto entry = Object::New(iso);
        set_prop(ctx, entry, "space_name", stats.space_name());
        set_prop(ctx, entry, "space_size", static_cast<double>(stats.space_size()));
        set_prop(ctx, entry, "space_used_size", static_cast<double>(stats.space_used_size()));
        set_prop(ctx, entry, "space_available_size",
                 static_cast<double>(stats.space_available_size()));
        set_prop(ctx, entry, "physical_space_size",
                 static_cast<double>(stats.physical_space_size()));
        (void)out->Set(ctx, static_cast<uint32_t>(i), entry);
      }
      info.GetReturnValue().Set(out);
    }

    void get_heap_code_statistics(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      HeapCodeStatistics stats;
      iso->GetHeapCodeAndMetadataStatistics(&stats);
      auto out = Object::New(iso);
      set_prop(ctx, out, "code_and_metadata_size",
               static_cast<double>(stats.code_and_metadata_size()));
      set_prop(ctx, out, "bytecode_and_metadata_size",
               static_cast<double>(stats.bytecode_and_metadata_size()));
      set_prop(ctx, out, "external_script_source_size",
               static_cast<double>(stats.external_script_source_size()));
      info.GetReturnValue().Set(out);
    }

    void cached_data_version_tag(const FunctionCallbackInfo<Value>& info) {
      info.GetReturnValue().Set(Number::New(
          info.GetIsolate(), static_cast<double>(ScriptCompiler::CachedDataVersionTag())));
    }

    void serialize_value(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      if (info.Length() < 1) {
        throw_type_error(iso, "__fxe_native.v8.serialize requires a value");
        return;
      }
      ValueSerializer serializer(iso);
      serializer.WriteHeader();
      auto ok = serializer.WriteValue(ctx, info[0]);
      if (ok.IsNothing() || !ok.FromJust()) {
        throw_error(iso, "DataCloneError");
        return;
      }
      auto released = serializer.Release();
      auto out = uint8_array_from_bytes(iso, released.first, released.second);
      std::free(released.first);
      info.GetReturnValue().Set(out);
    }

    void deserialize_value(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      auto ctx = iso->GetCurrentContext();
      byte_span bytes;
      if (info.Length() < 1 || !bytes_arg(info[0], bytes)) {
        throw_type_error(iso, "__fxe_native.v8.deserialize requires an ArrayBuffer or typed array");
        return;
      }
      ValueDeserializer deserializer(iso, bytes.data, bytes.size);
      auto header_ok = deserializer.ReadHeader(ctx);
      if (header_ok.IsNothing() || !header_ok.FromJust()) {
        throw_error(iso, "DataCloneError");
        return;
      }
      Local<Value> value;
      if (!deserializer.ReadValue(ctx).ToLocal(&value)) {
        throw_error(iso, "DataCloneError");
        return;
      }
      info.GetReturnValue().Set(value);
    }

    void write_heap_snapshot(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      if (info.Length() < 1 || !info[0]->IsString()) {
        throw_type_error(iso, "__fxe_native.v8.writeHeapSnapshot requires a file path string");
        return;
      }
      const auto path = string_arg(iso, info[0]);
      FILE* file = std::fopen(path.c_str(), "wb");
      if (!file) {
        info.GetReturnValue().Set(Boolean::New(iso, false));
        return;
      }
      bool ok = true;
      const auto* snapshot = iso->GetHeapProfiler()->TakeHeapSnapshot();
      if (!snapshot) {
        ok = false;
      } else {
        file_output_stream stream(file);
        snapshot->Serialize(&stream, HeapSnapshot::kJSON);
        ok = stream.ok() && std::fflush(file) == 0;
        const_cast<HeapSnapshot*>(snapshot)->Delete();
      }
      if (std::fclose(file) != 0)
        ok = false;
      info.GetReturnValue().Set(Boolean::New(iso, ok));
    }

    Local<Object> make_v8_namespace(Isolate* iso, Local<Context> ctx) {
      auto ns = Object::New(iso);
      add_function(iso, ctx, ns, "getHeapStatistics", get_heap_statistics);
      add_function(iso, ctx, ns, "getHeapSpaceStatistics", get_heap_space_statistics);
      add_function(iso, ctx, ns, "getHeapCodeStatistics", get_heap_code_statistics);
      add_function(iso, ctx, ns, "cachedDataVersionTag", cached_data_version_tag);
      add_function(iso, ctx, ns, "serialize", serialize_value);
      add_function(iso, ctx, ns, "deserialize", deserialize_value);
      add_function(iso, ctx, ns, "writeHeapSnapshot", write_heap_snapshot);
      return ns;
    }
  } // namespace

  void install_native_v8_module(Isolate* iso, Local<Context> ctx) {
    auto global = ctx->Global();
    Local<Value> native_value;
    if (!global->Get(ctx, "__fxe_native"_v8(iso)).ToLocal(&native_value) ||
        !native_value->IsObject()) {
      native_value = Object::New(iso);
      define_prop(ctx, global, "__fxe_native"_v8, native_value,
                  static_cast<PropertyAttribute>(DontEnum));
    }
    auto native = native_value.As<Object>();
    (void)native->Set(ctx, "v8"_v8(iso), make_v8_namespace(iso, ctx));
  }
} // namespace fxe::runtime
