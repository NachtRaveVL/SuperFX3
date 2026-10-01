#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from make_fx3_qspi_image import FX3_FIRMWARE_SIZE

RP2350_SRAM_SIZE = 520 * 1024


def colors() -> tuple[str, str, str, str]:
    if sys.stdout.isatty() and not os.environ.get("NO_COLOR") and \
            os.environ.get("TERM", "dumb") != "dumb":
        return "\033[1m", "\033[32m", "\033[31m", "\033[0m"
    return "", "", "", ""


def format_size(size: int) -> str:
    return f"{size:,} bytes ({size / 1024:.1f} KiB)"


def static_ram_size(output: str) -> int | None:
    for line in reversed(output.splitlines()):
        fields = line.split()
        if len(fields) < 6:
            continue
        try:
            data = int(fields[1], 10)
            bss = int(fields[2], 10)
        except ValueError:
            continue
        return data + bss
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Report SuperFX3 firmware size and verify the QSPI firmware partition."
    )
    parser.add_argument("firmware", type=Path, help="Built superfx3.bin")
    parser.add_argument("elf", type=Path, help="Built superfx3.elf")
    parser.add_argument("--size-tool", default="arm-none-eabi-size",
                        help="GNU size executable")
    args = parser.parse_args()

    if not args.firmware.is_file():
        parser.error(f"firmware binary not found: {args.firmware}")
    if not args.elf.is_file():
        parser.error(f"ELF file not found: {args.elf}")

    bold, green, red, reset = colors()

    print()
    print("== SuperFX3 firmware sizing ==")

    size_output = ""
    try:
        result = subprocess.run(
            [args.size_tool, str(args.elf)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        size_output = result.stdout
        if size_output:
            print(size_output.rstrip())
        if result.returncode != 0 and result.stderr:
            print(result.stderr.rstrip(), file=sys.stderr)
    except OSError as exc:
        print(f"Warning: unable to run {args.size_tool}: {exc}")

    static_ram = static_ram_size(size_output)
    if static_ram is not None:
        remaining_ram = RP2350_SRAM_SIZE - static_ram
        print()
        print(f"Static data+BSS : {format_size(static_ram)}")
        print(f"RP2350 SRAM     : {format_size(RP2350_SRAM_SIZE)}")
        print(f"Static used     : {static_ram * 100.0 / RP2350_SRAM_SIZE:.1f}%")
        if remaining_ram >= 0:
            print(f"Static headroom : {format_size(remaining_ram)}")
        else:
            print(f"Static overflow : {format_size(-remaining_ram)}")

    actual = args.firmware.stat().st_size
    limit = FX3_FIRMWARE_SIZE
    remaining = limit - actual
    percent = actual * 100.0 / limit

    print()
    print(f"Firmware binary : {format_size(actual)}")
    print(f"Partition limit : {format_size(limit)}")
    print(f"Partition used  : {percent:.1f}%")

    if remaining >= 0:
        print(f"Headroom        : {format_size(remaining)}")
        print(f"{bold}{green}Firmware size: PASS{reset}")
        return 0

    print(f"Overflow        : {format_size(-remaining)}")
    print(f"{bold}{red}Firmware size: FAIL{reset}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
