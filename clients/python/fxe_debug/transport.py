"""Async NDJSON transport over asyncio streams."""

from __future__ import annotations

import asyncio
import json
from typing import Any, AsyncIterator


class Transport:
    """Line-delimited JSON transport. One connection at a time."""

    def __init__(
        self,
        reader: asyncio.StreamReader,
        writer: asyncio.StreamWriter,
    ) -> None:
        self._reader = reader
        self._writer = writer
        self._buf = b""
        self._closed = False

    @classmethod
    async def connect(cls, host: str, port: int) -> "Transport":
        reader, writer = await asyncio.open_connection(host, port)
        return cls(reader, writer)

    async def send(self, obj: dict[str, Any]) -> None:
        if self._closed:
            raise ConnectionError("transport is closed")
        line = json.dumps(obj, separators=(",", ":")).encode("utf-8") + b"\n"
        self._writer.write(line)
        await self._writer.drain()

    async def __aiter__(self) -> AsyncIterator[dict[str, Any]]:
        while True:
            line = await self._read_line()
            if line is None:
                return
            line = line.rstrip(b"\r")
            if not line:
                continue
            try:
                yield json.loads(line.decode("utf-8"))
            except json.JSONDecodeError:
                # Skip malformed frames; the server should never emit them.
                continue

    async def _read_line(self) -> bytes | None:
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line = self._buf[:nl]
                self._buf = self._buf[nl + 1 :]
                return line
            try:
                chunk = await self._reader.read(4096)
            except (ConnectionResetError, asyncio.IncompleteReadError):
                return None
            if not chunk:
                # EOF; flush any remaining buffered content as a final line.
                if self._buf:
                    rest = self._buf
                    self._buf = b""
                    return rest
                return None
            self._buf += chunk

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._writer.close()
            await self._writer.wait_closed()
        except (ConnectionResetError, BrokenPipeError, OSError):
            pass
