// JS bindings for fxe::render_stats. Exposes a process-namespaced
// `RenderStats` object on the global with bumpers used by the JS reactive
// reconciler and a snapshot() reader for diagnostics.

#include <fxe/command_buffer.hpp>
#include <fxe/render_stats.hpp>

#include "bind_render_stats.hpp"

#include <cstdint>
#include <v8.h>

namespace fxe::js {
  namespace {
    using namespace v8;

    void rs_snapshot(const FunctionCallbackInfo<Value>& info) {
      auto* iso = info.GetIsolate();
      HandleScope hs(iso);
      auto ctx = iso->GetCurrentContext();
      const auto& s = current_render_stats();
      auto out = Object::New(iso);
      auto put = [&](const char* k, std::uint64_t v) {
        (void)out->Set(ctx, String::NewFromUtf8(iso, k).ToLocalChecked(),
                       Number::New(iso, static_cast<double>(v)));
      };
      put("verticesSubmitted", s.vertices_submitted);
      put("indicesSubmitted", s.indices_submitted);
      put("queueCalls", s.queue_calls);
      put("cacheHits", s.cache_hits);
      put("cacheMisses", s.cache_misses);
      put("rebuilds", s.rebuilds);
      put("frames", s.frames);
      put("queueFastIdentity", fxe::command_buffer::g_q_fast.load());
      put("queueXform", fxe::command_buffer::g_q_xform.load());
      put("queueTinted", fxe::command_buffer::g_q_tinted.load());
      info.GetReturnValue().Set(out);
    }

    void rs_reset(const FunctionCallbackInfo<Value>&) {
      current_render_stats().reset();
    }

    void rs_record_cache_hit(const FunctionCallbackInfo<Value>&) {
      ++current_render_stats().cache_hits;
    }

    void rs_record_cache_miss(const FunctionCallbackInfo<Value>&) {
      ++current_render_stats().cache_misses;
    }

    void rs_record_rebuild(const FunctionCallbackInfo<Value>&) {
      ++current_render_stats().rebuilds;
    }

    void rs_record_queue_call(const FunctionCallbackInfo<Value>&) {
      ++current_render_stats().queue_calls;
    }

    void rs_begin_frame(const FunctionCallbackInfo<Value>&) {
      ++current_render_stats().frames;
    }
  } // namespace

  void install_render_stats_global(Isolate* iso, Local<ObjectTemplate> global) {
    HandleScope hs(iso);
    auto ns = ObjectTemplate::New(iso);
    ns->Set(iso, "snapshot", FunctionTemplate::New(iso, rs_snapshot));
    ns->Set(iso, "reset", FunctionTemplate::New(iso, rs_reset));
    ns->Set(iso, "recordCacheHit", FunctionTemplate::New(iso, rs_record_cache_hit));
    ns->Set(iso, "recordCacheMiss", FunctionTemplate::New(iso, rs_record_cache_miss));
    ns->Set(iso, "recordRebuild", FunctionTemplate::New(iso, rs_record_rebuild));
    ns->Set(iso, "recordQueueCall", FunctionTemplate::New(iso, rs_record_queue_call));
    ns->Set(iso, "beginFrame", FunctionTemplate::New(iso, rs_begin_frame));
    global->Set(iso, "RenderStats", ns);
  }
} // namespace fxe::js
