#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

SRAM_START, SRAM_END = 0x20000000, 0x20082000
XIP_START, XIP_END = 0x10000000, 0x14000000
ROOTS = (
    "qspi_sd_transfer_sram(", "qspi_bus_core0_wait()",
    "snes_sd_read_irq_handler()", "snes_sd_control_irq_handler()",
    "fx_ram_read(", "fx_ram_write(", "fx_set_irq(", "snes_irq_write(",
)
INDIRECT = (
    "SuperFx::cpu_read(", "fx_sync_sd_ram_read(", "fx_sync_sd_ram_write(", "fx_set_irq(",
)


def audit(disassembly: str) -> list[str]:
    functions: dict[int, tuple[str, list[str]]] = {}
    current: list[str] | None = None
    for line in disassembly.splitlines():
        match = re.match(r"^([0-9a-fA-F]+) <(.+)>:$", line)
        if match:
            current = []
            functions[int(match[1], 16)] = (match[2], current)
        elif current is not None:
            current.append(line)
    pending = [address for address, (name, _) in functions.items()
               if any(root in name for root in ROOTS)]
    errors = [f"missing SRAM root: {root}" for root in ROOTS
              if not any(root in name for name, _ in functions.values())]
    visited: set[int] = set()
    starts = sorted(functions)
    while pending:
        address = pending.pop()
        if address in visited:
            continue
        visited.add(address)
        name, lines = functions[address]
        if not SRAM_START <= address < SRAM_END:
            errors.append(f"code outside SRAM: {name} at {address:#x}")
            continue
        next_start = next((start for start in starts if start > address), SRAM_END)
        halves: dict[str, int] = {}
        for line in lines:
            if re.search(r"\btb[bh](?:\.[wn])?\s", line):
                errors.append(f"unaudited branch table in {name}: {line.strip()}")
            literal = re.search(r"\.word\s+0x([0-9a-fA-F]+)", line)
            if literal and XIP_START <= int(literal[1], 16) < XIP_END:
                errors.append(f"XIP literal in {name}: {literal[0]}")
            immediate = re.search(r"\bmov([wt])\s+(r\d+),\s*#(\d+)", line)
            if immediate:
                if immediate[1] == "w":
                    halves[immediate[2]] = int(immediate[3])
                else:
                    value = (int(immediate[3]) << 16) | halves.get(immediate[2], 0)
                    if XIP_START <= value < XIP_END:
                        errors.append(f"XIP address constructed in {name}: {value:#x}")
            call = re.search(r"\b(?:bl|blx|b(?:eq|ne|cs|cc|hi|ls|ge|lt|gt|le|pl|mi)?)(?:\.[wn])?\s+"
                             r"([0-9a-fA-F]+)\s+<", line)
            if call:
                target = int(call[1], 16) & ~1
                if address <= target < next_start:
                    continue
                if target not in functions:
                    errors.append(f"unresolved branch in {name}: {line.strip()}")
                else:
                    pending.append(target)
            indirect = re.search(r"\b(?:blx|bx)\s+(r\d+|ip)\b", line)
            if indirect and not any(allowed in name for allowed in INDIRECT):
                errors.append(f"unaudited indirect call in {name}: {line.strip()}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit the live SD SRAM call graph and literals.")
    parser.add_argument("elf", type=Path)
    parser.add_argument("--objdump", default="arm-none-eabi-objdump")
    args = parser.parse_args()
    result = subprocess.run([args.objdump, "-d", "-C", str(args.elf)],
                            check=True, text=True, stdout=subprocess.PIPE)
    errors = audit(result.stdout)
    for error in errors:
        print(f"FAIL: {error}")
    print(f"Live SD SRAM audit: {'FAIL' if errors else 'PASS'}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
