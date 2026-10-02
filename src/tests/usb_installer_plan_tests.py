#!/usr/bin/env python3
"""Prove the in-place USB installer plan preserves every mapped source page."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024
PAGE = 4096
SECTOR = 128 * 1024
CAPACITY = 16 * MIB
LOWER_TEMP_BASE = 8 * MIB
UPPER_TEMP_BASE = 12 * MIB

spec = importlib.util.spec_from_file_location(
    "make_snes_rom_image", ROOT / "tools" / "make_snes_rom_image.py"
)
image_tool = importlib.util.module_from_spec(spec)
assert spec.loader
spec.loader.exec_module(image_tool)


def temp_page(source_page: int) -> int:
    if source_page < 4 * MIB:
        return UPPER_TEMP_BASE + source_page
    return LOWER_TEMP_BASE + source_page - 4 * MIB


def test_upper_source_copy_overlap(size: int, header: int) -> None:
    """The descending upper-half copy must not erase unread source bytes."""
    copied = max(0, size - 4 * MIB)
    if not copied:
        return
    first_sector = LOWER_TEMP_BASE
    last_sector = (LOWER_TEMP_BASE + copied - 1) // SECTOR * SECTOR
    unread_end = header + size
    for destination in range(last_sector, first_sector - 1, -SECTOR):
        source_begin = header + 4 * MIB + destination - LOWER_TEMP_BASE
        source_end = min(source_begin + SECTOR, unread_end)
        assert source_begin < source_end
        assert destination >= source_end, (
            f"header={header} size={size:#x}: erasing {destination:#x} "
            f"would destroy unread source through {source_end:#x}"
        )


def simulate_install(mapping: str, size: int) -> None:
    """Model ascending final-sector writes and representative fallbacks."""
    representatives: dict[int, int] = {}
    mapped_pages: set[int] = set()

    for destination_sector in range(0, CAPACITY, SECTOR):
        sector_pages: list[tuple[int, int]] = []
        for destination in range(destination_sector, destination_sector + SECTOR, PAGE):
            source = image_tool.rom_offset(
                image_tool.RomMap(mapping), destination, size
            )
            if source is None:
                continue
            source_page = source // PAGE * PAGE
            mapped_pages.add(source_page)
            temporary = temp_page(source_page)
            if temporary < destination_sector:
                assert source_page in representatives, (
                    f"{mapping} {size:#x}: source page {source_page:#x} lost "
                    f"before final sector {destination_sector:#x}"
                )
            sector_pages.append((source_page, destination))

        for source_page, destination in sector_pages:
            representatives[source_page] = destination

    assert mapped_pages, f"{mapping} {size:#x}: mapping produced no ROM pages"
    assert mapped_pages <= representatives.keys(), (
        f"{mapping} {size:#x}: not every bus-visible source page is represented"
    )
    if size > 4 * MIB:
        assert any(page >= 4 * MIB for page in mapped_pages), (
            f"{mapping} {size:#x}: extended source half is not bus-visible"
        )


def test_source_contract() -> None:
    source = (ROOT / "storage" / "snes_rom_installer.cpp").read_text()
    for contract in (
        "8u * MIB + 512u",
        "ParallelRomProgrammer::CAPACITY",
        "LOWER_TEMP_BASE, true",
        "UPPER_TEMP_BASE, false",
        "read_install_source(source, address)",
    ):
        assert contract in source, f"missing installer contract: {contract}"


def main() -> None:
    for header in (0, 512):
        for size in (4 * MIB + 32 * 1024, 6 * MIB, 8 * MIB):
            test_upper_source_copy_overlap(size, header)

    for mapping, sizes in {
        "lorom": (1 * MIB, 4 * MIB),
        "hirom": (1 * MIB, 4 * MIB),
        "exlorom": (4 * MIB + 32 * 1024, 6 * MIB, 8 * MIB),
        "exhirom": (4 * MIB + 32 * 1024, 6 * MIB, 8 * MIB),
        "fx3": (1 * MIB, 2 * MIB, 3 * MIB, 4 * MIB),
    }.items():
        for size in sizes:
            simulate_install(mapping, size)

    test_source_contract()
    print("usb_installer_plan_tests: PASS")


if __name__ == "__main__":
    main()
