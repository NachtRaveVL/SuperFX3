#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import subprocess
import sys
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
    require(image[0x7E0000:0x7E0004] == b"S3MP", "installed map descriptor missing")
    require(image[0x7E0010:0x7E0100] == b"\xFF" * 0xF0, "WRAM bank contains ROM payload")


def test_header_strip() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        path = Path(temp_dir) / "headered.smc"
        payload = bytearray(b"\xA5" * 0x8000)
        payload[0x7FC0:0x7FD5] = b" " * 21
        payload[0x7FD5] = 0x20
        payload[0x7FDC:0x7FE0] = b"\xCB\xED\x34\x12"
        payload[0x7FFC:0x7FFE] = b"\x00\x80"
        payload = bytes(payload)
        path.write_bytes(b"\x00" * 512 + payload)
        require(tool.load_rom(path, tool.RomMap.LOROM) == payload,
                "validated 512-byte copier header was not stripped")
        invalid = Path(temp_dir) / "invalid.smc"
        invalid.write_bytes(b"\x00" * 512 + b"\xA5" * 0x8000)
        require(tool.load_rom(invalid, tool.RomMap.LOROM) == invalid.read_bytes(),
                "512 bytes were stripped without a valid shifted SNES header")



def patterned_rom(size: int) -> bytes:
    rom = bytearray(size)
    for offset in range(0, size, tool.PAGE_SIZE):
        marker = (offset // tool.PAGE_SIZE).to_bytes(4, "little")
        rom[offset:offset + tool.PAGE_SIZE] = marker * (tool.PAGE_SIZE // len(marker))
    return bytes(rom)


def test_capacity_inference() -> None:
    rom = bytearray(patterned_rom(tool.MIB))
    rom[0x7FC0:0x7FD5] = b" " * 21
    rom[0x7FD5] = 0x20
    rom[0x7FD7] = 0x0A
    rom[0x7FDC:0x7FE0] = b"\xCB\xED\x34\x12"
    rom[0x7FFC:0x7FFE] = b"\x00\x80"
    require(tool.declared_rom_size(rom, tool.RomMap.LOROM) == tool.MIB,
            "header ROM size code was not decoded")
    require(tool.infer_chip_size_mbit(rom, tool.RomMap.LOROM) == 16,
            "LoROM direct-bus mapping size was not included in inference")
    rom[0x7FD7] = 0x0E
    require(tool.declared_rom_size(rom, tool.RomMap.LOROM) == 16 * tool.MIB and
                tool.infer_chip_size_mbit(rom, tool.RomMap.LOROM) == 128,
            "header ROM size code $0E was not treated as 128 Mbit")

    for size_mbit in tool.CHIP_SIZE_MBIT_CHOICES:
        size = tool.chip_size_bytes(size_mbit)
        require(tool.infer_chip_size_mbit(bytes(size), tool.RomMap.RAW) == size_mbit,
                f"{size_mbit}-Mbit raw image size was not inferred")
    try:
        tool.infer_chip_size_mbit(bytes(tool.MIB + 1), tool.RomMap.RAW)
    except tool.ImageCapacityError:
        pass
    else:
        require(False, "non-device-sized raw image was inferred")


def test_physical_capacity() -> None:
    rom = patterned_rom(4 * tool.MIB)

    require(tool.minimum_chip_size_mbit(rom, tool.RomMap.LOROM) == 64,
            "4 MiB LoROM did not require the expected 8 MiB device")

    image = tool.build_chip_image(rom, tool.RomMap.LOROM, 8 * tool.MIB)
    require(len(image) == 8 * tool.MIB, "64-Mbit ROM image has the wrong size")
    require(image[0x008000:0x009000] == rom[0x000000:0x001000],
            "striped LoROM bank $00 did not map to source offset 0")
    require(image[0x408000:0x409000] == rom[0x200000:0x201000],
            "striped LoROM full-bank window did not map to the upper source half")



def test_small_device_capacity() -> None:
    lorom_64k = patterned_rom(64 * tool.KIB)
    lorom_128k = patterned_rom(128 * tool.KIB)
    hirom_128k = patterned_rom(128 * tool.KIB)

    require(tool.minimum_chip_size_mbit(lorom_64k, tool.RomMap.LOROM) == 8,
            "64 KiB LoROM did not use the minimum supported 8-Mbit device")
    require(tool.minimum_chip_size_mbit(lorom_128k, tool.RomMap.LOROM) == 8,
            "128 KiB LoROM did not use the minimum supported 8-Mbit device")
    require(tool.minimum_chip_size_mbit(hirom_128k, tool.RomMap.HIROM) == 8,
            "128 KiB HiROM did not use the minimum supported 8-Mbit device")

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
    require(tool.minimum_chip_size_mbit(rom, mapping) == 128,
            "11 MiB extended SuperFX image did not require one 128-Mbit device")


def test_raw_128_mbit_rom() -> None:
    lower = b"\x35" * (8 * tool.MIB)
    upper = b"\xCA" * (8 * tool.MIB)
    image = tool.build_chip_image(lower + upper, tool.RomMap.RAW, 16 * tool.MIB)
    require(image == lower + upper, "16 MiB raw bus image did not fit the single 128-Mbit ROM")

    try:
        tool.build_chip_image(lower + upper, tool.RomMap.RAW, 8 * tool.MIB)
    except tool.ImageCapacityError:
        pass
    else:
        require(False, "conflicting 16 MiB raw bus image unexpectedly fit a 64-Mbit ROM")

    for size_mbit in tool.CHIP_SIZE_MBIT_CHOICES:
        size = tool.chip_size_bytes(size_mbit)
        source = patterned_rom(size)
        require(tool.build_chip_image(source, tool.RomMap.RAW, size) == source,
                f"{size_mbit}-Mbit raw image did not preserve its full unique capacity")


def test_fx3() -> None:
    rom = bytearray(patterned_rom(4 * tool.MIB))
    rom[0x7FC0:0x7FD5] = b" " * 21
    rom[0x7FBD] = 0x07
    rom[0x7FD5:0x7FD8] = b"\x20\x18\x0C"
    rom[0x7FDC:0x7FE0] = b"\xCB\xED\x34\x12"
    rom[0x7FFC:0x7FFE] = b"\x00\x80"
    rom = bytes(rom)
    image = tool.build_chip_image(rom, tool.RomMap.FX3, tool.BUS_IMAGE_SIZE)
    for bank in range(64):
        block = rom[bank * 0x8000:(bank + 1) * 0x8000]
        require(image[bank * 0x10000:(bank + 1) * 0x10000] == block * 2,
                "FX3 lower banks did not mirror 32 KiB halves")
    require(image[0x400000:0x600000] == rom[:0x200000], "FX3 linear first 2 MiB changed")
    require(image[0x600000:0x700000] == rom[0x200000:0x300000], "FX3 third MiB changed")
    require(image[0x700000:0x7E0000] == rom[0x300000:0x3E0000], "FX3 SNES-only region changed")
    require(image[0xF00000:] == rom[0x300000:], "FX3 full SNES-only CPU mirror changed")
    require(image[0x7E0000:0x7E0008] == b"S3MP\x05\x07\x18\x80",
            "FX3 descriptor omitted SRAM/type metadata")
    require(tool.fx3_payload(rom) == rom[:0x300000], "FX3 QSPI extraction changed bytes or included final MiB")
    with tempfile.TemporaryDirectory() as temp_dir:
        path = Path(temp_dir)
        (path / "game.sfc").write_bytes(rom)
        subprocess.run([sys.executable, str(TOOL_PATH), str(path / "game.sfc"),
                        str(path / "parallel.bin"), "--map", "fx3", "--chip-size-mbit",
                        "128", "--fx-rom-output", str(path / "fxrom.bin")],
                       check=True, stdout=subprocess.DEVNULL)
        require((path / "parallel.bin").read_bytes() == image, "CLI FX3 parallel image differs")
        require((path / "fxrom.bin").read_bytes() == rom[:0x300000], "CLI FX ROM output differs")
    dump = image[:0x400000] + rom
    for source in (rom, b"\x00" * 512 + rom, dump, dump + b"\xFF" * 256,
                   b"\x00" * 512 + dump + b"\xFF" * 256):
        require(tool.normalize_fx3_rom(source) == rom, "FX3 normalization failed")
        require(tool.fx3_payload(source) == rom[:0x300000], "FX3 dump payload differs")
    for offset in (0, 0x8000, 0x3FFFFF):
        bad = bytearray(dump)
        bad[offset] ^= 1
        try:
            tool.normalize_fx3_rom(bytes(bad))
        except ValueError:
            pass
        else:
            require(False, "malformed FX3 mirror accepted")
    for size in (tool.MIB, 2 * tool.MIB, 3 * tool.MIB):
        small = bytearray(rom[:size])
        small[0x7FD7] = (size - 1).bit_length() - 10
        payload = tool.fx3_payload(bytes(small))
        expected = (bytes(small) * 3)[:3 * tool.MIB]
        require(payload == expected, "small FX3 ROM did not fill the FX window by mirroring")
        bus = tool.build_bus_image(bytes(small), tool.RomMap.FX3)
        require(bus[0x400000:0x700000] == payload, "FX3 CPU/GSU shared mapping differs")
        require(bus[0x800000:0xC00000] == bus[:0x400000], "FX3 A23 upper striped mapping absent")
        final = bytes(small[-tool.MIB:])
        require(bus[0xF00000:] == final, "FX3 final CPU MiB mirrors incorrectly")
    for bad in (b"\x00" * (8 * tool.MIB), dump + b"\x00" * 256, rom[:0x8000]):
        try:
            tool.normalize_fx3_rom(bad)
        except ValueError:
            pass
        else:
            require(False, "invalid FX3 size/header/trailer accepted")

def main() -> None:
    test_offsets()
    test_bus_image()
    test_header_strip()
    test_capacity_inference()
    test_physical_capacity()
    test_small_device_capacity()
    test_superfx_extended()
    test_raw_128_mbit_rom()
    test_fx3()
    print("snes_rom_image_tests: PASS")


if __name__ == "__main__":
    main()
