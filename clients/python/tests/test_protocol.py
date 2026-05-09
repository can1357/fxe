"""Pure-protocol tests using a stub asyncio NDJSON server.

Zero deps: stdlib unittest only.
"""

from __future__ import annotations

import asyncio
import base64
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import Any, Awaitable, Callable

# Make `fxe_debug` importable when running `python -m unittest discover` from
# the repo root.
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from fxe_debug.client import Client, MethodNotFound, ProtocolError  # noqa: E402
from fxe_debug.page import Page  # noqa: E402
from fxe_debug.protocol import ConsoleMessage, Handshake  # noqa: E402


def _make_png_1x1() -> bytes:
    """Hand-roll a minimal valid 1×1 PNG (red pixel) for the screenshot test."""
    sig = b"\x89PNG\r\n\x1a\n"

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    raw = b"\x00\xff\x00\x00"  # filter byte + RGB pixel
    idat = zlib.compress(raw)
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


PNG_BYTES = _make_png_1x1()
PNG_B64 = base64.b64encode(PNG_BYTES).decode("ascii")


HandlerFn = Callable[[dict[str, Any]], Awaitable[Any]]


class StubServer:
    """One-shot asyncio NDJSON server. Starts on an OS-assigned port."""

    def __init__(self, handlers: dict[str, HandlerFn]) -> None:
        self._handlers = handlers
        self._server: asyncio.base_events.Server | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._client_task: asyncio.Task[None] | None = None
        self.port: int = 0

    async def __aenter__(self) -> "StubServer":
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        sock = self._server.sockets[0]
        self.port = sock.getsockname()[1]
        return self

    async def __aexit__(self, *exc: Any) -> None:
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:  # noqa: BLE001
                pass
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    async def emit(self, method: str, params: dict[str, Any]) -> None:
        assert self._writer is not None
        self._writer.write(
            (json.dumps({"method": method, "params": params}) + "\n").encode("utf-8")
        )
        await self._writer.drain()

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._writer = writer
        try:
            while True:
                line = await reader.readline()
                if not line:
                    return
                req = json.loads(line.decode("utf-8"))
                rid = req.get("id")
                method = req.get("method")
                params = req.get("params") or {}
                handler = self._handlers.get(method)
                if handler is None:
                    resp: dict[str, Any] = {
                        "id": rid,
                        "error": {"code": -32601, "message": f"unknown {method}"},
                    }
                else:
                    try:
                        result = await handler(params)
                        resp = {"id": rid, "result": result}
                    except ProtocolError as exc:
                        resp = {
                            "id": rid,
                            "error": {"code": exc.code, "message": exc.message},
                        }
                writer.write((json.dumps(resp) + "\n").encode("utf-8"))
                await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError):
            return


HANDSHAKE_RESULT = {
    "version": "0.1.0",
    "capabilities": ["System.handshake", "Page.screenshot", "Runtime.evaluate"],
    "features": {"window": True, "host": True},
}


def make_default_handlers(events: list[dict[str, Any]] | None = None) -> dict[str, HandlerFn]:
    async def handshake(_p: dict[str, Any]) -> dict[str, Any]:
        return HANDSHAKE_RESULT

    async def evaluate(p: dict[str, Any]) -> dict[str, Any]:
        # Mirror inputs back: `1 + 1` becomes 2 if it's that expr; otherwise
        # echo the expression as a string.
        expr = p.get("expression", "")
        if expr == "1 + 1":
            return {"value": 2, "type": "number"}
        return {"value": expr, "type": "string"}

    async def screenshot(p: dict[str, Any]) -> dict[str, Any]:
        return {"format": "png", "width": 1, "height": 1, "dataBase64": PNG_B64}

    async def fb(_p: dict[str, Any]) -> dict[str, Any]:
        return {"width": 640, "height": 480}

    async def globals_(_p: dict[str, Any]) -> dict[str, Any]:
        return {"names": ["Window", "Renderer", "Primitives"]}

    async def mouse(_p: dict[str, Any]) -> dict[str, Any]:
        if events is not None:
            events.append({"kind": "mouse", "params": _p})
        return {}

    async def key(_p: dict[str, Any]) -> dict[str, Any]:
        if events is not None:
            events.append({"kind": "key", "params": _p})
        return {}

    async def console_enable(_p: dict[str, Any]) -> dict[str, Any]:
        return {}

    async def page_close(_p: dict[str, Any]) -> dict[str, Any]:
        return {}

    return {
        "System.handshake": handshake,
        "Runtime.evaluate": evaluate,
        "Runtime.getGlobals": globals_,
        "Page.screenshot": screenshot,
        "Page.framebufferSize": fb,
        "Input.dispatchMouseEvent": mouse,
        "Input.dispatchKeyEvent": key,
        "Console.enable": console_enable,
        "Window.close": page_close,
    }


async def _connect_page(port: int) -> Page:
    client = await Client.connect("127.0.0.1", port)
    hs_raw = await client.call("System.handshake")
    return Page(client, Handshake.from_dict(hs_raw))


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_handshake_parsing(self) -> None:
        async with StubServer(make_default_handlers()) as srv:
            page = await _connect_page(srv.port)
            try:
                self.assertIsNotNone(page.handshake)
                self.assertEqual(page.handshake.version, "0.1.0")
                self.assertIn("Page.screenshot", page.handshake.capabilities)
                self.assertTrue(page.handshake.features.get("window"))
            finally:
                await page.client.aclose()

    async def test_evaluate(self) -> None:
        async with StubServer(make_default_handlers()) as srv:
            page = await _connect_page(srv.port)
            try:
                self.assertEqual(await page.evaluate("1 + 1"), 2)
            finally:
                await page.client.aclose()

    async def test_screenshot_writes_file(self) -> None:
        async with StubServer(make_default_handlers()) as srv:
            page = await _connect_page(srv.port)
            try:
                with tempfile.TemporaryDirectory() as td:
                    out = Path(td) / "shot.png"
                    await page.screenshot(out)
                    self.assertTrue(out.exists())
                    data = out.read_bytes()
                    self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
                    self.assertEqual(data, PNG_BYTES)
            finally:
                await page.client.aclose()

    async def test_mouse_click_expands_to_down_up(self) -> None:
        events: list[dict[str, Any]] = []
        async with StubServer(make_default_handlers(events)) as srv:
            page = await _connect_page(srv.port)
            try:
                await page.mouse.click(10, 20)
            finally:
                await page.client.aclose()
        mouse_events = [e for e in events if e["kind"] == "mouse"]
        self.assertEqual(len(mouse_events), 2)
        self.assertEqual(mouse_events[0]["params"]["type"], "down")
        self.assertEqual(mouse_events[1]["params"]["type"], "up")
        self.assertEqual(mouse_events[0]["params"]["button"], "left")
        self.assertEqual(mouse_events[0]["params"]["x"], 10)
        self.assertEqual(mouse_events[0]["params"]["y"], 20)

    async def test_keyboard_type(self) -> None:
        events: list[dict[str, Any]] = []
        async with StubServer(make_default_handlers(events)) as srv:
            page = await _connect_page(srv.port)
            try:
                await page.keyboard.type("hi")
            finally:
                await page.client.aclose()
        key_events = [e for e in events if e["kind"] == "key"]
        self.assertEqual(len(key_events), 2)
        self.assertEqual(key_events[0]["params"]["type"], "char")
        self.assertEqual(key_events[0]["params"]["key"], "h")
        self.assertEqual(key_events[0]["params"]["codepoint"], ord("h"))

    async def test_console_event_delivery(self) -> None:
        async with StubServer(make_default_handlers()) as srv:
            page = await _connect_page(srv.port)
            try:
                received: list[ConsoleMessage] = []

                async def consume() -> None:
                    async for msg in page.console_messages():
                        received.append(msg)
                        if len(received) >= 2:
                            return

                task = asyncio.create_task(consume())
                # let the iterator install its handler / send Console.enable
                await asyncio.sleep(0.05)
                await srv.emit(
                    "Console.messageAdded",
                    {"level": "log", "text": "hello", "ts": 1.0},
                )
                await srv.emit(
                    "Console.messageAdded",
                    {"level": "warn", "text": "uhoh", "ts": 2.0},
                )
                await asyncio.wait_for(task, timeout=2.0)
                self.assertEqual([m.text for m in received], ["hello", "uhoh"])
                self.assertEqual(received[0].level, "log")
                self.assertEqual(received[1].level, "warn")
            finally:
                await page.client.aclose()

    async def test_error_envelope_to_exception(self) -> None:
        async with StubServer(make_default_handlers()) as srv:
            page = await _connect_page(srv.port)
            try:
                with self.assertRaises(MethodNotFound) as ctx:
                    await page.client.call("Nonexistent.method")
                self.assertEqual(ctx.exception.code, -32601)
            finally:
                await page.client.aclose()

    async def test_capture_failed_surfaces_protocol_error(self) -> None:
        handlers = make_default_handlers()

        async def failing(_p: dict[str, Any]) -> dict[str, Any]:
            raise ProtocolError(-32001, "no frame yet")

        handlers["Page.screenshot"] = failing
        async with StubServer(handlers) as srv:
            page = await _connect_page(srv.port)
            try:
                with self.assertRaises(ProtocolError) as ctx:
                    await page.screenshot()
                self.assertEqual(ctx.exception.code, -32001)
            finally:
                await page.client.aclose()

    async def test_console_mirror_writes_to_stream(self) -> None:
        import io

        async with StubServer(make_default_handlers()) as srv:
            page = await _connect_page(srv.port)
            try:
                buf = io.StringIO()
                await page.enable_console_mirror(stream=buf)
                await asyncio.sleep(0.02)
                await srv.emit(
                    "Console.messageAdded",
                    {"level": "log", "text": "hello", "ts": 1.0},
                )
                await srv.emit(
                    "Console.messageAdded",
                    {"level": "error", "text": "boom", "ts": 2.0},
                )
                await asyncio.sleep(0.05)
                lines = buf.getvalue().splitlines()
                self.assertIn("[fxe:log] hello", lines)
                self.assertIn("[fxe:error] boom", lines)
                await page.disable_console_mirror()
                buf.truncate(0)
                buf.seek(0)
                await srv.emit(
                    "Console.messageAdded",
                    {"level": "log", "text": "after", "ts": 3.0},
                )
                await asyncio.sleep(0.05)
                self.assertEqual(buf.getvalue(), "")
            finally:
                await page.client.aclose()


if __name__ == "__main__":
    unittest.main()
