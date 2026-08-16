#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKER = ROOT / "tools/make_fx3_qspi_image.py"


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
        if data[0x100000:0x100004] != b"\x11\x22\x33\x44":
            fail("default 4 MiB layout did not place FX3 ROM at 0x100000")
        if data[0x100004:0x100100] != b"\xFF" * 0xFC:
            fail("unused FX3 ROM space was not padded with 0xFF")

        image8 = temp / "combined8.bin"
        run(str(firmware), str(rom), str(image8), "--flash-size", "8M")
        data8 = image8.read_bytes()
        if len(data8) != 8 * 1024 * 1024 or data8[0x500000:0x500004] != b"\x11\x22\x33\x44":
            fail("8 MiB layout did not keep the FX3 ROM in the top 3 MiB")

        overlap_fw = temp / "overlap.bin"
        overlap_fw.write_bytes(b"\xAA" * (0x100000 + 1))
        run(str(overlap_fw), str(rom), str(temp / "bad-overlap.bin"), ok=False)

        oversized_rom = temp / "oversized.bin"
        oversized_rom.write_bytes(b"\x00" * (3 * 1024 * 1024 + 1))
        run(str(firmware), str(oversized_rom), str(temp / "bad-rom.bin"), ok=False)

        run(str(firmware), str(rom), str(temp / "bad-align.bin"), "--rom-offset", "0x100001", ok=False)

    print("packer_tests: PASS")


if __name__ == "__main__":
    main()
