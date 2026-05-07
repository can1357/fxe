"""Smoke tests for multiple Python debug clients sharing one endpoint."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

from fxe_debug.client import Client  # noqa: E402
from fxe_debug.page import Page  # noqa: E402
from fxe_debug.protocol import Handshake  # noqa: E402
from test_protocol import HANDSHAKE_RESULT, StubServer, make_default_handlers  # noqa: E402


async def _connect_page(port: int) -> Page:
    client = await Client.connect("127.0.0.1", port)
    hs_raw = await client.call("System.handshake")
    return Page(client, Handshake.from_dict(hs_raw))


class MultiClientDebugTests(unittest.IsolatedAsyncioTestCase):
    async def test_two_pages_share_one_host(self) -> None:
        async with StubServer(make_default_handlers()) as srv:
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
