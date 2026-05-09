# fxe-debug-client

Puppeteer-style Python SDK for driving `fxe_run` over its NDJSON debug
protocol. Pure stdlib, no runtime dependencies.

## Quickstart

```python
import asyncio
from fxe_debug import launch

async def main():
    async with launch("examples/js/hello.ts") as page:
        await page.evaluate("1 + 1")
        await page.mouse.click(100, 100)
        await page.keyboard.type("hello")
        await page.screenshot("hello.png")

asyncio.run(main())
```

`launch()` defaults to `render_surface="offscreen"` so SDK-driven checks do
not pop up an application window. Pass `render_surface="window"` when you
specifically need the native window-backed renderer.

## Connect to an existing instance

```python
from fxe_debug import connect

async def main():
    async with await connect(host="127.0.0.1", port=9333) as page:
        print(await page.framebuffer_size())
```

## Console mirroring

Both `launch()` and `connect()` mirror the target app's `console.log` /
`warn` / `error` / etc. to your local stderr by default. Each line is
formatted as `[fxe:<level>] <text>`.

Disable globally with `mirror_console=False`, or toggle at runtime:

```python
page = await launch("examples/js/hello.ts", mirror_console=False)
await page.enable_console_mirror()                    # default: stderr
await page.enable_console_mirror(stream=open("log.txt", "w"))
await page.enable_console_mirror(formatter=lambda m: f"{m.ts:.3f} {m.level}: {m.text}")
await page.disable_console_mirror()
```

`page.console_messages()` continues to work alongside the mirror — both
handlers see every `Console.messageAdded` event.

## CLI

```
fxe-cli launch examples/js/hello.ts
fxe-cli screenshot --port 9333 --out shot.png
fxe-cli eval --port 9333 "1 + 1"
fxe-cli inspect --port 9333
```
