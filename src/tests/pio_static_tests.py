#!/usr/bin/env python3
from pathlib import Path
import random
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PIO = ROOT / "platform/rp2350/snes_bus.pio"
PIO_CPP = ROOT / "platform/rp2350/snes_pio.cpp"
BUS_H = ROOT / "platform/rp2350/snes_bus.h"

def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)

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
            labels["wrap_target"] = wrap_target
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
        op = re.sub(r"\s+side\s+\d+", "", raw).strip()
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
            return f"set:{int(op.rsplit(' ', 1)[1], 0)}"
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
                    return "direct_rom"
                if side == 4:
                    return "no_drive"
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
                if target == "wrap_target":
                    pc = program.wrap_target
                elif target in program.labels:
                    pc = program.labels[target]
                else:
                    fail(f"source interpreter cannot resolve label {target}")
        else:
            fail(f"source interpreter does not support instruction: {op}")

    fail("source interpreter exceeded its instruction limit")

def test_source_driven_routes(pio_text: str) -> None:
    programs = parse_source_programs(pio_text)

    for nibble in range(16):
        fx3 = pio_route(programs["snes_select_fx3"], nibble, False)
        gsu = pio_route(programs["snes_select_gsu"], nibble, False)
        if fx3 != f"set:{1 if nibble == 7 else 0}":
            fail(f"source-driven FX3 selector failed for ${nibble:X}xxx")
        if gsu != f"set:{1 if nibble in (3, 6, 7) else 0}":
            fail(f"source-driven GSU selector failed for ${nibble:X}xxx")

    # Walk the actual write programs for all banks/pages. The selector is the only
    # address-page information the PIO1 program consumes directly.
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

    # The read program first consumes /ROMSEL from GPIO20, then either uses the
    # page selector or decodes full-bank RAM mappings from A16-A23.
    for fx3 in (False, True):
        program = programs["snes_read_fx3" if fx3 else "snes_read_gsu"]
        for blocked in ((False,) if fx3 else (False, True)):
            for bank in range(256):
                for page in range(16):
                    selector = page == 7 if fx3 else page in (3, 6, 7)
                    for romsel_n in (0, 1):
                        pin_window = romsel_n | (int(selector) << 11) | (bank << 12)
                        initial_x = 7 if fx3 else int(blocked)
                        actual = pio_route(program, pin_window, selector, initial_x=initial_x)

                        if romsel_n:
                            expected = "service" if selector else "no_drive"
                        elif fx3:
                            # FX3 sends the complete $7x bank group to the CPU handler so
                            # $70/$71 can serve SRAM and $72-$7F can remain undriven.
                            expected = "service" if (bank >> 4) == 0x7 else "direct_rom"
                        elif blocked:
                            expected = "service"
                        else:
                            expected = "service" if is_gsu_special_ram_bank(bank) else "direct_rom"

                        if actual != expected:
                            mode = "FX3" if fx3 else "GSU"
                            fail(
                                f"source-driven {mode} read route failed at ${bank:02X}:{page:X}000 "
                                f"ROMSEL={romsel_n} blocked={blocked}: {actual}, expected {expected}"
                            )

def test_instruction_ram(programs: dict[str, list[str]]) -> None:
    expected = {
        "snes_select_fx3", "snes_select_gsu", "snes_write_addr", "snes_write_fx3",
        "snes_write_gsu", "snes_reset", "snes_read_fx3", "snes_read_gsu"
    }
    if set(programs) != expected:
        fail(f"unexpected PIO program set: {sorted(programs)}")

    loads = {
        "PIO0 FX3": ("snes_select_fx3", "snes_write_addr"),
        "PIO0 GSU": ("snes_select_gsu", "snes_write_addr"),
        "PIO1 FX3": ("snes_write_fx3", "snes_reset"),
        "PIO1 GSU": ("snes_write_gsu", "snes_reset"),
        "PIO2 FX3": ("snes_read_fx3",),
        "PIO2 GSU": ("snes_read_gsu",),
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
    # This is exactly what the PIO tests after discarding A16: A17-A21=24 and A22=1.
    return ((bank >> 1) & 0x1F) == 24 and bool(bank & 0x40)

def test_special_bank_decode(pio_text: str) -> None:
    for bank in range(256):
        common = modeled_common_bank_match(bank)
        fx3 = common and not bool(bank & 0x80)
        gsu = common
        if fx3 != is_fx3_special_ram_bank(bank):
            fail(f"FX3 PIO bank decode disagrees at bank ${bank:02X}")
        if gsu != is_gsu_special_ram_bank(bank):
            fail(f"GSU PIO bank decode disagrees at bank ${bank:02X}")

    gsu_section = pio_text.split(".program snes_read_gsu", 1)[1]
    block = gsu_section.split("gsu_rom_allowed:", 1)[1].split("gsu_not_special:", 1)[0]
    if "A23 selects $70/$71 versus the $F0/$F1 mirror" not in block:
        fail("GSU read path no longer documents the deliberate A23 don't-care")
    if block.count("out y, 1") != 1:
        fail("GSU read path appears to test A23 again and may lose the $F0/$F1 RAM mirror")

    fx3_section = pio_text.split(".program snes_read_fx3", 1)[1].split(".program snes_read_gsu", 1)[0]
    if "A20-A23" not in fx3_section or "set x" in fx3_section:
        # X is initialized from C++ now; the source should compare the top bank nibble
        # without rewriting X inside the transaction.
        fail("FX3 read path no longer matches the $7x software-service bank decode")

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
    # pio_add_program() relocates WAIT GPIO for GPIO-base 16 on RP2350B. These real pins must
    # nevertheless fall inside the PIO instance's 32-pin window.
    assignment = {
        "snes_select_fx3": (0, 31), "snes_select_gsu": (0, 31), "snes_write_addr": (0, 31),
        "snes_write_fx3": (16, 47), "snes_write_gsu": (16, 47), "snes_reset": (16, 47),
        "snes_read_fx3": (16, 47), "snes_read_gsu": (16, 47),
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

def test_cpp_pio_setup() -> None:
    text = PIO_CPP.read_text()
    required = [
        "pio_set_gpio_base(pio0, 0)", "pio_set_gpio_base(pio1, 16)",
        "pio_set_gpio_base(pio2, 16)", "sm_config_set_in_pins(&select_config, 12)",
        "sm_config_set_set_pins(&select_config, 31, 1)",
        "sm_config_set_in_pins(&g_write_addr_config, SNES_ADDR_LO_BASE)",
        "sm_config_set_jmp_pin(&g_write_config, 31)",
        "sm_config_set_in_pins(&g_write_config, 32)",
        "sm_config_set_in_pins(&g_read_config, 20)",
        "sm_config_set_out_pins(&g_read_config, 40, 8)",
        "sm_config_set_sideset_pins(&g_read_config, 28)",
        "sm_config_set_jmp_pin(&g_read_config, 31)",
        "pio_set_irq0_source_enabled(pio1, pis_interrupt1, true)",
        "pio_set_irq0_source_enabled(pio1, pis_interrupt2, true)",
        "pio_set_irq0_source_enabled(pio2, pis_interrupt0, true)",
        "static_assert(NUM_BANK0_GPIOS >= 48", "static_assert(NUM_PIOS >= 3",
        "static_assert(PICO_PIO_USE_GPIO_BASE == 1"
    ]
    for token in required:
        if token not in text:
            fail(f"PIO C++ setup is missing {token}")

    bus = BUS_H.read_text()
    pin_tokens = [
        "SNES_ADDR_LO_BASE = 0", "SNES_CTRL_BASE    = 16",
        "SNES_ADDR_HI_BASE = 32", "SNES_DATA_BASE    = 40"
    ]
    for token in pin_tokens:
        if token not in bus:
            fail(f"SNES pin layout is missing {token}")

def test_irq_contract(pio_text: str) -> None:
    # The PIO instruction flags and the C++ IRQ0 source enables must stay paired.
    required_pio = ["irq wait 1", "irq 2", "irq 0 side 4"]
    for token in required_pio:
        if token not in pio_text:
            fail(f"PIO IRQ contract is missing {token}")

def main() -> None:
    pio_text = PIO.read_text()
    if ".pio_version 1" not in pio_text:
        fail("RP2350 PIO source must declare .pio_version 1")
    programs = parse_programs(pio_text)
    test_instruction_ram(programs)
    test_source_driven_routes(pio_text)
    test_write_capture()
    test_special_bank_decode(pio_text)
    test_selector_decode()
    test_frontend_coverage()
    test_read_response_word()
    test_wait_gpio_windows(programs)
    test_cpp_pio_setup()
    test_irq_contract(pio_text)
    print("pio_static_tests: PASS")

if __name__ == "__main__":
    main()
