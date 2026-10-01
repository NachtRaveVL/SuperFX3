#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from make_fx3_qspi_image import FX3_FIRMWARE_SIZE


def colors() -> tuple[str, str, str, str]:
    if sys.stdout.isatty() and not os.environ.get("NO_COLOR") and \
            os.environ.get("TERM", "dumb") != "dumb":
        return "\033[1m", "\033[32m", "\033[31m", "\033[0m"
    return "", "", "", ""


def format_size(size: int) -> str:
    return f"{size:,} bytes ({size / 1024:.1f} KiB)"


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

    try:
        result = subprocess.run(
            [args.size_tool, str(args.elf)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.stdout:
            print(result.stdout.rstrip())
        if result.returncode != 0 and result.stderr:
            print(result.stderr.rstrip(), file=sys.stderr)
    except OSError as exc:
        print(f"Warning: unable to run {args.size_tool}: {exc}")

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
