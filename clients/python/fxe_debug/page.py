"""Puppeteer-style Page facade over a Client."""

from __future__ import annotations

import asyncio
import base64
from enum import Enum
from pathlib import Path
from typing import Any, AsyncIterator

from .client import Client
from .protocol import ConsoleMessage, Handshake


class MouseButton(str, Enum):
    LEFT = "left"
    RIGHT = "right"
    MIDDLE = "middle"


class Key(str, Enum):
    ENTER = "Enter"
    ESCAPE = "Escape"
    TAB = "Tab"
    BACKSPACE = "Backspace"
    SPACE = "Space"
    ARROW_LEFT = "ArrowLeft"
    ARROW_RIGHT = "ArrowRight"
    ARROW_UP = "ArrowUp"
    ARROW_DOWN = "ArrowDown"


_NAMED_KEYS = {k.value for k in Key}


def _normalize_key(key: str | Key) -> str:
    if isinstance(key, Key):
        return key.value
    return key


def _key_codepoint(key: str) -> int | None:
    if key in _NAMED_KEYS:
        return None
    if len(key) == 1:
        return ord(key)
    return None


class Mouse:
    def __init__(self, page: "Page") -> None:
        self._page = page

    async def move(self, x: float, y: float) -> None:
        await self._page._client.call("Input.dispatchMouseEvent", {"type": "move", "x": x, "y": y})

    async def down(self, x: float, y: float, button: str | MouseButton = "left") -> None:
        await self._page._client.call(
            "Input.dispatchMouseEvent",
            {
                "type": "down",
                "x": x,
                "y": y,
                "button": str(button.value if isinstance(button, MouseButton) else button),
            },
        )

    async def up(self, x: float, y: float, button: str | MouseButton = "left") -> None:
        await self._page._client.call(
            "Input.dispatchMouseEvent",
            {
                "type": "up",
                "x": x,
                "y": y,
                "button": str(button.value if isinstance(button, MouseButton) else button),
            },
        )

    async def click(self, x: float, y: float, button: str | MouseButton = "left") -> None:
        await self.down(x, y, button)
        await self.up(x, y, button)

    async def wheel(self, dx: float, dy: float, x: float = 0.0, y: float = 0.0) -> None:
        await self._page._client.call(
            "Input.dispatchMouseEvent",
            {"type": "wheel", "x": x, "y": y, "dx": dx, "dy": dy},
        )


class Keyboard:
    def __init__(self, page: "Page") -> None:
        self._page = page

    async def down(self, key: str | Key) -> None:
        k = _normalize_key(key)
        params: dict[str, Any] = {"type": "down", "key": k}
        cp = _key_codepoint(k)
        if cp is not None:
            params["codepoint"] = cp
        await self._page._client.call("Input.dispatchKeyEvent", params)

    async def up(self, key: str | Key) -> None:
        k = _normalize_key(key)
        params: dict[str, Any] = {"type": "up", "key": k}
        cp = _key_codepoint(k)
        if cp is not None:
            params["codepoint"] = cp
        await self._page._client.call("Input.dispatchKeyEvent", params)

    async def press(self, key: str | Key) -> None:
        await self.down(key)
        await self.up(key)

    async def type(self, text: str) -> None:
        for ch in text:
            await self._page._client.call(
                "Input.dispatchKeyEvent",
                {"type": "char", "key": ch, "codepoint": ord(ch)},
            )


class Page:
    def __init__(self, client: Client, handshake: Handshake | None = None) -> None:
        self._client = client
        self._handshake = handshake
        self.mouse = Mouse(self)
        self.keyboard = Keyboard(self)
        self._console_queue: asyncio.Queue[ConsoleMessage] | None = None
        self._console_enabled = False
        self._paused_event = asyncio.Event()
        self._resumed_event = asyncio.Event()
        self._client.on("Debugger.paused", lambda _p: self._on_paused())
        self._client.on("Debugger.resumed", lambda _p: self._on_resumed())

    # ---- lifecycle ----------------------------------------------------------
    @property
    def client(self) -> Client:
        return self._client

    @property
    def handshake(self) -> Handshake | None:
        return self._handshake

    async def __aenter__(self) -> "Page":
        return self

    async def __aexit__(self, *exc: Any) -> None:
        await self.close()

    async def close(self) -> None:
        try:
            await self._client.call("Window.close")
        except Exception:  # noqa: BLE001
            pass
        await self._client.aclose()

    # ---- runtime ------------------------------------------------------------
    async def evaluate(self, expression: str, *, return_by_value: bool = True) -> Any:
        result = await self._client.call(
            "Runtime.evaluate",
            {"expression": expression, "returnByValue": return_by_value},
        )
        return result.get("value") if return_by_value else result

    async def globals(self) -> list[str]:
        result = await self._client.call("Runtime.getGlobals")
        return list(result.get("names", []))

    async def reconciler_snapshot(self) -> dict[str, Any]:
        return await self._client.call("Reconciler.snapshot")

    # ---- page ---------------------------------------------------------------
    async def framebuffer_size(self) -> tuple[int, int]:
        result = await self._client.call("Page.framebufferSize")
        return int(result.get("width", 0)), int(result.get("height", 0))

    async def request_redraw(self) -> None:
        await self._client.call("Page.requestRedraw")

    async def screenshot(
        self,
        path: str | Path | None = None,
        *,
        return_bytes: bool = False,
        format: str = "png",
        quality: int | None = None,
        clip: dict[str, float] | tuple[float, float, float, float] | None = None,
        scale: float | None = None,
        max_width: int | None = None,
        max_height: int | None = None,
        delay_ms: float | None = None,
        save_on_server: bool = False,
        omit_data: bool = False,
    ) -> bytes | None:
        """Capture a screenshot via Page.screenshot.

        Mirrors the Chrome DevTools `Page.captureScreenshot` shape with a few
        fxe-specific extras:

        - ``format``: ``"png"`` (default) or ``"jpeg"``.
        - ``quality``: 1-100, jpeg only.
        - ``clip``: ``{"x", "y", "width", "height"}`` in framebuffer pixels,
          or a ``(x, y, w, h)`` tuple. Width/height of 0 means "to the edge".
        - ``scale``: multiplicative output scale (``0.5`` = half resolution).
        - ``max_width`` / ``max_height``: clamp output to a bounding box,
          preserving aspect ratio.
        - ``delay_ms``: wait this many milliseconds *server-side* before
          capturing. The render loop must keep ticking during the wait.
        - ``save_on_server``: when ``path`` is set, ask the server to write
          the encoded bytes to that path locally (alongside or instead of
          returning data — see ``omit_data``). When ``False`` (default) and
          ``path`` is set, the client writes the file itself.
        - ``omit_data``: skip ``dataBase64`` in the response (only takes
          effect together with ``save_on_server=True`` to avoid wire copy).
        """
        params: dict[str, Any] = {"format": format}
        if quality is not None:
            params["quality"] = int(quality)
        if clip is not None:
            if isinstance(clip, tuple):
                cx, cy, cw, ch = clip
                params["clip"] = {"x": cx, "y": cy, "width": cw, "height": ch}
            else:
                params["clip"] = dict(clip)
        if scale is not None:
            params["scale"] = float(scale)
        if max_width is not None:
            params["maxWidth"] = int(max_width)
        if max_height is not None:
            params["maxHeight"] = int(max_height)
        if delay_ms is not None and delay_ms > 0:
            params["delayMs"] = float(delay_ms)
        if save_on_server and path is not None:
            params["path"] = str(path)
            if omit_data:
                params["omitData"] = True

        result = await self._client.call("Page.screenshot", params)

        if save_on_server and path is not None and not result.get("saved", False):
            err = result.get("saveError", "save failed")
            raise RuntimeError(f"server-side save to {path} failed: {err}")

        encoded = result.get("dataBase64", "")
        data = base64.b64decode(encoded) if encoded else b""

        if path is not None and not save_on_server:
            Path(path).write_bytes(data)
        if return_bytes:
            return data
        return None if path is not None else data

    async def take_heap_snapshot(self, out_path: str) -> None:
        chunks: list[str] = []

        def _push(params: dict[str, Any]) -> None:
            chunk = params.get("chunk")
            if isinstance(chunk, str):
                chunks.append(chunk)

        self._client.on("HeapProfiler.addHeapSnapshotChunk", _push)
        try:
            await self._client.call("HeapProfiler.takeHeapSnapshot")
        finally:
            self._client.off("HeapProfiler.addHeapSnapshotChunk", _push)

        Path(out_path).write_text("".join(chunks), encoding="utf-8")

    # ---- console ------------------------------------------------------------
    async def console_messages(self) -> AsyncIterator[ConsoleMessage]:
        if self._console_queue is None:
            self._console_queue = asyncio.Queue()

            def _push(params: dict[str, Any]) -> None:
                msg = ConsoleMessage.from_dict(params)
                assert self._console_queue is not None
                self._console_queue.put_nowait(msg)

            self._client.on("Console.messageAdded", _push)
        if not self._console_enabled:
            await self._client.call("Console.enable")
            self._console_enabled = True
        queue = self._console_queue
        try:
            while True:
                yield await queue.get()
        except asyncio.CancelledError:
            return

    # ---- debugger -----------------------------------------------------------
    def _on_paused(self) -> None:
        self._paused_event.set()
        self._resumed_event.clear()

    def _on_resumed(self) -> None:
        self._resumed_event.set()
        self._paused_event.clear()

    async def pause(self) -> None:
        await self._client.call("Debugger.pause")

    async def resume(self) -> None:
        await self._client.call("Debugger.resume")

    async def step(self) -> None:
        await self._client.call("Debugger.step")

    async def wait_for_paused(self, timeout: float | None = None) -> None:
        await asyncio.wait_for(self._paused_event.wait(), timeout)

    async def wait_for_resumed(self, timeout: float | None = None) -> None:
        await asyncio.wait_for(self._resumed_event.wait(), timeout)

    # ---- window -------------------------------------------------------------
    async def poll_input(self) -> list[dict[str, Any]]:
        result = await self._client.call("Window.pollInput")
        return list(result.get("events", []))

    # ---- trace --------------------------------------------------------------
    async def trace_install(
        self,
        target: str,
        capture: str = "args",
        *,
        limit: int = 200,
        phases: tuple[str, ...] = ("call",),
    ) -> str:
        """Install a call tracer; see ``fxe_debug.trace`` for details."""
        from .trace import trace_install as _impl

        return await _impl(self, target, capture, limit=limit, phases=phases)  # type: ignore[arg-type]

    async def trace_drain(self, trace_id: str, *, clear: bool = True) -> list[Any]:
        from .trace import trace_drain as _impl

        return await _impl(self, trace_id, clear=clear)

    async def trace_uninstall(self, trace_id: str) -> bool:
        from .trace import trace_uninstall as _impl

        return await _impl(self, trace_id)

    async def trace_list(self) -> list[dict[str, Any]]:
        from .trace import trace_list as _impl

        return await _impl(self)

    async def layout_trace_enable(self, *, limit: int = 1000) -> None:
        from .trace import layout_trace_enable as _impl

        await _impl(self, limit=limit)

    async def layout_trace_disable(self) -> None:
        from .trace import layout_trace_disable as _impl

        await _impl(self)

    async def layout_trace_drain(self, *, clear: bool = True) -> list[dict[str, Any]]:
        from .trace import layout_trace_drain as _impl

        return await _impl(self, clear=clear)
