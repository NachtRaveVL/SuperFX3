#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
TOOLS = ROOT / "tools"
_QSPI_PACKER = runpy.run_path(str(REPO / "src/tools/make_fx3_qspi_image.py"))
FX3_ROM_PARTITION_SIZE = int(_QSPI_PACKER["FX3_ROM_SIZE"])
DEFAULT_QSPI_FLASH_SIZE = int(_QSPI_PACKER["DEFAULT_FLASH_SIZE"])
DEFAULT_FX3_ROM_OFFSET = DEFAULT_QSPI_FLASH_SIZE - FX3_ROM_PARTITION_SIZE
SYMBOL_RE = re.compile(r"\b([0-9A-Fa-f]{6,8})\s+\.?([A-Za-z_][A-Za-z0-9_]*)\s*$")


def run(command: list[str], cwd: Path = ROOT) -> None:
    print("+", " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def find_cc65_tool(name: str) -> str:
    candidates: list[Path] = []
    for env_name in ("CC65_HOME", "CC65_PATH"):
        base = os.environ.get(env_name)
        if base:
            candidates += [Path(base) / "bin" / name, Path(base) / name]
            if os.name == "nt":
                candidates += [Path(base) / "bin" / f"{name}.exe", Path(base) / f"{name}.exe"]
    found = shutil.which(name)
    if found:
        return found
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise SystemExit(
        f"{name} was not found. Install cc65 and put its bin directory on PATH, "
        "or set CC65_HOME. No DOS or DOSBox toolchain is required."
    )


def patch_snes_checksum(path: Path) -> None:
    data = bytearray(path.read_bytes())
    if len(data) != 0x8000:
        raise SystemExit(f"diagnostic SNES ROM must link to exactly 32 KiB, got {len(data)} bytes")
    complement_offset = 0x7FDC
    checksum_offset = 0x7FDE
    data[complement_offset:checksum_offset + 2] = b"\x00\x00\x00\x00"
    checksum = (sum(data) + 0x1FE) & 0xFFFF
    complement = checksum ^ 0xFFFF
    data[complement_offset:complement_offset + 2] = complement.to_bytes(2, "little")
    data[checksum_offset:checksum_offset + 2] = checksum.to_bytes(2, "little")
    path.write_bytes(data)


def validate_snes_rom(path: Path) -> None:
    data = path.read_bytes()
    if data[0x7FD5] != 0x20:
        raise SystemExit("diagnostic ROM header is not LoROM")
    reset = int.from_bytes(data[0x7FFC:0x7FFE], "little")
    if not 0x8000 <= reset <= 0xFFBF:
        raise SystemExit(f"reset vector points outside linked code: 0x{reset:04X}")
    checksum = int.from_bytes(data[0x7FDE:0x7FE0], "little")
    complement = int.from_bytes(data[0x7FDC:0x7FDE], "little")
    if (checksum ^ complement) != 0xFFFF:
        raise SystemExit("SNES checksum/complement pair is invalid")


def generate_sources(build_dir: Path) -> None:
    run([sys.executable, str(TOOLS / "verify.py"), "--root", str(REPO)])
    run([sys.executable, str(TOOLS / "gen_font.py"), str(ROOT / "generated/font4bpp.bin")])
    run([sys.executable, str(TOOLS / "gen_palette.py"), str(ROOT / "generated/palette.bin")])
    run([sys.executable, str(TOOLS / "gen_plot_pattern.py"), str(ROOT / "generated/plot_pattern8bpp.bin")])
    run([sys.executable, str(TOOLS / "gen_registry.py"), str(ROOT / "tests.json"), str(ROOT / "generated/test_registry.inc")])
    build_dir.mkdir(parents=True, exist_ok=True)


def build_fx(ca65: str, ld65: str, build_dir: Path) -> tuple[Path, Path]:
    obj = build_dir / "fx_kernels.o"
    image = build_dir / "fx3_test_fxrom.bin"
    labels = build_dir / "fx3_test_fxrom.sym"
    map_file = build_dir / "fx3_test_fxrom.map"
    run([
        ca65, "-g", "-I", "fx", "-I", "include", "-o", str(obj), "fx/main.asm"
    ])
    run([
        ld65, "-C", "linker/fxrom.cfg", "-o", str(image), "-Ln", str(labels),
        "-m", str(map_file), str(obj)
    ])
    run([
        sys.executable, str(TOOLS / "gen_entries.py"), str(labels),
        str(ROOT / "generated/fx_entries.inc"), "--source", str(ROOT / "fx/main.asm")
    ])
    if image.stat().st_size != 0x8000:
        raise SystemExit(f"FX test image must be one 32 KiB GSU bank, got {image.stat().st_size} bytes")
    return image, labels


def build_cpu(ca65: str, ld65: str, build_dir: Path) -> Path:
    obj = build_dir / "supervisor.o"
    rom = build_dir / "fx3_test.sfc"
    labels = build_dir / "fx3_test.sym"
    map_file = build_dir / "fx3_test.map"
    run([
        ca65, "-g", "-I", "cpu", "-I", "include", "-I", "generated",
        "-o", str(obj), "cpu/main.asm"
    ])
    run([
        ld65, "-C", "linker/lorom.cfg", "-o", str(rom), "-Ln", str(labels),
        "-m", str(map_file), str(obj)
    ])
    patch_snes_checksum(rom)
    validate_snes_rom(rom)
    return rom


def stage_fx_partition(fx_rom: Path, output: Path) -> Path:
    payload = fx_rom.read_bytes()
    if len(payload) > FX3_ROM_PARTITION_SIZE:
        raise SystemExit(
            f"FX test image is {len(payload)} bytes; the private partition is "
            f"{FX3_ROM_PARTITION_SIZE} bytes"
        )
    image = bytearray(b"\xFF" * FX3_ROM_PARTITION_SIZE)
    image[:len(payload)] = payload
    output.write_bytes(image)
    return output


def pack_parallel(rom: Path, output: Path, chip_size_mbit: int, rom_count: int) -> list[Path]:
    command = [
        sys.executable, str(REPO / "src/tools/make_snes_rom_image.py"), str(rom), str(output),
        "--map", "lorom", "--chip-size-mbit", str(chip_size_mbit), "--rom-count", str(rom_count),
    ]
    run(command, cwd=REPO)
    outputs = [output]
    if rom_count == 2:
        suffix = output.suffix or ".bin"
        stem = output.stem if output.suffix else output.name
        outputs.append(output.with_name(f"{stem}.rom1{suffix}"))
    return outputs


def pack_qspi(firmware: Path, fx_rom: Path, output: Path) -> None:
    run([
        sys.executable, str(REPO / "src/tools/make_fx3_qspi_image.py"),
        str(firmware), str(fx_rom), str(output)
    ], cwd=REPO)


def parse_labels(path: Path) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        match = SYMBOL_RE.search(line)
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
            continue
        fields = line.split()
        if len(fields) >= 3 and fields[0].lower() in {"al", "la"}:
            try:
                value = int(fields[1], 16)
            except ValueError:
                continue
            symbols[fields[2].lstrip(".")] = value
    return symbols


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_record(path: Path, role: str, *, flash_offset: int | None = None) -> dict:
    record = {
        "file": path.name,
        "role": role,
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
    }
    if flash_offset is not None:
        record["flash_offset"] = flash_offset
        record["flash_offset_hex"] = f"0x{flash_offset:06X}"
    return record


def write_manifest(
    output: Path,
    snes_rom: Path,
    fx_rom: Path,
    fx_partition: Path,
    fx_labels: Path,
    extras: list[tuple[Path, str, int | None]],
) -> None:
    tests = json.loads((ROOT / "tests.json").read_text())
    symbols = parse_labels(fx_labels)

    manifest_tests = []
    for index, test in enumerate(tests):
        entry: dict = {
            "index": index,
            "id": test["id"],
            "name": test["name"],
            "kernel": test["kernel"],
        }
        if test["kernel"]:
            address = symbols.get(test["kernel"])
            if address is None:
                raise SystemExit(f"manifest is missing linked symbol {test['kernel']}")
            entry["fx_bank"] = 0
            entry["fx_address"] = address
            entry["fx_address_hex"] = f"0x{address:04X}"
        manifest_tests.append(entry)

    artifacts = {
        "snes_supervisor": artifact_record(snes_rom, "parallel SNES CPU LoROM source image"),
        "fxrom_compact": artifact_record(fx_rom, "linked private GSU test payload"),
        "fxrom_partition": artifact_record(
            fx_partition,
            "3 MiB programming-ready private FX3 QSPI ROM partition",
            flash_offset=DEFAULT_FX3_ROM_OFFSET,
        ),
    }
    for path, role, flash_offset in extras:
        artifacts[path.stem] = artifact_record(path, role, flash_offset=flash_offset)

    pair_digest = hashlib.sha256()
    pair_digest.update(snes_rom.read_bytes())
    pair_digest.update(fx_partition.read_bytes())

    manifest = {
        "format": 1,
        "suite": "NR-RetroWorks FX3 Diagnostic ROM",
        "pair_id": pair_digest.hexdigest()[:16],
        "qspi_layout": {
            "default_flash_size": DEFAULT_QSPI_FLASH_SIZE,
            "fxrom_partition_size": FX3_ROM_PARTITION_SIZE,
            "fxrom_default_offset": DEFAULT_FX3_ROM_OFFSET,
            "fxrom_default_offset_hex": f"0x{DEFAULT_FX3_ROM_OFFSET:06X}",
            "erased_fill": "0xFF",
        },
        "artifacts": artifacts,
        "tests": manifest_tests,
    }
    output.write_text(json.dumps(manifest, indent=2) + "\n")


def self_test_build_helpers() -> None:
    tests = json.loads((ROOT / "tests.json").read_text())
    with tempfile.TemporaryDirectory(prefix="fx3-testrom-") as temp_name:
        temp = Path(temp_name)

        snes = temp / "fx3_test.sfc"
        snes_data = bytearray(b"\xFF" * 0x8000)
        snes_data[0x7FD5] = 0x20
        snes_data[0x7FFC:0x7FFE] = (0x8000).to_bytes(2, "little")
        snes.write_bytes(snes_data)
        patch_snes_checksum(snes)
        validate_snes_rom(snes)

        fx_rom = temp / "fx3_test_fxrom.bin"
        fx_rom.write_bytes(bytes((index & 0xFF) for index in range(0x8000)))
        partition = stage_fx_partition(fx_rom, temp / "fx3_test_fxrom_partition.bin")
        partition_data = partition.read_bytes()
        if len(partition_data) != FX3_ROM_PARTITION_SIZE:
            raise SystemExit("FX3 partition staging did not produce exactly 3 MiB")
        if partition_data[:0x8000] != fx_rom.read_bytes():
            raise SystemExit("FX3 partition staging changed the linked payload")
        if partition_data[0x8000:] != b"\xFF" * (FX3_ROM_PARTITION_SIZE - 0x8000):
            raise SystemExit("FX3 partition staging did not erase-fill unused space")

        labels = temp / "fx3_test_fxrom.sym"
        kernels = [test["kernel"] for test in tests if test["kernel"]]
        labels.write_text("\n".join(
            f"al {0x0100 + index * 0x10:06X} .{kernel}"
            for index, kernel in enumerate(dict.fromkeys(kernels))
        ) + "\n")

        manifest_path = temp / "fx3_test_manifest.json"
        write_manifest(manifest_path, snes, fx_rom, partition, labels, [])
        manifest = json.loads(manifest_path.read_text())
        if manifest["qspi_layout"]["fxrom_partition_size"] != FX3_ROM_PARTITION_SIZE:
            raise SystemExit("manifest QSPI partition size drifted from the firmware layout")
        if manifest["artifacts"]["fxrom_partition"]["flash_offset"] != DEFAULT_FX3_ROM_OFFSET:
            raise SystemExit("manifest QSPI offset drifted from the firmware layout")
        if len(manifest["tests"]) != len(tests):
            raise SystemExit("manifest lost diagnostic tests")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build the native SuperFX3 diagnostic ROM pair.")
    parser.add_argument("--build-dir", type=Path, default=REPO / "build/testrom")
    parser.add_argument("--check", action="store_true", help="validate sources and generate assets without requiring cc65")
    parser.add_argument("--pack-parallel", action="store_true", help="also create physical parallel-ROM programming image(s)")
    parser.add_argument("--chip-size-mbit", type=int, default=64)
    parser.add_argument("--rom-count", type=int, choices=(1, 2), default=1)
    parser.add_argument("--firmware-bin", type=Path, help="also combine the FX test image with an RP2350 firmware .bin")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    generate_sources(build_dir)
    if args.check:
        self_test_build_helpers()
        print("Diagnostic ROM source/build-helper checks: PASS")
        return 0

    ca65 = find_cc65_tool("ca65")
    ld65 = find_cc65_tool("ld65")
    fx_rom, fx_labels = build_fx(ca65, ld65, build_dir)
    snes_rom = build_cpu(ca65, ld65, build_dir)
    fx_partition = stage_fx_partition(fx_rom, build_dir / "fx3_test_fxrom_partition.bin")

    extras: list[tuple[Path, str, int | None]] = []
    if args.pack_parallel:
        parallel = pack_parallel(snes_rom, build_dir / "fx3_test_rom0.bin", args.chip_size_mbit, args.rom_count)
        for index, path in enumerate(parallel):
            extras.append((path, f"parallel ROM{index} programming image", None))

    if args.firmware_bin:
        firmware = args.firmware_bin.resolve()
        if not firmware.is_file():
            raise SystemExit(f"firmware binary does not exist: {firmware}")
        combined = build_dir / "superfx3_test_qspi.bin"
        pack_qspi(firmware, fx_rom, combined)
        extras.append((combined, "complete RP2350 QSPI image with firmware and FX3 test ROM", 0))

    manifest = build_dir / "fx3_test_manifest.json"
    write_manifest(manifest, snes_rom, fx_rom, fx_partition, fx_labels, extras)

    print(f"SNES supervisor      : {snes_rom}")
    print(f"FX3 linked payload   : {fx_rom}")
    print(f"FX3 QSPI partition   : {fx_partition}")
    print(f"Matched-set manifest : {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
