#include <fxe/window.hpp>

#include <cstdio>
#include <vector>

namespace {
  int g_fail = 0;

  void check(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
      ++g_fail;
      std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expr);
    }
  }

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

  class fake_window final : public fxe::window {
  public:
    void poll() override {}
    void wait_events() override {}
    void wait_events_timeout(double) override {}
    void post_redraw() override {}
    [[nodiscard]] bool take_redraw_request() override {
      return false;
    }
    [[nodiscard]] bool peek_redraw_request() const override {
      return false;
    }
    void close() override {}
    [[nodiscard]] bool should_close() const override {
      return false;
    }
    [[nodiscard]] fxe::math::uvec2 framebuffer_size() const override {
      return {};
    }
    void set_vsync(bool) override {}
    void* native_handle() const override {
      return nullptr;
    }
    [[nodiscard]] bool is_transparent() const override {
      return false;
    }
  };

  void test_defaults() {
    fxe::input_event ev{};
    CHECK(ev.kind == fxe::input_event::kind_t::mouse_move);
    CHECK(ev.magnification == 0.0f);
    CHECK(ev.rotation_radians == 0.0f);
    CHECK(ev.swipe_dx == 0);
    CHECK(ev.swipe_dy == 0);
    CHECK(ev.scroll_phase == fxe::input_event::scroll_phase_t::none);
    CHECK(!ev.precision);
  }

  void test_gesture_round_trip() {
    fake_window w;
    fxe::input_event ev{};
    ev.kind = fxe::input_event::kind_t::gesture_rotate_change;
    ev.magnification = 0.25f;
    ev.rotation_radians = 0.5f;
    ev.swipe_dx = 3;
    ev.swipe_dy = -2;
    ev.scroll_phase = fxe::input_event::scroll_phase_t::changed;
    ev.precision = true;
    w.inject(ev);

    std::vector<fxe::input_event> drained = w.drain_input_events();
    CHECK(drained.size() == 1);
    CHECK(drained[0].kind == fxe::input_event::kind_t::gesture_rotate_change);
    CHECK(drained[0].magnification == 0.25f);
    CHECK(drained[0].rotation_radians == 0.5f);
    CHECK(drained[0].swipe_dx == 3);
    CHECK(drained[0].swipe_dy == -2);
    CHECK(drained[0].scroll_phase == fxe::input_event::scroll_phase_t::changed);
    CHECK(drained[0].precision);
  }
} // namespace

int main() {
  test_defaults();
  test_gesture_round_trip();
  return g_fail == 0 ? 0 : 1;
}
