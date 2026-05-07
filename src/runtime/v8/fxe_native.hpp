#pragma once

#include <v8.h>

namespace fxe::runtime {

  // Installs the non-enumerable globalThis.__fxe_native Phase 0 namespace object.
  void install_fxe_native(v8::Isolate* iso, v8::Local<v8::Context> ctx);

  struct worker_bootstrap {
    int thread_id = 0;
    const char* worker_data_json = "null";
    void* native_handle = nullptr;
  };

  class worker_bootstrap_scope {
  public:
    explicit worker_bootstrap_scope(const worker_bootstrap& bootstrap) noexcept;
    ~worker_bootstrap_scope() noexcept;
    worker_bootstrap_scope(const worker_bootstrap_scope&) = delete;
    worker_bootstrap_scope& operator=(const worker_bootstrap_scope&) = delete;

  private:
    const worker_bootstrap* previous_ = nullptr;
  };

  const worker_bootstrap* current_worker_bootstrap() noexcept;

} // namespace fxe::runtime
