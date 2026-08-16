#!/usr/bin/env python3
"""Compare normalized Mashiro and Sora RecordRingBuffer assembly probes instruction by instruction."""

from __future__ import annotations

import argparse
import difflib
import re
from pathlib import Path


FUNCTION = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:$")
INSTRUCTION = re.compile(r"^\s*[0-9a-fA-F]+:\s+(.+)$")
TARGET = re.compile(r"0x[0-9a-fA-F]+ <[^>]+?(\+0x[0-9a-fA-F]+)?>")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="llvm-objdump --disassemble --no-show-raw-insn output")
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--expect-sora-nonexpanding",
        action="store_true",
        help="accept intentional instruction changes only when the Sora path has no more instructions",
    )
    return parser.parse_args()


def normalize(instruction: str) -> str:
    return TARGET.sub(lambda match: f"<target{match.group(1) or ''}>", instruction.strip())


def trim_alignment(instructions: list[str]) -> list[str]:
    end = len(instructions)
    while end != 0 and instructions[end - 1].split(maxsplit=1)[0] in {"nop", "nopw", "nopl"}:
        end -= 1
    return instructions[:end]


def load_functions(path: Path) -> dict[str, list[str]]:
    functions: dict[str, list[str]] = {}
    current: list[str] | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        function = FUNCTION.match(line)
        if function:
            current = functions.setdefault(function.group(1), [])
            continue
        instruction = INSTRUCTION.match(line)
        if current is not None and instruction:
            current.append(normalize(instruction.group(1)))
    return functions


def main() -> None:
    args = parse_args()
    functions = load_functions(args.input)
    rows = []
    failures = []
    for topology in ("Local", "Spsc", "Mpsc", "Spmc", "Mpmc"):
        for operation in ("Write", "Consume"):
            mashiro_name = f"Mashiro{topology}Record{operation}"
            sora_name = f"Sora{topology}Record{operation}"
            if mashiro_name not in functions or sora_name not in functions:
                raise ValueError(f"missing assembly probe pair: {mashiro_name}, {sora_name}")
            mashiro = trim_alignment(functions[mashiro_name])
            sora = trim_alignment(functions[sora_name])
            identical = mashiro == sora
            accepted = identical or (args.expect_sora_nonexpanding and len(sora) <= len(mashiro))
            rows.append((topology, operation, len(mashiro), len(sora), identical, accepted))
            if not accepted:
                failures.extend(
                    difflib.unified_diff(mashiro, sora, fromfile=mashiro_name, tofile=sora_name, lineterm="")
                )

    lines = [
        "# RecordRingBuffer assembly audit",
        "",
        "Absolute code addresses, symbol names, and trailing alignment NOPs are normalized. Executable instructions",
        "and operands are compared exactly. Non-identical paths are accepted only in explicit non-expansion mode and",
        "only when Sora emits no more instructions than Mashiro.",
        "",
        "| Topology | Operation | Mashiro instructions | Sora instructions | Delta | Exact match | Accepted |",
        "|---|---|---:|---:|---:|---|---|",
    ]
    lines.extend(
        f"| {topology} | {operation} | {mashiro_count} | {sora_count} | {sora_count - mashiro_count:+d} | "
        f"{'yes' if identical else 'no'} | {'yes' if accepted else 'no'} |"
        for topology, operation, mashiro_count, sora_count, identical, accepted in rows
    )

    logger_rows = []
    for operation in ("Write64", "Drain"):
        legacy_name = f"LegacyLogger{operation}"
        sora_name = f"SoraLogger{operation}"
        if legacy_name not in functions or sora_name not in functions:
            raise ValueError(f"missing Logger assembly probe pair: {legacy_name}, {sora_name}")
        legacy = trim_alignment(functions[legacy_name])
        sora = trim_alignment(functions[sora_name])
        logger_rows.append((operation, len(legacy), len(sora)))

    lines.extend(
        [
            "",
            "## StructuredLogger replacement diagnostics",
            "",
            "These paths intentionally compare different data structures and are descriptive rather than an exact-code",
            "migration gate.",
            "",
            "| Operation | Legacy ThreadRing instructions | Sora RecordRingBuffer instructions | Delta |",
            "|---|---:|---:|---:|",
        ]
    )
    lines.extend(
        f"| {operation} | {legacy_count} | {sora_count} | {sora_count - legacy_count:+d} |"
        for operation, legacy_count, sora_count in logger_rows
    )
    if failures:
        lines.extend(["", "```diff", *failures, "```"])
    report = "\n".join(lines) + "\n"
    print(report, end="")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
