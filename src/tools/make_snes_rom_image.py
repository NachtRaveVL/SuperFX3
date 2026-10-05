#!/usr/bin/env python3
from __future__ import annotations

import argparse
import struct
from enum import Enum
from pathlib import Path

MIB = 1024 * 1024
KIB = 1024
BUS_IMAGE_SIZE = 16 * MIB
PAGE_SIZE = 0x1000
CHIP_SIZE_MBIT_CHOICES = (8, 16, 32, 64, 128)
DESCRIPTOR_ADDRESS = 0x7E0000
FX3_TYPE = 0x17
FX3_BATTERY_TYPE = 0x18


class RomMap(Enum):
    LOROM = "lorom"
    HIROM = "hirom"
    EXLOROM = "exlorom"
    EXHIROM = "exhirom"
    FX3 = "fx3"
    SUPERFX_EXTENDED = "superfx-extended"
    RAW = "raw"


class ImageCapacityError(ValueError):
    pass


def header_score(rom: bytes, offset: int, mapping: RomMap) -> int:
    if offset < 0 or offset + 0x40 > len(rom):
        return -1
    map_mode = rom[offset + 0x15]
    complement = int.from_bytes(rom[offset + 0x1C:offset + 0x1E], "little")
    checksum = int.from_bytes(rom[offset + 0x1E:offset + 0x20], "little")
    reset_vector = int.from_bytes(rom[offset + 0x3C:offset + 0x3E], "little")
    score = 0
    if checksum not in (0, 0xFFFF) and checksum ^ complement == 0xFFFF:
        score += 8
    hirom = mapping in (RomMap.HIROM, RomMap.EXHIROM)
    if bool(map_mode & 1) == hirom:
        score += 4
    if mapping == RomMap.EXHIROM and map_mode & 0x0F == 0x05:
        score += 3
    if mapping == RomMap.EXLOROM and map_mode & 0x0F in (0x00, 0x02):
        score += 2
    if map_mode & 0x20:
        score += 1
    if reset_vector >= 0x8000:
        score += 2
    title = rom[offset:offset + 21]
    if sum(value == 0 or value == 0x20 or 0x21 <= value <= 0x7E for value in title) >= 19:
        score += 2
    return score


def source_header_score(rom: bytes, mapping: RomMap) -> int:
    if mapping in (RomMap.LOROM, RomMap.FX3):
        return header_score(rom, 0x7FC0, RomMap.LOROM)
    if mapping == RomMap.HIROM:
        return header_score(rom, 0xFFC0, mapping)
    if mapping == RomMap.EXLOROM:
        return max(header_score(rom, 0x7FC0, mapping),
                   header_score(rom, 0x407FC0, mapping))
    if mapping == RomMap.EXHIROM:
        return header_score(rom, 0x40FFC0, mapping)
    if mapping == RomMap.SUPERFX_EXTENDED:
        return max(header_score(rom, 0x7FC0, RomMap.LOROM),
                   header_score(rom, 0x407FC0, RomMap.LOROM))
    return -1


def best_header_offset(rom: bytes, mapping: RomMap) -> int | None:
    if mapping in (RomMap.LOROM, RomMap.FX3):
        offsets = (0x7FC0,)
    elif mapping == RomMap.HIROM:
        offsets = (0xFFC0,)
    elif mapping == RomMap.EXLOROM:
        offsets = (0x7FC0, 0x407FC0)
    elif mapping == RomMap.EXHIROM:
        offsets = (0x40FFC0,)
    elif mapping == RomMap.SUPERFX_EXTENDED:
        offsets = (0x7FC0, 0x407FC0)
    else:
        return None

    candidates = [(header_score(rom, offset, mapping), offset)
                  for offset in offsets if offset + 0x40 <= len(rom)]
    if not candidates:
        return None
    best_score = max(score for score, _ in candidates)
    if best_score < 8:
        return None
    offsets = {offset for score, offset in candidates if score == best_score}
    if len({rom[offset + 0x17] for offset in offsets}) != 1:
        raise ValueError("ambiguous SNES ROM size headers")
    return min(offsets)


def declared_rom_size(rom: bytes, mapping: RomMap) -> int | None:
    offset = best_header_offset(rom, mapping)
    if offset is None:
        return None
    code = rom[offset + 0x17]
    if code < 0x07 or code > 0x0E:
        return None
    return 1 << (code + 10)


def declared_sram(rom: bytes, mapping: RomMap) -> tuple[int, int]:
    offset = best_header_offset(rom, mapping)
    if offset is None:
        return 128 * KIB, FX3_BATTERY_TYPE
    cartridge_type = rom[offset + 0x16]
    code = rom[offset + 0x18]
    if 0x13 <= cartridge_type <= 0x18 and rom[offset - 3] <= 7:
        code = rom[offset - 3]
    if code == 0:
        return 0, cartridge_type
    return (1 << (code + 10) if code <= 7 else 128 * KIB), cartridge_type


def strip_copier_header(rom: bytes, mapping: RomMap) -> bytes:
    plain_score = source_header_score(rom, mapping)
    shifted_score = source_header_score(rom[512:], mapping) if len(rom) > 512 else -1
    plain_valid = plain_score >= 8
    shifted_valid = shifted_score >= 8
    if plain_valid and shifted_valid:
        if plain_score == shifted_score:
            raise ValueError("ambiguous 512-byte copier header")
        return rom[512:] if shifted_score > plain_score else rom
    if shifted_valid:
        return rom[512:]
    return rom


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
    if mapping in (RomMap.LOROM, RomMap.HIROM, RomMap.FX3):
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


def normalize_fx3_rom(rom: bytes) -> bytes:
    if len(rom) in (8 * MIB, 8 * MIB + 256, 8 * MIB + 512, 8 * MIB + 768):
        plain_base = 4 * MIB
        shifted_base = 512 + 4 * MIB
        plain_score = header_score(rom, plain_base + 0x7FC0, RomMap.LOROM)
        shifted_score = header_score(rom, shifted_base + 0x7FC0, RomMap.LOROM)
        if shifted_score >= 8 and shifted_score > plain_score:
            rom = rom[512:]
        elif plain_score >= 8 and shifted_score >= 8 and plain_score == shifted_score:
            raise ValueError("ambiguous 512-byte copier header")
    else:
        rom = strip_copier_header(rom, RomMap.FX3)
    if len(rom) == 8 * MIB + 256 and rom[-256:] == b"\xFF" * 256:
        rom = rom[:-256]
    if len(rom) == 8 * MIB:
        canonical = rom[4 * MIB:]
        for offset in range(0, 2 * MIB, 0x8000):
            block = canonical[offset:offset + 0x8000]
            if rom[offset * 2:offset * 2 + 0x10000] != block * 2:
                raise ValueError("invalid FX3 physical mirror region")
        rom = canonical
    validate_source(rom, RomMap.FX3)
    if source_header_score(rom, RomMap.FX3) < 8 or \
            rom[0x7FD6] not in (FX3_TYPE, FX3_BATTERY_TYPE) or \
            rom[0x7FD7] != (len(rom) - 1).bit_length() - 10:
        raise ValueError("FX3 requires ROM type $17/$18 and a matching header size")
    return rom


def fx3_payload(rom: bytes) -> bytes:
    rom = normalize_fx3_rom(rom)
    image = bytearray()
    for offset in range(0, 3 * MIB, PAGE_SIZE):
        source = mirror_offset(len(rom), offset)
        image.extend(rom[source:source + PAGE_SIZE])
    return bytes(image)


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

    if mapping == RomMap.FX3:
        if address >> 16 in (0x7E, 0x7F):
            return None
        bank = (address >> 16) & 0x7F
        offset = ((bank << 15) | (address & 0x7FFF)) if bank < 0x40 else \
            ((bank - 0x40) << 16) | (address & 0xFFFF)
        return mirror_offset(rom_size, offset)

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
    if mapping == RomMap.FX3:
        rom = normalize_fx3_rom(rom)
    validate_source(rom, mapping)
    image = bytearray(b"\xFF") * BUS_IMAGE_SIZE

    for address in range(0, BUS_IMAGE_SIZE, PAGE_SIZE):
        page = source_page(rom, mapping, address)
        if page is not None:
            image[address:address + PAGE_SIZE] = page

    add_descriptor(image, mapping, rom)
    return image


def add_descriptor(image: bytearray, mapping: RomMap, rom: bytes) -> None:
    # Value 4 belongs to the descriptor-less Fx3Physical fallback in firmware.
    maps = (RomMap.LOROM, RomMap.HIROM, RomMap.EXLOROM, RomMap.EXHIROM, None, RomMap.FX3)
    if len(image) != BUS_IMAGE_SIZE or mapping not in maps:
        return
    mode = maps.index(mapping)
    ram_size, cartridge_type = declared_sram(rom, mapping)
    ram_code = 0 if not ram_size else ram_size.bit_length() - 11
    attributes = 0x80000000 | cartridge_type << 16 | ram_code << 8 | mode
    magic = 0x504D3353
    image[DESCRIPTOR_ADDRESS:DESCRIPTOR_ADDRESS + 16] = struct.pack(
        "<IIII", magic, attributes, len(rom),
        ~(magic ^ attributes ^ len(rom)) & 0xFFFFFFFF
    )


def chip_size_bytes(size_mbit: int) -> int:
    if size_mbit not in CHIP_SIZE_MBIT_CHOICES:
        choices = ", ".join(str(value) for value in CHIP_SIZE_MBIT_CHOICES)
        raise ValueError(f"parallel ROM size must be one of: {choices} Mbit")
    return size_mbit * MIB // 8


def build_chip_image(rom: bytes, mapping: RomMap, chip_size: int) -> bytearray:
    if mapping == RomMap.FX3:
        rom = normalize_fx3_rom(rom)
    validate_source(rom, mapping)

    if chip_size < MIB or chip_size > BUS_IMAGE_SIZE or chip_size & (chip_size - 1):
        raise ValueError("parallel ROM size must be a power of two from 8 Mbit through 128 Mbit")

    image = bytearray(b"\xFF") * chip_size
    assigned: dict[int, bytes] = {}

    for address in range(0, BUS_IMAGE_SIZE, PAGE_SIZE):
        page = source_page(rom, mapping, address)
        if page is None:
            continue

        physical = address & (chip_size - 1)
        page_index = physical // PAGE_SIZE
        previous = assigned.get(page_index)

        if previous is not None and previous != page:
            raise ImageCapacityError(
                f"bus addresses alias conflicting data in the ROM at 0x{physical:06X}"
            )

        assigned[page_index] = page
        image[physical:physical + PAGE_SIZE] = page

    add_descriptor(image, mapping, rom)
    return image


def minimum_chip_size_mbit(rom: bytes, mapping: RomMap) -> int | None:
    for size_mbit in CHIP_SIZE_MBIT_CHOICES:
        try:
            build_chip_image(rom, mapping, chip_size_bytes(size_mbit))
            return size_mbit
        except ImageCapacityError:
            continue
    return None


def minimum_mapping_size_mbit(rom_size: int, mapping: RomMap) -> int | None:
    for size_mbit in CHIP_SIZE_MBIT_CHOICES:
        chip_size = chip_size_bytes(size_mbit)
        assigned: dict[int, int] = {}
        fits = True
        for address in range(0, BUS_IMAGE_SIZE, PAGE_SIZE):
            source = rom_offset(mapping, address, rom_size)
            if source is None:
                continue
            physical_page = (address & (chip_size - 1)) // PAGE_SIZE
            source_page_index = source // PAGE_SIZE
            previous = assigned.get(physical_page)
            if previous is not None and previous != source_page_index:
                fits = False
                break
            assigned[physical_page] = source_page_index
        if fits:
            return size_mbit
    return None


def infer_chip_size_mbit(rom: bytes, mapping: RomMap) -> int:
    if mapping == RomMap.RAW:
        for size_mbit in CHIP_SIZE_MBIT_CHOICES:
            if len(rom) == chip_size_bytes(size_mbit):
                return size_mbit
        raise ImageCapacityError("raw image size does not match an 8, 16, 32, 64, or 128 Mbit ROM")

    minimum = minimum_mapping_size_mbit(len(rom), mapping)
    if minimum is None:
        raise ImageCapacityError("ROM mapping does not fit any supported parallel ROM")
    intended_size = declared_rom_size(rom, mapping) or 0
    required_size = max(chip_size_bytes(minimum), intended_size)
    for size_mbit in CHIP_SIZE_MBIT_CHOICES:
        if chip_size_bytes(size_mbit) >= required_size:
            return size_mbit
    raise ImageCapacityError("ROM header declares a size larger than 128 Mbit")


def load_rom(path: Path, mapping: RomMap) -> bytes:
    return strip_copier_header(path.read_bytes(), mapping)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stripe a SNES ROM for the direct A0-A23 parallel-flash cartridge bus."
    )
    parser.add_argument("rom", type=Path, help="input .sfc/.smc ROM or raw bus image")
    parser.add_argument("output", type=Path, help="single parallel-ROM programming image")
    parser.add_argument("--fx-rom-output", type=Path, help="write the 3 MiB FX-visible ROM (requires --map fx3)")
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
        help="capacity of the single parallel ROM in Mbit (inferred when omitted)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    mapping = RomMap(args.mapping)
    rom = args.rom.read_bytes() if mapping == RomMap.RAW else load_rom(args.rom, mapping)
    if args.fx_rom_output and mapping != RomMap.FX3:
        raise SystemExit("--fx-rom-output requires --map fx3")
    if mapping == RomMap.FX3:
        rom = normalize_fx3_rom(rom)
    try:
        size_mbit = args.chip_size_mbit if args.chip_size_mbit is not None else \
            infer_chip_size_mbit(rom, mapping)
        chip_size = chip_size_bytes(size_mbit)
        if mapping == RomMap.RAW and len(rom) != chip_size:
            raise ImageCapacityError("raw image size must exactly match the selected parallel ROM")
    except (ImageCapacityError, ValueError) as exc:
        raise SystemExit(str(exc)) from exc

    try:
        image = build_chip_image(rom, mapping, chip_size)
    except ImageCapacityError as exc:
        minimum = minimum_chip_size_mbit(rom, mapping)
        if minimum is not None:
            raise SystemExit(f"{exc}; use at least a {minimum} Mbit ROM") from exc
        raise SystemExit(str(exc)) from exc

    args.output.write_bytes(image)
    if args.fx_rom_output:
        args.fx_rom_output.write_bytes(fx3_payload(rom))

    minimum = minimum_chip_size_mbit(rom, mapping)
    print(f"Map: {mapping.value}")
    print(f"Source ROM: {len(rom)} bytes")
    print(f"Parallel ROM: {args.output} ({len(image)} bytes)")
    print(f"Device size: {size_mbit} Mbit"
          f"{' (inferred)' if args.chip_size_mbit is None else ''}")
    if minimum is not None:
        print(f"Minimum device size: {minimum} Mbit")


if __name__ == "__main__":
    main()
