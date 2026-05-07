"""Smoke tests for multiple Python debug clients sharing one endpoint."""

from __future__ import annotations

import asyncio
import json
import sys
import unittest
from pathlib import Path
from typing import Any, Awaitable, Callable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from fxe_debug.client import Client  # noqa: E402
from fxe_debug.page import Page  # noqa: E402
from fxe_debug.protocol import Handshake  # noqa: E402

HandlerFn = Callable[[dict[str, Any]], Awaitable[Any]]

HANDSHAKE_RESULT = {
    "version": "0.1.0",
    "capabilities": ["System.handshake", "Runtime.evaluate"],
    "features": {"window": True, "host": True},
}


class MultiClientStubServer:
    def __init__(self, handlers: dict[str, HandlerFn]) -> None:
        self._handlers = handlers
        self._server: asyncio.base_events.Server | None = None
        self._writers: list[asyncio.StreamWriter] = []
        self.port = 0

    async def __aenter__(self) -> "MultiClientStubServer":
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        sock = self._server.sockets[0]
        self.port = sock.getsockname()[1]
        return self

    async def __aexit__(self, *exc: Any) -> None:
        for writer in list(self._writers):
            writer.close()
        for writer in list(self._writers):
            try:
                await writer.wait_closed()
            except (ConnectionResetError, BrokenPipeError, OSError):
                pass
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._writers.append(writer)
        try:
            while True:
                line = await reader.readline()
                if not line:
                    return
                req = json.loads(line.decode("utf-8"))
                rid = req.get("id")
                method = req.get("method")
                params = req.get("params") or {}
                handler = self._handlers[method]
                result = await handler(params)
                writer.write((json.dumps({"id": rid, "result": result}) + "\n").encode("utf-8"))
                await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError):
            return
        finally:
            if writer in self._writers:
                self._writers.remove(writer)


def make_handlers() -> dict[str, HandlerFn]:
    async def handshake(_p: dict[str, Any]) -> dict[str, Any]:
        return HANDSHAKE_RESULT

    async def evaluate(p: dict[str, Any]) -> dict[str, Any]:
        return {"value": p.get("expression", ""), "type": "string"}

    return {"System.handshake": handshake, "Runtime.evaluate": evaluate}


async def _connect_page(port: int) -> Page:
    client = await Client.connect("127.0.0.1", port)
    hs_raw = await client.call("System.handshake")
    return Page(client, Handshake.from_dict(hs_raw))


class MultiClientDebugTests(unittest.IsolatedAsyncioTestCase):
    async def test_two_pages_share_one_host(self) -> None:
        async with MultiClientStubServer(make_handlers()) as srv:
            page_a = await _connect_page(srv.port)
            page_b = await _connect_page(srv.port)
            try:
                self.assertEqual(page_a.handshake.version, HANDSHAKE_RESULT["version"])
                self.assertEqual(page_b.handshake.version, HANDSHAKE_RESULT["version"])
                self.assertEqual(await page_a.evaluate("alpha"), "alpha")
                self.assertEqual(await page_b.evaluate("beta"), "beta")
            finally:
                await page_a.client.aclose()
                await page_b.client.aclose()


if __name__ == "__main__":
    unittest.main()
