#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "check_qspi_sd_sram", Path(__file__).resolve().parents[1] / "tools/check_qspi_sd_sram.py")
assert spec and spec.loader
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def fixture(body: str = "", helper: str = "", helper_address: int = 0x20001000) -> str:
    roots = "\n".join(f"{0x20000000 + i * 0x100:08x} <{name}>:\n"
                      f"{body if i == 0 else '    4770 bx lr'}"
                      for i, name in enumerate(checker.ROOTS))
    return roots + f"\n{helper_address:08x} <helper()>:\n{helper or '    4770 bx lr'}\n"


def main() -> None:
    assert checker.audit(fixture()) == []
    branch = "20000000: f000 f800 bl 20001000 <helper()>"
    assert checker.audit(fixture(branch)) == []
    assert any("code outside SRAM" in error for error in checker.audit(
        fixture("20000000: f000 f800 bl 10001000 <helper()>", helper_address=0x10001000)))
    assert any("XIP literal" in error for error in checker.audit(
        fixture(branch, "20001000: 10012345 .word 0x10012345")))
    assert any("XIP address constructed" in error for error in checker.audit(fixture(
        "20000000: f240 0000 movw r0, #4660\n20000004: f2c1 0000 movt r0, #4096")))
    assert any("unaudited indirect call" in error for error in checker.audit(
        fixture("20000000: 4798 blx r3")))
    assert any("unaudited branch table" in error for error in checker.audit(
        fixture("20000000: e8df f003 tbb [pc, r3]")))
    assert any("unresolved branch" in error for error in checker.audit(
        fixture("20000000: f000 f800 bl 20002000 <absent()>")))
    assert any("missing SRAM root" in error for error in checker.audit(""))
    print("qspi_sd_sram_tests: PASS")


if __name__ == "__main__":
    main()
