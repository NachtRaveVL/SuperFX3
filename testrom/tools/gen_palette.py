#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

PALETTE_COLORS = 256
PALETTE_BYTES = PALETTE_COLORS * 2


def diagnostic_color(index: int) -> int:
    """Return a deterministic SNES BGR555 color for one 8bpp palette index."""
    if index == 0:
        return 0x0000
    if index == 1:
        return 0x7FFF

    # Spread the eight index bits across all three channels. Apart from color 0,
    # every entry gets some visible energy and nearby values are still visually
    # distinct enough to make bad pixel indices obvious on hardware.
    red = index & 0x1F
    green = ((index >> 3) | (index << 2)) & 0x1F
    blue = ((index >> 1) | (index << 4)) & 0x1F
    return red | (green << 5) | (blue << 10)


def build_palette() -> bytes:
    out = bytearray()
    for index in range(PALETTE_COLORS):
        out += diagnostic_color(index).to_bytes(2, "little")
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_palette())
    print(f"Wrote {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
