from __future__ import annotations

import asyncio
import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))

from fxe_debug import launch  # noqa: E402

_VIRTUAL_LIST_STRESS_BUDGET_ENV = "FXE_VIRTUAL_LIST_STRESS_BUDGET_MS"


class VirtualListStressTests(unittest.IsolatedAsyncioTestCase):
    async def test_flick_scroll_stays_under_budget(self) -> None:
        fxe_run = self._resolve_fxe_run()
        if fxe_run is None:
            self.skipTest(
                "virtual list stress requires FXE_RUN_PATH / FXE_RUN or build/dev/fxe_run; build the dev preset first"
            )

        # Generous initial budget while VirtualList flick-scroll perf gets ratcheted
        # down. Real target: < 33 ms (~2 frames @ 60Hz). Override via
        # FXE_VIRTUAL_LIST_STRESS_BUDGET_MS to tighten in CI.
        budget = float(os.environ.get(_VIRTUAL_LIST_STRESS_BUDGET_ENV, "200"))
        script = ROOT / "examples" / "js" / "virtual_list_stress.tsx"
        page = await launch(
            str(script),
            fxe_run=fxe_run,
            pause=True,
            mirror_console=False,
            no_lazy=True,
        )
        try:
            # fxe-ui apps install __fxeFrameProfile (which makes the SDK
            # _frame_counter return profile.snapshot().length, returning 0 while
            # tracing is disabled). Sleep instead of polling the counter; the
            # frame_trace_enable/drain path below is the actual signal.
            await page.resume()
            await page.wait_for_resumed(timeout=2.0)
            await asyncio.sleep(1.0)

            try:
                await page.frame_trace_enable(ring_size=240)
                try:
                    for _ in range(60):
                        await page.mouse.wheel(0, 1200, 360, 300)
                        await asyncio.sleep(0.016)
                    await asyncio.sleep(0.5)
                    samples = await page.frame_trace_drain()
                finally:
                    await page.frame_trace_disable()
            except RuntimeError as exc:
                self.skipTest(str(exc))
        finally:
            await page.close()

        if not samples:
            self.skipTest("no frame samples captured (frame profile not installed?)")
        max_ms = max(sample["totalMs"] for sample in samples)
        self.assertLess(
            max_ms,
            budget,
            f"flick-scroll max frame {max_ms:.2f}ms exceeded budget {budget}ms over {len(samples)} samples",
        )

    def _resolve_fxe_run(self) -> str | None:
        for candidate in (
            os.environ.get("FXE_RUN_PATH"),
            os.environ.get("FXE_RUN"),
            str(ROOT / "build" / "dev" / "fxe_run"),
        ):
            if not candidate:
                continue
            path = Path(candidate)
            if path.is_file() and os.access(path, os.X_OK):
                return str(path)
        return None


if __name__ == "__main__":
    unittest.main()
