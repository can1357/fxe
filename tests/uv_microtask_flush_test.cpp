#include "runtime/uv_loop.hpp"

#include <fxe/v8_host.hpp>
#include <v8.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

#ifndef FXE_V8_ICUDTL_PATH
#define FXE_V8_ICUDTL_PATH ""
#endif

namespace {
  int g_pass = 0;
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (ok) {
      ++g_pass;
    } else {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

#if FXE_HAS_LIBUV
  struct timer_state {
    v8::Isolate* isolate = nullptr;
    v8::Global<v8::Context>* context = nullptr;
    v8::Global<v8::Promise::Resolver>* resolver = nullptr;
    bool fired = false;
    bool closed = false;
  };

  bool global_bool(v8::Isolate* isolate, v8::Local<v8::Context> ctx, const char* name) {
    v8::Local<v8::Value> value;
    if (!ctx->Global()
             ->Get(ctx, v8::String::NewFromUtf8(isolate, name).ToLocalChecked())
             .ToLocal(&value)) {
      return false;
    }
    return value->BooleanValue(isolate);
  }

  void test_libuv_timer_flushes_v8_microtasks_in_same_pump() {
    std::size_t checkpoint_id = 0;

    auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
        v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    auto* isolate = v8::Isolate::New(params);

    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope handle_scope(isolate);
      auto ctx = v8::Context::New(isolate);
      v8::Context::Scope context_scope(ctx);
      v8::Global<v8::Context> context_global(isolate, ctx);

      checkpoint_id = fxe::runtime::uv_loop_runtime::instance().register_microtask_checkpoint(
          [isolate, &context_global] {
            v8::Isolate::Scope isolate_scope(isolate);
            v8::HandleScope handle_scope(isolate);
            auto local_ctx = context_global.Get(isolate);
            v8::Context::Scope context_scope(local_ctx);
            isolate->PerformMicrotaskCheckpoint();
          });
      CHECK(checkpoint_id != 0);

      auto resolver = v8::Promise::Resolver::New(ctx).ToLocalChecked();
      v8::Global<v8::Promise::Resolver> resolver_global(isolate, resolver);
      CHECK(ctx->Global()
                ->Set(ctx, v8::String::NewFromUtf8Literal(isolate, "p"), resolver->GetPromise())
                .FromMaybe(false));

      auto source = v8::String::NewFromUtf8Literal(
          isolate, "globalThis.thenRan = false; p.then(() => { globalThis.thenRan = true; });");
      auto script = v8::Script::Compile(ctx, source).ToLocalChecked();
      CHECK(!script->Run(ctx).IsEmpty());
      CHECK(!global_bool(isolate, ctx, "thenRan"));

      timer_state state{isolate, &context_global, &resolver_global, false, false};
      auto* loop = fxe::runtime::default_loop();
      CHECK(loop != nullptr);
      if (loop == nullptr) {
        fxe::runtime::uv_loop_runtime::instance().unregister_microtask_checkpoint(checkpoint_id);
        return;
      }
      uv_timer_t timer{};
      timer.data = &state;
      if (uv_timer_init(loop, &timer) != 0) {
        CHECK(false);
        fxe::runtime::uv_loop_runtime::instance().unregister_microtask_checkpoint(checkpoint_id);
        return;
      }
      CHECK(uv_timer_start(
                &timer,
                [](uv_timer_t* handle) {
                  auto* state = static_cast<timer_state*>(handle->data);
                  state->fired = true;
                  auto* isolate = state->isolate;
                  v8::Isolate::Scope isolate_scope(isolate);
                  v8::HandleScope handle_scope(isolate);
                  auto ctx = state->context->Get(isolate);
                  v8::Context::Scope context_scope(ctx);
                  auto resolver = state->resolver->Get(isolate);
                  CHECK(resolver->Resolve(ctx, v8::True(isolate)).FromMaybe(false));
                  uv_timer_stop(handle);
                  uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* closed) {
                    static_cast<timer_state*>(closed->data)->closed = true;
                  });
                },
                0, 0) == 0);

      CHECK(fxe::runtime::uv_loop_runtime::instance().pump_nowait() >= 0);
      CHECK(state.fired);
      CHECK(global_bool(isolate, ctx, "thenRan"));

      fxe::runtime::uv_loop_runtime::instance().unregister_microtask_checkpoint(checkpoint_id);
      for (int i = 0; i < 8 && !state.closed; ++i)
        (void)fxe::runtime::uv_loop_runtime::instance().pump_nowait();
      CHECK(state.closed);

      resolver_global.Reset();
      context_global.Reset();
    }

    isolate->Dispose();
  }
#else
  void test_no_libuv_stub() {
    CHECK(fxe::runtime::default_loop() == nullptr);
  }
#endif
} // namespace

int main(int argc, char** argv) {
  fxe::js::initialize(argc > 0 ? argv[0] : "", FXE_V8_ICUDTL_PATH);

#if FXE_HAS_LIBUV
  test_libuv_timer_flushes_v8_microtasks_in_same_pump();
  fxe::runtime::shutdown_loop();
#else
  test_no_libuv_stub();
#endif

  if (g_fail != 0) {
    std::fprintf(stderr, "uv_microtask_flush_test: %d failed, %d passed\n", g_fail, g_pass);
  } else {
    std::fprintf(stdout, "uv_microtask_flush_test: %d passed\n", g_pass);
  }
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(g_fail == 0 ? 0 : 1);
}
