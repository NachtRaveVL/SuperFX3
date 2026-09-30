#!/usr/bin/env python3
"""Lock the QSPI/parallel-NOR ownership boundary into the source tree."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(source: str, token: str, message: str) -> None:
    if token not in source:
        raise AssertionError(message)


def reject(source: str, token: str, message: str) -> None:
    if token in source:
        raise AssertionError(message)


def main() -> None:
    layout = (ROOT / "storage" / "fx3_qspi_layout.h").read_text()
    require(layout, "FIRMWARE_OFFSET = 0u", "QSPI firmware partition is not fixed at zero")
    require(layout, "SAVE_OFFSET = FIRMWARE_OFFSET + FIRMWARE_SIZE",
            "QSPI save partition does not follow firmware")
    require(layout, "FX_CODE_OFFSET = SAVE_OFFSET + SAVE_SIZE",
            "QSPI FX code partition does not follow saves")
    require(layout, "FX_CODE_OFFSET + FX_CODE_SIZE == FLASH_SIZE",
            "QSPI partitions do not cover exactly one W25Q32")

    save = (ROOT / "storage" / "fx3_save_journal.cpp").read_text()
    save_header = (ROOT / "storage" / "fx3_save_journal.h").read_text()
    require(save_header, "struct QspiFlash", "save backend is not explicitly QSPI")
    require(save, "fx3_qspi::SAVE_OFFSET", "save journal is not based in QSPI save space")
    require(save, "fx3_qspi::SAVE_SIZE", "save journal is not bounded to QSPI save space")
    reject(save, "ParallelRom", "save journal references the external parallel ROM")

    installer = (ROOT / "storage" / "snes_rom_installer.cpp").read_text()
    require(installer, "parallel_rom_", "game installer target is not named as parallel ROM")
    require(installer, "ParallelRomProgrammer::CAPACITY",
            "game installer is not bounded to the external parallel ROM")
    reject(installer, "fx3_qspi", "game installer references the QSPI layout")
    reject(installer, "fx3_save", "game installer references the QSPI save journal")

    usb = (ROOT / "usb" / "usb_rom_loader.cpp").read_text()
    require(usb, "parallel_rom_gpio_bus()",
            "USB game loading is not routed to the parallel ROM bus")

    print("storage_separation_tests: PASS")


if __name__ == "__main__":
    main()
