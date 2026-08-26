#!/usr/bin/env python3
"""Build and query Sora's deterministic source-navigation index."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable


PROJECT_ROOT = Path(__file__).resolve().parents[1]
JSON_PATH = PROJECT_ROOT / "docs" / "code-index.json"
MARKDOWN_PATH = PROJECT_ROOT / "docs" / "code-index.md"
SCAN_ROOTS = ("include", "src", "tests", "benchmarks", "demos", "docs", "Experimental", "tools")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ixx", ".md", ".py"}
EXCLUDED_PARTS = {".git", ".vs", ".vscode", "__pycache__", "build"}
GENERATED_PATHS = {"docs/code-index.json", "docs/code-index.md"}

BRIEF_PATTERN = re.compile(r"@brief\s+(.*?)(?=\n\s*\*\s*@|\n\s*\*/)", re.DOTALL)
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
NAMESPACE_PATTERN = re.compile(r"\bnamespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\{")
DECLARATION_PATTERN = re.compile(
    r"\b(?:class|struct|union|enum\s+class|enum|concept)\s+(?:\[\[[^\]]*\]\]\s*)*([A-Za-z_]\w*)"
)
ALIAS_PATTERN = re.compile(r"\busing\s+([A-Za-z_]\w*)\s*=")
COMMENT_PATTERN = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)


def configure_output() -> None:
    """Make query output independent of the active Windows code page."""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")


def relative_path(path: Path) -> str:
    return path.relative_to(PROJECT_ROOT).as_posix()


def is_source_file(path: Path) -> bool:
    relative = relative_path(path)
    return (
        relative not in GENERATED_PATHS
        and not any(part in EXCLUDED_PARTS for part in path.relative_to(PROJECT_ROOT).parts)
        and (path.suffix.lower() in SOURCE_SUFFIXES or path.name == "CMakeLists.txt")
    )


def discover_files() -> list[Path]:
    files: set[Path] = set()
    for root_file_name in ("AGENTS.md", "CMakeLists.txt"):
        root_file = PROJECT_ROOT / root_file_name
        if root_file.is_file():
            files.add(root_file)
    for root_name in SCAN_ROOTS:
        root = PROJECT_ROOT / root_name
        if not root.is_dir():
            continue
        files.update(path for path in root.rglob("*") if path.is_file() and is_source_file(path))
    return sorted(files, key=relative_path)


def collapse_documentation(text: str) -> str:
    lines = []
    for line in text.splitlines():
        line = re.sub(r"^\s*\*\s?", "", line).strip()
        if line:
            lines.append(line)
    return " ".join(lines)


def markdown_brief(text: str) -> str:
    heading = re.search(r"^#\s+(.+?)\s*$", text, re.MULTILINE)
    if heading is None:
        return ""
    following = text[heading.end() :]
    paragraph = re.search(r"\n\s*([^#\n].*?)(?=\n\s*\n|\Z)", following, re.DOTALL)
    return collapse_documentation(paragraph.group(1)) if paragraph else heading.group(1).strip()


def python_brief(text: str) -> str:
    try:
        module = ast.parse(text)
    except SyntaxError:
        return ""
    doc = ast.get_docstring(module, clean=True)
    return doc.splitlines()[0].strip() if doc else ""


def extract_brief(path: Path, text: str) -> str:
    match = BRIEF_PATTERN.search(text)
    if match is not None:
        return collapse_documentation(match.group(1))
    if path.suffix.lower() == ".md":
        return markdown_brief(text)
    if path.suffix.lower() == ".py":
        return python_brief(text)
    first_comment = re.search(r"^\s*#\s+(.+?)\s*$", text, re.MULTILINE)
    return first_comment.group(1).strip() if first_comment else ""


def unique(values: Iterable[str], limit: int | None = None) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
        if limit is not None and len(result) == limit:
            break
    return result


def module_name(path: Path) -> str:
    parts = path.relative_to(PROJECT_ROOT).parts
    if len(parts) >= 3 and parts[0] == "include" and parts[1] == "Sora":
        return Path(parts[2]).stem
    if len(parts) >= 2 and parts[0] in {"src", "tests", "benchmarks", "demos", "Experimental"}:
        return Path(parts[1]).stem
    return "Project"


def file_kind(path: Path) -> str:
    suffix = path.suffix.lower()
    if path.name == "CMakeLists.txt":
        return "cmake"
    if suffix in {".h", ".hh", ".hpp", ".hxx", ".ixx"}:
        return "header"
    if suffix in {".c", ".cc", ".cpp", ".cxx"}:
        return "source"
    return suffix.removeprefix(".") or "file"


def build_entry(path: Path) -> dict[str, Any]:
    content = path.read_bytes()
    text = content.decode("utf-8", errors="replace")
    code = COMMENT_PATTERN.sub(" ", text)
    declarations = list(DECLARATION_PATTERN.findall(code))
    declarations.extend(ALIAS_PATTERN.findall(code))
    return {
        "kind": file_kind(path),
        "module": module_name(path),
        "brief": extract_brief(path, text),
        "namespaces": unique(NAMESPACE_PATTERN.findall(code)),
        "keySymbols": unique(declarations, limit=32),
        "soraIncludes": unique(value for value in INCLUDE_PATTERN.findall(text) if value.startswith("Sora/")),
        "lineCount": len(text.splitlines()),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


def build_index() -> dict[str, Any]:
    return {
        "schemaVersion": 1,
        "files": {relative_path(path): build_entry(path) for path in discover_files()},
    }


def json_text(index: dict[str, Any]) -> str:
    return json.dumps(index, ensure_ascii=False, indent=2) + "\n"


def markdown_cell(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ")


def markdown_text(index: dict[str, Any]) -> str:
    lines = [
        "# Sora Code Index",
        "",
        "Generated by `tools/code_index.py`; do not edit manually. Source declarations remain authoritative.",
        "",
        "| File | Module | Brief | Key symbols |",
        "|---|---|---|---|",
    ]
    for path, entry in index["files"].items():
        symbols = ", ".join(entry["keySymbols"][:8])
        lines.append(
            f"| `{markdown_cell(path)}` | {markdown_cell(entry['module'])} | "
            f"{markdown_cell(entry['brief'])} | {markdown_cell(symbols)} |"
        )
    lines.append("")
    return "\n".join(lines)


def write_index() -> int:
    index = build_index()
    JSON_PATH.parent.mkdir(parents=True, exist_ok=True)
    JSON_PATH.write_text(json_text(index), encoding="utf-8", newline="\n")
    MARKDOWN_PATH.write_text(markdown_text(index), encoding="utf-8", newline="\n")
    print(f"Indexed {len(index['files'])} files.")
    return 0


def check_index() -> int:
    index = build_index()
    expected = ((JSON_PATH, json_text(index)), (MARKDOWN_PATH, markdown_text(index)))
    stale = [path for path, content in expected if not path.is_file() or path.read_text(encoding="utf-8") != content]
    if stale:
        for path in stale:
            print(f"stale: {relative_path(path)}", file=sys.stderr)
        print("Run: python tools/code_index.py build", file=sys.stderr)
        return 1
    print(f"Code index is current ({len(index['files'])} files).")
    return 0


def query_index(terms: list[str], limit: int) -> int:
    index = build_index()
    needles = [term.casefold() for term in terms]
    matches: list[tuple[int, str, dict[str, Any]]] = []
    for path, entry in index["files"].items():
        path_text = path.casefold()
        symbol_text = " ".join(entry["keySymbols"]).casefold()
        brief_text = entry["brief"].casefold()
        dependency_text = " ".join(entry["soraIncludes"]).casefold()
        namespace_text = " ".join(entry["namespaces"]).casefold()
        searchable = " ".join((path_text, symbol_text, brief_text, dependency_text, namespace_text))
        if not all(needle in searchable for needle in needles):
            continue
        score = sum(
            12 if Path(path).stem.casefold() == needle else 7 if needle in path_text else 5 if needle in symbol_text
            else 3 if needle in brief_text else 1
            for needle in needles
        )
        matches.append((score, path, entry))
    matches.sort(key=lambda item: (-item[0], item[1]))
    for _, path, entry in matches[:limit]:
        symbols = ", ".join(entry["keySymbols"][:8])
        print(f"{path}\n  {entry['brief'] or '(no brief)'}\n  symbols: {symbols or '(none)'}")
    return 0 if matches else 1


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    subcommands.add_parser("build", help="regenerate JSON and Markdown indexes")
    subcommands.add_parser("check", help="fail when generated indexes differ from live sources")
    query = subcommands.add_parser("query", help="search live source metadata")
    query.add_argument("terms", nargs="+", help="terms that must all match an indexed entry")
    query.add_argument("--limit", type=int, default=20, help="maximum number of results")
    return parser.parse_args()


def main() -> int:
    configure_output()
    arguments = parse_arguments()
    if arguments.command == "build":
        return write_index()
    if arguments.command == "check":
        return check_index()
    return query_index(arguments.terms, max(arguments.limit, 1))


if __name__ == "__main__":
    raise SystemExit(main())
