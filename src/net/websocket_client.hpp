// Minimal RFC 6455 WebSocket client used by bind_websocket.
//
// One worker thread per socket owns the blocking TCP reads/writes. When libuv
// is available the client also registers a non-blocking runtime pump hook so
// queued transport notifications advance on the same app loop that dispatches
// V8 callbacks; without libuv the worker/pump fallback remains unchanged.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fxe/types.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace fxe::net {

#if !defined(FXE_HAS_NATIVE_TLS_HTTP2_DEPS)
#define FXE_HAS_NATIVE_TLS_HTTP2_DEPS 0
#endif

#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
  class tls_client;
#endif

  enum class ws_event_kind {
    open,
    message_text,
    message_binary,
    error_,
    close,
  };

  struct ws_event {
    ws_event_kind kind;
    std::string text;       // text for message_text, error reason for error_
    std::vector<u8> binary; // for message_binary
    u16 code = 0;           // close code
    std::string reason;     // close reason / error message
    bool was_clean = false; // close cleanliness
  };

  enum class ws_ready_state : int {
    connecting = 0,
    open = 1,
    closing = 2,
    closed = 3,
  };

  struct ws_client_options {
    usize max_message_bytes = 64ull * 1024ull * 1024ull;
    u32 idle_timeout_ms = 0;
    u32 pong_timeout_ms = 30000;
    usize max_fragment_bytes = 1024ull * 1024ull;
    bool compress = true;
  };

  class websocket_client {
  public:
    explicit websocket_client(ws_client_options options = {});
    ~websocket_client();

    // Begin connecting. Returns false synchronously for trivially malformed
    // urls (empty, non-ws scheme). The handshake proceeds on the worker
    // thread; success/failure is reported as ws_event::open / ws_event::error_.
    bool connect(std::string url, std::vector<std::string> protocols);

    void send_text(std::string s, usize max_fragment_bytes = 0);
    void send_binary(std::vector<u8> data, usize max_fragment_bytes = 0);
    void close(u16 code, std::string reason);

    // Move events out for processing on the V8 thread. Cheap: short critical
    // section, swap the inner queue.
    std::vector<ws_event> drain_events();

    ws_ready_state ready_state() const {
      return state_.load();
    }
    usize buffered_amount() const {
      return buffered_.load();
    }
    const std::string& selected_protocol() const {
      return selected_protocol_;
    }
    const std::string& url() const {
      return url_;
    }
    const std::string& negotiated_extensions() const {
      return negotiated_extensions_;
    }
    u16 close_code() const;
    std::string close_reason() const;

    websocket_client(const websocket_client&) = delete;
    websocket_client& operator=(const websocket_client&) = delete;

  private:
    enum class out_op {
      text,
      binary,
      close,
    };
    struct out_msg {
      out_op op;
      std::vector<u8> bytes;
      u16 code = 0;
      usize max_fragment_bytes = 0;
    };

    void worker_main();
    bool do_handshake();
    bool transport_send_all(const u8* data, usize n);
    bool transport_recv_n(u8* buf, usize n);
    bool transport_recv_http_headers(std::string& out);
    bool send_message(u8 opcode, const u8* data, usize n, usize max_fragment_bytes,
                      bool compressed = false);
    bool send_frame(u8 opcode, const u8* data, usize n, bool fin = true, bool rsv1 = false);
    bool send_close_frame(u16 code, const std::string& reason);
    void record_close(u16 code, std::string reason);
    void emit_local_close(u16 code, std::string reason);
    void push_event(ws_event ev);
    void close_socket_now(bool shutdown_first);
    void runtime_pump() noexcept;

    struct ws_deflate_state;
    bool negotiate_permessage_deflate(const std::string& headers);
    bool deflate_message(const u8* data, usize n, std::vector<u8>& out);
    bool inflate_message(const std::vector<u8>& data, std::vector<u8>& out, bool& too_big);
    std::string url_;
    ws_client_options options_;
    std::vector<std::string> protocols_;
    std::string selected_protocol_;
    std::string handshake_error_;
    std::string negotiated_extensions_;

    std::atomic<ws_ready_state> state_{ws_ready_state::connecting};
    std::atomic<usize> buffered_{0};
    std::atomic<bool> stop_{false};
    std::atomic<i64> last_frame_ms_{0};
    std::atomic<bool> awaiting_pong_{false};
    std::atomic<i64> ping_sent_ms_{0};

    std::atomic<int> sock_{-1};
#if FXE_HAS_NATIVE_TLS_HTTP2_DEPS
    std::unique_ptr<tls_client> tls_;
#endif

    std::thread worker_;

    std::mutex out_mu_;
    std::condition_variable out_cv_;
    std::queue<out_msg> out_q_;
    std::mutex send_mu_;

    mutable std::mutex close_mu_;
    u16 close_code_ = 0;
    std::string close_reason_;

    std::mutex in_mu_;
    std::vector<ws_event> in_q_;
    usize uv_pump_callback_id_ = 0;
    std::unique_ptr<ws_deflate_state> deflate_;
  };

} // namespace fxe::net
