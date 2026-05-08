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
    "capabilities": ["System.handshake", "WebAuthn.enable"],
    "features": {"window": True, "host": True},
}


class StubServer:
    def __init__(self, handlers: dict[str, HandlerFn]) -> None:
        self._handlers = handlers
        self._server: asyncio.base_events.Server | None = None
        self.port = 0

    async def __aenter__(self) -> "StubServer":
        self._server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        sock = self._server.sockets[0]
        self.port = sock.getsockname()[1]
        return self

    async def __aexit__(self, *exc: Any) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()

    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while True:
                line = await reader.readline()
                if not line:
                    return
                req = json.loads(line.decode("utf-8"))
                method = req.get("method")
                rid = req.get("id")
                params = req.get("params") or {}
                result = await self._handlers[method](params)
                writer.write((json.dumps({"id": rid, "result": result}) + "\n").encode("utf-8"))
                await writer.drain()
        except (ConnectionResetError, asyncio.IncompleteReadError):
            return
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass


async def _connect_page(port: int) -> Page:
    client = await Client.connect("127.0.0.1", port)
    hs_raw = await client.call("System.handshake")
    return Page(client, Handshake.from_dict(hs_raw))


class WebAuthnPageTests(unittest.IsolatedAsyncioTestCase):
    async def test_webauthn_helper_methods(self) -> None:
        calls: list[tuple[str, dict[str, Any]]] = []
        credential = {
            "credentialId": "cred-id",
            "isResidentCredential": True,
            "rpId": "example.test",
            "privateKey": "pkcs8-key",
            "userHandle": "user-handle",
            "signCount": 7,
        }
        canned_credentials = [credential]

        async def handshake(_params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("System.handshake", dict(_params)))
            return HANDSHAKE_RESULT

        async def enable(params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("WebAuthn.enable", dict(params)))
            return {}

        async def add_virtual_authenticator(params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("WebAuthn.addVirtualAuthenticator", dict(params)))
            return {"authenticatorId": "auth-7"}

        async def add_credential(params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("WebAuthn.addCredential", dict(params)))
            return {}

        async def get_credentials(params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("WebAuthn.getCredentials", dict(params)))
            return {"credentials": canned_credentials}

        async def clear_credentials(params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("WebAuthn.clearCredentials", dict(params)))
            return {}

        async def set_user_verified(params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("WebAuthn.setUserVerified", dict(params)))
            return {}

        async def window_close(params: dict[str, Any]) -> dict[str, Any]:
            calls.append(("Window.close", dict(params)))
            return {}

        handlers = {
            "System.handshake": handshake,
            "WebAuthn.enable": enable,
            "WebAuthn.addVirtualAuthenticator": add_virtual_authenticator,
            "WebAuthn.addCredential": add_credential,
            "WebAuthn.getCredentials": get_credentials,
            "WebAuthn.clearCredentials": clear_credentials,
            "WebAuthn.setUserVerified": set_user_verified,
            "Window.close": window_close,
        }

        async with StubServer(handlers) as server:
            page = await _connect_page(server.port)
            try:
                await page.webauthn.enable()
                authenticator_id = await page.webauthn.add_virtual_authenticator()
                await page.webauthn.add_credential(authenticator_id, credential)
                credentials = await page.webauthn.get_credentials(authenticator_id)
                await page.webauthn.clear_credentials(authenticator_id)
                await page.webauthn.set_user_verified(authenticator_id, is_user_verified=True)
            finally:
                await page.client.aclose()

        self.assertEqual(authenticator_id, "auth-7")
        self.assertEqual(credentials, canned_credentials)
        self.assertEqual(calls[1][0], "WebAuthn.enable")
        self.assertEqual(calls[1][1], {"enableUI": False})
        self.assertEqual(calls[2][0], "WebAuthn.addVirtualAuthenticator")
        self.assertEqual(
            calls[2][1],
            {
                "options": {
                    "protocol": "ctap2",
                    "transport": "internal",
                    "hasResidentKey": True,
                    "hasUserVerification": True,
                    "isUserVerified": True,
                    "automaticPresenceSimulation": True,
                }
            },
        )
        self.assertEqual(calls[3], ("WebAuthn.addCredential", {"authenticatorId": "auth-7", "credential": credential}))
        self.assertEqual(calls[4], ("WebAuthn.getCredentials", {"authenticatorId": "auth-7"}))
        self.assertEqual(calls[5], ("WebAuthn.clearCredentials", {"authenticatorId": "auth-7"}))
        self.assertEqual(
            calls[6],
            ("WebAuthn.setUserVerified", {"authenticatorId": "auth-7", "isUserVerified": True}),
        )


if __name__ == "__main__":
    unittest.main()
