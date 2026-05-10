from __future__ import annotations

import asyncio
import os
import struct
import sys
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))

from fxe_debug import ProtocolError, launch  # noqa: E402

_RETRY_DELAY_S = 0.05
_SPLASH_BG = "#112233"
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
class RunnerStartupSplashTests(unittest.IsolatedAsyncioTestCase):
    async def test_paused_launch_paints_splash_before_resume(self) -> None:
        page = await launch(
            str(ROOT / "examples" / "js" / "hello.ts"),
            pause=True,
            mirror_console=False,
            render_surface="window",
            env={"FXE_SPLASH_BG": _SPLASH_BG},
        )
        try:
            png = await self._capture_splash(page)
            pixel = decode_png_first_rgba(png)
            self.assertEqual(pixel, (0x11, 0x22, 0x33, 0xFF))
            self.assertNotEqual(pixel, (0x00, 0x00, 0x00, 0xFF))
            self.assertNotEqual(pixel, (0xFF, 0x00, 0xFF, 0xFF))
        finally:
            await page.close()

    async def _capture_splash(self, page, retries: int = 20) -> bytes:
        for _ in range(retries):
            try:
                data = await page.screenshot(return_bytes=True)
                if data:
                    return data
            except ProtocolError as exc:
                if exc.code != -32001:
                    raise
            await asyncio.sleep(_RETRY_DELAY_S)
        raise RuntimeError("startup splash screenshot never became available")


def decode_png_first_rgba(data: bytes) -> tuple[int, int, int, int]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError("screenshot is not a PNG")

    width = 0
    height = 0
    color_type = None
    bit_depth = None
    idat = bytearray()
    offset = 8
    while offset < len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        chunk_type = data[offset + 4 : offset + 8]
        chunk_data = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(
                ">IIBBBBB", chunk_data
            )
            if compression != 0 or filter_method != 0 or interlace != 0:
                raise AssertionError("unsupported PNG encoding")
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if width <= 0 or height <= 0 or color_type != 6 or bit_depth != 8:
        raise AssertionError("expected RGBA8 PNG")

    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    if len(raw) != height * (stride + 1):
        raise AssertionError("unexpected PNG payload size")

    rows = []
    prev = bytes(stride)
    cursor = 0
    for _ in range(height):
        filter_kind = raw[cursor]
        cursor += 1
        row = bytearray(raw[cursor : cursor + stride])
        cursor += stride
        if filter_kind == 0:
            pass
        elif filter_kind == 1:
            for i in range(stride):
                left = row[i - 4] if i >= 4 else 0
                row[i] = (row[i] + left) & 0xFF
        elif filter_kind == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif filter_kind == 3:
            for i in range(stride):
                left = row[i - 4] if i >= 4 else 0
                up = prev[i]
                row[i] = (row[i] + ((left + up) // 2)) & 0xFF
        elif filter_kind == 4:
            for i in range(stride):
                left = row[i - 4] if i >= 4 else 0
                up = prev[i]
                up_left = prev[i - 4] if i >= 4 else 0
                p = left + up - up_left
                pa = abs(p - left)
                pb = abs(p - up)
                pc = abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else up_left
                row[i] = (row[i] + predictor) & 0xFF
        else:
            raise AssertionError(f"unsupported PNG filter {filter_kind}")
        rows.append(bytes(row))
        prev = rows[-1]

    center = rows[height // 2]
    i = (width // 2) * 4
    return tuple(center[i : i + 4])  # type: ignore[return-value]


if __name__ == "__main__":
    unittest.main()
