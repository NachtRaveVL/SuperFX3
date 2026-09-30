#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PIO = ROOT / "platform/rp2350/snes_bus.pio"
PIO_CPP = ROOT / "platform/rp2350/snes_pio.cpp"
BUS_CPP = ROOT / "platform/rp2350/snes_bus.cpp"
LAYOUT_H = ROOT / "platform/rp2350/snes_bus_layout.h"
BOARD_H = ROOT.parent / "boards/snes_fx3.h"
CMAKE = ROOT.parent / "CMakeLists.txt"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def define(name: str) -> int:
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+(0x[0-9A-Fa-f]+|\d+)\s*$",
        BOARD_H.read_text(), re.MULTILINE,
    )
    if not match:
        fail(f"board definition is missing numeric {name}")
    return int(match.group(1), 0)


def parse_programs(text: str) -> dict[str, list[str]]:
    programs: dict[str, list[str]] = {}
    current: list[str] | None = None
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith(".program "):
            name = line.split()[1]
            current = programs.setdefault(name, [])
        elif current is not None and not line.startswith(".") and not line.endswith(":"):
            current.append(re.sub(r"\s+\[\d+\]\s*$", "", line))
    return programs


def test_pin_map() -> None:
    expected = {
        "SNES_PRES_N_PIN": 0,
        "SNES_ROM_RD_N_PIN": 1, "SNES_ROM_WR_N_PIN": 2, "SNES_ROM_CE_N_PIN": 3,
        "SNES_ADDR_OE_N_PIN": 4, "SNES_CONTROL_OE_N_PIN": 5,
        "SNES_DATA_DIR_PIN": 6, "SNES_DATA_OE_N_PIN": 7,
        "SNES_A12_PIN": 8, "SNES_A11_PIN": 9, "SNES_A13_PIN": 10,
        "SNES_A10_PIN": 11, "SNES_A14_PIN": 12, "SNES_A9_PIN": 13,
        "SNES_A15_PIN": 14, "SNES_A8_PIN": 15, "SNES_A16_PIN": 16,
        "SNES_A7_PIN": 17, "SNES_A17_PIN": 18, "SNES_A6_PIN": 19,
        "SNES_A18_PIN": 20, "SNES_A5_PIN": 21, "SNES_A19_PIN": 22,
        "SNES_A4_PIN": 23, "SNES_A20_PIN": 24, "SNES_A3_PIN": 25,
        "SNES_A21_PIN": 26, "SNES_A2_PIN": 27, "SNES_A22_PIN": 28,
        "SNES_A1_PIN": 29, "SNES_A23_PIN": 30, "SNES_A0_PIN": 31,
        "SNES_I_IRQ_N_PIN": 32, "SNES_I_CART_N_PIN": 33,
        "SNES_I_RD_N_PIN": 34, "SNES_I_WR_N_PIN": 35,
        "SNES_I_RESET_N_PIN": 36, "SNES_I_CLK_PIN": 37,
        "SNES_O_IRQ_N_PIN": 38, "SNES_O_RESET_N_PIN": 39,
        "SNES_D4_PIN": 40, "SNES_D0_PIN": 41, "SNES_D5_PIN": 42,
        "SNES_D1_PIN": 43, "SNES_D6_PIN": 44, "SNES_D2_PIN": 45,
        "SNES_D7_PIN": 46, "SNES_D3_PIN": 47,
    }
    actual = {name: define(name) for name in expected}
    if actual != expected:
        fail("production GPIO map does not match the routed Rev A board")
    if len(set(actual.values())) != 48:
        fail("production GPIO map contains a duplicate pin")

    controls = {
        "SNES_CONTROL_CONSOLE_IDLE": 0x07,
        "SNES_CONTROL_DIRECT_READ": 0x22,
        "SNES_CONTROL_SERVICE_READ": 0x27,
        "SNES_CONTROL_BUS_ISOLATED": 0x4F,
        "SNES_CONTROL_ROM_READ": 0x4A,
        "SNES_CONTROL_ROM_VERIFY": 0x5A,
        "SNES_CONTROL_ROM_WRITE": 0x59,
        "SNES_CONTROL_ROM_WRITE_SETUP": 0x5B,
        "SNES_CONTROL_STANDALONE": 0x5F,
    }
    if any(define(name) != value for name, value in controls.items()):
        fail("local GPIO1-GPIO7 control words no longer match the hardware truth table")


def test_instruction_ram(programs: dict[str, list[str]]) -> None:
    read = programs["snes_read"]
    release = read.index("wait 1 gpio 34")
    if read[release + 1:release + 3] != ["mov osr, null", "out pindirs, 8"]:
        fail("read release must clear all eight OUT pin directions, not a five-pin SET group")
    expected = {
        "snes_control_output", "snes_write_address", "snes_write_trigger",
        "snes_write_capture", "snes_reset", "snes_read",
    }
    if set(programs) != expected:
        fail(f"unexpected PIO program set: {sorted(programs)}")

    loads = {
        "PIO0": ("snes_control_output", "snes_write_address"),
        "PIO1": ("snes_write_trigger", "snes_write_capture", "snes_reset"),
        "PIO2": ("snes_read",),
    }
    for pio, names in loads.items():
        count = sum(len(programs[name]) for name in names)
        if count > 32:
            fail(f"{pio} needs {count} PIO instructions")
        print(f"{pio}: {count}/32 instructions")

    for name, instructions in programs.items():
        for instruction in instructions:
            match = re.match(r"set\s+\w+,\s*(0x[0-9A-Fa-f]+|\d+)$", instruction)
            if match and int(match.group(1), 0) > 31:
                fail(f"{name} uses an unencodable SET immediate: {instruction}")


def test_input_strobes(programs: dict[str, list[str]]) -> None:
    expected_waits = {
        "snes_write_trigger": define("SNES_I_WR_N_PIN"),
        "snes_write_capture": define("SNES_I_WR_N_PIN"),
        "snes_reset": define("SNES_I_RESET_N_PIN"),
        "snes_read": define("SNES_I_RD_N_PIN"),
    }
    wait_re = re.compile(r"wait\s+[01]\s+gpio\s+(\d+)")
    for name, pin in expected_waits.items():
        waits = [int(match.group(1)) for insn in programs[name]
                 if (match := wait_re.match(insn))]
        if not waits or any(wait != pin for wait in waits):
            fail(f"{name} waits on {waits}, expected translated input GPIO{pin}")

    all_waits = [int(match.group(1)) for instructions in programs.values() for insn in instructions
                 if (match := wait_re.match(insn))]
    if define("SNES_ROM_RD_N_PIN") in all_waits or define("SNES_ROM_WR_N_PIN") in all_waits:
        fail("PIO is sampling ROM output strobes instead of /I_RD and /I_WR")

    bus = BUS_CPP.read_text()
    forbidden = [
        "gpio_set_dir(SNES_I_RD_N_PIN, GPIO_OUT)",
        "gpio_set_dir(SNES_I_WR_N_PIN, GPIO_OUT)",
        "gpio_put(SNES_I_RD_N_PIN",
        "gpio_put(SNES_I_WR_N_PIN",
    ]
    for token in forbidden:
        if token in bus:
            fail(f"translated console strobe is no longer input-only: {token}")
    required = [
        "gpio_set_dir_masked64(SNES_I_CONTROL_MASK, 0)",
        "gpio_put(SNES_I_RESET_N_PIN, 0)",
        "gpio_set_dir(SNES_I_RESET_N_PIN, GPIO_OUT)",
        "snes_local_control(SNES_CONTROL_STANDALONE)",
        "snes_local_control(SNES_CONTROL_ROM_READ)",
    ]
    for token in required:
        if token not in bus:
            fail(f"bus ownership sequencing is missing {token}")

    programmer = (ROOT / "platform/rp2350/parallel_rom_gpio.cpp").read_text()
    if programmer.count("gpio_set_dir_masked64(SNES_ADDR_MASK, 0)") < 2:
        fail("USB ROM read/write cycles do not release every address GPIO")


def test_pio_source_contract(pio_text: str) -> None:
    required = [
        "out pins, 7", "in pins, 24", "in pins, 8",
        "irq wait 1", "irq 2", "irq 0", "out pindirs, 8",
        "wait 0 gpio 34", "wait 0 gpio 35", "wait 0 gpio 36",
    ]
    for token in required:
        if token not in pio_text:
            fail(f"routed PIO source is missing {token}")
    if "dual" in pio_text.lower() or "ROM0" in pio_text or "ROM1" in pio_text:
        fail("dual-ROM terminology remains in the production PIO source")


def test_cpp_setup() -> None:
    text = PIO_CPP.read_text()
    required = [
        "pio_set_gpio_base(pio0, SNES_PIO_LOWER_BASE)",
        "pio_set_gpio_base(pio1, SNES_PIO_UPPER_BASE)",
        "pio_set_gpio_base(pio2, SNES_PIO_UPPER_BASE)",
        "sm_config_set_out_pins(&g_control_config, SNES_LOCAL_CONTROL_BASE, SNES_LOCAL_CONTROL_COUNT)",
        "sm_config_set_in_pins(&g_write_address_config, SNES_ADDR_RAW_BASE)",
        "sm_config_set_in_pins(&g_write_capture_config, SNES_DATA_RAW_BASE)",
        "sm_config_set_jmp_pin(&g_read_config, SNES_I_CART_N_PIN)",
        "sm_config_set_out_pins(&g_read_config, SNES_DATA_RAW_BASE, SNES_DATA_RAW_COUNT)",
        "&pio0->txf[g_control_sm], &pio2->rxf[g_read_sm]",
        "&pio0->txf[g_write_address_sm], &pio1->rxf[g_write_trigger_sm]",
        "&pio1->txf[g_write_capture_sm], &pio0->rxf[g_write_address_sm]",
        "static_assert(NUM_BANK0_GPIOS >= 48",
        "static_assert(NUM_PIOS >= 3",
        "static_assert(PICO_PIO_USE_GPIO_BASE == 1",
    ]
    for token in required:
        if token not in text:
            fail(f"PIO C++ setup is missing {token}")


def test_layout_and_response() -> None:
    layout = LAYOUT_H.read_text()
    for name in (
        "snes_unpack_address_raw", "snes_pack_address_raw",
        "snes_unpack_data_raw", "snes_pack_data_raw",
    ):
        if name not in layout:
            fail(f"routed bus helper is missing {name}")

    cpp = PIO_CPP.read_text()
    if "READ_RESPONSE_CONTROL_SHIFT = 9" not in cpp or \
            "READ_RESPONSE_PINDIRS_SHIFT = 16" not in cpp:
        fail("read response word fields drifted from the PIO consumer")


def test_cmake() -> None:
    text = CMAKE.read_text()
    required = [
        'list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")',
        'set(PICO_BOARD snes_fx3 CACHE STRING "Pico board")',
        "pico_generate_pio_header(",
        "${CMAKE_CURRENT_LIST_DIR}/src/platform/rp2350/snes_bus.pio",
        "tinyusb_device",
        "src/usb/usb_rom_loader.cpp",
    ]
    for token in required:
        if token not in text:
            fail(f"CMake setup is missing {token}")
    if "SNES_PARALLEL_ROM_COUNT" in text:
        fail("CMake still exposes removed dual-ROM configuration")


def main() -> None:
    pio_text = PIO.read_text()
    if ".pio_version 1" not in pio_text:
        fail("RP2350 PIO source must declare .pio_version 1")
    programs = parse_programs(pio_text)
    test_pin_map()
    test_instruction_ram(programs)
    test_input_strobes(programs)
    test_pio_source_contract(pio_text)
    test_cpp_setup()
    test_layout_and_response()
    test_cmake()
    print("pio_static_tests: PASS")


if __name__ == "__main__":
    main()
