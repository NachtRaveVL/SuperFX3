#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


CORE_PREFIX = "fx/"
PLATFORM_FILES = {
    "platform/rp2350/fx_sync.cpp",
    "platform/rp2350/fx_backend.cpp",
}

CORE_LINE_MIN = 95.0
TOTAL_LINE_MIN = 90.0
BRANCH_TAKEN_MIN = 75.0


@dataclass
class Coverage:
    path: str
    line_percent: float
    lines: int
    branch_reached_percent: float | None = None
    branches: int = 0
    branch_taken_percent: float | None = None

    @property
    def covered_lines(self) -> float:
        return self.lines * self.line_percent / 100.0

    @property
    def reached_branches(self) -> float:
        if self.branch_reached_percent is None:
            return 0.0
        return self.branches * self.branch_reached_percent / 100.0

    @property
    def taken_branches(self) -> float:
        if self.branch_taken_percent is None:
            return 0.0
        return self.branches * self.branch_taken_percent / 100.0


def fail(message: str) -> None:
    print(f"coverage_summary: {message}", file=sys.stderr)
    raise SystemExit(2)


def parse(path: Path) -> list[Coverage]:
    text = path.read_text(encoding="utf-8", errors="replace")
    records: list[Coverage] = []

    current_file: str | None = None
    current_lines: tuple[float, int] | None = None
    branch_reached: tuple[float, int] | None = None
    branch_taken: tuple[float, int] | None = None

    def flush() -> None:
        nonlocal current_file, current_lines, branch_reached, branch_taken
        if current_file is not None and current_lines is not None:
            wanted = current_file.startswith(CORE_PREFIX) or current_file in PLATFORM_FILES
            if wanted and current_file.endswith(".cpp"):
                records.append(Coverage(
                    path=current_file,
                    line_percent=current_lines[0],
                    lines=current_lines[1],
                    branch_reached_percent=branch_reached[0] if branch_reached else None,
                    branches=branch_reached[1] if branch_reached else 0,
                    branch_taken_percent=branch_taken[0] if branch_taken else None,
                ))
        current_file = None
        current_lines = None
        branch_reached = None
        branch_taken = None

    file_re = re.compile(r"^File '(.+)'$")
    percent_re = re.compile(r"^Lines executed:([0-9.]+)% of ([0-9]+)$")
    branch_re = re.compile(r"^Branches executed:([0-9.]+)% of ([0-9]+)$")
    taken_re = re.compile(r"^Taken at least once:([0-9.]+)% of ([0-9]+)$")

    for line in text.splitlines():
        match = file_re.match(line)
        if match:
            flush()
            current_file = match.group(1)
            continue

        if current_file is None:
            continue

        match = percent_re.match(line)
        if match and current_lines is None:
            current_lines = (float(match.group(1)), int(match.group(2)))
            continue

        match = branch_re.match(line)
        if match:
            branch_reached = (float(match.group(1)), int(match.group(2)))
            continue

        match = taken_re.match(line)
        if match:
            branch_taken = (float(match.group(1)), int(match.group(2)))

    flush()

    # gcov may emit several records for the same source if a command was accidentally
    # repeated. Keep one deterministic record per production file.
    unique: dict[str, Coverage] = {}
    for record in records:
        unique[record.path] = record

    if not unique:
        fail("no production coverage records were found in gcov output")

    return [unique[path] for path in sorted(unique)]


def weighted_lines(records: list[Coverage]) -> tuple[float, int]:
    total = sum(item.lines for item in records)
    covered = sum(item.covered_lines for item in records)
    return ((covered / total) * 100.0 if total else 100.0, total)


def weighted_branches(records: list[Coverage], taken: bool) -> tuple[float, int]:
    with_branches = [item for item in records if item.branches]
    total = sum(item.branches for item in with_branches)
    covered = sum(item.taken_branches if taken else item.reached_branches for item in with_branches)
    return ((covered / total) * 100.0 if total else 100.0, total)


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: coverage_summary.py <gcov-output.txt>")

    records = parse(Path(sys.argv[1]))
    core = [item for item in records if item.path.startswith(CORE_PREFIX)]

    core_lines, core_line_count = weighted_lines(core)
    total_lines, total_line_count = weighted_lines(records)
    branch_reached, branch_count = weighted_branches(records, taken=False)
    branch_taken, _ = weighted_branches(records, taken=True)

    print("Host coverage by production source")
    print("----------------------------------")
    print(f"{'Source':42} {'Lines':>10} {'Branch sites':>13} {'Branches taken':>15}")
    for item in records:
        reached = "n/a" if item.branch_reached_percent is None else f"{item.branch_reached_percent:6.2f}%"
        taken = "n/a" if item.branch_taken_percent is None else f"{item.branch_taken_percent:6.2f}%"
        print(f"{item.path:42} {item.line_percent:9.2f}% {reached:>13} {taken:>15}")

    print()
    print(f"Portable core line coverage:       {core_lines:6.2f}% of {core_line_count} lines")
    print(f"Host-testable line coverage:       {total_lines:6.2f}% of {total_line_count} lines")
    print(f"Branch sites reached:              {branch_reached:6.2f}% of {branch_count} branches")
    print(f"Branch alternatives taken:         {branch_taken:6.2f}% of {branch_count} branches")
    print()
    print("Not included in the coverage percentage: main.cpp, snes_bus.cpp, and snes_pio.cpp.")
    print("snes_bus.cpp/snes_pio.cpp are dynamically exercised by the stateful integration harness;")
    print("PIO routing is also interpreted exhaustively. Real GPIO/PIO/DMA timing still requires hardware tests.")

    failures: list[str] = []
    if core_lines < CORE_LINE_MIN:
        failures.append(f"portable core line coverage {core_lines:.2f}% < {CORE_LINE_MIN:.2f}%")
    if total_lines < TOTAL_LINE_MIN:
        failures.append(f"host-testable line coverage {total_lines:.2f}% < {TOTAL_LINE_MIN:.2f}%")
    if branch_taken < BRANCH_TAKEN_MIN:
        failures.append(f"branch alternatives taken {branch_taken:.2f}% < {BRANCH_TAKEN_MIN:.2f}%")

    if failures:
        print("\nCoverage threshold failure:", file=sys.stderr)
        for item in failures:
            print(f"  - {item}", file=sys.stderr)
        raise SystemExit(1)

    print()
    print("Coverage thresholds: PASS")


if __name__ == "__main__":
    main()
