#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from gen_background import VISIBLE_SCANLINES, build_all_tables
from gen_font import GLYPHS, build_font
from gen_palette import PALETTE_BYTES, build_palette, diagnostic_color
from gen_plot_pattern import TILE_BYTES, build_pattern, pixel_value
from gen_registry import load_tests, validate


def parse_cpp_constant(text: str, name: str) -> int:
    match = re.search(rf"\b{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
    if not match:
        raise ValueError(f"could not find C++ constant {name}")
    return int(match.group(1), 0)


def parse_asm_constant(text: str, name: str) -> int:
    match = re.search(rf"^\s*{name}\s*=\s*\$([0-9A-Fa-f]+)\s*$", text, re.MULTILINE)
    if not match:
        raise ValueError(f"could not find assembly constant {name}")
    return int(match.group(1), 16)


def require_source(pattern: str, text: str, message: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise SystemExit(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.root.resolve()
    testrom = root / "testrom"

    tests = load_tests(testrom / "tests.json")
    validate(tests)

    fx_source = (testrom / "fx/main.asm").read_text()

    # ca65 reserves A/X/Y as processor tokens. Using X or Y as a .repeat
    # counter looks innocent but causes the body to explode into hundreds of
    # misleading macro errors. Catch that before invoking the assembler.
    reserved_repeat = re.search(r"^\s*\.repeat\b[^\n,]*,\s*([AXY])\s*(?:;.*)?$", fx_source, re.MULTILINE | re.IGNORECASE)
    if reserved_repeat:
        raise SystemExit(
            f"ca65-incompatible .repeat counter {reserved_repeat.group(1)!r}; "
            "A, X, and Y are reserved processor tokens"
        )

    labels = set(re.findall(r"^(FxKernel_[A-Za-z0-9_]+):", fx_source, re.MULTILINE))
    requested = {test["kernel"] for test in tests if test["kernel"]}
    missing = sorted(requested - labels)
    if missing:
        raise SystemExit(f"tests reference missing GSU kernels: {', '.join(missing)}")

    strings = [test["name"] for test in tests]
    strings += [
        "NR-RETROWORKS FX3 TEST SUITE", "A RUN TEST", "START RUN ALL", "B BACK",
        "PASS", "FAIL", "TIMEOUT", "EXPECTED", "ACTUAL", "ADDRESS", "RUNNING",
        "RUN ALL", "TESTS PASSED", "TESTS FAILED", "LAST FAILURE", "PRESS B TO RETURN",
    ]
    unsupported = sorted({ch for text in strings for ch in text if ch not in GLYPHS})
    if unsupported:
        raise SystemExit(f"font is missing UI characters: {unsupported}")
    if len(build_font()) != 96 * 32:
        raise SystemExit("font generator must emit exactly 96 SNES 4bpp tiles")

    background_tables = build_all_tables()
    if len(background_tables) != 12:
        raise SystemExit("background generator must emit four RGB themes")
    expected_table_bytes = VISIBLE_SCANLINES * 2 + 1
    for name, table in background_tables.items():
        if len(table) != expected_table_bytes or table[-1] != 0:
            raise SystemExit(f"background HDMA table {name} has an invalid size/terminator")
        if any(table[offset] != 1 for offset in range(0, VISIBLE_SCANLINES * 2, 2)):
            raise SystemExit(f"background HDMA table {name} must update every visible scanline")

    background_source = (testrom / "cpu/background.asm").read_text()
    require_source(r"lda\s+#\$20\s*;[^\n]*backdrop", background_source,
                   "background effect must apply color math to the backdrop only")
    require_source(r"lda\s+#\$E0.*?sta\s+HDMAEN", background_source,
                   "background effect must enable only HDMA channels 5-7")
    require_source(r"sta\s+A1T5L.*?stx\s+A1T6L.*?sty\s+A1T7L", background_source,
                   "background themes must switch HDMA source tables without touching the UI palettes")

    ui_source = (testrom / "cpu/ui.asm").read_text()
    for routine in ("BackgroundSetNormal", "BackgroundSetForResult", "BackgroundSetForSummary"):
        if f"jsr {routine}" not in ui_source:
            raise SystemExit(f"UI no longer selects background theme through {routine}")
    require_source(r"UpdateResultVisual:.*?cmp\s+#TEST_RESULT_TIMEOUT.*?PpuHideVisual", ui_source,
                   "timeout results must not display uninitialized/sentinel BG1 test data")

    startup_source = (testrom / "cpu/startup.asm").read_text()
    require_source(r"sta\s+current_test.*?jsr\s+RenderRunning.*?jsr\s+WaitFrame.*?jsr\s+PpuUploadTextMap.*?jsr\s+RunCurrentTest",
                   startup_source,
                   "selected tests must draw the RUNNING screen before executing/waiting")

    ppu_source = (testrom / "cpu/ppu.asm").read_text()
    require_source(r"PpuClearBg1Map:.*?lda\s+#BG1_BLANK_TILE.*?BG1_VISUAL_MAP_OFFSET", ppu_source,
                   "BG1 visual map must be blank except for the single result tile")
    require_source(r"PpuUploadBlankBg1Tile:", ppu_source,
                   "BG1 visual path must upload a guaranteed blank tile")

    palette = build_palette()
    if len(palette) != PALETTE_BYTES:
        raise SystemExit("diagnostic palette must initialize all 256 CGRAM entries")
    if diagnostic_color(0) != 0x0000 or diagnostic_color(1) != 0x7FFF:
        raise SystemExit("diagnostic palette must keep backdrop black and font color white")
    if any(diagnostic_color(index) == 0 for index in range(1, 256)):
        raise SystemExit("diagnostic palette contains an invisible nonzero 8bpp color")

    pattern = build_pattern()
    if len(pattern) != TILE_BYTES or len(set(pixel_value(x, y) for y in range(8) for x in range(8))) != 64:
        raise SystemExit("8bpp architectural PLOT pattern is not the expected 64-pixel tile")

    layout = (root / "src/fx/fx3_layout.h").read_text()
    expected = {
        "PLANAR_BASE": 0x10000,
        "X_TILES": 27,
        "Y_TILES": 18,
        "PLANAR_Y_TILE_STRIDE": 20,
    }
    for name, value in expected.items():
        actual = parse_cpp_constant(layout, name)
        if actual != value:
            raise SystemExit(f"test ROM layout drift: firmware {name} is {actual}, expected {value}")

    # These are architectural contracts between the diagnostic ROM and firmware,
    # not arbitrary test constants. Refuse to build if either side drifts.
    fx3_inc = (testrom / "include/fx3.inc").read_text()
    register_source = (root / "src/fx/fx_registers.cpp").read_text()
    command_source = (root / "src/fx/fx3_commands.cpp").read_text()
    memory_source = (root / "src/fx/fx_memory.cpp").read_text()

    asm_contract = {
        "FX_R0": 0x7000,
        "FX_R15": 0x701E,
        "FX_SFR": 0x7030,
        "FX_PBR": 0x7034,
        "FX_CFGR": 0x7037,
        "FX_SCBR": 0x7038,
        "FX_CLSR": 0x7039,
        "FX_SCMR": 0x703A,
        "FX_VCR": 0x703B,
        "FX_RAMBR": 0x703C,
        "FX3_VCR_VALUE": 0x52,
        "FX3_DEFAULT_SCBR": 0x40,
        "FX3_SCMR_8BPP": 0x03,
        "FX3_CFGR_TEST": 0xA0,
        "FX3_CLSR_FAST": 0x01,
    }
    for name, value in asm_contract.items():
        actual = parse_asm_constant(fx3_inc, name)
        if actual != value:
            raise SystemExit(f"test ROM ABI drift: {name} is 0x{actual:X}, expected 0x{value:X}")

    require_source(
        r"config_\.chip\s*==\s*FxChip::FX3\s*&&\s*\(addr\s*&\s*0xF000\)\s*==\s*0x7000",
        register_source,
        "test ROM ABI drift: firmware no longer exposes FX3 registers in $7000-$7FFF",
    )
    require_source(
        r"case\s+0x303B\s*:\s*//\s*VCR.*?FxChip::FX3\s*\?\s*0x52",
        register_source,
        "test ROM ABI drift: firmware FX3 VCR no longer reports $52",
    )
    require_source(
        r"case\s+3\s*:\s*state_\.plot_bpp\s*=\s*8",
        register_source,
        "test ROM ABI drift: SCMR mode 3 no longer selects 8bpp PLOT",
    )
    require_source(
        r"bank\s*!=\s*0x70\s*&&\s*bank\s*!=\s*0x71",
        memory_source,
        "test ROM ABI drift: GSU shared RAM is no longer mapped through banks $70/$71",
    )

    for command, value in (("ChunkyToPlanarA", 0), ("ChunkyToPlanarB", 1), ("ChunkyToPlanarC", 2),
                           ("ClearA", 3), ("ClearB", 4), ("ClearC", 5)):
        require_source(
            rf"\b{command}\s*=\s*{value}\b",
            command_source,
            f"test ROM ABI drift: FX3 command {command} is no longer {value}",
        )
    for command, first, last in (("ClearA", 0, 8), ("ClearB", 9, 17), ("ClearC", 18, 26)):
        require_source(
            rf"case\s+Fx3Command::{command}\s*:\s*fx3_clear\({first},\s*{last}\)",
            command_source,
            f"test ROM ABI drift: {command} clear range changed",
        )
    require_source(
        r"case\s+Fx3Command::ChunkyToPlanarA\s*:.*?case\s+Fx3Command::ChunkyToPlanarC\s*:.*?break\s*;",
        command_source,
        "test ROM ABI drift: C2P 0-2 are no longer the current no-op command policy",
    )

    if len(tests) > 18:
        raise SystemExit("first-pass menu supports at most 18 visible tests")

    print(f"Test ROM static checks: PASS ({len(tests)} tests, {len(requested)} GSU kernels)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
