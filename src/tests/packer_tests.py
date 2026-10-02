#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKER = ROOT / "tools/make_fx3_qspi_image.py"
LAYOUT = ROOT / "storage/fx3_qspi_layout.h"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(*args: str, ok: bool = True) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        [sys.executable, str(PACKER), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if ok and result.returncode != 0:
        fail(result.stderr.decode(errors="replace"))
    if not ok and result.returncode == 0:
        fail("packer unexpectedly accepted an invalid image layout")
    return result


def main() -> None:
    layout = LAYOUT.read_text()
    required_layout = [
        "constexpr uint32_t FLASH_SIZE = 4u * 1024u * 1024u;",
        "constexpr uint32_t FIRMWARE_OFFSET = 0u;",
        "constexpr uint32_t FIRMWARE_SIZE = 496u * 1024u;",
        "constexpr uint32_t SAVE_OFFSET = FIRMWARE_OFFSET + FIRMWARE_SIZE;",
        "constexpr uint32_t SAVE_SIZE = 528u * 1024u;",
        "constexpr uint32_t FX_CODE_OFFSET = SAVE_OFFSET + SAVE_SIZE;",
        "constexpr uint32_t FX_CODE_SIZE = 3u * 1024u * 1024u;",
        "FX_CODE_OFFSET + FX_CODE_SIZE == FLASH_SIZE",
    ]
    for token in required_layout:
        if token not in layout:
            fail(f"C++ QSPI layout is missing {token}")

    with tempfile.TemporaryDirectory() as temp_dir:
        temp = Path(temp_dir)
        firmware = temp / "firmware.bin"
        rom = temp / "fx3.bin"
        image = temp / "combined.bin"

        firmware.write_bytes(bytes((i & 0xFF) for i in range(0x20000)))
        rom.write_bytes(b"\x11\x22\x33\x44")

        run(str(firmware), str(rom), str(image))
        data = image.read_bytes()
        if len(data) != 4 * 1024 * 1024:
            fail("default packed image is not 4 MiB")
        if data[: firmware.stat().st_size] != firmware.read_bytes():
            fail("packer changed the firmware payload")
        if data[0x7C000:0x100000] != b"\xFF" * 0x84000:
            fail("default image did not leave the save journal erased")
        if data[0x100000:0x100004] != b"\x11\x22\x33\x44":
            fail("default layout did not place private FX code at 0x100000")
        if data[0x100004:0x100100] != b"\xFF" * 0xFC:
            fail("unused FX code space was not padded with 0xFF")

        run(str(firmware), str(rom), str(temp / "bad-size.bin"),
            "--flash-size", "8M", ok=False)

        overlap_fw = temp / "overlap.bin"
        overlap_fw.write_bytes(b"\xAA" * 0x7C000)
        run(str(overlap_fw), str(rom), str(image))
        boundary = image.read_bytes()
        if boundary[:0x7C000] != overlap_fw.read_bytes() or \
                boundary[0x7C000:0x100000] != b"\xFF" * 0x84000:
            fail("maximum firmware did not preserve the full 528 KiB save partition")
        if boundary[0x100000:0x100004] != rom.read_bytes():
            fail("maximum firmware moved the FX-code partition")
        overlap_fw.write_bytes(b"\xAA" * (0x7C000 + 1))
        run(str(overlap_fw), str(rom), str(temp / "bad-overlap.bin"), ok=False)

        oversized_rom = temp / "oversized.bin"
        oversized_rom.write_bytes(b"\x00" * (3 * 1024 * 1024 + 1))
        run(str(firmware), str(oversized_rom), str(temp / "bad-rom.bin"), ok=False)

        run(str(firmware), str(rom), str(temp / "bad-offset.bin"),
            "--rom-offset", "0x101000", ok=False)

        canonical = bytearray(b"\x55" * 0x300000 + b"\xAA" * 0x100000)
        canonical[0x7FD6:0x7FD8] = b"\x17\x0C"
        rom.write_bytes(canonical)
        run(str(firmware), str(rom), str(image), "--fx3-rom")
        if image.read_bytes()[0x100000:] != canonical[:0x300000]:
            fail("canonical FX3 extraction changed the shared payload or included SNES-only bytes")
        striped = b"".join(canonical[i:i + 0x8000] * 2 for i in range(0, 0x200000, 0x8000))
        rom.write_bytes(striped + canonical + b"\xFF" * 256)
        run(str(firmware), str(rom), str(image), "--fx3-rom")
        if image.read_bytes()[0x100000:] != canonical[:0x300000]:
            fail("production FX3 extraction differs from canonical input")
        rom.write_bytes(b"\x00" * 0x800000)
        run(str(firmware), str(rom), str(image), "--fx3-rom", ok=False)

    print("packer_tests: PASS")


if __name__ == "__main__":
    main()
