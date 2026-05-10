from __future__ import annotations

import sys
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from fxe_debug.trace import frame_trace_enable, frame_trace_record  # noqa: E402


class FakePage:
    def __init__(self, responses: list[Any]) -> None:
        self._responses = list(responses)
        self.expressions: list[str] = []

    async def evaluate(self, expression: str) -> Any:
        self.expressions.append(expression)
        if not self._responses:
            raise AssertionError(f"unexpected evaluate call: {expression}")
        return self._responses.pop(0)


def _sample(frame_id: int, total_ms: float) -> dict[str, Any]:
    return {
        "frameId": frame_id,
        "startMs": 0.0,
        "totalMs": total_ms,
        "dtMs": 16.6,
        "phases": {
            "js": total_ms,
            "animations": 1.0,
            "reconcile": 2.0,
            "frameCallbacks": 0.5,
        },
    }


class FrameTraceTests(unittest.IsolatedAsyncioTestCase):
    async def test_frame_trace_enable_sends_enable_expression(self) -> None:
        page = FakePage([True])

        await frame_trace_enable(page)

        self.assertEqual(len(page.expressions), 1)
        self.assertIn("__fxeFrameProfile.enable", page.expressions[0])
        self.assertIn("ringSize: 240", page.expressions[0])

    async def test_frame_trace_record_aggregates_and_orders_calls(self) -> None:
        samples = [_sample(1, 8.0), _sample(2, 12.0)]
        page = FakePage([True, samples, True])
        slept: list[float] = []

        async def fake_sleep(seconds: float) -> None:
            slept.append(seconds)

        with patch("fxe_debug.trace.asyncio.sleep", new=fake_sleep):
            result = await frame_trace_record(page, duration_ms=1)

        self.assertEqual(slept, [0.001])
        self.assertEqual(len(page.expressions), 3)
        self.assertIn("__fxeFrameProfile.enable", page.expressions[0])
        self.assertIn("__fxeFrameProfile.drain", page.expressions[1])
        self.assertIn("__fxeFrameProfile.disable", page.expressions[2])
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["maxMs"], 12.0)
        self.assertEqual(result["avgMs"], 10.0)
        self.assertEqual(result["samples"], samples)

    async def test_frame_trace_record_raises_when_budget_exceeded(self) -> None:
        page = FakePage([True, [_sample(7, 12.5)], True])

        async def fake_sleep(_seconds: float) -> None:
            return None

        with patch("fxe_debug.trace.asyncio.sleep", new=fake_sleep):
            with self.assertRaisesRegex(
                AssertionError,
                r"frame budget exceeded: max=12\.50ms > 10ms over 1 samples",
            ):
                await frame_trace_record(page, duration_ms=1, max_frame_ms=10)

        self.assertEqual(len(page.expressions), 3)
        self.assertIn("__fxeFrameProfile.disable", page.expressions[2])


if __name__ == "__main__":
    unittest.main()
