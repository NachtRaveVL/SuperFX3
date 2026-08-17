#!/usr/bin/env python3
from __future__ import annotations

import argparse
from enum import Enum
from pathlib import Path

MIB = 1024 * 1024
KIB = 1024
BUS_IMAGE_SIZE = 16 * MIB
BUS_HALF_SIZE = 8 * MIB
PAGE_SIZE = 0x1000
CHIP_SIZE_MBIT_CHOICES = (1, 2, 4, 8, 16, 32, 64)


class RomMap(Enum):
    LOROM = "lorom"
    HIROM = "hirom"
    EXLOROM = "exlorom"
    EXHIROM = "exhirom"
    SUPERFX_EXTENDED = "superfx-extended"
    RAW = "raw"


class ImageCapacityError(ValueError):
    pass


def mirror_offset(size: int, offset: int) -> int:
    if size <= 0:
        raise ValueError("ROM image is empty")
    if offset < size:
        return offset

    mask = 1 << (offset.bit_length() - 1)
    if size <= (offset & mask):
        return mirror_offset(size, offset - mask)
    return mask + mirror_offset(size - mask, offset - mask)


def standard_rom_window(address: int) -> bool:
    bank = (address >> 16) & 0xFF
    addr = address & 0xFFFF

    if bank in (0x7E, 0x7F):
        return False
    if bank <= 0x3F or 0x80 <= bank <= 0xBF:
        return addr >= 0x8000
    return True


def source_limit(mapping: RomMap) -> int:
    if mapping in (RomMap.LOROM, RomMap.HIROM):
        return 4 * MIB
    if mapping in (RomMap.EXLOROM, RomMap.EXHIROM):
        return 8 * MIB
    if mapping == RomMap.SUPERFX_EXTENDED:
        return 11 * MIB
    return BUS_IMAGE_SIZE


def validate_source(rom: bytes, mapping: RomMap) -> None:
    if not rom:
        raise ValueError("ROM image is empty")
    if len(rom) > source_limit(mapping):
        raise ValueError(
            f"{mapping.value} supports source images up to {source_limit(mapping) // MIB} MiB"
        )
    if mapping != RomMap.RAW and len(rom) % 0x8000:
        raise ValueError("SNES ROM size must be a multiple of 32 KiB")
    if mapping == RomMap.SUPERFX_EXTENDED and len(rom) != 11 * MIB:
        raise ValueError("superfx-extended currently models the 11 MiB Snes9x SuperFX layout")


# Matches the standard LoROM, HiROM, ExLoROM, and ExHiROM layouts used by SNES emulators.
def standard_linear_offset(mapping: RomMap, address: int) -> int:
    bank = (address >> 16) & 0xFF
    addr = address & 0xFFFF

    if mapping == RomMap.LOROM:
        return ((bank & 0x7F) << 15) | (addr & 0x7FFF)
    if mapping == RomMap.HIROM:
        return ((bank & 0x3F) << 16) | addr
    if mapping == RomMap.EXLOROM:
        return ((bank ^ 0x80) << 15) | (addr & 0x7FFF)
    if mapping == RomMap.EXHIROM:
        return ((bank & 0x3F) << 16) | addr | (0x400000 if bank < 0x80 else 0)

    raise ValueError(f"{mapping.value} is not a standard SNES ROM map")


def superfx_extended_offset(address: int) -> int | None:
    bank = (address >> 16) & 0xFF
    addr = address & 0xFFFF

    # Matches Snes9x's 11 MiB SuperFX CPU map. The GSU itself still sees its own ROM window.
    if bank <= 0x3F and addr >= 0x8000:
        return (bank << 15) | (addr & 0x7FFF)
    if 0x80 <= bank <= 0xBF and addr >= 0x8000:
        return 0x200000 + ((bank - 0x80) << 15) + (addr & 0x7FFF)
    if 0x40 <= bank <= 0x6F:
        return 0x800000 + ((bank - 0x40) << 16) + addr
    if 0xC0 <= bank <= 0xFF:
        return 0x400000 + ((bank - 0xC0) << 16) + addr
    return None


def rom_offset(mapping: RomMap, address: int, rom_size: int) -> int | None:
    if not 0 <= address < BUS_IMAGE_SIZE:
        return None

    if mapping == RomMap.RAW:
        return address if address < rom_size else None

    if mapping == RomMap.SUPERFX_EXTENDED:
        offset = superfx_extended_offset(address)
        return offset if offset is not None and offset < rom_size else None

    if not standard_rom_window(address):
        return None

    return mirror_offset(rom_size, standard_linear_offset(mapping, address))


def source_page(rom: bytes, mapping: RomMap, address: int) -> bytes | None:
    offset = rom_offset(mapping, address, len(rom))
    if offset is None:
        return None

    if mapping == RomMap.RAW:
        return rom[offset:offset + PAGE_SIZE].ljust(PAGE_SIZE, b"\xFF")

    if offset + PAGE_SIZE > len(rom):
        raise ValueError("mapped ROM page crosses the end of the source image")
    return rom[offset:offset + PAGE_SIZE]


def build_bus_image(rom: bytes, mapping: RomMap) -> bytearray:
    validate_source(rom, mapping)
    image = bytearray(b"\xFF") * BUS_IMAGE_SIZE

    for address in range(0, BUS_IMAGE_SIZE, PAGE_SIZE):
        page = source_page(rom, mapping, address)
        if page is not None:
            image[address:address + PAGE_SIZE] = page

    return image


def chip_size_bytes(size_mbit: int) -> int:
    if size_mbit not in CHIP_SIZE_MBIT_CHOICES:
        choices = ", ".join(str(value) for value in CHIP_SIZE_MBIT_CHOICES)
        raise ValueError(f"parallel ROM size must be one of: {choices} Mbit")
    return size_mbit * MIB // 8


def build_chip_images(
    rom: bytes,
    mapping: RomMap,
    chip_size: int,
    rom_count: int,
) -> list[bytearray]:
    validate_source(rom, mapping)

    if rom_count not in (1, 2):
        raise ValueError("parallel ROM count must be 1 or 2")
    if chip_size < 128 * KIB or chip_size > BUS_HALF_SIZE or chip_size & (chip_size - 1):
        raise ValueError("parallel ROM size must be a power of two from 1 Mbit through 64 Mbit")

    images = [bytearray(b"\xFF") * chip_size for _ in range(rom_count)]
    assigned: list[dict[int, bytes]] = [dict() for _ in range(rom_count)]

    for address in range(0, BUS_IMAGE_SIZE, PAGE_SIZE):
        page = source_page(rom, mapping, address)
        if page is None:
            continue

        chip = (address >> 23) & 1 if rom_count == 2 else 0
        physical = (address & (BUS_HALF_SIZE - 1)) & (chip_size - 1)
        page_index = physical // PAGE_SIZE
        previous = assigned[chip].get(page_index)

        if previous is not None and previous != page:
            raise ImageCapacityError(
                f"striped bus addresses alias conflicting data in ROM{chip} at 0x{physical:06X}"
            )

        assigned[chip][page_index] = page
        images[chip][physical:physical + PAGE_SIZE] = page

    return images


def minimum_chip_size_mbit(rom: bytes, mapping: RomMap, rom_count: int) -> int | None:
    for size_mbit in CHIP_SIZE_MBIT_CHOICES:
        try:
            build_chip_images(rom, mapping, chip_size_bytes(size_mbit), rom_count)
            return size_mbit
        except ImageCapacityError:
            continue
    return None


def load_rom(path: Path) -> bytes:
    rom = path.read_bytes()
    if len(rom) % 0x8000 == 512:
        rom = rom[512:]
    return rom


def default_rom1_output(path: Path) -> Path:
    suffix = path.suffix or ".bin"
    stem = path.stem if path.suffix else path.name
    return path.with_name(f"{stem}.rom1{suffix}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stripe a SNES ROM for the direct A0-A23 parallel-flash cartridge bus."
    )
    parser.add_argument("rom", type=Path, help="input .sfc/.smc ROM or raw bus image")
    parser.add_argument("output", type=Path, help="ROM0 programming image")
    parser.add_argument(
        "--map",
        dest="mapping",
        choices=[mapping.value for mapping in RomMap],
        required=True,
        help="SNES mapping used by the source image",
    )
    parser.add_argument(
        "--chip-size-mbit",
        type=int,
        choices=CHIP_SIZE_MBIT_CHOICES,
        default=64,
        help="capacity of each populated parallel ROM in Mbit (default: 64)",
    )
    parser.add_argument(
        "--rom-count",
        type=int,
        choices=(1, 2),
        default=1,
        help="number of populated parallel ROMs (default: 1)",
    )
    parser.add_argument(
        "--rom1-output",
        type=Path,
        help="ROM1 programming image when --rom-count=2",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    mapping = RomMap(args.mapping)
    rom = args.rom.read_bytes() if mapping == RomMap.RAW else load_rom(args.rom)
    chip_size = chip_size_bytes(args.chip_size_mbit)

    try:
        images = build_chip_images(rom, mapping, chip_size, args.rom_count)
    except ImageCapacityError as exc:
        minimum = minimum_chip_size_mbit(rom, mapping, args.rom_count)
        if minimum is None and args.rom_count == 1:
            raise SystemExit(f"{exc}; this mapping requires the optional second ROM") from exc
        if minimum is not None:
            raise SystemExit(f"{exc}; use at least {minimum} Mbit per ROM") from exc
        raise SystemExit(str(exc)) from exc

    args.output.write_bytes(images[0])
    rom1_output = None
    if args.rom_count == 2:
        rom1_output = args.rom1_output or default_rom1_output(args.output)
        rom1_output.write_bytes(images[1])
    elif args.rom1_output is not None:
        raise SystemExit("--rom1-output requires --rom-count=2")

    minimum = minimum_chip_size_mbit(rom, mapping, args.rom_count)
    print(f"Map: {mapping.value}")
    print(f"Source ROM: {len(rom)} bytes")
    print(f"ROM0: {args.output} ({len(images[0])} bytes)")
    if rom1_output is not None:
        print(f"ROM1: {rom1_output} ({len(images[1])} bytes)")
    if minimum is not None:
        print(f"Minimum device size for this population: {minimum} Mbit")


if __name__ == "__main__":
    main()
