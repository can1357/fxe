"""Live function-call tracing over the debug protocol.

Installs a thin JS wrapper around a target function and records a captured
sample on every call. The wrapper, registry, and drain helpers all live in
the running V8 isolate under ``globalThis.__fxeTrace``; this module just
ferries small JS snippets through ``Runtime.evaluate``.

Why it exists
-------------
Debugging a layout/render glitch usually means "what arguments did this
helper see, in order, while the bug reproduced?". Editing source to add
``console.log`` requires a rebuild + relaunch and pollutes the tree. The
``trace`` primitive lets an agent answer the same question without touching
disk: install a tracer, exercise the bug, drain the samples.

Capture expression
------------------
The ``capture`` argument is a JS *expression* (not a statement) evaluated
on every call with these names in scope:

- ``args``    — ``Array`` of call arguments (always present)
- ``self``    — the receiver (``this`` at call time)
- ``result``  — the function's return value (``undefined`` on entry/throw)
- ``error``   — the thrown value (``undefined`` unless the call threw)
- ``phase``   — ``"call"`` or ``"return"`` or ``"throw"``

Whatever the expression evaluates to is JSON-serialised and pushed onto the
ring buffer. Default capture is ``args``.

Example
-------
.. code-block:: python

    tid = await page.trace_install("Primitives.fillRect", "args.slice(1)")
    await asyncio.sleep(0.5)
    samples = await page.trace_drain(tid)
    print(samples[:3])
    await page.trace_uninstall(tid)
"""

from __future__ import annotations

import json
from typing import Any, Literal

# JS bootstrap: defines the tracing primitives once per isolate. Idempotent —
# subsequent installs hit the cached registry.
_BOOTSTRAP = r"""
(() => {
  if (globalThis.__fxeTrace) return;
  const traces = new Map();
  let nextId = 1;

  const resolveTarget = (path) => {
    const parts = path.split('.');
    let owner = globalThis;
    for (let i = 0; i < parts.length - 1; ++i) {
      if (owner == null) throw new Error(`trace: cannot resolve '${path}': '${parts.slice(0, i + 1).join('.')}' is null`);
      owner = owner[parts[i]];
    }
    if (owner == null) {
      throw new Error(`trace: cannot resolve '${path}': owner is null`);
    }
    const key = parts[parts.length - 1];
    if (typeof owner[key] !== 'function') {
      throw new Error(`trace: '${path}' is not a function (got ${typeof owner[key]})`);
    }
    return { owner, key };
  };

  const install = (path, captureSrc, opts) => {
    const { owner, key } = resolveTarget(path);
    const captureFn = new Function(
      'args', 'self', 'result', 'error', 'phase',
      `return (${captureSrc});`
    );
    const limit = (opts && opts.limit) || 200;
    const phases = (opts && opts.phases) || ['call'];
    const original = owner[key];
    const samples = [];
    const record = (entry) => {
      if (samples.length >= limit) samples.shift();
      samples.push(entry);
    };
    const wrapper = function (...args) {
      let captured;
      if (phases.includes('call')) {
        try { captured = captureFn(args, this, undefined, undefined, 'call'); }
        catch (e) { captured = { __traceError: String(e && e.message || e) }; }
        record(captured);
      }
      let result;
      try {
        result = original.apply(this, args);
      } catch (err) {
        if (phases.includes('throw')) {
          try { captured = captureFn(args, this, undefined, err, 'throw'); }
          catch (e) { captured = { __traceError: String(e && e.message || e) }; }
          record(captured);
        }
        throw err;
      }
      if (phases.includes('return')) {
        try { captured = captureFn(args, this, result, undefined, 'return'); }
        catch (e) { captured = { __traceError: String(e && e.message || e) }; }
        record(captured);
      }
      return result;
    };
    Object.defineProperty(wrapper, 'name', { value: original.name + '$traced' });
    owner[key] = wrapper;
    const id = `t${nextId++}`;
    traces.set(id, { id, path, owner, key, original, samples, limit, phases });
    return id;
  };

  const drain = (id, clear) => {
    const t = traces.get(id);
    if (!t) throw new Error(`trace: unknown id '${id}'`);
    const out = t.samples.slice();
    if (clear !== false) t.samples.length = 0;
    return out;
  };

  const uninstall = (id) => {
    const t = traces.get(id);
    if (!t) return false;
    if (t.owner[t.key] !== undefined) t.owner[t.key] = t.original;
    traces.delete(id);
    return true;
  };

  const list = () => Array.from(traces.values()).map((t) => ({
    id: t.id, path: t.path, count: t.samples.length, limit: t.limit, phases: t.phases,
  }));

  globalThis.__fxeTrace = { install, drain, uninstall, list };
})();
"""


Phase = Literal["call", "return", "throw"]


def _js_string(s: str) -> str:
    return json.dumps(s)


def _js_value(v: Any) -> str:
    return json.dumps(v)


async def trace_install(
    page: Any,
    target: str,
    capture: str = "args",
    *,
    limit: int = 200,
    phases: tuple[Phase, ...] = ("call",),
) -> str:
    """Install a tracer on ``target`` and return its id.

    ``target`` is a dotted path resolved against ``globalThis``; use
    ``Foo.prototype.bar`` to wrap an instance method. ``capture`` is a JS
    expression (see module docstring). ``phases`` selects which call phases
    record a sample.
    """
    expr = (
        _BOOTSTRAP + f";globalThis.__fxeTrace.install({_js_string(target)},"
        f"{_js_string(capture)},"
        f"{{limit:{int(limit)},phases:{_js_value(list(phases))}}})"
    )
    tid = await page.evaluate(expr)
    if not isinstance(tid, str):
        raise RuntimeError(f"trace_install: unexpected response: {tid!r}")
    return tid


async def trace_drain(page: Any, trace_id: str, *, clear: bool = True) -> list[Any]:
    """Drain accumulated samples. Pass ``clear=False`` to peek without resetting."""
    expr = _BOOTSTRAP + f";globalThis.__fxeTrace.drain({_js_string(trace_id)},{_js_value(clear)})"
    out = await page.evaluate(expr)
    if not isinstance(out, list):
        raise RuntimeError(f"trace_drain: unexpected response: {out!r}")
    return out


async def trace_uninstall(page: Any, trace_id: str) -> bool:
    """Restore the original function. Returns False if the id was unknown."""
    expr = _BOOTSTRAP + f";globalThis.__fxeTrace.uninstall({_js_string(trace_id)})"
    return bool(await page.evaluate(expr))


async def trace_list(page: Any) -> list[dict[str, Any]]:
    """List currently-installed tracers with their sample counts."""
    expr = _BOOTSTRAP + ";globalThis.__fxeTrace.list()"
    out = await page.evaluate(expr)
    if not isinstance(out, list):
        raise RuntimeError(f"trace_list: unexpected response: {out!r}")
    return out


# ---------------------------------------------------------------------------
# Layout tracing — built into fxe-ui as a first-class debug primitive.
# ---------------------------------------------------------------------------
#
# Unlike `trace_install`, this doesn't depend on V8 property dispatch staying
# uninlined: every layout-aware component (View, Text, …) calls into the
# fxe-ui `recordLayout` sink directly, so a tracing flag flip is enough to
# capture the whole render. No source edits, no rebuild.
#
# Wire format mirrors `LayoutTraceEntry` in
# packages/fxe-ui/src/debug/layout_trace.ts:
#     {component, rect, hasParentLayout, styleWidth, styleHeight, tag?}

# The framework lazily creates `globalThis.__fxeLayoutTrace` on first call
# to `recordLayout`. The bootstrap below tolerates the pre-init case so
# `enable()` can be called immediately after `resume()` without races.
_LAYOUT_BOOTSTRAP = r"""
(() => {
  if (!globalThis.__fxeLayoutTrace) {
    globalThis.__fxeLayoutTrace = { enabled: false, buffer: [], limit: 1000 };
  }
})();
"""


async def layout_trace_enable(page: Any, *, limit: int = 1000) -> None:
    """Start recording layout decisions. Pass before exercising the bug."""
    expr = (
        _LAYOUT_BOOTSTRAP + f"globalThis.__fxeLayoutTrace.enabled = true;"
        f"globalThis.__fxeLayoutTrace.limit = {int(limit)};"
        f"globalThis.__fxeLayoutTrace.buffer.length = 0;"
        f"true"
    )
    await page.evaluate(expr)


async def layout_trace_disable(page: Any) -> None:
    """Stop recording. Buffer is preserved for one final drain."""
    await page.evaluate(_LAYOUT_BOOTSTRAP + "globalThis.__fxeLayoutTrace.enabled = false; true")


async def layout_trace_drain(page: Any, *, clear: bool = True) -> list[dict[str, Any]]:
    """Read accumulated layout entries (and clear by default)."""
    expr = (
        _LAYOUT_BOOTSTRAP
        + "(() => {"
        + "  const s = globalThis.__fxeLayoutTrace;"
        + "  const out = s.buffer.slice();"
        + f" if ({_js_value(clear)}) s.buffer.length = 0;"
        + "  return out;"
        + "})()"
    )
    out = await page.evaluate(expr)
    if not isinstance(out, list):
        raise RuntimeError(f"layout_trace_drain: unexpected response: {out!r}")
    return out
