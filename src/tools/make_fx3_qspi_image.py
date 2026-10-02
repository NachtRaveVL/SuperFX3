#!/usr/bin/env python3
"""Build the fixed W25Q32 image containing firmware, saves, and FX-visible ROM."""

from __future__ import annotations

import argparse
from pathlib import Path

QSPI_FLASH_SIZE = 4 * 1024 * 1024
FX3_FIRMWARE_OFFSET = 0
FX3_FIRMWARE_SIZE = 496 * 1024
FX3_SAVE_OFFSET = FX3_FIRMWARE_OFFSET + FX3_FIRMWARE_SIZE
FX3_SAVE_SIZE = 528 * 1024
FX3_CODE_OFFSET = FX3_SAVE_OFFSET + FX3_SAVE_SIZE
FX3_CODE_SIZE = 3 * 1024 * 1024


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
            "Pack RP2350 firmware, an erased save journal, and FX-visible ROM "
            "into the fixed three-part W25Q32 QSPI image."
        )
    )
    parser.add_argument("firmware", type=Path, help="RP2350 firmware .bin")
    parser.add_argument("fx_code", type=Path, help="prepared linear FX-visible ROM image, up to 3 MiB")
    parser.add_argument("output", type=Path, help="combined raw flash image")
    parser.add_argument("--fx3-rom", action="store_true",
                        help="derive FX-visible ROM from a canonical or production-dump FX3 ROM")
    parser.add_argument(
        "--flash-size",
        type=parse_size,
        default=QSPI_FLASH_SIZE,
        help="physical QSPI flash size; the production layout requires 4M",
    )
    parser.add_argument(
        "--rom-offset",
        type=parse_size,
        default=None,
        help="compatibility check; production FX code offset is fixed at 0x100000",
    )
    args = parser.parse_args()

    firmware = args.firmware.read_bytes()
    fx_code = args.fx_code.read_bytes()
    if args.fx3_rom:
        from make_snes_rom_image import fx3_payload

        try:
            fx_code = fx3_payload(fx_code)
        except ValueError as exc:
            parser.error(str(exc))

    if args.flash_size != QSPI_FLASH_SIZE:
        parser.error("the production SuperFX3 QSPI layout requires exactly 4 MiB")
    if len(fx_code) > FX3_CODE_SIZE:
        parser.error(f"FX code is {len(fx_code)} bytes; maximum is {FX3_CODE_SIZE}")

    code_offset = FX3_CODE_OFFSET if args.rom_offset is None else args.rom_offset

    if code_offset != FX3_CODE_OFFSET:
        parser.error("the production FX code partition must start at 0x100000")
    if len(firmware) > FX3_FIRMWARE_SIZE:
        parser.error(
            f"firmware ends at 0x{len(firmware):X}, overlapping the save journal "
            f"at 0x{FX3_SAVE_OFFSET:X}"
        )
    if FX3_CODE_OFFSET != FX3_SAVE_OFFSET + FX3_SAVE_SIZE:
        parser.error("FX code partition does not immediately follow the save journal")
    if FX3_CODE_OFFSET + FX3_CODE_SIZE != QSPI_FLASH_SIZE:
        parser.error("the three QSPI partitions do not cover the W25Q32")

    image = bytearray(b"\xFF" * QSPI_FLASH_SIZE)
    image[: len(firmware)] = firmware
    image[FX3_CODE_OFFSET : FX3_CODE_OFFSET + len(fx_code)] = fx_code
    args.output.write_bytes(image)

    print(f"Firmware : 0x000000-0x{len(firmware) - 1:06X} ({len(firmware)} bytes)")
    print(
        f"Saves    : 0x{FX3_SAVE_OFFSET:06X}-"
        f"0x{FX3_SAVE_OFFSET + FX3_SAVE_SIZE - 1:06X} (erased journal)"
    )
    print(
        f"FX code  : 0x{FX3_CODE_OFFSET:06X}-"
        f"0x{FX3_CODE_OFFSET + FX3_CODE_SIZE - 1:06X} "
        f"({len(fx_code)} bytes used, padded with 0xFF)"
    )
    print(f"Flash    : {QSPI_FLASH_SIZE} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
