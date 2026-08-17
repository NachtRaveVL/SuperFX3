#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

SETUPS = {"NONE", "CPU_REG", "RAM_MAGIC", "PLOT", "CLEAR", "C2P_NOOP"}
VALIDATORS = {"VCR", "CPU_REG", "COMPLETE", "RAM_MAGIC", "ALU", "ROM", "PLOT", "RPIX", "CLEAR", "C2P_NOOP", "PIPELINE", "PLOT_PATTERN"}
FLAGS = {"CPU_ONLY", "VISUAL", "REPEAT"}


def load_tests(path: Path) -> list[dict]:
    tests = json.loads(path.read_text())
    if not isinstance(tests, list) or not tests:
        raise ValueError("tests.json must contain a non-empty array")
    return tests


def validate(tests: list[dict]) -> None:
    ids: set[str] = set()
    for index, test in enumerate(tests):
        missing = {"id", "name", "kernel", "setup", "validator", "flags", "timeout", "param0", "param1", "expected"} - test.keys()
        if missing:
            raise ValueError(f"test {index} is missing {sorted(missing)}")
        if test["id"] in ids:
            raise ValueError(f"duplicate test id: {test['id']}")
        ids.add(test["id"])
        if test["setup"] not in SETUPS:
            raise ValueError(f"unknown setup {test['setup']} in {test['id']}")
        if test["validator"] not in VALIDATORS:
            raise ValueError(f"unknown validator {test['validator']} in {test['id']}")
        unknown_flags = set(test["flags"]) - FLAGS
        if unknown_flags:
            raise ValueError(f"unknown flags {sorted(unknown_flags)} in {test['id']}")
        if test["kernel"] is None and "CPU_ONLY" not in test["flags"]:
            raise ValueError(f"{test['id']} has no kernel but is not CPU_ONLY")
        if test["kernel"] is not None and "CPU_ONLY" in test["flags"]:
            raise ValueError(f"{test['id']} is CPU_ONLY but also has a kernel")
        for field in ("timeout", "param0", "param1", "expected"):
            value = test[field]
            if not isinstance(value, int) or not 0 <= value <= 0xFFFF:
                raise ValueError(f"{test['id']} {field} is outside 0..65535")
        try:
            test["name"].encode("ascii")
        except UnicodeEncodeError as exc:
            raise ValueError(f"{test['id']} name must be ASCII") from exc
        if len(test["name"]) > 28:
            raise ValueError(f"{test['id']} name is wider than the menu")


def emit(tests: list[dict]) -> str:
    lines = [
        "; Generated from testrom/tests.json. Do not edit by hand.",
        f"TEST_COUNT = {len(tests)}",
        "",
        '.segment "RODATA"',
        ".export TestRegistry",
        "TestRegistry:",
    ]

    for test in tests:
        ident = test["id"].upper().replace("-", "_")
        kernel = test["kernel"]
        flags = test["flags"]
        flag_expr = " | ".join(f"TEST_FLAG_{flag}" for flag in flags) or "0"
        lines += [
            f"    ; {test['id']}",
            f"    .word TestName_{ident}",
            f"    .word {'0' if kernel is None else 'FX_ENTRY_' + kernel}",
            f"    .word ${test['timeout']:04X}",
            f"    .word ${test['param0']:04X}",
            f"    .word ${test['param1']:04X}",
            f"    .word ${test['expected']:04X}",
            f"    .byte {'0' if kernel is None else 'FX_BANK_' + kernel}",
            f"    .byte SETUP_{test['setup']}",
            f"    .byte VALIDATE_{test['validator']}",
            f"    .byte {flag_expr}",
        ]

    lines += ["", "; Menu labels", ""]
    for test in tests:
        ident = test["id"].upper().replace("-", "_")
        escaped = test["name"].replace('"', '\\"')
        lines += [f"TestName_{ident}:", f'    .byte "{escaped}", 0']
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tests", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    tests = load_tests(args.tests)
    validate(tests)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(emit(tests))
    print(f"Wrote {args.output} ({len(tests)} tests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
