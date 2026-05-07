"""Move the cursor in a circle, then dump buffered input events."""

import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path[:] = [p for p in _sys.path if _os.path.abspath(p) != _HERE]
_sys.path.insert(0, _os.path.dirname(_HERE))  # clients/python/

import asyncio
import json
import math
import sys

from fxe_debug import connect


async def main(port: int, host: str = "127.0.0.1") -> int:
    page = await connect(host=host, port=port)
    try:
        w, h = await page.framebuffer_size()
        cx, cy = w / 2, h / 2
        r = min(w, h) / 4
        for i in range(36):
            theta = 2 * math.pi * i / 36
            await page.mouse.move(cx + r * math.cos(theta), cy + r * math.sin(theta))
        events = await page.poll_input()
        json.dump(events, sys.stdout, indent=2)
        sys.stdout.write("\n")
    finally:
        await page.client.aclose()
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: mouse_demo.py PORT [HOST]", file=sys.stderr)
        sys.exit(2)
    port = int(sys.argv[1])
    host = sys.argv[2] if len(sys.argv) >= 3 else "127.0.0.1"
    sys.exit(asyncio.run(main(port, host)))
