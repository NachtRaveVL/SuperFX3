#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "tools/make_snes_rom_image.py"

spec = importlib.util.spec_from_file_location("make_snes_rom_image", TOOL_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError("Unable to load SNES ROM image tool")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def test_offsets() -> None:
    M = tool.RomMap
    size4 = 4 * tool.MIB
    size8 = 8 * tool.MIB

    expected = [
        (M.LOROM, 0x008000, size4, 0x000000),
        (M.LOROM, 0x018000, size4, 0x008000),
        (M.LOROM, 0x400000, size4, 0x200000),
        (M.LOROM, 0x808000, size4, 0x000000),
        (M.HIROM, 0xC00000, size4, 0x000000),
        (M.HIROM, 0x400000, size4, 0x000000),
        (M.HIROM, 0x008000, size4, 0x008000),
        (M.HIROM, 0xFFFFFF, size4, 0x3FFFFF),
        (M.EXLOROM, 0x808000, size8, 0x000000),
        (M.EXLOROM, 0xC00000, size8, 0x200000),
        (M.EXLOROM, 0x008000, size8, 0x400000),
        (M.EXLOROM, 0x400000, size8, 0x600000),
        (M.EXHIROM, 0xC00000, size8, 0x000000),
        (M.EXHIROM, 0x008000, size8, 0x408000),
        (M.EXHIROM, 0x400000, size8, 0x400000),
        (M.EXHIROM, 0xFFFFFF, size8, 0x3FFFFF),
    ]

    for mapping, address, size, offset in expected:
        require(tool.rom_offset(mapping, address, size) == offset,
                f"{mapping.value} mapped ${address:06X} incorrectly")

    require(tool.rom_offset(M.LOROM, 0x007FFF, size4) is None,
            "LoROM exposed the low half of bank $00")
    require(tool.rom_offset(M.HIROM, 0x7E8000, size4) is None,
            "HiROM exposed WRAM bank $7E")


def test_bus_image() -> None:
    rom = bytes(range(256)) * 128
    image = tool.build_bus_image(rom, tool.RomMap.LOROM)

    require(len(image) == tool.BUS_IMAGE_SIZE, "parallel-ROM bus image is not 16 MiB")
    require(image[0x008000:0x010000] == rom, "LoROM bank $00 upper half is wrong")
    require(image[0x808000:0x810000] == rom, "LoROM bank $80 mirror is wrong")
    require(image[0x400000:0x408000] == rom, "LoROM full-bank mirror is wrong")
    require(image[0x7E0000:0x7E0100] == b"\xFF" * 0x100, "WRAM bank $7E was populated")


def test_header_strip() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        path = Path(temp_dir) / "headered.smc"
        payload = b"\xA5" * 0x8000
        path.write_bytes(b"\x00" * 512 + payload)
        require(tool.load_rom(path) == payload, "512-byte copier header was not stripped")



def patterned_rom(size: int) -> bytes:
    rom = bytearray(size)
    for offset in range(0, size, tool.PAGE_SIZE):
        marker = (offset // tool.PAGE_SIZE).to_bytes(4, "little")
        rom[offset:offset + tool.PAGE_SIZE] = marker * (tool.PAGE_SIZE // len(marker))
    return bytes(rom)


def test_physical_capacity() -> None:
    rom = patterned_rom(4 * tool.MIB)

    require(tool.minimum_chip_size_mbit(rom, tool.RomMap.LOROM, 1) == 64,
            "4 MiB LoROM did not require the expected 8 MiB striped device")

    image = tool.build_chip_images(rom, tool.RomMap.LOROM, 8 * tool.MIB, 1)[0]
    require(len(image) == 8 * tool.MIB, "64-Mbit ROM0 image has the wrong size")
    require(image[0x008000:0x009000] == rom[0x000000:0x001000],
            "striped LoROM bank $00 did not map to source offset 0")
    require(image[0x408000:0x409000] == rom[0x200000:0x201000],
            "striped LoROM full-bank window did not map to the upper source half")



def test_small_device_capacity() -> None:
    lorom_64k = patterned_rom(64 * tool.KIB)
    lorom_128k = patterned_rom(128 * tool.KIB)
    hirom_128k = patterned_rom(128 * tool.KIB)

    require(tool.minimum_chip_size_mbit(lorom_64k, tool.RomMap.LOROM, 1) == 1,
            "64 KiB LoROM did not fit the minimum 1-Mbit device")
    require(tool.minimum_chip_size_mbit(lorom_128k, tool.RomMap.LOROM, 1) == 2,
            "128 KiB LoROM did not account for the striped 2-Mbit physical image")
    require(tool.minimum_chip_size_mbit(hirom_128k, tool.RomMap.HIROM, 1) == 1,
            "128 KiB HiROM did not fit a 1-Mbit device")

def test_superfx_extended() -> None:
    rom = patterned_rom(11 * tool.MIB)
    mapping = tool.RomMap.SUPERFX_EXTENDED

    expected = [
        (0x008000, 0x000000),
        (0x808000, 0x200000),
        (0x400000, 0x800000),
        (0x6FFFFF, 0xAFFFFF),
        (0xC00000, 0x400000),
        (0xFFFFFF, 0x7FFFFF),
    ]
    for address, offset in expected:
        require(tool.rom_offset(mapping, address, len(rom)) == offset,
                f"extended SuperFX map translated ${address:06X} incorrectly")

    require(tool.rom_offset(mapping, 0x006000, len(rom)) is None,
            "extended SuperFX map exposed the low SRAM/I/O window as ROM")
    require(tool.rom_offset(mapping, 0x700000, len(rom)) is None,
            "extended SuperFX map exposed bank $70 as ROM")
    require(tool.minimum_chip_size_mbit(rom, mapping, 1) is None,
            "11 MiB extended SuperFX image unexpectedly fit one 64-Mbit device")
    require(tool.minimum_chip_size_mbit(rom, mapping, 2) == 64,
            "11 MiB extended SuperFX image did not fit two 64-Mbit devices")


def test_raw_dual_rom() -> None:
    lower = b"\x35" * (8 * tool.MIB)
    upper = b"\xCA" * (8 * tool.MIB)
    images = tool.build_chip_images(
        lower + upper, tool.RomMap.RAW, 8 * tool.MIB, 2
    )

    require(images[0] == lower, "raw bus-image lower half did not become ROM0")
    require(images[1] == upper, "raw bus-image upper half did not become ROM1")

    try:
        tool.build_chip_images(lower + upper, tool.RomMap.RAW, 8 * tool.MIB, 1)
    except tool.ImageCapacityError:
        pass
    else:
        require(False, "conflicting 16 MiB raw bus image unexpectedly fit one ROM")

def main() -> None:
    test_offsets()
    test_bus_image()
    test_header_strip()
    test_physical_capacity()
    test_small_device_capacity()
    test_superfx_extended()
    test_raw_dual_rom()
    print("snes_rom_image_tests: PASS")


if __name__ == "__main__":
    main()
