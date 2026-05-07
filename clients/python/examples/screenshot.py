"""Spawn fxe_run with examples/js/hello.ts and grab a screenshot."""

# NOTE: this file lives in a directory containing a sibling `inspect.py`,
# which Python would otherwise shadow the stdlib `inspect` module with
# (the script directory is auto-prepended to sys.path). Sanitize before
# importing anything that might transitively pull in `inspect`.
import os as _os
import sys as _sys

_HERE = _os.path.dirname(_os.path.abspath(__file__))
_sys.path[:] = [p for p in _sys.path if _os.path.abspath(p) != _HERE]
_sys.path.insert(0, _os.path.dirname(_HERE))  # clients/python/

import asyncio
import sys
from pathlib import Path

from fxe_debug import launch
from fxe_debug.client import ProtocolError


PROJECT_ROOT = Path(_HERE).resolve().parent.parent.parent  # → repo root


async def main() -> int:
    script = PROJECT_ROOT / "examples" / "js" / "hello.ts"
    out = Path("hello.png")
    try:
        async with await launch(str(script), pause=False) as page:
            await page.request_redraw()
            await asyncio.sleep(0.5)
            try:
                data = await page.screenshot(str(out), return_bytes=True)
                size = len(data) if data is not None else 0
                print(f"wrote {out} ({size} bytes)")
            except ProtocolError as exc:
                print(f"screenshot failed (server-side): {exc}", file=sys.stderr)
                return 2
    except Exception as exc:  # noqa: BLE001
        print(f"launch error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
