#pragma once
#include <v8.h>

namespace fxe::js {
  // Installs setTimeout/setInterval/clearTimeout/clearInterval/queueMicrotask/
  // setImmediate/requestAnimationFrame/cancelAnimationFrame as free globals.
  void install_timers_global(v8::Isolate* iso, v8::Local<v8::ObjectTemplate> global);

  // Returns seconds-from-now until the next pending timer fires. Returns
  // +infinity if no timers are scheduled. Caller MAY clamp to a minimum
  // wait when computing `glfwWaitEventsTimeout`.
  double next_timer_deadline_seconds(v8::Isolate* iso);

  // Invokes any timer whose deadline has passed. Microtasks are pumped after
  // each callback so chained promises resolve before the next timer fires.
  void drain_due_timers(v8::Isolate* iso);

  // Invokes the requestAnimationFrame queue once. Callbacks queued during
  // dispatch are deferred to the next call (FIFO).
  void drain_animation_frames(v8::Isolate* iso);

  // Posts an empty event to the GLFW main loop so a sleeping
  // glfwWaitEventsTimeout returns and the host can re-evaluate the timer
  // schedule. Safe to call from the JS thread.
  void wake_event_loop();
} // namespace fxe::js
