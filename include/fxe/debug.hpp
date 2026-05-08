// Public façade for the fxe debug protocol server.
//
// fxe::debug::server runs an NDJSON-over-TCP listener on a worker thread and
// dispatches JSON-RPC-flavoured requests against the V8 host, the window, and
// the renderer. All V8/GPU work happens on the render thread via the task
// pump (see attach_host / pump_tasks).
//
// Lifecycle:
//   1. construct with desired host/port/options.
//   2. attach_host(...) / attach_window(...) / attach_renderer(...) before start().
//   3. start() — opens the listening socket, spawns the accept thread.
//   4. on the render thread: pump_tasks() once per frame.
//   5. stop() (or destructor) — closes connections and joins.

#pragma once

#include <fxe/types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
namespace fxe {
  class window;
  class renderer;
} // namespace fxe

namespace fxe::js {
  class host;
}

namespace fxe::debug {
  using json = nlohmann::ordered_json;

  struct server_options {
    // 0 -> OS-assigned port. Bound port is reported via bound_port() and printed
    // to stdout as `FXE_DEBUG_PORT=<n>` after start() succeeds.
    u16 port = 0;
    std::string host = "127.0.0.1";
    // When true, a fresh connection is accepted in a paused state — pump_tasks
    // will not invoke the user callback until Debugger.resume arrives.
    bool start_paused = false;
    // Keep the process / pump alive after the script finishes so a debugger can
    // continue to introspect & screenshot.
    bool keepalive = false;
    // Non-zero -> verbose protocol logging on stderr.
    int log_level = 0;
    // Conservative cap on simultaneously attached debug clients.
    u16 max_clients = 8;
  };

  class server {
  public:
    explicit server(server_options opts);
    ~server();
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    // Wire-up (must precede start()).
    void attach_host(js::host* h) noexcept;
    void attach_window(window* w) noexcept;
    void attach_renderer(renderer* r) noexcept;
    // Optional: invoked from the server thread when an unrecognised request
    // demands a wakeup of the render loop. Defaults to a no-op.
    void set_wake_callback(std::function<void()> wake) noexcept;

    // Open the listening socket. Returns false on bind failure (errno-style
    // error available via last_error()).
    bool start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] u16 bound_port() const noexcept;
    [[nodiscard]] std::string last_error() const;

    // Drain queued debug tasks on the render thread. Each task is a closure
    // produced by the dispatcher; it has access to host/window/renderer and is
    // free to touch V8 state. Safe to call any time after start().
    void pump_tasks();

    // True while the loop should remain blocked. Used by win.run() to gate the
    // user callback. Calling thread: render thread.
    [[nodiscard]] bool is_paused() const noexcept;

    // Internal helpers used by the dispatcher; not part of the stable API.
    void _internal_set_pause(bool paused, bool single_step) noexcept;
    void _internal_set_console_enabled(bool on) noexcept;
    void _internal_set_session_console_enabled(u64 id, bool on) noexcept;
    void _internal_set_session_channel_enabled(u64 id, int channel, bool on) noexcept;

    // Enqueue a Console.messageAdded event. Safe from the render thread.
    void emit_console(std::string_view level, std::string_view text);

    // Enqueue an arbitrary protocol event (pushed to the connected client).
    void emit_event(std::string_view method, json params);

    struct impl;

  private:
    std::unique_ptr<impl> p_;
  };
} // namespace fxe::debug
