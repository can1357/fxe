from __future__ import annotations

import asyncio
import os
import shutil
import struct
import sys
import unittest
import zlib
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))

from fxe_debug import ProtocolError, launch  # noqa: E402

_WINDOW_WIDTH = 320
_WINDOW_HEIGHT = 240
_RETRY_DELAY_S = 0.05
_SETTLE_DELAY_S = 0.15
_OUTLIER_FRACTION_BUDGET = 0.001
_MEAN_CHANNEL_BUDGET = 2.0
_MAX_CHANNEL_BUDGET = 8
_VARIANTS: tuple[str, ...] = (
    "button-default",
    "button-pressed",
    "textinput-empty",
    "textinput-focused",
    "pressable-hover",
    "scrollview-default",
    "view-bordered",
)


@dataclass(frozen=True)
class VariantAction:
    hover: tuple[float, float] | None = None
    press: tuple[float, float] | None = None
    focus: tuple[float, float] | None = None


_VARIANT_ACTIONS: dict[str, VariantAction] = {
    "button-default": VariantAction(),
    "button-pressed": VariantAction(hover=(160, 123), press=(160, 123)),
    "textinput-empty": VariantAction(),
    "textinput-focused": VariantAction(focus=(160, 123)),
    "pressable-hover": VariantAction(hover=(160, 123)),
    "scrollview-default": VariantAction(),
    "view-bordered": VariantAction(),
}


@dataclass(frozen=True)
class DecodedPng:
    width: int
    height: int
    rgba: bytes


@dataclass(frozen=True)
class DiffMetrics:
    mean_channel_l1: float
    outlier_pixels: int
    outlier_fraction: float
    max_channel_delta: int


class ComponentGoldenTests(unittest.IsolatedAsyncioTestCase):
    async def test_component_goldens(self) -> None:
        fxe_run = self._resolve_fxe_run()
        if fxe_run is None:
            self.skipTest(
                "component goldens require FXE_RUN_PATH / FXE_RUN or build/dev/fxe_run; build the dev preset first"
            )

        actual_dir = ROOT / "tests" / "golden" / "components"
        actual_dir.mkdir(parents=True, exist_ok=True)
        update = os.environ.get("FXE_GOLDEN_UPDATE") == "1"
        missing_goldens: list[str] = []
        failures: list[str] = []

        for variant in _VARIANTS:
            actual_path = actual_dir / f"{variant}.actual.png"
            golden_path = actual_dir / f"{variant}.png"
            await self._capture_variant(variant, actual_path, fxe_run)

            if update:
                shutil.copyfile(actual_path, golden_path)
                continue
            if not golden_path.is_file():
                missing_goldens.append(variant)
                continue

            metrics = compare_pngs(golden_path, actual_path)
            if not within_budget(metrics):
                failures.append(
                    f"{variant}: mean={metrics.mean_channel_l1:.3f}, "
                    f"outliers={metrics.outlier_pixels}/{_WINDOW_WIDTH * _WINDOW_HEIGHT} "
                    f"({metrics.outlier_fraction:.4%}), max={metrics.max_channel_delta}"
                )

        if failures:
            self.fail("component goldens exceeded tolerance budget:\n" + "\n".join(failures))
        if missing_goldens:
            self.skipTest(
                "component goldens missing for "
                + ", ".join(missing_goldens)
                + "; run `bun run build && FXE_GOLDEN_UPDATE=1 python -m unittest tests.test_component_goldens`"
            )

    async def _capture_variant(self, variant: str, actual_path: Path, fxe_run: str) -> None:
        page = await launch(
            str(ROOT / "examples" / "js" / "_golden_harness.tsx"),
            fxe_run=fxe_run,
            pause=True,
            mirror_console=False,
            no_lazy=True,
            env={"FXE_GOLDEN_COMPONENT": variant},
        )
        try:
            await page.resume()
            await page.wait_for_resumed(timeout=1.0)
            await asyncio.sleep(_SETTLE_DELAY_S)
            await self._apply_variant_action(page, variant)
            await shot(page, actual_path)
        finally:
            await page.close()

    async def _apply_variant_action(self, page, variant: str) -> None:
        action = _VARIANT_ACTIONS[variant]
        if action.hover is not None:
            await page.mouse.move(*action.hover)
            await asyncio.sleep(_RETRY_DELAY_S)
        if action.focus is not None:
            await page.mouse.click(*action.focus)
            await asyncio.sleep(_RETRY_DELAY_S)
        if action.press is not None:
            await page.mouse.down(*action.press)
            await asyncio.sleep(_RETRY_DELAY_S)

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


async def shot(page, path: Path, retries: int = 10) -> None:
    for _ in range(retries):
        try:
            await page.screenshot(path)
            return
        except ProtocolError as exc:
            if exc.code == -32001:
                await asyncio.sleep(_RETRY_DELAY_S)
                continue
            raise
    raise RuntimeError(f"no frame captured after retries for {path}")


def within_budget(metrics: DiffMetrics) -> bool:
    return (
        metrics.mean_channel_l1 <= _MEAN_CHANNEL_BUDGET
        and metrics.max_channel_delta <= _MAX_CHANNEL_BUDGET
        and metrics.outlier_fraction <= _OUTLIER_FRACTION_BUDGET
    )


def compare_pngs(expected_path: Path, actual_path: Path) -> DiffMetrics:
    expected = decode_png_rgba(expected_path)
    actual = decode_png_rgba(actual_path)
    if (expected.width, expected.height) != (actual.width, actual.height):
        raise AssertionError(
            f"image size mismatch: expected {expected.width}x{expected.height}, got {actual.width}x{actual.height}"
        )
    if len(expected.rgba) != len(actual.rgba):
        raise AssertionError("decoded byte lengths differ")

    total_delta = 0
    max_channel_delta = 0
    outlier_pixels = 0
    pixel_count = expected.width * expected.height

    for pixel_start in range(0, len(expected.rgba), 4):
        pixel_max = 0
        for channel in range(4):
            delta = abs(expected.rgba[pixel_start + channel] - actual.rgba[pixel_start + channel])
            total_delta += delta
            if delta > max_channel_delta:
                max_channel_delta = delta
            if delta > pixel_max:
                pixel_max = delta
        if pixel_max > _MAX_CHANNEL_BUDGET:
            outlier_pixels += 1

    mean_channel_l1 = total_delta / (pixel_count * 4)
    return DiffMetrics(
        mean_channel_l1=mean_channel_l1,
        outlier_pixels=outlier_pixels,
        outlier_fraction=outlier_pixels / pixel_count,
        max_channel_delta=max_channel_delta,
    )


def decode_png_rgba(path: Path) -> DecodedPng:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError(f"{path} is not a PNG")

    offset = 8
    width = height = 0
    bit_depth = color_type = interlace = None
    idat = bytearray()
    while offset < len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        chunk_type = data[offset + 4 : offset + 8]
        chunk_data = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(
                ">IIBBBBB", chunk_data
            )
            if compression != 0 or filter_method != 0:
                raise AssertionError(f"{path} uses unsupported PNG compression/filter mode")
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if width <= 0 or height <= 0 or bit_depth != 8 or color_type != 6 or interlace != 0:
        raise AssertionError(f"{path} must be non-interlaced RGBA8 PNG")

    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    expected_len = height * (stride + 1)
    if len(raw) != expected_len:
        raise AssertionError(f"{path} decoded to unexpected byte length {len(raw)} != {expected_len}")

    out = bytearray(height * stride)
    prev = bytearray(stride)
    for row in range(height):
        row_start = row * (stride + 1)
        filter_type = raw[row_start]
        scanline = bytearray(raw[row_start + 1 : row_start + 1 + stride])
        unfilter_scanline(scanline, prev, filter_type)
        out[row * stride : (row + 1) * stride] = scanline
        prev = scanline
    return DecodedPng(width=width, height=height, rgba=bytes(out))


def unfilter_scanline(scanline: bytearray, prev: bytearray, filter_type: int) -> None:
    if filter_type == 0:
        return
    if filter_type == 1:
        for i in range(4, len(scanline)):
            scanline[i] = (scanline[i] + scanline[i - 4]) & 0xFF
        return
    if filter_type == 2:
        for i in range(len(scanline)):
            scanline[i] = (scanline[i] + prev[i]) & 0xFF
        return
    if filter_type == 3:
        for i in range(len(scanline)):
            left = scanline[i - 4] if i >= 4 else 0
            up = prev[i]
            scanline[i] = (scanline[i] + ((left + up) >> 1)) & 0xFF
        return
    if filter_type == 4:
        for i in range(len(scanline)):
            left = scanline[i - 4] if i >= 4 else 0
            up = prev[i]
            up_left = prev[i - 4] if i >= 4 else 0
            scanline[i] = (scanline[i] + paeth(left, up, up_left)) & 0xFF
        return
    raise AssertionError(f"unsupported PNG filter type {filter_type}")


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c
