"""argparse-based CLI front-end."""

from __future__ import annotations

import argparse
import asyncio
import json
import signal
import sys

from .client import ProtocolError
from .launcher import LaunchError, connect, launch
from .page import Page


async def _connect_existing(host: str, port: int) -> Page:
    return await connect(host=host, port=port)


def _add_port(p: argparse.ArgumentParser) -> None:
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--host", default="127.0.0.1")


# ---- subcommand impls --------------------------------------------------------


async def cmd_screenshot(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        clip = None
        if ns.clip:
            parts = [float(x) for x in ns.clip.split(",")]
            if len(parts) != 4:
                print("--clip expects x,y,w,h", file=sys.stderr)
                return 2
            clip = tuple(parts)  # type: ignore[assignment]
        try:
            await page.screenshot(
                ns.out,
                format=ns.format,
                quality=ns.quality,
                clip=clip,
                scale=ns.scale,
                max_width=ns.max_width,
                max_height=ns.max_height,
                delay_ms=ns.delay_ms,
                save_on_server=ns.save_on_server,
                omit_data=ns.save_on_server,
            )
            print(f"wrote {ns.out}")
        except ProtocolError as exc:
            print(f"screenshot failed: {exc}", file=sys.stderr)
            return 2
    finally:
        await page.client.aclose()
    return 0


async def cmd_eval(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        result = await page.evaluate(ns.expression)
        json.dump(result, sys.stdout, indent=2, default=str)
        sys.stdout.write("\n")
    finally:
        await page.client.aclose()
    return 0


async def cmd_inspect(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        hs = page.handshake
        size = await page.framebuffer_size()
        names = await page.globals()
        out = {
            "handshake": hs.raw if hs else None,
            "framebuffer": {"width": size[0], "height": size[1]},
            "globals_count": len(names),
            "globals_sample": names[:32],
        }
        json.dump(out, sys.stdout, indent=2)
        sys.stdout.write("\n")
    finally:
        await page.client.aclose()
    return 0


async def cmd_mouse(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        if ns.action == "move":
            await page.mouse.move(ns.x, ns.y)
        elif ns.action == "click":
            await page.mouse.click(ns.x, ns.y, ns.button)
        elif ns.action == "wheel":
            await page.mouse.wheel(ns.x, ns.y)
    finally:
        await page.client.aclose()
    return 0


async def cmd_key(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        if ns.action == "down":
            await page.keyboard.down(ns.value)
        elif ns.action == "up":
            await page.keyboard.up(ns.value)
        elif ns.action == "press":
            await page.keyboard.press(ns.value)
        elif ns.action == "type":
            await page.keyboard.type(ns.value)
    finally:
        await page.client.aclose()
    return 0


async def cmd_console(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    stop = asyncio.Event()

    def _stop(*_a: object) -> None:
        stop.set()

    loop = asyncio.get_running_loop()
    try:
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                pass
    except RuntimeError:
        pass

    try:

        async def consume() -> None:
            async for msg in page.console_messages():
                print(f"[{msg.level}] {msg.text}")

        consumer = asyncio.create_task(consume())
        await stop.wait()
        consumer.cancel()
        try:
            await consumer
        except (asyncio.CancelledError, Exception):
            pass
    finally:
        await page.client.aclose()
    return 0


async def cmd_pause(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        await page.pause()
    finally:
        await page.client.aclose()
    return 0


async def cmd_resume(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        await page.resume()
    finally:
        await page.client.aclose()
    return 0


async def cmd_step(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        await page.step()
    finally:
        await page.client.aclose()
    return 0


async def cmd_close(ns: argparse.Namespace) -> int:
    page = await _connect_existing(ns.host, ns.port)
    try:
        await page.close()
    except Exception:  # noqa: BLE001
        pass
    return 0


async def _repl(page: Page) -> None:
    print("attached. commands: eval <expr> | screenshot <path> | resume | pause | step | quit")
    loop = asyncio.get_running_loop()
    while True:
        try:
            line = await loop.run_in_executor(None, sys.stdin.readline)
        except (KeyboardInterrupt, EOFError):
            return
        if not line:
            return
        cmd = line.strip()
        if not cmd:
            continue
        if cmd in {"quit", "exit"}:
            return
        try:
            if cmd.startswith("eval "):
                print(await page.evaluate(cmd[len("eval ") :]))
            elif cmd.startswith("screenshot "):
                path = cmd[len("screenshot ") :].strip()
                await page.screenshot(path)
                print(f"wrote {path}")
            elif cmd == "resume":
                await page.resume()
            elif cmd == "pause":
                await page.pause()
            elif cmd == "step":
                await page.step()
            else:
                print(f"unknown command: {cmd!r}")
        except ProtocolError as exc:
            print(f"error: {exc}")


async def cmd_launch(ns: argparse.Namespace) -> int:
    try:
        page = await launch(
            ns.script,
            port=ns.port,
            pause=not ns.no_pause,
            vsync=ns.vsync,
            fps_limit=ns.fps_limit,
            msaa=ns.msaa,
            bloom=ns.bloom,
            show_fps=ns.show_fps,
            no_lazy=ns.no_lazy,
            watch=ns.watch,
            render_surface=ns.render_surface,
        )
    except LaunchError as exc:
        print(f"launch failed: {exc}", file=sys.stderr)
        return 1
    try:
        await _repl(page)
    finally:
        await page.close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="fxe-cli")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("screenshot")
    _add_port(s)
    s.add_argument("--out", required=True, help="output file path")
    s.add_argument("--format", default="png", choices=["png", "jpeg", "jpg"])
    s.add_argument("--quality", type=int, default=None, help="jpeg quality 1-100")
    s.add_argument("--clip", default=None, help="x,y,w,h in framebuffer pixels")
    s.add_argument("--scale", type=float, default=None, help="output scale (e.g. 0.5)")
    s.add_argument("--max-width", type=int, default=None, dest="max_width")
    s.add_argument("--max-height", type=int, default=None, dest="max_height")
    s.add_argument(
        "--delay-ms",
        type=float,
        default=None,
        dest="delay_ms",
        help="server-side wait before capture",
    )
    s.add_argument(
        "--save-on-server",
        action="store_true",
        dest="save_on_server",
        help="ask the server to write --out locally (skips wire copy)",
    )
    s.set_defaults(func=cmd_screenshot)

    s = sub.add_parser("eval")
    _add_port(s)
    s.add_argument("expression")
    s.set_defaults(func=cmd_eval)

    s = sub.add_parser("inspect")
    _add_port(s)
    s.set_defaults(func=cmd_inspect)

    s = sub.add_parser("mouse")
    _add_port(s)
    s.add_argument("action", choices=["move", "click", "wheel"])
    s.add_argument("x", type=float)
    s.add_argument("y", type=float)
    s.add_argument("--button", default="left")
    s.set_defaults(func=cmd_mouse)

    s = sub.add_parser("key")
    _add_port(s)
    s.add_argument("action", choices=["down", "up", "press", "type"])
    s.add_argument("value")
    s.set_defaults(func=cmd_key)

    s = sub.add_parser("console")
    _add_port(s)
    s.set_defaults(func=cmd_console)

    for name, fn in (
        ("pause", cmd_pause),
        ("resume", cmd_resume),
        ("step", cmd_step),
        ("close", cmd_close),
    ):
        s = sub.add_parser(name)
        _add_port(s)
        s.set_defaults(func=fn)

    s = sub.add_parser("launch")
    s.add_argument("script")
    s.add_argument("--port", type=int, default=0)
    s.add_argument("--no-pause", action="store_true")
    s.add_argument(
        "--vsync",
        default=None,
        action=argparse.BooleanOptionalAction,
        help="Override Renderer vsync (--vsync / --no-vsync)",
    )
    s.add_argument(
        "--fps-limit",
        type=float,
        default=None,
        dest="fps_limit",
        help="Override fps limit",
    )
    s.add_argument(
        "--msaa",
        type=int,
        default=None,
        help="Override Renderer multisampleCount",
    )
    s.add_argument(
        "--bloom",
        default=None,
        action=argparse.BooleanOptionalAction,
        help="Override Renderer enableBloom (--bloom / --no-bloom)",
    )
    s.add_argument(
        "--show-fps",
        action="store_true",
        dest="show_fps",
        help="Draw a top-left FPS counter overlay",
    )
    s.add_argument(
        "--no-lazy",
        action="store_true",
        dest="no_lazy",
        help="Disable lazy frames (continuous redraw)",
    )
    s.add_argument(
        "--render-surface",
        choices=["offscreen", "window"],
        default="offscreen",
        dest="render_surface",
        help="Renderer backing for launched scripts (default: offscreen)",
    )
    s.add_argument(
        "--watch",
        action="store_true",
        help="Enable HMR file watcher",
    )
    s.set_defaults(func=cmd_launch)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    ns = parser.parse_args(argv)
    return asyncio.run(ns.func(ns))


if __name__ == "__main__":
    sys.exit(main())
