# Debugging FXE Apps with the Python SDK

When you (the agent) need to verify behavior of a JS/TS example or a user project — e.g. "does my new primitive draw correctly?", "did this script actually call `endFrame`?", "what does `console.log` print at frame 30?" — drive `fxe_run` through the Python SDK rather than reading source and guessing. The SDK is Puppeteer-style: launch the app, attach, evaluate, screenshot, inject input.

**Prerequisite:** `bun run build` (default `dev` preset includes V8 + Dawn).

## Quick recipes

Always launch with `pause=True` so you can install setup hooks before the render loop starts; call `page.resume()` once setup is done. Wrap the page in a `try/finally` so `page.close()` always runs — the SDK doesn't yet support `async with`.

```python
# clients/python/examples/screenshot.py — golden-image diff a script
import asyncio, sys
sys.path.insert(0, "clients/python")  # only when running outside the package
from fxe_debug import launch

async def main():
    page = await launch("examples/js/hello.ts", pause=True)
    try:
        await page.resume()
        await asyncio.sleep(0.3)        # let the script paint at least once
        await page.screenshot("hello.png")
        w, h = await page.framebuffer_size()
        print(f"captured {w}x{h}")
    finally:
        await page.close()

asyncio.run(main())
```

## What you can do without modifying the script

- `await page.evaluate("expr")` — run any expression in the live V8 isolate. The result is JSON-serialised. Use this to read script-side state without adding `console.log` lines.
- `await page.globals()` — list every own property on the global; cheap way to discover what the script exposes.
- `await page.screenshot(path)` — RGBA8 PNG of the most recent frame, decoded from base64. **First call after launch arms capture and returns an error ("retry after the next render"); while readback is still pending it may return "capture in progress; retry shortly". The SDK does not auto-retry — sleep for a frame and call again.** A small helper:
  ```python
  async def shot(page, path, retries=3):
      from fxe_debug import ProtocolError
      for _ in range(retries):
          try:
              return await page.screenshot(path)
          except ProtocolError as e:
              if e.code == -32001: await asyncio.sleep(0.05); continue
              raise
      raise RuntimeError("no frame captured after retries")
  ```
- `page.console_messages` — async iterator of `Console.messageAdded` events (`level`, `text`, `ts`). Lazily calls `Console.enable` on first iteration. Use to assert `console.log("loaded fonts")` actually fires.
- `await page.mouse.click(x, y)` / `mouse.move` / `mouse.wheel` — synthesised GLFW input. Hits the script's `window.on("mouse_button"|"cursor_pos", ...)` handlers exactly as a real OS event would.
- `await page.keyboard.press("Enter")` / `keyboard.type("hello")` — same semantics for keys and char input. Named keys: `Enter`, `Escape`, `Tab`, `Backspace`, `ArrowLeft/Right/Up/Down`, `Space`.
- `await page.pause()` / `resume()` / `step()` — gate the render loop. `step` resumes for exactly one frame then re-pauses, useful for deterministic capture of frame N.
- `await page.close()` — sends `Window.close` and tears the child down.

## Investigation patterns

**"Does the script set `window.foo` correctly after init?"**
```python
page = await launch("user.ts")
try:
    await asyncio.sleep(0.1)
    print(await page.evaluate("JSON.stringify(window.foo)"))
finally:
    await page.close()
```

**"Reproduce a click bug"**
```python
page = await launch("user.ts")
try:
    msgs = []
    async def collect():
        async for m in page.console_messages: msgs.append(m)
    asyncio.create_task(collect())
    await page.mouse.click(120, 80)
    await asyncio.sleep(0.3)
    for m in msgs: print(m.level, m.text)
finally:
    await page.close()
```

**"Capture frame N for golden comparison"**
```python
page = await launch("user.ts", pause=True)
try:
    for i in range(N): await page.step()
    await page.screenshot(f"frame_{N}.png")
finally:
    await page.close()
```

**Driving an unknown user project (no source read first)**
```python
page = await launch("/path/to/user/script.ts")
try:
    print("globals:", await page.globals())
    print("size:", await page.framebuffer_size())
    print("eval:", await page.evaluate("Object.keys(globalThis)"))
finally:
    await page.close()
```

## CLI alternative (one-shot, no Python)

```bash
bun run js --dev hello --debug=9333 --debug-pause   # spawn paused on port 9333
bun run pycli inspect --port 9333    # handshake + globals + framebuffer
bun run pycli screenshot --port 9333 --out shot.png
bun run pycli eval --port 9333 'window.foo'
bun run pycli mouse click --port 9333 100 100
bun run pycli console --port 9333    # tail console.* messages until Ctrl-C
bun run pycli resume --port 9333
```

## Live function tracing (no source edits)

When debugging "what arguments did this helper see while the bug reproduced?", reach for `page.trace_install(target, capture)` instead of adding `console.log` and rebuilding. The wrapper, ring buffer, and drain helpers live entirely in the running V8 isolate (under `globalThis.__fxeTrace`); the SDK just ferries small JS snippets through `Runtime.evaluate`.

- `target` is a dotted path resolved against `globalThis`. Use `Foo.prototype.bar` for instance methods.
- `capture` is a JS *expression* with `args`, `self`, `result`, `error`, `phase` in scope. Whatever it returns is JSON-serialised and pushed onto the (bounded) ring buffer. Default capture is `args`.
- `phases` selects the call phases that record a sample (`"call"`, `"return"`, `"throw"`). Default is `("call",)`.

```python
tid = await page.trace_install(
    "Primitives.fillRect",
    "{x: args[1], y: args[2], w: args[3], h: args[4]}",
    limit=50,
)
await asyncio.sleep(0.5)              # let the bug reproduce
for sample in await page.trace_drain(tid): print(sample)
await page.trace_uninstall(tid)
```

Trace handles survive across runtime changes; always `trace_uninstall` when done so the wrapper doesn't outlive your investigation.

Caveat: V8 may inline calls through static namespaces (e.g. `Primitives.fillRect`) after a function is hot, in which case replacing the property won't intercept already-optimised callsites. Prototype methods (`Renderer.prototype.beginFrame`, `View.prototype.paint`) and freshly-installed traces against cold paths intercept reliably; if you're tracing a hot static helper, also `trace_install` its caller (a prototype method or component render) where dispatch is virtual.

## Layout tracing (fxe-ui)

For layout-specific bugs ("why is this rect at x=-250?"), `page.trace_install` is the wrong tool — it depends on V8 dispatch staying uninlined, and you'd have to instrument every layout-aware component. Instead, fxe-ui has a built-in `recordLayout` sink that every layout primitive (`View`, eventually `Text`, `Button`, …) pushes to when tracing is enabled. Defining-side cost is one boolean check per render, so it's safe to leave wired in.

```python
await page.layout_trace_enable(limit=200)
await page.evaluate("App.windows()[0].setSize(W+1, H+1)")  # invalidate caches
await asyncio.sleep(0.2)
await page.layout_trace_disable()
for entry in await page.layout_trace_drain():
    r = entry["rect"]
    print(f"{entry['component']:5s} sw={entry['styleWidth']!r} -> "
          f"{r['x']},{r['y']} {r['width']}x{r['height']}")
```

Each entry carries `{component, rect, hasParentLayout, styleWidth, styleHeight, tag?}`. Layer caching means re-renders skip clean subtrees; bumping the window size by 1px (or any deps-invalidating change) is the easiest way to force a full layout pass into the buffer.

## Memo trace (fxe-ui reconciler)

When a `memo()`-wrapped component is rebuilding more than expected ("why did `Sidebar` re-render when nothing visible changed?"), `page.memo_trace_*` surfaces the exact bail decision the reconciler made each render. Every memoised component is bucketed by displayName into one of:

- `hit` — bail succeeded (cache reused)
- `dirty` — fiber explicitly marked dirty (setState in a child, etc.)
- `noCache` — first render or cache discarded
- `noLastProps` — first render with this fiber identity
- `epoch` — atlas repacked under the cache; forced rebuild for correctness
- `propsDiff` — `areEqual(prev, next)` returned false

The first observed `propsDiff` per component also captures `{last, next, lastKeys, nextKeys}` so you can see exactly which prop changed. Cost when disabled: one nullable-load + branch per memoised component per render.

```python
await page.memo_trace_enable()
await asyncio.sleep(1.0)              # let the suspect frames go by
snap = await page.memo_trace_snapshot()
await page.memo_trace_disable()

for name, slot in sorted(snap["byName"].items(), key=lambda kv: -kv[1]["propsDiff"]):
    if slot["propsDiff"] == 0: continue
    print(f"{name:20s} total={slot['total']:5d} hit={slot['hit']:5d} "
          f"propsDiff={slot['propsDiff']:5d} dirty={slot['dirty']:5d}")
    dump = snap["propsDump"].get(name)
    if dump:
        added = set(dump["nextKeys"]) - set(dump["lastKeys"])
        removed = set(dump["lastKeys"]) - set(dump["nextKeys"])
        print(f"  +keys={sorted(added)} -keys={sorted(removed)}")
```

`memo_trace_reset()` zeros the counters without disabling, useful for "snap before / snap after a single user gesture". Calling on an app that hasn't imported `fxe-ui` raises (the devtools module installs the global on import); that's intentional — memo tracing only makes sense for UI apps.

## When NOT to use the SDK

- Modifying the script under test — just edit + re-run `bun run js …`.
- Pure C++ unit tests — use `fxe_core_tests` / `fxe_debug_tests`.
- Type errors — `bun run typecheck` is faster than launching V8.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `LaunchError: FXE_DEBUG_PORT not detected` | binary missing or build stale | `bun run build` |
| `ProtocolError(-32002, "V8 host not attached")` | called Runtime.* before the host loaded | wait for handshake / `await asyncio.sleep(0.05)` |
| `ProtocolError(-32001, "capture armed; retry after the next render")` or `"capture in progress; retry shortly"` | screenshot capture is not ready yet | retry after a short sleep (see helper above) |
| `ProtocolError(-32002, "window not attached")` / `"renderer not attached"` | the script hasn't run `new Window`/`new Renderer` yet | the SDK should `await page.resume()` if launched paused; otherwise wait |
| Hangs on close | child blocked in `win.run` | the SDK's `__aexit__` sends `Window.close`; if your script ignores close, also call `await page.evaluate("window.close()")` |
| Screenshot all transparent | the renderer hasn't rendered yet — first arming returns nothing | second call after the next `endFrame` will succeed |
