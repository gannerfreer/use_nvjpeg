#!/usr/bin/env python3
"""Generate one tightly packed YUY2 or UYVY test frame."""

import argparse
from pathlib import Path
from typing import Iterable, Tuple


RGB = Tuple[int, int, int]


def clamp(value: float, low: int, high: int) -> int:
    return max(low, min(high, int(round(value))))


def rgb_to_bt709_limited(rgb: RGB) -> Tuple[int, int, int]:
    """Convert 8-bit RGB to limited-range BT.709 Y'CbCr."""
    red, green, blue = rgb
    y_value = 16.0 + 0.182586 * red + 0.614231 * green + 0.062007 * blue
    u_value = 128.0 - 0.100644 * red - 0.338572 * green + 0.439216 * blue
    v_value = 128.0 + 0.439216 * red - 0.398942 * green - 0.040274 * blue
    return (
        clamp(y_value, 16, 235),
        clamp(u_value, 16, 240),
        clamp(v_value, 16, 240),
    )


def pack_pair(y0: int, u: int, y1: int, v: int, pixel_format: str) -> bytes:
    if pixel_format == "yuy2":
        return bytes((y0, u, y1, v))
    return bytes((u, y0, v, y1))


def color_bar_row(width: int, pixel_format: str) -> bytes:
    # White, yellow, cyan, green, magenta, red, blue, black. Boundaries are
    # calculated in units of two pixels so each pair has a single chroma value.
    colors: Iterable[RGB] = (
        (255, 255, 255),
        (255, 255, 0),
        (0, 255, 255),
        (0, 255, 0),
        (255, 0, 255),
        (255, 0, 0),
        (0, 0, 255),
        (0, 0, 0),
    )
    converted = [rgb_to_bt709_limited(color) for color in colors]
    pair_count = width // 2
    row = bytearray(width * 2)
    for pair_index in range(pair_count):
        bar_index = min(7, pair_index * 8 // pair_count)
        y_value, u_value, v_value = converted[bar_index]
        offset = pair_index * 4
        row[offset : offset + 4] = pack_pair(
            y_value, u_value, y_value, v_value, pixel_format
        )
    return bytes(row)


def grayscale_row(width: int, pixel_format: str) -> bytes:
    pair_count = width // 2
    row = bytearray(width * 2)
    for pair_index in range(pair_count):
        x0 = pair_index * 2
        x1 = x0 + 1
        y0 = 16 + (219 * x0) // max(1, width - 1)
        y1 = 16 + (219 * x1) // max(1, width - 1)
        offset = pair_index * 4
        row[offset : offset + 4] = pack_pair(y0, 128, y1, 128, pixel_format)
    return bytes(row)


def generate_frame(width: int, height: int, pixel_format: str) -> bytes:
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    if width % 2 != 0 or height % 2 != 0:
        raise ValueError("width and height must both be even")

    bars = color_bar_row(width, pixel_format)
    ramp = grayscale_row(width, pixel_format)
    bar_rows = (height * 3) // 4
    return bars * bar_rows + ramp * (height - bar_rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate one packed BT.709 limited-range YUV422 test frame."
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument(
        "--format", choices=("yuy2", "uyvy"), default="yuy2", dest="pixel_format"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        frame = generate_frame(args.width, args.height, args.pixel_format)
    except ValueError as error:
        raise SystemExit(f"error: {error}") from error

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(frame)
    print(
        f"wrote {args.output}: {args.width}x{args.height} "
        f"{args.pixel_format.upper()}, {len(frame)} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
