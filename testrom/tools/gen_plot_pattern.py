#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

TILE_BYTES = 64


def pixel_value(x: int, y: int) -> int:
    # Exercise many bitplane combinations without using transparent color 0.
    return y * 8 + x + 1


def build_pattern() -> bytes:
    tile = bytearray(TILE_BYTES)
    for y in range(8):
        for plane in range(8):
            value = 0
            for x in range(8):
                value |= ((pixel_value(x, y) >> plane) & 1) << (7 - x)
            offset = ((plane >> 1) << 4) + (plane & 1) + (y << 1)
            tile[offset] = value
    return bytes(tile)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_pattern())
    print(f"Wrote {args.output} ({args.output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
