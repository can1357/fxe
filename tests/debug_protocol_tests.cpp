// Tests for the fxe debug protocol primitives (base64, dispatch lookup).
// No socket / V8 / GPU dependency — runs in the `dev` preset.

#include "../src/debug/base64.hpp"
#include "../src/debug/dispatch.hpp"
#include "../src/debug/server.hpp"
#include <fxe/renderer.hpp>
#include <fxe/types.hpp>
#include <fxe/window.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifdef FXE_HAS_V8
#include <atomic>
#include <fxe/v8_host.hpp>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
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
  bool schema_domain_exists(const fxe::debug::json& domains, const char* name) {
    for (const auto& domain : domains) {
      if (domain.is_object() && domain.contains("name") && domain.at("name").is_string() &&
          domain.at("name").get<std::string>() == name) {
        return true;
      }
    }
    return false;
  }

  class screenshot_test_window final : public fxe::window {
  public:
    screenshot_test_window(unsigned w, unsigned h) : size_{w, h} {}
    void poll() override {}
    void wait_events() override {}
    void wait_events_timeout(double) override {}
    void post_redraw() override {
      dirty_ = true;
    }
    bool take_redraw_request() override {
      bool out = dirty_;
      dirty_ = false;
      return out;
    }
    void close() override {
      closed_ = true;
    }
    bool should_close() const override {
      return closed_;
    }
    fxe::math::uvec2 framebuffer_size() const override {
      return size_;
    }
    void set_vsync(bool) override {}
    void* native_handle() const override {
      return nullptr;
    }

  private:
    fxe::math::uvec2 size_{};
    bool dirty_ = false;
    bool closed_ = false;
  };

  class async_capture_test_renderer final : public fxe::renderer {
  public:
    explicit async_capture_test_renderer(screenshot_test_window& w) : win_(w) {}

    void begin_frame(const fxe::math::vec3& = {}, const fxe::math::vec3& = {},
                     const fxe::math::mat4x4& = fxe::math::identity()) override {}
    void end_frame() override {
      if (state_ == state::pending)
        state_ = state::ready;
    }
    bool queue_dev(const fxe::command_buffer&, const fxe::vshader_cbuf&,
                   const fxe::render_config&) override {
      return true;
    }
    fxe::renderer::capture_result capture_frame() override {
      ++capture_calls_;
      fxe::renderer::capture_result r;
      if (state_ == state::idle) {
        state_ = state::pending;
        win_.post_redraw();
        r.error = "capture armed; retry after the next render";
        return r;
      }
      if (state_ == state::pending) {
        r.error = "capture in progress; retry shortly";
        return r;
      }
      r.ok = true;
      r.width = 2;
      r.height = 1;
      r.rgba = known_rgba_;
      return r;
    }
    fxe::window& get_window() override {
      return win_;
    }
    const fxe::window& get_window() const override {
      return win_;
    }
    int capture_calls() const {
      return capture_calls_;
    }
    const std::vector<::u8>& known_rgba() const {
      return known_rgba_;
    }

  private:
    enum class state { idle, pending, ready };
    screenshot_test_window& win_;
    state state_ = state::idle;
    int capture_calls_ = 0;
    // Known-color bytes after BGRA->RGBA conversion: red-ish then blue-ish.
    std::vector<::u8> known_rgba_{0x11, 0x22, 0x33, 0xff, 0xaa, 0xbb, 0xcc, 0x80};
  };

  bool png_has_ihdr_size(const std::vector<::u8>& bytes, unsigned w, unsigned h) {
    const unsigned char sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes.size() < 24 || std::memcmp(bytes.data(), sig, sizeof(sig)) != 0)
      return false;
    auto be32 = [&](std::size_t off) {
      return (unsigned(bytes[off]) << 24u) | (unsigned(bytes[off + 1]) << 16u) |
             (unsigned(bytes[off + 2]) << 8u) | unsigned(bytes[off + 3]);
    };
    return be32(16) == w && be32(20) == h;
  }

  void test_base64() {
    using namespace fxe::debug;
    CHECK(base64::encode("") == "");
    CHECK(base64::encode("f") == "Zg==");
    CHECK(base64::encode("fo") == "Zm8=");
    CHECK(base64::encode("foo") == "Zm9v");
    CHECK(base64::encode("foob") == "Zm9vYg==");
    CHECK(base64::encode("fooba") == "Zm9vYmE=");
    CHECK(base64::encode("foobar") == "Zm9vYmFy");

    auto out = base64::decode("");
    CHECK(out.has_value());
    CHECK(out->empty());

    out = base64::decode("Zm9vYmFy");
    CHECK(out.has_value());
    CHECK(out->size() == 6);
    CHECK(std::memcmp(out->data(), "foobar", 6) == 0);

    out = base64::decode("Zm8=");
    CHECK(out.has_value());
    CHECK(out->size() == 2);
    CHECK(std::memcmp(out->data(), "fo", 2) == 0);

    CHECK(!base64::decode("Z"));    // length not multiple of 4
    CHECK(!base64::decode("Z!==")); // bad char
  }

  void test_method_table() {
    using namespace fxe::debug;
    CHECK(method_exists("System.handshake"));
    CHECK(method_exists("Page.screenshot"));
    CHECK(method_exists("Runtime.evaluate"));
    CHECK(!method_exists("Bogus.method"));
    CHECK(method_exists("Runtime.fireHmr"));
    CHECK(method_exists("Schema.getDomains"));
    CHECK(method_exists("Reconciler.snapshot"));
    CHECK(method_exists("Runtime.enable"));
    CHECK(method_exists("Debugger.enable"));
    CHECK(method_exists("Profiler.enable"));
    CHECK(method_exists("Profiler.disable"));
    CHECK(method_exists("Profiler.start"));
    CHECK(method_exists("Profiler.stop"));
    CHECK(method_exists("HeapProfiler.enable"));
    CHECK(method_exists("HeapProfiler.disable"));
    CHECK(method_exists("HeapProfiler.takeHeapSnapshot"));
    CHECK(method_exists("HeapProfiler.collectGarbage"));
  }

  void test_handshake_dispatch() {
    using namespace fxe::debug;
    dispatch_context cx{};
    auto out = dispatch(cx, "System.handshake", json{json::object()});
    CHECK(out.is_object());
    CHECK(out.at("engine").get<std::string>() == "fxe");
    const auto& caps = out.at("capabilities");
    CHECK(caps.is_array());
    CHECK(caps.size() >= 10);
  }

  void test_cdp_schema_dispatch() {
    using namespace fxe::debug;
    dispatch_context cx{};
    auto out = dispatch(cx, "Schema.getDomains", json{json::object()});
    CHECK(out.is_object());
    const auto& domains = out.at("domains");
    CHECK(domains.is_array());
    CHECK(domains.size() >= 3);
    CHECK(schema_domain_exists(domains, "Schema"));
    CHECK(schema_domain_exists(domains, "Runtime"));
    CHECK(schema_domain_exists(domains, "Debugger"));
    CHECK(schema_domain_exists(domains, "Reconciler"));
  }

  void test_page_screenshot_async_retry_dispatch() {
    using namespace fxe::debug;
    screenshot_test_window win{2, 1};
    async_capture_test_renderer rdr{win};
    dispatch_context cx{};
    cx.win = &win;
    cx.rdr = &rdr;

    bool first_threw = false;
    try {
      (void)dispatch(cx, "Page.screenshot", json{json::object()});
    } catch (const dispatch_error& e) {
      first_threw = true;
      CHECK(static_cast<int>(e.code) == -32001);
      CHECK(e.message == "capture armed; retry after the next render");
    }
    CHECK(first_threw);
    CHECK(win.take_redraw_request());

    auto fb = dispatch(cx, "Page.framebufferSize", json{json::object()});
    CHECK(fb.at("width").get<double>() == 2.0);
    CHECK(fb.at("height").get<double>() == 1.0);

    bool pending_threw = false;
    try {
      (void)dispatch(cx, "Page.screenshot", json{json::object()});
    } catch (const dispatch_error& e) {
      pending_threw = true;
      CHECK(static_cast<int>(e.code) == -32001);
      CHECK(e.message == "capture in progress; retry shortly");
    }
    CHECK(pending_threw);

    rdr.end_frame();
    auto out = dispatch(cx, "Page.screenshot", json{json::object()});
    CHECK(out.at("format").get<std::string>() == "png");
    CHECK(out.at("width").get<double>() == 2.0);
    CHECK(out.at("height").get<double>() == 1.0);
    CHECK(out.at("byteSize").get<double>() > 0.0);
    auto png = base64::decode(out.at("dataBase64").get<std::string>());
    CHECK(png.has_value());
    if (png)
      CHECK(png_has_ihdr_size(*png, 2, 1));

    auto cap = rdr.capture_frame();
    CHECK(cap.ok);
    CHECK(cap.rgba == rdr.known_rgba());
    CHECK(rdr.capture_calls() >= 4);
  }

  void test_cdp_enable_dispatch() {
    using namespace fxe::debug;
    dispatch_context cx{};
    auto runtime = dispatch(cx, "Runtime.enable", json{json::object()});
    CHECK(runtime.is_object());
    CHECK(runtime.empty());
    auto debugger = dispatch(cx, "Debugger.enable", json{json::object()});
    CHECK(debugger.is_object());
    CHECK(debugger.empty());

    bool profiler_threw = false;
    try {
      (void)dispatch(cx, "Profiler.enable", json{json::object()});
    } catch (const dispatch_error& e) {
      profiler_threw = true;
      CHECK(static_cast<int>(e.code) == -32002);
      CHECK(e.message == "Profiler unavailable: V8 host not attached");
      CHECK(e.data == "profiler_unavailable");
    }
    CHECK(profiler_threw);

    bool heap_profiler_threw = false;
    try {
      (void)dispatch(cx, "HeapProfiler.enable", json{json::object()});
    } catch (const dispatch_error& e) {
      heap_profiler_threw = true;
      CHECK(static_cast<int>(e.code) == -32002);
      CHECK(e.message == "HeapProfiler unavailable: V8 host not attached");
      CHECK(e.data == "heap_profiler_unavailable");
    }
    CHECK(heap_profiler_threw);
  }

  void test_cdp_profiler_dispatch() {
    using namespace fxe::debug;
    dispatch_context cx{};

    bool start_threw = false;
    try {
      (void)dispatch(cx, "Profiler.start", json{json::object()});
    } catch (const dispatch_error& e) {
      start_threw = true;
      CHECK(static_cast<int>(e.code) == -32002);
      CHECK(e.message == "Profiler unavailable: V8 host not attached");
      CHECK(e.data == "profiler_unavailable");
    }
    CHECK(start_threw);

    bool stop_threw = false;
    try {
      (void)dispatch(cx, "Profiler.stop", json{json::object()});
    } catch (const dispatch_error& e) {
      stop_threw = true;
      CHECK(static_cast<int>(e.code) == -32002);
      CHECK(e.message == "Profiler unavailable: V8 host not attached");
      CHECK(e.data == "profiler_unavailable");
    }
    CHECK(stop_threw);
  }

  void test_runtime_unavailable_dispatch() {
    using namespace fxe::debug;
    dispatch_context cx{};

    const char* methods[] = {"Runtime.evaluate",       "Runtime.getGlobals",
                             "Runtime.fireHmr",        "Runtime.invalidateModule",
                             "Runtime.reimportModule", "Reconciler.snapshot"};
    for (const char* method : methods) {
      bool threw = false;
      try {
        (void)dispatch(cx, method, json{json::object()});
      } catch (const dispatch_error& e) {
        threw = true;
        CHECK(static_cast<int>(e.code) == -32002);
        CHECK(e.message == "Runtime unavailable: V8 host not attached");
        CHECK(e.data == "runtime_unavailable");
      }
      CHECK(threw);
    }
  }

  void test_performance_timeline_unavailable() {
    using namespace fxe::debug;
    dispatch_context cx{};

    auto out = dispatch(cx, "Performance.timeline", json{json::object()});
    CHECK(out.is_object());
    CHECK(out.at("marks").is_object());
    CHECK(out.at("render").is_object());
    CHECK(out.at("available").is_boolean());
    CHECK(!out.at("available").get<bool>());
  }

  void test_cdp_metadata_serialization() {
    using namespace fxe::debug;
    auto target = make_cdp_target_descriptor("127.0.0.1", 9229);
    CHECK(target.at("id").get<std::string>() == "fxe-main");
    CHECK(target.at("type").get<std::string>() == "node");
    CHECK(target.at("title").get<std::string>() == "fxe application");
    CHECK(!target.at("webSocketDebuggerUrl").is_null());
    const auto ws_url = target.at("webSocketDebuggerUrl").get<std::string>();
    CHECK(ws_url.find("ws://") == 0);
    CHECK(ws_url == "ws://127.0.0.1:9229/devtools/page/fxe-main");
    CHECK(target.at("protocol").get<std::string>() == "cdp-websocket");

    auto list_response = make_cdp_discovery_http_response("/json/list", "127.0.0.1", 9229);
    CHECK(list_response.has_value());
    CHECK(list_response->find("HTTP/1.1 200 OK") == 0);
    CHECK(list_response->find(
              "\"webSocketDebuggerUrl\":\"ws://127.0.0.1:9229/devtools/page/fxe-main\"") !=
          std::string::npos);
    auto version_response = make_cdp_discovery_http_response("/json/version", "127.0.0.1", 9229);
    CHECK(version_response.has_value());
    CHECK(version_response->find("\"Protocol-Version\":\"1.3\"") != std::string::npos);
    CHECK(!make_cdp_discovery_http_response("/not-cdp", "127.0.0.1", 9229).has_value());
  }

  void test_method_not_found() {
    using namespace fxe::debug;
    dispatch_context cx{};
    bool threw = false;
    try {
      (void)dispatch(cx, "Nope.nope", json{json::object()});
    } catch (const dispatch_error& e) {
      threw = true;
      CHECK(static_cast<int>(e.code) == -32601);
    }
    CHECK(threw);
  }

#ifdef FXE_HAS_V8
#if defined(_WIN32)
  using socket_t = SOCKET;
  constexpr socket_t k_invalid_socket = INVALID_SOCKET;
  void close_socket(socket_t s) {
    closesocket(s);
  }
#else
  using socket_t = int;
  constexpr socket_t k_invalid_socket = -1;
  void close_socket(socket_t s) {
    ::close(s);
  }
#endif

  void init_sockets() {
#if defined(_WIN32)
    static bool once = [] {
      WSADATA wsa{};
      return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)once;
#endif
  }

  socket_t connect_loopback(unsigned port) {
    init_sockets();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == k_invalid_socket)
      return k_invalid_socket;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close_socket(s);
      return k_invalid_socket;
    }
    return s;
  }

  void set_recv_timeout(socket_t s, int milliseconds) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
#else
    timeval tv{};
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  }

  bool send_all(socket_t s, std::string_view data) {
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
#if defined(_WIN32)
      int n = ::send(s, p, static_cast<int>(left), 0);
#else
      ssize_t n = ::send(s, p, left, 0);
#endif
      if (n <= 0)
        return false;
      p += n;
      left -= static_cast<std::size_t>(n);
    }
    return true;
  }

  bool recv_json_line(socket_t s, std::string& buf, fxe::debug::json& out) {
    for (;;) {
      auto nl = buf.find('\n');
      if (nl != std::string::npos) {
        auto line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (line.empty())
          continue;
        out = fxe::debug::json::parse(line);
        return true;
      }
      char chunk[4096];
#if defined(_WIN32)
      int n = ::recv(s, chunk, sizeof(chunk), 0);
#else
      ssize_t n = ::recv(s, chunk, sizeof(chunk), 0);
#endif
      if (n <= 0)
        return false;
      buf.append(chunk, static_cast<std::size_t>(n));
    }
  }

  bool pump_until_json(fxe::debug::server& srv, socket_t s, std::string& buf,
                       fxe::debug::json& out) {
    for (int i = 0; i < 250; ++i) {
      srv.pump_tasks();
      if (recv_json_line(s, buf, out))
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  bool send_request(socket_t s, int id, std::string_view method, std::string_view params = "{}") {
    std::string line = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + std::string(method) +
                       "\",\"params\":" + std::string(params) + "}\n";
    return send_all(s, line);
  }

  bool pump_until_id(fxe::debug::server& srv, socket_t s, std::string& buf, int id,
                     fxe::debug::json& out) {
    for (int i = 0; i < 250; ++i) {
      srv.pump_tasks();
      while (recv_json_line(s, buf, out)) {
        if (out.contains("id") && out.at("id").get<int>() == id)
          return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  bool recv_until_method(socket_t s, std::string& buf, std::string_view method,
                         fxe::debug::json& out) {
    for (int i = 0; i < 250; ++i) {
      while (recv_json_line(s, buf, out)) {
        if (out.contains("method") && out.at("method").is_string() &&
            out.at("method").get<std::string>() == method)
          return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
  }

  // Minimal in-test window stub: GLFW is on in dev, so we cannot reuse the
  // FXE_HAS_GLFW=0 stub_window. Implement only what the dispatch handlers
  // touch (framebuffer_size, post_redraw, drain, close, should_close).
  class test_window final : public fxe::window {
  public:
    test_window(unsigned w, unsigned h) : size_{w, h} {}
    void poll() override {}
    void wait_events() override {}
    void wait_events_timeout(double) override {}
    void post_redraw() override {
      dirty_.store(true, std::memory_order_release);
    }
    bool take_redraw_request() override {
      return dirty_.exchange(false, std::memory_order_acq_rel);
    }
    void close() override {
      closed_ = true;
    }
    bool should_close() const override {
      return closed_;
    }
    fxe::math::uvec2 framebuffer_size() const override {
      return size_;
    }
    void set_vsync(bool) override {}
    void* native_handle() const override {
      return nullptr;
    }

  private:
    fxe::math::uvec2 size_{};
    bool closed_ = false;
    std::atomic<bool> dirty_{false};
  };

  void test_page_windows_empty_registry(fxe::js::host& host) {
    using namespace fxe::debug;
    dispatch_context cx{};
    cx.host = &host;
    auto out = dispatch(cx, "Page.windows", json{json::object()});
    CHECK(out.is_object());
    const auto& arr = out.at("windows");
    CHECK(arr.is_array());
    CHECK(arr.empty());
  }

  void test_page_windows_two_registered(fxe::js::host& host) {
    using namespace fxe::debug;
    test_window w0{640, 480};
    test_window w1{800, 600};
    host.register_window(&w0);
    host.register_window(&w1);
    dispatch_context cx{};
    cx.host = &host;

    auto out = dispatch(cx, "Page.windows", json{json::object()});
    const auto& arr = out.at("windows");
    CHECK(arr.is_array());
    CHECK(arr.size() == 2);
    CHECK(arr.at(0).at("id").get<double>() == 0.0);
    CHECK(arr.at(0).at("width").get<double>() == 640.0);
    CHECK(arr.at(1).at("id").get<double>() == 1.0);
    CHECK(arr.at(1).at("height").get<double>() == 600.0);

    // Default windowId resolves to window 0 via active_window().
    auto fb0 = dispatch(cx, "Page.framebufferSize", json{json::object()});
    CHECK(fb0.at("width").get<double>() == 640.0);

    // Explicit windowId routes through window_at(idx).
    json p1{json::object()};
    p1["windowId"] = 1.0;
    auto fb1 = dispatch(cx, "Page.framebufferSize", p1);
    CHECK(fb1.at("width").get<double>() == 800.0);

    host.unregister_window(&w0);
    host.unregister_window(&w1);
  }

  void test_page_window_not_found(fxe::js::host& host) {
    using namespace fxe::debug;
    test_window w0{640, 480};
    host.register_window(&w0);
    dispatch_context cx{};
    cx.host = &host;
    json p{json::object()};
    p["windowId"] = 99.0;
    bool threw = false;
    try {
      (void)dispatch(cx, "Page.framebufferSize", p);
    } catch (const dispatch_error& e) {
      threw = true;
      CHECK(e.data == "window_not_found");
    }
    CHECK(threw);
    host.unregister_window(&w0);

    p["windowId"] = 9007199254740992.0;
    threw = false;
    try {
      (void)dispatch(cx, "Page.framebufferSize", p);
    } catch (const dispatch_error& e) {
      threw = true;
      CHECK(static_cast<int>(e.code) == -32602);
      CHECK(e.message == "windowId is outside the lossless JSON integer range");
    }
    CHECK(threw);
  }

  void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
  }

  bool snapshot_has_component(const fxe::debug::json& tree, const char* display_name) {
    for (const auto& node : tree) {
      if (!node.is_object())
        continue;
      if (node.contains("type") && node.at("type").is_string() &&
          node.at("type").get<std::string>() == "component" && node.contains("displayName") &&
          node.at("displayName").is_string() &&
          node.at("displayName").get<std::string>() == display_name) {
        return true;
      }
      if (node.contains("children") && node.at("children").is_array() &&
          snapshot_has_component(node.at("children"), display_name))
        return true;
    }
    return false;
  }

  void test_reconciler_snapshot_dispatch(fxe::js::host& host) {
    using namespace fxe::debug;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "fxe-reconciler-debug-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto entry = dir / "entry.ts";

    write_text(entry, "import { CommandBuffer } from 'fxe';\n"
                      "import { render, Text, View } from 'fxe-ui';\n"
                      "const cb = new CommandBuffer();\n"
                      "render(View({ style: { width: 160, height: 40 }, children: Text({ children: "
                      "'hello' }) }), cb);\n");

    auto loaded = host.run_module_file(entry);
    CHECK(loaded.ok);

    dispatch_context cx{};
    cx.host = &host;
    auto out = dispatch(cx, "Reconciler.snapshot", json{json::object()});
    CHECK(out.is_object());
    const auto& tree = out.at("tree");
    CHECK(tree.is_array());
    CHECK(!tree.empty());
    CHECK(snapshot_has_component(tree, "View"));
    CHECK(snapshot_has_component(tree, "Text"));
    for (const auto& node : tree) {
      CHECK(node.contains("id"));
      CHECK(node.contains("type"));
      CHECK(node.contains("displayName"));
      CHECK(node.contains("propsSummary"));
      CHECK(node.contains("lastRebuildFrame"));
      CHECK(node.contains("deps"));
      CHECK(node.contains("cacheHitMiss"));
    }

    fs::remove_all(dir);
  }
  void test_hmr_fire_reloads_cached_module(fxe::js::host& host) {
    using namespace fxe::debug;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "fxe-hmr-debug-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto dep = dir / "dep.ts";
    auto entry = dir / "entry.ts";

    write_text(dep, "globalThis.__hmrReloads = globalThis.__hmrReloads || [];\n"
                    "globalThis.__hmrReloads.push('v1');\n"
                    "export const value = 1;\n");
    write_text(entry, "import './dep.ts';\n");

    auto first = host.run_module_file(entry);
    CHECK(first.ok);
    auto before = host.debug_evaluate("globalThis.__hmrReloads.join(',')");
    CHECK(before.exception.empty());
    CHECK(before.json_value == "\"v1\"");

    write_text(dep, "globalThis.__hmrReloads = globalThis.__hmrReloads || [];\n"
                    "globalThis.__hmrReloads.push('v2');\n"
                    "export const value = 2;\n");

    dispatch_context cx{};
    cx.host = &host;
    json params{json::object()};
    params["path"] = dep.string();
    auto out = dispatch(cx, "Runtime.fireHmr", params);
    CHECK(out.at("handlersCalled").get<double>() == 0.0);

    auto after = host.debug_evaluate("globalThis.__hmrReloads.join(',')");
    CHECK(after.exception.empty());
    CHECK(after.json_value == "\"v1,v2\"");

    fs::remove_all(dir);
  }

  void test_cpu_profiler_dispatch(fxe::js::host& host) {
    using namespace fxe::debug;
    dispatch_context cx{};
    cx.host = &host;

    auto schema = dispatch(cx, "Schema.getDomains", json{json::object()});
    CHECK(schema_domain_exists(schema.at("domains"), "Profiler"));
    CHECK(dispatch(cx, "Profiler.enable", json{json::object()}).empty());
    CHECK(dispatch(cx, "Profiler.start", json{json::object()}).empty());

    auto busy =
        host.debug_evaluate("(() => { const end = Date.now() + 200; let x = 0; "
                            "while (Date.now() < end) { x += Math.sqrt(x + 1); } return x; })()",
                            true);
    CHECK(busy.exception.empty());

    auto stopped = dispatch(cx, "Profiler.stop", json{json::object()});
    CHECK(stopped.is_object());
    CHECK(stopped.contains("profile"));
    const auto& profile = stopped.at("profile");
    CHECK(profile.contains("nodes"));
    CHECK(profile.at("nodes").is_array());
    CHECK(profile.contains("samples"));
    CHECK(profile.at("samples").is_array());
    CHECK(!profile.at("samples").empty());
    CHECK(profile.contains("timeDeltas"));
    CHECK(profile.at("timeDeltas").is_array());
    CHECK(dispatch(cx, "Profiler.disable", json{json::object()}).empty());
  }

  void test_heap_profiler_snapshot_stream(fxe::js::host& host) {
    using namespace fxe::debug;

    auto seeded =
        host.run_script("globalThis.__heapProfilerMarker = { value: 42 };", "<heap-profiler-test>");
    CHECK(seeded.ok);

    dispatch_context direct_cx{};
    direct_cx.host = &host;
    auto schema = dispatch(direct_cx, "Schema.getDomains", json{json::object()});
    CHECK(schema_domain_exists(schema.at("domains"), "HeapProfiler"));

    server_options opts;
    opts.port = 0;
    opts.host = "127.0.0.1";
    server srv(opts);
    srv.attach_host(&host);
    if (!srv.start()) {
      CHECK(false);
      return;
    }

    socket_t s = connect_loopback(srv.bound_port());
    if (s == k_invalid_socket) {
      CHECK(false);
      srv.stop();
      return;
    }
    set_recv_timeout(s, 20);

    std::string recv_buf;
    json frame{json::object()};
    CHECK(send_all(s, "{\"id\":1,\"method\":\"HeapProfiler.enable\",\"params\":{}}\n"));
    CHECK(pump_until_json(srv, s, recv_buf, frame));
    CHECK(frame.at("id").get<int>() == 1);
    CHECK(frame.at("result").is_object());

    CHECK(send_all(s, "{\"id\":2,\"method\":\"HeapProfiler.takeHeapSnapshot\",\"params\":{}}\n"));
    std::string snapshot;
    bool got_reply = false;
    int chunk_count = 0;
    for (int i = 0; i < 500 && !got_reply; ++i) {
      srv.pump_tasks();
      while (recv_json_line(s, recv_buf, frame)) {
        if (frame.contains("method") &&
            frame.at("method").get<std::string>() == "HeapProfiler.addHeapSnapshotChunk") {
          const auto& params = frame.at("params");
          CHECK(params.contains("chunk"));
          CHECK(params.at("chunk").is_string());
          snapshot += params.at("chunk").get<std::string>();
          ++chunk_count;
        } else if (frame.contains("method") && frame.at("method").get<std::string>() ==
                                                   "HeapProfiler.reportHeapSnapshotProgress") {
          const auto& params = frame.at("params");
          CHECK(params.contains("finished"));
          CHECK(params.at("finished").get<bool>());
        } else if (frame.contains("id") && frame.at("id").get<int>() == 2) {
          CHECK(frame.at("result").is_object());
          got_reply = true;
          break;
        }
      }
      if (!got_reply)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    CHECK(got_reply);
    CHECK(chunk_count > 0);
    CHECK(!snapshot.empty());
    auto parsed = json::parse(snapshot);
    CHECK(parsed.is_object());
    CHECK(parsed.contains("snapshot"));
    CHECK(parsed.at("snapshot").is_object());

    close_socket(s);
    srv.stop();
  }

  void test_ndjson_multiclient_routing_and_events(fxe::js::host& host) {
    using namespace fxe::debug;
    server_options opts;
    opts.port = 0;
    opts.host = "127.0.0.1";
    server srv(opts);
    srv.attach_host(&host);
    if (!srv.start()) {
      CHECK(false);
      return;
    }

    socket_t a = connect_loopback(srv.bound_port());
    socket_t b = connect_loopback(srv.bound_port());
    if (a == k_invalid_socket || b == k_invalid_socket) {
      CHECK(false);
      if (a != k_invalid_socket)
        close_socket(a);
      if (b != k_invalid_socket)
        close_socket(b);
      srv.stop();
      return;
    }
    set_recv_timeout(a, 20);
    set_recv_timeout(b, 20);

    std::string bufa, bufb;
    json fa{json::object()};
    json fb{json::object()};
    CHECK(send_request(a, 101, "Runtime.evaluate", "{\"expression\":\"41+1\"}"));
    CHECK(send_request(b, 202, "Runtime.evaluate", "{\"expression\":\"20+22\"}"));
    CHECK(pump_until_id(srv, a, bufa, 101, fa));
    CHECK(pump_until_id(srv, b, bufb, 202, fb));
    CHECK(fa.at("id").get<int>() == 101);
    CHECK(fb.at("id").get<int>() == 202);
    CHECK(fa.at("result").at("result").at("value").get<int>() == 42);
    CHECK(fb.at("result").at("result").at("value").get<int>() == 42);

    CHECK(send_request(a, 103, "Console.enable"));
    CHECK(send_request(b, 204, "Console.enable"));
    CHECK(pump_until_id(srv, a, bufa, 103, fa));
    CHECK(pump_until_id(srv, b, bufb, 204, fb));
    srv.emit_console("log", "both");
    CHECK(recv_until_method(a, bufa, "Console.messageAdded", fa));
    CHECK(recv_until_method(b, bufb, "Console.messageAdded", fb));
    CHECK(fa.at("params").at("text").get<std::string>() == "both");
    CHECK(fb.at("params").at("text").get<std::string>() == "both");

    close_socket(a);
    for (int i = 0; i < 20; ++i) {
      srv.pump_tasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    srv.emit_console("log", "survivor");
    CHECK(recv_until_method(b, bufb, "Console.messageAdded", fb));
    CHECK(fb.at("params").at("text").get<std::string>() == "survivor");

    close_socket(b);
    srv.stop();
  }

  void test_ndjson_session_cap_and_gap_reuse(fxe::js::host& host) {
    using namespace fxe::debug;
    server_options opts;
    opts.port = 0;
    opts.host = "127.0.0.1";
    server srv(opts);
    srv.attach_host(&host);
    if (!srv.start()) {
      CHECK(false);
      return;
    }

    std::vector<socket_t> clients;
    std::vector<std::string> bufs(8);
    clients.reserve(8);
    for (int i = 0; i < 8; ++i) {
      socket_t s = connect_loopback(srv.bound_port());
      CHECK(s != k_invalid_socket);
      if (s != k_invalid_socket) {
        set_recv_timeout(s, 20);
        clients.push_back(s);
        CHECK(send_request(s, i + 1, "System.handshake"));
      }
    }
    for (int i = 0; i < static_cast<int>(clients.size()); ++i) {
      json frame{json::object()};
      CHECK(pump_until_id(srv, clients[static_cast<std::size_t>(i)],
                          bufs[static_cast<std::size_t>(i)], i + 1, frame));
    }

    socket_t overflow = connect_loopback(srv.bound_port());
    CHECK(overflow != k_invalid_socket);
    if (overflow != k_invalid_socket) {
      set_recv_timeout(overflow, 2000);
      std::string buf;
      json frame{json::object()};
      bool got_overflow = recv_json_line(overflow, buf, frame);
      CHECK(got_overflow);
      if (got_overflow) {
        CHECK(frame.at("error").at("code").get<int>() == -32003);
        CHECK(frame.at("error").at("message").get<std::string>() == "server full");
      }
      close_socket(overflow);
    }

    if (clients.size() == 8) {
      close_socket(clients.front());
      for (int i = 0; i < 50; ++i) {
        srv.pump_tasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      socket_t replacement = connect_loopback(srv.bound_port());
      CHECK(replacement != k_invalid_socket);
      if (replacement != k_invalid_socket) {
        set_recv_timeout(replacement, 20);
        std::string buf;
        json frame{json::object()};
        CHECK(send_request(replacement, 77, "System.handshake"));
        CHECK(pump_until_id(srv, replacement, buf, 77, frame));
        CHECK(frame.at("id").get<int>() == 77);
        close_socket(replacement);
      }
    }

    for (std::size_t i = clients.size() == 8 ? 1u : 0u; i < clients.size(); ++i)
      close_socket(clients[i]);
    srv.stop();
  }
#endif
} // namespace

int main([[maybe_unused]] int argc, char** argv) {
#ifdef FXE_HAS_V8
  fxe::js::initialize(argv[0]);
#else
  (void)argc;
  (void)argv;
#endif
  test_base64();
  test_method_table();
  test_handshake_dispatch();
  test_method_not_found();
  test_cdp_schema_dispatch();
  test_page_screenshot_async_retry_dispatch();
  test_cdp_enable_dispatch();
  test_cdp_profiler_dispatch();
  test_runtime_unavailable_dispatch();
  test_performance_timeline_unavailable();
  test_cdp_metadata_serialization();
#ifdef FXE_HAS_V8
  {
    fxe::js::host host;
    test_page_windows_empty_registry(host);
    test_page_windows_two_registered(host);
    test_page_window_not_found(host);
    test_cpu_profiler_dispatch(host);
    test_heap_profiler_snapshot_stream(host);
    test_hmr_fire_reloads_cached_module(host);
    test_reconciler_snapshot_dispatch(host);
    test_ndjson_multiclient_routing_and_events(host);
    test_ndjson_session_cap_and_gap_reuse(host);
  }
#endif

#ifdef FXE_HAS_V8
  fxe::js::shutdown();
#endif

  std::fprintf(stderr, "debug_protocol_tests: %d passed, %d failed\n", g_pass, g_fail);
  std::fflush(stderr);
#ifdef FXE_HAS_V8
  // Skip static destructors — V8's thread_local FunctionTemplate persistents
  // hang at process exit on macOS once an isolate has been disposed.
  std::_Exit(g_fail == 0 ? 0 : 1);
#else
  return g_fail == 0 ? 0 : 1;
#endif
}
