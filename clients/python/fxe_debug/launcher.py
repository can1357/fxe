"""Spawn `fxe_run` and connect to its debug server."""

from __future__ import annotations

import asyncio
import os
from pathlib import Path
from typing import Any

from .client import Client
from .page import Page
from .protocol import Handshake


class LaunchError(RuntimeError):
    pass


def _candidate_binaries(explicit: str | None) -> list[Path]:
    cands: list[Path] = []
    if explicit:
        cands.append(Path(explicit))
    env = os.environ.get("FXE_RUN")
    if env:
        cands.append(Path(env))
    cands.append(Path.cwd() / "build" / "dev-v8-wgpu" / "fxe_run")
    return cands


def _resolve_fxe_run(explicit: str | None) -> Path:
    cands = _candidate_binaries(explicit)
    for p in cands:
        if p.is_file() and os.access(p, os.X_OK):
            return p.resolve()
    paths = "\n  ".join(str(c) for c in cands)
    raise LaunchError(
        "could not locate fxe_run; tried:\n  "
        + paths
        + "\nSet FXE_RUN, pass fxe_run=, or build the dev-v8-wgpu preset."
    )


async def _read_port_line(proc: asyncio.subprocess.Process, timeout: float) -> int:
    assert proc.stdout is not None
    deadline = asyncio.get_event_loop().time() + timeout

    async def read_one() -> bytes:
        return await proc.stdout.readline()

    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            raise LaunchError("timed out waiting for FXE_DEBUG_PORT")
        try:
            line = await asyncio.wait_for(read_one(), timeout=remaining)
        except asyncio.TimeoutError as exc:
            raise LaunchError("timed out waiting for FXE_DEBUG_PORT") from exc
        if not line:
            raise LaunchError("fxe_run exited before reporting debug port")
        text = line.decode("utf-8", errors="replace").strip()
        if text.startswith("FXE_DEBUG_PORT="):
            try:
                return int(text.split("=", 1)[1])
            except ValueError as exc:
                raise LaunchError(f"malformed port line: {text!r}") from exc
        # ignore other stdout chatter


async def _drain_stderr(proc: asyncio.subprocess.Process, sink: list[bytes]) -> None:
    if proc.stderr is None:
        return
    try:
        while True:
            chunk = await proc.stderr.read(4096)
            if not chunk:
                return
            sink.append(chunk)
            if sum(len(c) for c in sink) > 64 * 1024:
                # cap retained tail
                joined = b"".join(sink)[-32 * 1024 :]
                sink.clear()
                sink.append(joined)
    except asyncio.CancelledError:
        return


async def _terminate(proc: asyncio.subprocess.Process, timeout: float = 3.0) -> int | None:
    if proc.returncode is not None:
        return proc.returncode
    try:
        proc.terminate()
    except ProcessLookupError:
        return proc.returncode
    try:
        await asyncio.wait_for(proc.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        try:
            proc.kill()
        except ProcessLookupError:
            pass
        try:
            await asyncio.wait_for(proc.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            return None
    return proc.returncode


class _LaunchedPage(Page):
    """Page that owns a child process; cleans it up on close."""

    def __init__(
        self,
        client: Client,
        handshake: Handshake | None,
        proc: asyncio.subprocess.Process,
        stderr_sink: list[bytes],
        stderr_task: asyncio.Task[None],
    ) -> None:
        super().__init__(client, handshake)
        self._proc = proc
        self._stderr_sink = stderr_sink
        self._stderr_task = stderr_task

    @property
    def process(self) -> asyncio.subprocess.Process:
        return self._proc

    @property
    def stderr_tail(self) -> bytes:
        return b"".join(self._stderr_sink)[-8 * 1024 :]

    async def close(self) -> None:
        try:
            await super().close()
        finally:
            await _terminate(self._proc)
            self._stderr_task.cancel()
            try:
                await self._stderr_task
            except (asyncio.CancelledError, Exception):
                pass


async def launch(
    script: str | Path,
    *,
    fxe_run: str | None = None,
    port: int = 0,
    host: str = "127.0.0.1",
    pause: bool = True,
    keepalive: bool = False,
    env: dict[str, str] | None = None,
    args: list[str] | None = None,
    ready_timeout: float = 10.0,
) -> Page:
    binary = _resolve_fxe_run(fxe_run)
    script_path = Path(script)

    cmd: list[str] = [str(binary), f"--debug={port}", f"--debug-host={host}"]
    if pause:
        cmd.append("--debug-pause")
    if keepalive:
        cmd.append("--debug-keepalive")
    if args:
        cmd.extend(args)
    cmd.append(str(script_path))

    sub_env = os.environ.copy()
    if env:
        sub_env.update(env)

    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=sub_env,
    )

    stderr_sink: list[bytes] = []
    stderr_task = asyncio.create_task(_drain_stderr(proc, stderr_sink))

    try:
        bound_port = await _read_port_line(proc, ready_timeout)
    except LaunchError:
        await _terminate(proc)
        stderr_task.cancel()
        try:
            await stderr_task
        except (asyncio.CancelledError, Exception):
            pass
        tail = b"".join(stderr_sink)[-4 * 1024 :].decode("utf-8", "replace")
        raise LaunchError(f"fxe_run failed to start; stderr tail:\n{tail}") from None

    try:
        client = await Client.connect(host, bound_port)
    except (ConnectionRefusedError, OSError) as exc:
        await _terminate(proc)
        stderr_task.cancel()
        try:
            await stderr_task
        except (asyncio.CancelledError, Exception):
            pass
        raise LaunchError(
            f"could not connect to debug server at {host}:{bound_port}: {exc}"
        ) from exc

    handshake_raw = await client.call("System.handshake")
    handshake = Handshake.from_dict(handshake_raw)
    return _LaunchedPage(client, handshake, proc, stderr_sink, stderr_task)


async def connect(*, host: str = "127.0.0.1", port: int) -> Page:
    """Attach to an already-running fxe_run debug server."""
    client = await Client.connect(host, port)
    handshake_raw = await client.call("System.handshake")
    return Page(client, Handshake.from_dict(handshake_raw))
