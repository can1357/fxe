from __future__ import annotations

import asyncio
import os
import sys
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))

from fxe_debug import launch  # noqa: E402

_COLD_START_BUDGET_ENV = "FXE_COLD_START_BUDGET_MS"
_DEFAULT_COLD_START_BUDGET_MS = 2500.0
_FXE_RUN = os.environ.get("FXE_RUN")
_FXE_RUN_CANDIDATES = [
    Path(_FXE_RUN) if _FXE_RUN else None,
    ROOT / "build" / "dev" / "fxe_run",
    ROOT / "build" / "release" / "fxe_run",
]
_HAS_FXE_RUN = any(
    candidate is not None and candidate.is_file() and os.access(candidate, os.X_OK)
    for candidate in _FXE_RUN_CANDIDATES
)


@unittest.skipUnless(_HAS_FXE_RUN, "fxe_run binary not found")
class ColdStartLatencyTests(unittest.IsolatedAsyncioTestCase):
    async def test_launch_to_first_paint_budget(self) -> None:
        budget_raw = os.environ.get(_COLD_START_BUDGET_ENV)
        if budget_raw == "skip":
            self.skipTest(f"{_COLD_START_BUDGET_ENV}=skip")

        script = ROOT / "examples" / "js" / "hello.ts"
        t0 = time.monotonic()
        page = await launch(str(script), pause=False, mirror_console=False)
        try:
            # Arm the frame hook before requesting a redraw; hello.ts paints once at startup.
            await page._frame_counter()
            await page.request_redraw()
            await page._wait_for_frame_counter_advance(0, timeout_ms=10_000.0)
            elapsed_ms = (time.monotonic() - t0) * 1000.0
            print(f"cold-start TTFP: {elapsed_ms:.1f}ms", file=sys.stderr)
            budget = (
                _DEFAULT_COLD_START_BUDGET_MS
                if budget_raw is None
                else float(budget_raw)
            )
            self.assertLess(
                elapsed_ms,
                budget,
                f"cold start should reach first paint under {budget:.1f}ms; got {elapsed_ms:.1f}ms",
            )
        finally:
            await page.close()


if __name__ == "__main__":
    unittest.main()
