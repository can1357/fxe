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

## Connect to an existing instance

```python
from fxe_debug import connect

async def main():
    async with await connect(host="127.0.0.1", port=9333) as page:
        print(await page.framebuffer_size())
```

## CLI

```
fxe-cli launch examples/js/hello.ts
fxe-cli screenshot --port 9333 --out shot.png
fxe-cli eval --port 9333 "1 + 1"
fxe-cli inspect --port 9333
```
