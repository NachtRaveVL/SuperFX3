#!/usr/bin/env python3
"""Prove the variable-capacity in-place installer preserves mapped source pages."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024
PAGE = 4096
SECTOR = 128 * 1024
BUS_SIZE = 16 * MIB

spec = importlib.util.spec_from_file_location(
    "make_snes_rom_image", ROOT / "tools" / "make_snes_rom_image.py"
)
image_tool = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(image_tool)


def sector_range(begin: int, end: int, descending: bool = False) -> list[int]:
    sectors = list(range(begin, end, SECTOR))
    return list(reversed(sectors)) if descending else sectors


def program_order(mode: image_tool.RomMap, capacity: int, data_offset: int) -> list[int]:
    half = capacity // 2
    if mode == image_tool.RomMap.LOROM:
        return sector_range(0, capacity, True)
    if mode == image_tool.RomMap.HIROM and capacity > 4 * MIB:
        order = sector_range(4 * MIB, 8 * MIB)
        if capacity > 8 * MIB:
            order += sector_range(12 * MIB, capacity)
        order += sector_range(0, 4 * MIB)
        if capacity > 8 * MIB:
            order += sector_range(8 * MIB, 12 * MIB)
        return order
    if mode in (image_tool.RomMap.EXLOROM, image_tool.RomMap.EXHIROM):
        return sector_range(half, capacity) + sector_range(0, half)
    if mode == image_tool.RomMap.FX3:
        return sector_range(0, half) + sector_range(half, capacity) if data_offset >= half else \
            sector_range(half, capacity) + sector_range(0, half)
    return sector_range(0, capacity)


def simulate_install(mapping: str, size: int, capacity: int, data_offset: int = 0) -> None:
    mode = image_tool.RomMap(mapping)
    representatives: dict[int, int] = {}
    mapped_pages: set[int] = set()
    final_sectors: set[int] = set()

    for destination_sector in program_order(mode, capacity, data_offset):
        sector_pages: list[tuple[int, int]] = []
        for destination in range(destination_sector, destination_sector + SECTOR, PAGE):
            for alias in range(destination, BUS_SIZE, capacity):
                source = image_tool.rom_offset(mode, alias, size)
                if source is None:
                    continue
                source_page = source // PAGE * PAGE
                mapped_pages.add(source_page)
                staged_sector = (data_offset + source_page) // SECTOR
                if staged_sector in final_sectors:
                    assert source_page in representatives, (
                        f"{mapping} {size:#x}/{capacity:#x}: source {source_page:#x} "
                        f"lost before sector {destination_sector:#x}"
                    )
                sector_pages.append((source_page, destination))

        for source_page, destination in sector_pages:
            representatives[source_page] = destination
        final_sectors.add(destination_sector // SECTOR)

    assert mapped_pages and mapped_pages <= representatives.keys()


def patterned_rom(size: int, fx3: bool = False) -> bytes:
    rom = bytearray()
    for page in range(size // PAGE):
        rom.extend(bytes([page % 251]) * PAGE)
    if fx3:
        rom[0x7FC0:0x7FD5] = b" " * 21
        rom[0x7FD5] = 0x20
        rom[0x7FD6] = 0x17
        rom[0x7FD7] = (size - 1).bit_length() - 10
        rom[0x7FDC:0x7FE0] = b"\xCB\xED\x34\x12"
        rom[0x7FFC:0x7FFE] = b"\x00\x80"
    return bytes(rom)


def test_all_capacities() -> None:
    cases = {
        "lorom": (512 * 1024, 1 * MIB, 2 * MIB, 4 * MIB),
        "hirom": (1 * MIB, 2 * MIB, 3 * MIB, 4 * MIB),
        "exlorom": (5 * MIB, 6 * MIB, 8 * MIB),
        "exhirom": (5 * MIB, 6 * MIB, 8 * MIB),
        "fx3": (1 * MIB, 2 * MIB, 3 * MIB, 4 * MIB),
    }
    for capacity in (1 * MIB, 2 * MIB, 4 * MIB, 8 * MIB, 16 * MIB):
        for mapping, sizes in cases.items():
            mode = image_tool.RomMap(mapping)
            for size in sizes:
                rom = patterned_rom(size, mapping == "fx3")
                try:
                    image_tool.build_chip_image(rom, mode, capacity)
                except image_tool.ImageCapacityError:
                    continue
                simulate_install(mapping, size, capacity)

    simulate_install("fx3", 4 * MIB, 8 * MIB, 4 * MIB)


def test_source_contract() -> None:
    source = (ROOT / "storage" / "snes_rom_installer.cpp").read_text()
    for contract in (
        "program_range(0, capacity_, true",
        "alias < ParallelRomProgrammer::MAX_CAPACITY",
        "source_info.map == SnesRomMap::HiRom",
        "source_info.data_offset >= half",
        "capacity_ == ParallelRomProgrammer::MAX_CAPACITY",
    ):
        assert contract in source, f"missing installer contract: {contract}"


def main() -> None:
    test_all_capacities()
    test_source_contract()
    print("usb_installer_plan_tests: PASS")


if __name__ == "__main__":
    main()
