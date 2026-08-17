#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path

SYMBOL_RE = re.compile(r"\b([0-9A-Fa-f]{6,8})\s+\.?([A-Za-z_][A-Za-z0-9_]*)\s*$")


def parse_labels(text: str) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for raw in text.splitlines():
        line = raw.strip()
        match = SYMBOL_RE.search(line)
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
            continue
        # ld65 -Ln commonly emits: al 000100 .Symbol
        fields = line.split()
        if len(fields) >= 3 and fields[0].lower() in {"al", "la"}:
            try:
                value = int(fields[1], 16)
            except ValueError:
                continue
            symbols[fields[2].lstrip(".")] = value
    return symbols


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("labels", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, required=True, help="GSU source used to discover exported kernels")
    args = parser.parse_args()

    symbols = parse_labels(args.labels.read_text())
    exports = re.findall(r"^\.export\s+(FxKernel_[A-Za-z0-9_]+)\s*$", args.source.read_text(), re.MULTILINE)
    missing = [name for name in exports if name not in symbols]
    if missing:
        raise SystemExit(f"missing FX linker symbols: {', '.join(missing)}")

    lines = ["; Generated from the linked GSU image. Do not edit by hand."]
    for name in exports:
        address = symbols[name]
        if not 0 <= address < 0x8000:
            raise SystemExit(f"{name} linked outside GSU bank 00: 0x{address:X}")
        lines.append(f"FX_ENTRY_{name} = ${address:04X}")
        lines.append(f"FX_BANK_{name} = $00")
    lines.append("")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))
    print(f"Wrote {args.output} ({len(exports)} entry points)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
