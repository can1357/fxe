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

_FIRST_PAINT_BUDGET_ENV = "FXE_FIRST_PAINT_BUDGET_MS"


class FirstPaintLatencyTests(unittest.IsolatedAsyncioTestCase):
    async def test_input_to_first_paint_budget(self) -> None:
        budget_raw = os.environ.get(_FIRST_PAINT_BUDGET_ENV)
        if budget_raw == "skip":
            self.skipTest(f"{_FIRST_PAINT_BUDGET_ENV}=skip")
        first_paint_budget_ms = 50.0 if budget_raw is None else float(budget_raw)
        cold_start_budget_ms = 1500.0
        page = await launch(
            str(ROOT / "examples" / "js" / "hello.ts"),
            pause=True,
            mirror_console=False,
            no_lazy=True,
        )
        launch_returned_at = time.monotonic()
        try:
            await page._frame_counter()
            await page.resume()
            await page.wait_for_resumed(timeout=1.0)
            await page._wait_for_frame_counter_advance(0, timeout_ms=cold_start_budget_ms)
            cold_start_ms = (time.monotonic() - launch_returned_at) * 1000.0
            # Tighten this toward ~250 ms once CI/dev variance is under control.
            self.assertLess(
                cold_start_ms,
                cold_start_budget_ms,
                f"cold start should reach first paint under {cold_start_budget_ms:.1f}ms; got {cold_start_ms:.2f}ms",
            )

            await asyncio.sleep(0.1)
            first_paint_ms = await page.first_paint_after_event(lambda: page.mouse.click(100, 100))
            # The real target is < 1 frame (~16.67 ms at 60 Hz); keep this loose
            # until CI is stable, then ratchet the env/default budget down.
            self.assertLess(
                first_paint_ms,
                first_paint_budget_ms,
                f"input-to-first-paint should stay under {first_paint_budget_ms:.1f}ms; got {first_paint_ms:.2f}ms",
            )
        finally:
            await page.close()


if __name__ == "__main__":
    unittest.main()
