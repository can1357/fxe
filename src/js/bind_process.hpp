#pragma once
#include <string>
#include <v8.h>
#include <vector>

namespace fxe::js {
  // Set host CLI argv before context creation so `process.argv` reflects it.
  // Safe to call multiple times; the most recent value wins.
  void set_host_argv(std::vector<std::string> argv);
  // Installs the `process` global: argv, env (Proxy-like), cwd/chdir,
  // platform/arch/pid, exit/kill/umask/hrtime, versions/release,
  // stdio.write, on/off, nextTick.
  void install_process_global(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);

  // Invoke every JS listener registered via `process.on(event, fn)` for the
  // given event. Listeners are called in registration order with `process` as
  // `this`. Returns the number of listeners that were actually invoked
  // (non-empty, not previously removed). The caller must hold an active
  // HandleScope and a current Context.
  int emit_process_event(v8::Isolate* iso, v8::Local<v8::Context> ctx, std::string_view event,
                         int argc, v8::Local<v8::Value> argv[]);

  // Drop every per-isolate listener registered via `process.on`. Call when
  // the owning isolate is being torn down so we don't keep dangling Globals
  // (and so a subsequent host reusing the same `Isolate*` slot starts clean).
  void clear_process_listeners(v8::Isolate* iso);
} // namespace fxe::js
