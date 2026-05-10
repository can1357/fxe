# Frame contract

This document is the single source of truth for "where does 16.6 ms go?"
in an FXE app. It pins the frame loop's vsync source, present-mode
semantics, threading model, microtask checkpoints, and the JS-visible
phase markers exposed via `globalThis.__fxeFrameProfile`.

If a behaviour described here disagrees with the code, the **code** is
wrong; file a bug and fix the implementation, not this doc.

## Pipeline

A single frame proceeds through these phases, in order:

```
input → animations → reconcile → frameCallbacks → layout → paint → submit → present-wait
└───────────── JS (frame_loop.ts::tickFrame) ─────────┘   └───── C++ (renderer_dawn.cpp::end_frame) ─────┘
```

`tickFrame(dtMs)` is the canonical JS entry point. It lives in
`packages/fxe-ui/src/reconciler/frame_loop.ts` and contains nothing else
worth mentioning — the file should fit on one screen because it answers
the question "what does the JS thread do every frame?".

| Phase            | Owner            | Where                                                   |
| ---------------- | ---------------- | ------------------------------------------------------- |
| `input`          | C++ window thread (libuv → main) | `glfwPollEvents`, IME, drop-target, gestures (macOS NSEvent / Win32 WM_POINTER) |
| `animations`     | JS main          | `tickAnimatedFrames()` (`animated/timing.ts`) — Animated.timing/spring drivers |
| `reconcile`      | JS main          | `tickSchedulerFrame()` (`reconciler/scheduler.ts`) — drains transition + sync lanes |
| `frameCallbacks` | JS main          | `useFrame()` callbacks + `g_frame_callbacks`            |
| `layout`         | C++ main         | Yoga-style solver (`fxe_layout`) — fed by reconciler outputs |
| `paint`          | C++ main         | View painter walks the layer tree, emits opcodes into the shared `CommandBuffer` |
| `submit`         | C++ main (GPU)   | Dawn encoder + `Queue.Submit` (`renderer_dawn.cpp::end_frame`) |
| `present-wait`   | GPU              | `wgpuSurfacePresent` blocks per the active vsync mode   |

## Vsync source of truth

Vsync intent is set per `Window` via `Window.setVsync(mode)` and read
back in C++ at `Renderer.endFrame()` (the `set_vsync(want_vsync_)` call
inside `end_frame` re-asserts it after `GetCurrentTexture` because Dawn
may reset it on macOS).

| Mode        | Dawn `PresentMode` | Behaviour                                              |
| ----------- | ------------------ | ------------------------------------------------------ |
| `'auto'`    | Fifo               | Default. Frame is paced to display refresh rate. Best for steady UI; never tears. |
| `'fifo'`    | Fifo               | Same as `auto`.                                        |
| `'immediate'` | Immediate        | No pacing; queue submits as fast as possible. May tear. Use only for benchmarking. |
| `'mailbox'` | Mailbox            | Triple-buffered, no tearing, no waiting. Use for input-bound UIs (editors, drawing) where latency matters more than power. |

On macOS the `CAMetalLayer.displaySyncEnabled` flag is the actual hardware
gate; FXE re-asserts it from `set_vsync(want_vsync_)` after every Dawn
surface acquisition because Dawn's Metal backend resets it during FIFO
texture acquisition.

The animation scheduler does **not** read vsync — it ticks at whatever rate
`requestAnimationFrame` fires at, which the GLFW polling thread paces to
the display refresh.

## Threading

| Thread                     | Owns                                                                           |
| -------------------------- | ------------------------------------------------------------------------------ |
| Main                       | V8 isolate, GPU command encoding, GLFW window/input pump, libuv loop           |
| Debug accept thread        | `accept(2)` on the debug TCP socket                                            |
| Debug session thread (one per connection) | Reads NDJSON / WebSocket frames, posts commands onto the main-thread render-queue |
| Net workers                | HTTP (libcurl), HTTP/2 (nghttp2), WebSocket, native TLS (mbedTLS); each owns its own thread |
| FS watcher                 | One thread per platform: inotify (Linux) / FSEvents (macOS) / `ReadDirectoryChangesW` (Windows) |
| Audio                      | miniaudio internal threads                                                     |

Cross-thread results land back on the main thread via either the libuv
loop or the render-queue task pump that drains between frames.

The JS frame loop assumes `tickFrame` runs on the V8-pinned main thread.
Calling it from any other thread is a bug.

## Microtask checkpoints

`v8::Isolate::PerformMicrotaskCheckpoint()` is invoked at three points:

1. After every `uv_run(UV_RUN_NOWAIT)` tick in `src/runtime/uv_loop.cpp` —
   so promise resolution and `queueMicrotask` callbacks are flushed
   between libuv ticks.
2. After every `onFrame` JS callback in `src/js/bind_window.cpp:2224`.
3. After every window-event JS callback in `src/js/bind_window.cpp:792`.

Microtasks queued from inside `tickFrame()` therefore observe the rest of
the same frame; microtasks queued from a `setTimeout` callback observe
the *next* uv tick within the same frame budget.

Never call `PerformMicrotaskCheckpoint` from a worker thread.

## Frame cancellation

A queued frame is cancelled when:

- `Window.close()` runs (the `glfw_window` destructor releases the GLFW
  handle; subsequent `redraw_handler` invocations are no-ops).
- `setRenderTarget(null)` is called (`reconciler/render_target.ts`) and no
  active animation re-arms it.
- The frame loop's `dispose()` returned by `startFrameLoop()` is invoked.

Animated drivers re-arm the loop automatically by setting
`g_render_target` and calling `__fxeUiEnsureFrameLoop()` from
`reconciler/fiber.ts`. The render-target's `redraw_handler` (installed by
`bind_window.cpp:2137-2141`) keeps frames flowing during live-resize and
window-events even when no JS animation is running.

## JS-visible profiling: `globalThis.__fxeFrameProfile`

`packages/fxe-ui/src/reconciler/frame_profile.ts` installs the API:

```ts
type FramePhases = {
  js: number;             // total tickFrame body
  animations: number;     // tickAnimatedFrames
  reconcile: number;      // tickSchedulerFrame
  frameCallbacks: number; // useFrame + g_frame_callbacks
};
type FrameSample = {
  frameId: number;
  startMs: number;
  totalMs: number;        // === phases.js
  dtMs: number;           // gap from previous frame
  phases: FramePhases;
};
type FrameProfileApi = {
  enable(opts?: { ringSize?: number }): void;
  disable(): void;
  isEnabled(): boolean;
  drain(): FrameSample[];   // returns and clears
  snapshot(): FrameSample[]; // returns without clearing
};
declare const __fxeFrameProfile: FrameProfileApi;
```

When disabled, `frameProfileBeginFrame` returns null and per-phase wrappers
are O(1) no-ops (one boolean check). When enabled, each phase pays exactly
two `performance.now()` calls. The ring buffer is bounded (default 240
samples ≈ 4 s at 60 Hz); overflow drops the oldest sample.

The C++-side phases (`layout`, `paint`, `submit`, `present-wait`) are NOT
yet captured into `FramePhases`; they are observable through
`RenderStats.snapshot()` instead. Extending `FrameSample` with a `gpuMs`
field is tracked under T1 of `TODO.md`.

## Devtools / SDK consumers

- **Devtools panel** — `packages/fxe-devtools/src/panels/Frame.tsx` polls
  `snapshot()` every ~250 ms and renders a stacked-bar timeline.
- **Python SDK** — `clients/python/fxe_debug/page.py` exposes
  `Page.frame_trace_enable()` / `frame_trace_disable()` /
  `frame_trace_drain()` / `frame_trace_snapshot()` /
  `frame_trace_record(duration_ms, max_frame_ms=...)`. The last form is
  the regression-gate primitive: it asserts no frame exceeded the budget
  during the captured window.

## Invariants

- `tickFrame()` must not throw. Exceptions inside `frameCallbacks` are
  caught and logged via `console.error`; nothing else is wrapped.
- Phase order in `tickFrame()` is **reconcile → animations →
  frameCallbacks**. Reconcile runs first so that scheduled state
  transitions land before the same frame's animation tick reads them.
- The render thread MUST NOT touch any V8 handle. All cross-thread
  signalling goes through `glfwPostEmptyEvent` + main-thread task pump.
- The frame budget at 60 Hz is 16.6 ms total wall clock from `Frame.start`
  to `Frame.end`. Anything in the JS phases above 6 ms is suspect —
  reconcile + animations + frameCallbacks combined should leave the C++
  pipeline at least 8 ms.
