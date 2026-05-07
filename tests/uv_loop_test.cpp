#include "runtime/uv_loop.hpp"

#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

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
  bool pump_until(const std::function<bool()>& done) {
    for (int i = 0; i < 128; ++i) {
      fxe::runtime::pump_nonblocking();
      if (done()) {
        return true;
      }
      std::this_thread::yield();
    }
    return done();
  }

  struct timer_state {
    bool fired = false;
    bool closed = false;
  };

  void test_default_loop_and_timer() {
    auto* loop = fxe::runtime::default_loop();
    CHECK(loop != nullptr);

    uv_timer_t timer{};
    timer_state state{};
    timer.data = &state;
    CHECK(uv_timer_init(loop, &timer) == 0);
    CHECK(uv_timer_start(
              &timer,
              [](uv_timer_t* handle) {
                auto* s = static_cast<timer_state*>(handle->data);
                s->fired = true;
                uv_timer_stop(handle);
                uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* closed) {
                  static_cast<timer_state*>(closed->data)->closed = true;
                });
              },
              0, 0) == 0);

    CHECK(pump_until([&] { return state.closed; }));
    CHECK(state.fired);
    CHECK(state.closed);
  }

  void test_post_to_loop() {
    std::atomic<int> ran{0};
    std::thread worker(
        [&] { fxe::runtime::post_to_loop([&] { ran.fetch_add(1, std::memory_order_relaxed); }); });
    worker.join();

    CHECK(pump_until([&] { return ran.load(std::memory_order_relaxed) == 1; }));
    CHECK(ran.load(std::memory_order_relaxed) == 1);
  }

  void test_registered_pump_callback_routes_through_public_pump() {
    auto& runtime = fxe::runtime::uv_loop_runtime::instance();
    std::atomic<int> calls{0};
    const std::size_t id =
        runtime.register_pump_callback([&] { calls.fetch_add(1, std::memory_order_relaxed); });
    CHECK(id != 0);

    fxe::runtime::pump_nonblocking();
    CHECK(calls.load(std::memory_order_relaxed) == 1);

    runtime.unregister_pump_callback(id);
    fxe::runtime::pump_nonblocking();
    CHECK(calls.load(std::memory_order_relaxed) == 1);
  }
#else
  void test_no_libuv_stubs() {
    CHECK(fxe::runtime::default_loop() == nullptr);
    fxe::runtime::post_to_loop([] {});
    fxe::runtime::pump_nonblocking();
    fxe::runtime::shutdown_loop();

    auto& runtime = fxe::runtime::uv_loop_runtime::instance();
    CHECK(!runtime.available());
    CHECK(runtime.register_pump_callback([] {}) == 0);
    runtime.unregister_pump_callback(0);
  }
#endif
} // namespace

int main() {
#if FXE_HAS_LIBUV
  test_default_loop_and_timer();
  test_post_to_loop();
  test_registered_pump_callback_routes_through_public_pump();
  fxe::runtime::shutdown_loop();
  CHECK(fxe::runtime::default_loop() == nullptr);
#else
  test_no_libuv_stubs();
#endif

  if (g_fail != 0) {
    std::fprintf(stderr, "uv_loop_test: %d failed, %d passed\n", g_fail, g_pass);
    return 1;
  }
  std::fprintf(stdout, "uv_loop_test: %d passed\n", g_pass);
  return 0;
}
