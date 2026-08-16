#!/usr/bin/env python3
"""Build one RP2350 QSPI image containing firmware plus the 3 MiB FX3 ROM."""

from __future__ import annotations

import argparse
from pathlib import Path

FX3_ROM_SIZE = 3 * 1024 * 1024
DEFAULT_FLASH_SIZE = 4 * 1024 * 1024


def parse_size(value: str) -> int:
    text = value.strip().lower()
    multiplier = 1
    if text.endswith("m"):
        multiplier = 1024 * 1024
        text = text[:-1]
    elif text.endswith("k"):
        multiplier = 1024
        text = text[:-1]
    return int(text, 0) * multiplier


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Pack an RP2350 firmware .bin and a linear FX3 ROM image into the "
            "same primary QSPI flash image."
        )
    )
    parser.add_argument("firmware", type=Path, help="RP2350 firmware .bin")
    parser.add_argument("fx_rom", type=Path, help="linear FX3 ROM image, up to 3 MiB")
    parser.add_argument("output", type=Path, help="combined raw flash image")
    parser.add_argument(
        "--flash-size",
        type=parse_size,
        default=DEFAULT_FLASH_SIZE,
        help="physical QSPI flash size, e.g. 4M or 8M (default: 4M)",
    )
    parser.add_argument(
        "--rom-offset",
        type=parse_size,
        default=None,
        help="override FX3 ROM flash offset; default is the top 3 MiB of flash",
    )
    args = parser.parse_args()

    firmware = args.firmware.read_bytes()
    fx_rom = args.fx_rom.read_bytes()

    if args.flash_size < DEFAULT_FLASH_SIZE:
        parser.error("FX3 requires at least 4 MiB of QSPI flash for firmware plus the 3 MiB ROM")
    if len(fx_rom) > FX3_ROM_SIZE:
        parser.error(f"FX3 ROM is {len(fx_rom)} bytes; maximum is {FX3_ROM_SIZE}")

    rom_offset = (
        args.flash_size - FX3_ROM_SIZE
        if args.rom_offset is None
        else args.rom_offset
    )

    if rom_offset & 0xFFF:
        parser.error("FX3 ROM offset must be aligned to a 4 KiB flash sector")
    if rom_offset < len(firmware):
        parser.error(
            f"firmware ends at 0x{len(firmware):X}, overlapping FX3 ROM at 0x{rom_offset:X}"
        )
    if rom_offset + FX3_ROM_SIZE > args.flash_size:
        parser.error("FX3 ROM partition extends beyond the selected flash size")

    image = bytearray(b"\xFF" * args.flash_size)
    image[: len(firmware)] = firmware
    image[rom_offset : rom_offset + len(fx_rom)] = fx_rom
    args.output.write_bytes(image)

    print(f"Firmware : 0x000000-0x{len(firmware) - 1:06X} ({len(firmware)} bytes)")
    print(
        f"FX3 ROM  : 0x{rom_offset:06X}-0x{rom_offset + FX3_ROM_SIZE - 1:06X} "
        f"({len(fx_rom)} bytes used, padded with 0xFF)"
    )
    print(f"Flash    : {args.flash_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
