"""Attach to an existing fxe_run instance and dump some state."""

# Don't let our own filename shadow stdlib `inspect`.
import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path[:] = [p for p in _sys.path if _os.path.abspath(p) != _HERE]
_sys.path.insert(0, _os.path.dirname(_HERE))  # clients/python/

import asyncio
import json
import sys

from fxe_debug import connect


async def main(port: int, host: str = "127.0.0.1") -> int:
    page = await connect(host=host, port=port)
    try:
        size = await page.framebuffer_size()
        sample = await page.evaluate("1 + 1")
        out = {
            "handshake": page.handshake.raw if page.handshake else None,
            "framebuffer": {"width": size[0], "height": size[1]},
            "evaluate(1+1)": sample,
        }
        json.dump(out, sys.stdout, indent=2)
        sys.stdout.write("\n")
    finally:
        await page.client.aclose()
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: inspect.py PORT [HOST]", file=sys.stderr)
        sys.exit(2)
    port = int(sys.argv[1])
    host = sys.argv[2] if len(sys.argv) >= 3 else "127.0.0.1"
    sys.exit(asyncio.run(main(port, host)))
