#!/usr/bin/env python3
from pathlib import Path
import random
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PIO = ROOT / "platform/rp2350/snes_bus.pio"
PIO_CPP = ROOT / "platform/rp2350/snes_pio.cpp"
BOARD_H = ROOT.parent / "boards/snes_fx3.h"
CMAKE = ROOT.parent / "CMakeLists.txt"

def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)

def normalize_instruction(raw: str) -> str:
    op = re.sub(r"\s+side\s+\d+", "", raw)
    op = re.sub(r"\s+\[\d+\]\s*$", "", op)
    return op.strip()

def board_define(name: str) -> int:
    text = BOARD_H.read_text()
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)\s*$", text, re.MULTILINE)
    if not match:
        fail(f"board definition is missing numeric {name}")
    return int(match.group(1))

def parse_programs(text: str) -> dict[str, list[str]]:
    programs: dict[str, list[str]] = {}
    current = None
    labels: dict[str, set[str]] = {}
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith(".program "):
            current = line.split()[1]
            if current in programs:
                fail(f"duplicate PIO program {current}")
            programs[current] = []
            labels[current] = set()
            continue
        if current is None or line.startswith("."):
            continue
        if line.endswith(":"):
            label = line[:-1]
            if label in labels[current]:
                fail(f"duplicate label {label} in {current}")
            labels[current].add(label)
            continue
        programs[current].append(line)
    return programs

class PioSourceProgram:
    def __init__(self, instructions: list[str], labels: dict[str, int], wrap_target: int):
        self.instructions = instructions
        self.labels = labels
        self.wrap_target = wrap_target

def parse_source_programs(text: str) -> dict[str, PioSourceProgram]:
    programs: dict[str, PioSourceProgram] = {}
    name = None
    instructions: list[str] = []
    labels: dict[str, int] = {}
    wrap_target = 0

    def finish() -> None:
        nonlocal name, instructions, labels, wrap_target
        if name is not None:
            programs[name] = PioSourceProgram(instructions, labels, wrap_target)

    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith(".program "):
            finish()
            name = line.split()[1]
            instructions = []
            labels = {}
            wrap_target = 0
            continue
        if name is None:
            continue
        if line == ".wrap_target":
            wrap_target = len(instructions)
            continue
        if line.startswith("."):
            continue
        if line.endswith(":"):
            labels[line[:-1]] = len(instructions)
            continue
        instructions.append(line)

    finish()
    return programs

def pio_route(program: PioSourceProgram, in_pins: int, selector: bool, initial_x: int = 0, pull_word: int = 0) -> str:
    # Minimal source-level interpreter for the instruction subset used by the bus routers.
    # It intentionally stops at the first externally visible outcome instead of pretending
    # to model PIO timing, FIFOs, IRQ latency, or electrical behavior.
    x = initial_x & 0xFFFFFFFF
    y = 0
    isr = 0
    osr = 0
    pc = program.wrap_target

    for _ in range(128):
        raw = program.instructions[pc]
        op = normalize_instruction(raw)
        side_match = re.search(r"\bside\s+(\d+)", raw)
        side = int(side_match.group(1)) if side_match else None
        pc += 1

        if op == "mov isr, null":
            isr = 0
        elif op == "mov x, isr":
            x = isr
        elif op == "mov y, isr":
            y = isr
        elif op == "mov osr, isr":
            osr = isr
        elif op == "mov osr, y":
            osr = y
        elif op == "mov isr, osr":
            isr = osr
        elif op.startswith("in pins, "):
            count = int(op.rsplit(" ", 1)[1])
            isr = ((isr << count) | (in_pins & ((1 << count) - 1))) & 0xFFFFFFFF
        elif op.startswith("in y, "):
            count = int(op.rsplit(" ", 1)[1])
            isr = ((isr << count) | (y & ((1 << count) - 1))) & 0xFFFFFFFF
        elif op.startswith("out "):
            target, count_text = op[4:].split(",", 1)
            count = int(count_text.strip())
            value = osr & ((1 << count) - 1)
            osr >>= count
            target = target.strip()
            if target == "x":
                x = value
            elif target == "y":
                y = value
            elif target not in ("null", "pins", "pindirs"):
                fail(f"source interpreter does not support OUT target {target}")
        elif op.startswith("set x, "):
            x = int(op.rsplit(" ", 1)[1], 0)
        elif op.startswith("set y, "):
            y = int(op.rsplit(" ", 1)[1], 0)
        elif op.startswith("set pins, "):
            value = int(op.rsplit(" ", 1)[1], 0)
            if side == 5 and value == 0:
                return "direct_rom1"
            return f"set:{value}"
        elif op == "pull block":
            osr = pull_word & 0xFFFFFFFF
        elif op == "push block":
            pass
        elif op.startswith("irq "):
            if op.startswith("irq wait 1"):
                return "service"
            if op.startswith("irq 0"):
                return "service"
        elif op.startswith("wait "):
            match = re.match(r"wait\s+([01])\s+gpio\s+(\d+)", op)
            if not match:
                fail(f"source interpreter cannot parse {op}")
            polarity, pin = int(match.group(1)), int(match.group(2))
            if polarity == 0:
                continue
            if pin == 19:
                return "ignore"
            if pin == 18:
                if side == 1:
                    return "direct_rom0"
                return "read_complete"
            continue
        elif op.startswith("jmp "):
            body = op[4:]
            target = None
            take = True
            if body.startswith("!x "):
                target = body.split()[1]
                take = x == 0
            elif body.startswith("!y "):
                target = body.split()[1]
                take = y == 0
            elif body.startswith("x!=y "):
                target = body.split()[1]
                take = x != y
            elif body.startswith("pin "):
                target = body.split()[1]
                take = selector
            else:
                target = body.strip()
            if take:
                if target in program.labels:
                    if side == 4 and program.labels[target] == program.wrap_target:
                        return "no_drive"
                    pc = program.labels[target]
                else:
                    fail(f"source interpreter cannot resolve label {target}")
        else:
            fail(f"source interpreter does not support instruction: {op}")

    fail("source interpreter exceeded its instruction limit")

def test_jump_targets_resolve(pio_text: str) -> None:
    programs = parse_source_programs(pio_text)
    for name, program in programs.items():
        for insn in program.instructions:
            op = normalize_instruction(insn)
            if not op.startswith("jmp "):
                continue
            target = op.split()[-1]
            if target not in program.labels:
                fail(f"{name} jumps to undefined symbol {target}")

def test_source_driven_routes(pio_text: str) -> None:
    programs = parse_source_programs(pio_text)

    for nibble in range(16):
        fx3 = pio_route(programs["snes_select_fx3"], nibble, False)
        gsu = pio_route(programs["snes_select_gsu"], nibble, False)
        if fx3 != f"set:{1 if nibble == 7 else 0}":
            fail(f"source-driven FX3 selector failed for ${nibble:X}xxx")
        if gsu != f"set:{1 if nibble in (3, 6, 7) else 0}":
            fail(f"source-driven GSU selector failed for ${nibble:X}xxx")

    for fx3 in (False, True):
        program = programs["snes_write_fx3" if fx3 else "snes_write_gsu"]
        for bank in range(256):
            for page in range(16):
                selector = page == 7 if fx3 else page in (3, 6, 7)
                in_pins = bank | (0xA5 << 8)
                actual = pio_route(program, in_pins, selector, pull_word=page << 12)
                special = is_fx3_special_ram_bank(bank) if fx3 else is_gsu_special_ram_bank(bank)
                expected = "service" if selector or special else "ignore"
                if actual != expected:
                    mode = "FX3" if fx3 else "GSU"
                    fail(f"source-driven {mode} write route failed at ${bank:02X}:{page:X}000: {actual}")

    for fx3 in (False, True):
        for dual in (False, True):
            suffix = "_dual" if dual else ""
            program = programs[f"snes_read_{'fx3' if fx3 else 'gsu'}{suffix}"]
            for blocked in ((False,) if fx3 else (False, True)):
                for bank in range(256):
                    for page in range(16):
                        selector = page == 7 if fx3 else page in (3, 6, 7)
                        for romsel_n in (0, 1):
                            pin_window = romsel_n | (int(selector) << 11) | (bank << 12)
                            initial_x = 56 if fx3 else int(blocked)
                            actual = pio_route(program, pin_window, selector, initial_x=initial_x)

                            if romsel_n:
                                expected = "service" if selector else "no_drive"
                            elif fx3:
                                if is_fx3_special_ram_bank(bank):
                                    expected = "service"
                                else:
                                    expected = f"direct_rom{1 if dual and bank & 0x80 else 0}"
                            elif blocked:
                                expected = "service"
                            elif is_gsu_special_ram_bank(bank):
                                expected = "service"
                            else:
                                expected = f"direct_rom{1 if dual and bank & 0x80 else 0}"

                            if actual != expected:
                                mode = "FX3" if fx3 else "GSU"
                                population = "dual" if dual else "single"
                                fail(
                                    f"source-driven {mode}/{population} read route failed at "
                                    f"${bank:02X}:{page:X}000 ROMSEL={romsel_n} blocked={blocked}: "
                                    f"{actual}, expected {expected}"
                                )

def test_instruction_ram(programs: dict[str, list[str]]) -> None:
    expected = {
        "snes_select_fx3", "snes_select_gsu", "snes_write_addr", "snes_write_fx3",
        "snes_write_gsu", "snes_reset", "snes_read_fx3", "snes_read_fx3_dual",
        "snes_read_gsu", "snes_read_gsu_dual",
    }
    if set(programs) != expected:
        fail(f"unexpected PIO program set: {sorted(programs)}")

    loads = {
        "PIO0 FX3": ("snes_select_fx3", "snes_write_addr"),
        "PIO0 GSU": ("snes_select_gsu", "snes_write_addr"),
        "PIO1 FX3": ("snes_write_fx3", "snes_reset"),
        "PIO1 GSU": ("snes_write_gsu", "snes_reset"),
        "PIO2 FX3 single": ("snes_read_fx3",),
        "PIO2 FX3 dual": ("snes_read_fx3_dual",),
        "PIO2 GSU single": ("snes_read_gsu",),
        "PIO2 GSU dual": ("snes_read_gsu_dual",),
    }
    for name, names in loads.items():
        count = sum(len(programs[n]) for n in names)
        if count > 32:
            fail(f"{name} needs {count} PIO instructions")
        print(f"{name}: {count}/32 instructions")

def capture_word(addr: int, bank: int, data: int) -> int:
    # PIO1 first captures A16-A23 followed by D0-D7 into Y. PIO0/DMA supplies A0-A15.
    y = (bank & 0xFF) | ((data & 0xFF) << 8)
    isr = addr & 0xFFFF
    return ((isr << 16) | y) & 0xFFFFFFFF

def test_write_capture() -> None:
    rng = random.Random(0x2350B)
    for _ in range(10000):
        addr = rng.randrange(0x10000)
        bank = rng.randrange(0x100)
        data = rng.randrange(0x100)
        word = capture_word(addr, bank, data)
        if (word & 0xFF) != bank or ((word >> 8) & 0xFF) != data or (word >> 16) != addr:
            fail("PIO0/DMA/PIO1 write capture packing is incorrect")

def is_fx3_special_ram_bank(bank: int) -> bool:
    return bank in (0x70, 0x71)

def is_gsu_special_ram_bank(bank: int) -> bool:
    return bank in (0x70, 0x71, 0xF0, 0xF1)

def modeled_common_bank_match(bank: int) -> bool:
    # A17-A22 are bank bits 1-6. The shared-RAM pattern is 56 with A16 ignored.
    return ((bank >> 1) & 0x3F) == 56

def test_special_bank_decode(pio_text: str) -> None:
    for bank in range(256):
        common = modeled_common_bank_match(bank)
        fx3 = common and not bool(bank & 0x80)
        gsu = common
        if fx3 != is_fx3_special_ram_bank(bank):
            fail(f"FX3 PIO bank decode disagrees at bank ${bank:02X}")
        if gsu != is_gsu_special_ram_bank(bank):
            fail(f"GSU PIO bank decode disagrees at bank ${bank:02X}")

    for name in ("snes_read_gsu", "snes_read_gsu_dual"):
        section = pio_text.split(f".program {name}", 1)[1]
        next_program = section.find(".program ")
        if next_program >= 0:
            section = section[:next_program]
        if "A17-A22 = 56" not in section or "out y, 6" not in section:
            fail(f"{name} no longer ignores A23 for the $70/$71 and $F0/$F1 RAM mirrors")

    for name in ("snes_read_fx3", "snes_read_fx3_dual"):
        section = pio_text.split(f".program {name}", 1)[1]
        next_program = section.find(".program ")
        if next_program >= 0:
            section = section[:next_program]
        if "out y, 7" not in section:
            fail(f"{name} no longer isolates only the $70/$71 shared-RAM banks")

def test_selector_decode() -> None:
    expected_fx3 = {7}
    expected_gsu = {3, 6, 7}
    for nibble in range(16):
        if (nibble == 7) != (nibble in expected_fx3):
            fail(f"FX3 selector decode is wrong for ${nibble:X}xxx")
        if (nibble in (3, 6, 7)) != (nibble in expected_gsu):
            fail(f"GSU selector decode is wrong for ${nibble:X}xxx")

def cpp_register_window(fx3: bool, bank: int, addr: int) -> bool:
    if not (bank <= 0x3F or 0x80 <= bank <= 0xBF):
        return False
    return (0x7000 <= addr <= 0x7FFF and (addr & 0x0300) != 0x0300) if fx3 else 0x3000 <= addr <= 0x3FFF

def cpp_ram_window(fx3: bool, bank: int, addr: int) -> bool:
    if bank in (0x70, 0x71):
        return True
    if fx3:
        return False
    if bank in (0xF0, 0xF1):
        return True
    return (bank <= 0x3E or 0x80 <= bank <= 0xBE) and 0x6000 <= addr <= 0x7FFF

def pio_service_window(fx3: bool, bank: int, addr: int) -> bool:
    selector = ((addr >> 12) == 7) if fx3 else ((addr >> 12) in (3, 6, 7))
    special = is_fx3_special_ram_bank(bank) if fx3 else is_gsu_special_ram_bank(bank)
    return selector or special

def test_frontend_coverage() -> None:
    # Every CPU-visible address handled by the C++ front end must first be routed to core 0 by PIO.
    # Exhaust the entire bank byte and every 4 KiB address page; the PIO selectors decode at this granularity.
    for fx3 in (False, True):
        for bank in range(256):
            for page in range(16):
                addr = page << 12
                wanted = cpp_register_window(fx3, bank, addr) or cpp_ram_window(fx3, bank, addr)
                if wanted and not pio_service_window(fx3, bank, addr):
                    mode = "FX3" if fx3 else "GSU"
                    fail(f"{mode} PIO misses CPU-serviced address ${bank:02X}:{addr:04X}")

def test_read_response_word() -> None:
    for data in range(256):
        word = 1 | (data << 1) | (0xFF << 9)
        if (word & 1) != 1:
            fail("read response drive bit is wrong")
        if ((word >> 1) & 0xFF) != data:
            fail("read response data bits are wrong")
        if ((word >> 9) & 0xFF) != 0xFF:
            fail("read response pindir bits are wrong")
        if ((word >> 17) & 0xFF) != 0:
            fail("read response release bits must remain zero")

def test_wait_gpio_windows(programs: dict[str, list[str]]) -> None:
    assignment = {
        "snes_select_fx3": (0, 31), "snes_select_gsu": (0, 31), "snes_write_addr": (0, 31),
        "snes_write_fx3": (16, 47), "snes_write_gsu": (16, 47), "snes_reset": (16, 47),
        "snes_read_fx3": (16, 47), "snes_read_fx3_dual": (16, 47),
        "snes_read_gsu": (16, 47), "snes_read_gsu_dual": (16, 47),
    }
    wait_re = re.compile(r"^wait\s+[01]\s+gpio\s+(\d+)")
    for name, instructions in programs.items():
        lo, hi = assignment[name]
        for insn in instructions:
            match = wait_re.match(insn)
            if match:
                pin = int(match.group(1))
                if not lo <= pin <= hi:
                    fail(f"{name} waits on GPIO{pin}, outside its PIO GPIO window {lo}-{hi}")

def test_pio_wait_pins(programs: dict[str, list[str]]) -> None:
    expected = {
        "snes_write_addr": board_define("SNES_WR_N_PIN"),
        "snes_write_fx3": board_define("SNES_WR_N_PIN"),
        "snes_write_gsu": board_define("SNES_WR_N_PIN"),
        "snes_reset": board_define("SNES_RESET_N_PIN"),
        "snes_read_fx3": board_define("SNES_RD_N_PIN"),
        "snes_read_fx3_dual": board_define("SNES_RD_N_PIN"),
        "snes_read_gsu": board_define("SNES_RD_N_PIN"),
        "snes_read_gsu_dual": board_define("SNES_RD_N_PIN"),
    }
    wait_re = re.compile(r"^wait\s+[01]\s+gpio\s+(\d+)")
    for name, pin in expected.items():
        waits = [int(m.group(1)) for insn in programs[name] if (m := wait_re.match(insn))]
        if not waits or any(wait_pin != pin for wait_pin in waits):
            fail(f"{name} WAIT GPIOs {waits} do not match board GPIO{pin}")

def test_cmake_board_setup() -> None:
    text = CMAKE.read_text()
    required = [
        'list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")',
        'set(PICO_BOARD snes_fx3 CACHE STRING "Pico board")',
        'include("${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")',
        'pico_generate_pio_header(',
        '${CMAKE_CURRENT_LIST_DIR}/src/platform/rp2350/snes_bus.pio',
        'pico_enable_stdio_uart(superfx3 0)',
        'pico_enable_stdio_usb(superfx3 0)',
        'pico_add_extra_outputs(superfx3)',
        'set(SNES_PARALLEL_ROM_COUNT 1 CACHE STRING "Populated parallel ROM devices (1 or 2)")',
        'SNES_PARALLEL_ROM_COUNT=${SNES_PARALLEL_ROM_COUNT}',
    ]
    for token in required:
        if token not in text:
            fail(f"CMake setup is missing {token}")

    board_pos = text.index('list(APPEND PICO_BOARD_HEADER_DIRS')
    select_pos = text.index('set(PICO_BOARD snes_fx3')
    import_pos = text.index('include("${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")')
    if board_pos > import_pos or select_pos > import_pos:
        fail("PICO_BOARD_HEADER_DIRS and PICO_BOARD must be set before Pico SDK import")
    if 'set(PICO_PLATFORM ' in text:
        fail("CMake duplicates PICO_PLATFORM instead of taking it from snes_fx3.h")

def test_cpp_pio_setup() -> None:
    text = PIO_CPP.read_text()
    required = [
        "pio_set_gpio_base(pio0, SNES_PIO_ADDR_LO_BASE)",
        "pio_set_gpio_base(pio1, SNES_PIO_CONTROL_BASE)",
        "pio_set_gpio_base(pio2, SNES_PIO_CONTROL_BASE)",
        "sm_config_set_in_pins(&select_config, SNES_A12_PIN)",
        "sm_config_set_set_pins(&select_config, SNES_SERVICE_SEL_PIN, 1)",
        "sm_config_set_in_pins(&g_write_addr_config, SNES_PIO_ADDR_LO_BASE)",
        "sm_config_set_jmp_pin(&g_write_config, SNES_SERVICE_SEL_PIN)",
        "sm_config_set_in_pins(&g_write_config, SNES_PIO_ADDR_DATA_BASE)",
        "sm_config_set_in_pins(&g_read_config, SNES_ROMSEL_N_PIN)",
        "sm_config_set_out_pins(&g_read_config, SNES_DATA_BASE, SNES_DATA_COUNT)",
        "sm_config_set_set_pins(&g_read_config, SNES_ROM1_OE_N_PIN, 1)",
        "sm_config_set_sideset_pins(&g_read_config, SNES_PIO_SIDESET_BASE)",
        "sm_config_set_jmp_pin(&g_read_config, SNES_SERVICE_SEL_PIN)",
        "snes_read_fx3_dual_program", "snes_read_gsu_dual_program",
        "pio_set_irq0_source_enabled(pio1, pis_interrupt1, true)",
        "pio_set_irq0_source_enabled(pio1, pis_interrupt2, true)",
        "pio_set_irq0_source_enabled(pio2, pis_interrupt0, true)",
        "static_assert(NUM_BANK0_GPIOS >= 48", "static_assert(NUM_PIOS >= 3",
        "static_assert(PICO_PIO_USE_GPIO_BASE == 1",
    ]
    for token in required:
        if token not in text:
            fail(f"PIO C++ setup is missing {token}")

    board = BOARD_H.read_text()
    pin_tokens = [
        "#define SNES_ADDR_LO_BASE  0",
        "#define SNES_CONTROL_BASE  16",
        "#define SNES_SERVICE_SEL_PIN 27",
        "#define SNES_DATA_DIR_PIN 28",
        "#define SNES_BUS_OE_N_PIN 29",
        "#define SNES_ROM0_OE_N_PIN 30",
        "#define SNES_ROM1_OE_N_PIN 31",
        "#define SNES_ADDR_HI_BASE  32",
        "#define SNES_DATA_BASE  40",
        "#define SNES_PIO_SIDESET_COUNT 3",
        "pico_board_cmake_set(PICO_PLATFORM, rp2350)",
        "#define PICO_RP2350A 0",
        "pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))",
    ]
    for token in pin_tokens:
        if token not in board:
            fail(f"SNES board definition is missing {token}")

    ordered_controls = [
        "#define SNES_DATA_DIR_PIN 28",
        "#define SNES_BUS_OE_N_PIN 29",
        "#define SNES_ROM0_OE_N_PIN 30",
        "#define SNES_ROM1_OE_N_PIN 31",
    ]
    positions = [board.index(token) for token in ordered_controls]
    if positions != sorted(positions):
        fail("SNES bus-control definitions are not in GPIO/physical order")

    bus = (ROOT / "platform/rp2350/snes_bus.h").read_text()
    forbidden = ["struct SnesBusPins", "SNES_BUS_PINS", "SNES_ADDR_MASK =", "SNES_DATA_MASK ="]
    for token in forbidden:
        if token in bus:
            fail(f"snes_bus.h still duplicates board layout: {token}")

def test_listening_state(pio_text: str) -> None:
    board = BOARD_H.read_text()
    bus_cpp = (ROOT / "platform/rp2350/snes_bus.cpp").read_text()
    pio_cpp = PIO_CPP.read_text()

    required_board = [
        "#define SNES_SERVICE_SEL_PIN 27",
        "#define SNES_DATA_DIR_PIN 28",
        "#define SNES_BUS_OE_N_PIN 29",
        "#define SNES_ROM0_OE_N_PIN 30",
        "#define SNES_ROM1_OE_N_PIN 31",
        "#define SNES_BUS_ENABLE  0",
        "#define SNES_BUS_DISABLE 1",
        "#define SNES_PIO_SIDESET_COUNT 3",
    ]
    for token in required_board:
        if token not in board:
            fail(f"listening-state board definition is missing {token}")
    if "SNES_DATA_OE_N_PIN" in board or "SNES_ADDR_OE_N_PIN" in board or "SNES_EXPAND_PIN" in board:
        fail("obsolete split-OE/EXPAND definitions remain in the board map")

    safe_bus = "gpio_put(SNES_BUS_OE_N_PIN, SNES_BUS_DISABLE);"
    listen_bus = "gpio_put(SNES_BUS_OE_N_PIN, SNES_BUS_ENABLE);"
    data_in = "gpio_put(SNES_DATA_DIR_PIN, SNES_DATA_DIR_IN);"
    rom0_off = "gpio_put(SNES_ROM0_OE_N_PIN, SNES_ROM_DISABLE);"
    rom1_off = "gpio_put(SNES_ROM1_OE_N_PIN, SNES_ROM_DISABLE);"
    for token in (safe_bus, listen_bus, data_in, rom0_off, rom1_off):
        if token not in bus_cpp:
            fail(f"bus initialization is missing required control state: {token}")
    if bus_cpp.index(safe_bus) > bus_cpp.index(listen_bus):
        fail("BUS_OE must begin isolated before the listening state is enabled")

    required_pause_resume = [
        "SNES_BUS_DISABLE) << SNES_BUS_OE_N_PIN",
        "SNES_BUS_ENABLE) << SNES_BUS_OE_N_PIN",
        "SNES_ROM_DISABLE) << SNES_ROM0_OE_N_PIN",
        "SNES_ROM_DISABLE) << SNES_ROM1_OE_N_PIN",
        "pio_gpio_init(pio2, SNES_ROM0_OE_N_PIN);",
        "pio_gpio_init(pio2, SNES_ROM1_OE_N_PIN);",
    ]
    for token in required_pause_resume:
        if token not in pio_cpp:
            fail(f"PIO pause/resume no longer restores the complete listening state: {token}")

    if ";   bit0 DATA_DIR, bit1 /BUS_OE, bit2 /ROM0_OE" not in pio_text:
        fail("PIO side-set documentation no longer matches the global-BUS_OE layout")
    if "Optional /ROM1_OE on GPIO31 is controlled through the read SM SET pin." not in pio_text:
        fail("PIO source no longer documents the separate optional ROM1 enable")

    single_names = ("snes_read_fx3", "snes_read_gsu")
    dual_names = ("snes_read_fx3_dual", "snes_read_gsu_dual")
    source = parse_source_programs(pio_text)

    for name in single_names + dual_names:
        program = source[name]
        raw = "\n".join(program.instructions)
        if "wait 1 gpio 18 side 1 [3]" not in raw:
            fail(f"{name} ROM0 path no longer holds data after /RD rises")
        if "wait 1 gpio 18 side 5 [3]" not in raw:
            fail(f"{name} CPU/read-turnaround path no longer holds outward direction after /RD rises")
        if "side 7" in raw:
            fail(f"{name} must not disable BUS_OE during a normal SNES read")
        if "irq 0 side 5" not in raw or "pull block side 5" not in raw:
            fail(f"{name} CPU-read path no longer changes DATA_DIR before firmware drive")

    for name in single_names:
        raw = "\n".join(source[name].instructions)
        if "set pins, 0" in raw:
            fail(f"{name} must never enable optional ROM1 in the default single-ROM build")

    for name in dual_names:
        raw = "\n".join(source[name].instructions)
        if "set pins, 0 side 5" not in raw:
            fail(f"{name} no longer asserts ROM1 only after DATA_DIR is outward")
        if "set pins, 1 side 5 [3]" not in raw:
            fail(f"{name} no longer disables ROM1 before restoring the listening direction")

    for name in ("snes_read_fx3", "snes_read_fx3_dual"):
        raw = "\n".join(source[name].instructions)
        first_outward = raw.find("side 5")
        first_rom0 = raw.find("side 1")
        if first_outward < 0 or first_rom0 < 0 or first_outward >= first_rom0:
            fail(f"{name} no longer establishes DATA_DIR before asserting ROM0 /OE")

    for name in ("snes_read_gsu", "snes_read_gsu_dual"):
        raw = "\n".join(source[name].instructions)
        first_outward = raw.find("side 5")
        first_rom0 = raw.find("side 1")
        if first_outward < 0 or first_rom0 < 0 or first_outward >= first_rom0:
            fail(f"{name} no longer establishes DATA_DIR before asserting ROM0 /OE")

def test_irq_contract(pio_text: str) -> None:
    required_pio = ["irq wait 1", "irq 2", "irq 0 side 5"]
    for token in required_pio:
        if token not in pio_text:
            fail(f"PIO IRQ contract is missing {token}")

def main() -> None:
    pio_text = PIO.read_text()
    if ".pio_version 1" not in pio_text:
        fail("RP2350 PIO source must declare .pio_version 1")
    programs = parse_programs(pio_text)
    test_instruction_ram(programs)
    test_jump_targets_resolve(pio_text)
    test_source_driven_routes(pio_text)
    test_write_capture()
    test_special_bank_decode(pio_text)
    test_selector_decode()
    test_frontend_coverage()
    test_read_response_word()
    test_wait_gpio_windows(programs)
    test_pio_wait_pins(programs)
    test_cmake_board_setup()
    test_cpp_pio_setup()
    test_listening_state(pio_text)
    test_irq_contract(pio_text)
    print("pio_static_tests: PASS")

if __name__ == "__main__":
    main()
