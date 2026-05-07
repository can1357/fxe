"""High-level client: id allocation, futures, event dispatch."""

from __future__ import annotations

import asyncio
import itertools
import logging
from typing import Any, Awaitable, Callable

from .transport import Transport

log = logging.getLogger("fxe_debug.client")

EventHandler = Callable[[dict[str, Any]], None | Awaitable[None]]


class ProtocolError(Exception):
    """Raised when the server returns an `error` envelope."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.message = message
        self.data = data


class MethodNotFound(ProtocolError):
    """Specialization for -32601."""


class Client:
    def __init__(self, transport: Transport) -> None:
        self._transport = transport
        self._ids = itertools.count(1)
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._handlers: dict[str, list[EventHandler]] = {}
        self._lock = asyncio.Lock()
        self._reader_task: asyncio.Task[None] | None = None
        self._closed = False

    @classmethod
    async def connect(cls, host: str, port: int) -> "Client":
        transport = await Transport.connect(host, port)
        client = cls(transport)
        client._start()
        return client

    def _start(self) -> None:
        if self._reader_task is None:
            self._reader_task = asyncio.create_task(self._reader_loop())

    async def __aenter__(self) -> "Client":
        return self

    async def __aexit__(self, *exc: Any) -> None:
        await self.aclose()

    async def call(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        if self._closed:
            raise ConnectionError("client is closed")
        rid = next(self._ids)
        loop = asyncio.get_running_loop()
        fut: asyncio.Future[dict[str, Any]] = loop.create_future()
        async with self._lock:
            self._pending[rid] = fut
            await self._transport.send({"id": rid, "method": method, "params": params or {}})
        try:
            return await fut
        finally:
            self._pending.pop(rid, None)

    def on(self, event: str, callback: EventHandler) -> None:
        self._handlers.setdefault(event, []).append(callback)

    def off(self, event: str, callback: EventHandler) -> None:
        lst = self._handlers.get(event)
        if not lst:
            return
        try:
            lst.remove(callback)
        except ValueError:
            pass

    async def _reader_loop(self) -> None:
        try:
            async for frame in self._transport:
                if "id" in frame and frame["id"] is not None:
                    rid = int(frame["id"])
                    fut = self._pending.get(rid)
                    if fut is None or fut.done():
                        continue
                    if "error" in frame:
                        err = frame["error"] or {}
                        code = int(err.get("code", -32603))
                        msg = str(err.get("message", "error"))
                        cls = MethodNotFound if code == -32601 else ProtocolError
                        fut.set_exception(cls(code, msg, err.get("data")))
                    else:
                        fut.set_result(frame.get("result") or {})
                else:
                    method = frame.get("method")
                    params = frame.get("params") or {}
                    if not method:
                        continue
                    handlers = list(self._handlers.get(method, ()))
                    if not handlers:
                        log.debug("unhandled event %s %r", method, params)
                        continue
                    for h in handlers:
                        try:
                            res = h(params)
                            if asyncio.iscoroutine(res):
                                asyncio.create_task(res)
                        except Exception:  # noqa: BLE001
                            log.exception("event handler failed for %s", method)
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001
            self._fail_pending(exc)
            return
        # graceful EOF
        self._fail_pending(ConnectionError("debug server closed connection"))

    def _fail_pending(self, exc: BaseException) -> None:
        for rid, fut in list(self._pending.items()):
            if not fut.done():
                fut.set_exception(exc)
            self._pending.pop(rid, None)

    async def aclose(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._reader_task is not None:
            self._reader_task.cancel()
            try:
                await self._reader_task
            except (asyncio.CancelledError, Exception):
                pass
        await self._transport.close()
        self._fail_pending(ConnectionError("client closed"))
