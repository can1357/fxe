#pragma once
#include <fxe/types.hpp>

#include <cstdint>
#include <v8.h>

namespace fxe {
  struct command_buffer;
  class renderer;
  class window;
} // namespace fxe

namespace fxe::js {
  // Per-isolate template-holder lifecycle hooks. Bindings register a
  // callback in their TU; the host invokes every registered callback in
  // ~host() right before isolate disposal so v8::Global<FunctionTemplate>
  // holders are Reset() while the isolate is still alive.
  using template_reset_fn = void (*)(v8::Isolate*);
  void register_template_resetter(template_reset_fn fn);
  void run_template_resetters(v8::Isolate*);

  class host;
  // Type tags stored alongside the C++ pointer in object internal field 1.
  // They allow runtime type checks at unwrap time.
  inline constexpr u32 TAG_COMMAND_BUFFER = 0x434D4442u; // 'CMDB'
  inline constexpr u32 TAG_RENDERER = 0x52454E44u;       // 'REND'
  inline constexpr u32 TAG_WINDOW = 0x57494E44u;         // 'WIND'

  // Each install_*_template registers a constructor (or namespace) on the supplied
  // isolate-global template. They MUST be idempotent per isolate.
  void install_command_buffer_template(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
  void install_renderer_template(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
  void install_window_template(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);
  void install_primitives_namespace(v8::Isolate*, v8::Local<v8::ObjectTemplate> global);

  // Wrap an externally-owned native into the JS class registered above. Used by
  // host::install_*_global to surface engine-owned resources to scripts.
  v8::Local<v8::Object> make_command_buffer_object(v8::Isolate*, v8::Local<v8::Context>,
                                                   command_buffer*);
  v8::Local<v8::Object> make_renderer_object(v8::Isolate*, v8::Local<v8::Context>, renderer*);
  v8::Local<v8::Object> make_window_object(v8::Isolate*, v8::Local<v8::Context>, window*);

  // Wrap a raw C++ pointer as a v8::Object instantiated from `tpl`'s instance
  // template. Internal field 0 holds the External(pointer), field 1 the tag.
  v8::Local<v8::Object> wrap(v8::Isolate*, v8::Local<v8::Context>, v8::Local<v8::FunctionTemplate>,
                             void* native, u32 type_tag);
  // Recover a C++ pointer from a wrapped object. Returns nullptr if the tag does
  // not match or the object lacks two internal fields.
  void* unwrap(v8::Local<v8::Object>, u32 expected_type_tag);

  // Debug protocol task pump (defined in v8_host.cpp). Bindings call these to
  // honor pending requests / pause boundaries during script execution.
  void pump_debug_for_isolate(v8::Isolate* iso);
  bool is_paused_for_isolate(v8::Isolate* iso);

  // Registers Runtime.* handlers with fxe_debug. Called automatically from
  // host::host(); also safe to call directly to force the link-anchor.
  void install_runtime_dispatch_handlers();

  // Multi-window registry helpers. JS bindings call these from the
  // constructor and the GC finaliser; the renderer variant takes the owning
  // window so the host can answer renderer_for(window*) queries.
  void register_window_for_isolate(v8::Isolate* iso, fxe::window* w);
  void unregister_window_for_isolate(v8::Isolate* iso, fxe::window* w);
  void register_renderer_for_isolate(v8::Isolate* iso, fxe::window* owner, fxe::renderer* r);
  void unregister_renderer_for_isolate(v8::Isolate* iso, fxe::renderer* r);
  fxe::window* get_active_window_for_isolate(v8::Isolate* iso);
  fxe::renderer* get_active_renderer_for_isolate(v8::Isolate* iso);
  // Walks back from the V8 isolate slot to the owning host. nullptr if no
  // host owns the isolate (only happens during teardown / synthetic isolates).
  host* host_for_isolate(v8::Isolate* iso);
} // namespace fxe::js
