#!/usr/bin/env python3
"""Generate the native Direct3D ABI snapshot consumed by D3DAPI.h."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile


HEADERS = (
    ("um", "d3dcommon.h"),
    ("shared", "dxgiformat.h"),
    ("shared", "dxgitype.h"),
    ("shared", "dxgicommon.h"),
    ("shared", "dxgi.h"),
    ("shared", "dxgi1_2.h"),
    ("shared", "dxgi1_3.h"),
    ("shared", "dxgi1_4.h"),
    ("shared", "dxgi1_5.h"),
    ("shared", "dxgi1_6.h"),
    ("um", "d3d12sdklayers.h"),
    ("um", "d3d12.h"),
    ("um", "dxcore_interface.h"),
    ("um", "dxcore.h"),
    ("um", "d3d11shader.h"),
    ("um", "d3d12shader.h"),
    ("um", "d3dcompiler.h"),
    ("um", "dxcapi.h"),
)

PRELUDE = r"""
#include <Windows.h>
#include <objidl.h>
#define SORA_D3D_DETAIL_BEGIN namespace Sora::PAL::D3D::Detail {
#define SORA_D3D_DETAIL_END }
SORA_D3D_DETAIL_BEGIN

"""

POSTLUDE = "\nSORA_D3D_DETAIL_END\n"

WINDOWS_TYPE_NAMES = {
    "_ENUM_FLAG_SIZED_INTEGER": "EnumFlagSizedInteger",
    "BOOL": "Bool",
    "BSTR": "AutomationString",
    "BYTE": "Byte",
    "CHAR": "Char",
    "CLSID": "ClassId",
    "DWORD": "DWord",
    "FILETIME": "FileTime",
    "FLOAT": "Float",
    "GUID": "Guid",
    "HANDLE": "Handle",
    "HDC": "DeviceContextHandle",
    "HMODULE": "ModuleHandle",
    "HMONITOR": "MonitorHandle",
    "HRESULT": "HResult",
    "HWND": "WindowHandle",
    "IID": "InterfaceId",
    "IMalloc": "MemoryAllocator",
    "INT": "Int",
    "INT8": "Int8",
    "INT16": "Int16",
    "INT32": "Int32",
    "INT64": "Int64",
    "IPropertyBag": "PropertyBag",
    "ISequentialStream": "SequentialStream",
    "IStream": "Stream",
    "IUnknown": "Unknown",
    "LARGE_INTEGER": "LargeInteger",
    "LONG": "Long",
    "LONG_PTR": "PointerSizedLong",
    "LPCSTR": "ConstCharPointer",
    "LPCVOID": "ConstVoidPointer",
    "LPCWSTR": "ConstWideCharPointer",
    "LPSTR": "CharPointer",
    "LPVOID": "VoidPointer",
    "LPWSTR": "WideCharPointer",
    "LUID": "Luid",
    "POINT": "Point",
    "PVOID": "VoidPointer",
    "RECT": "Rect",
    "REFCLSID": "RefClassId",
    "REFGUID": "RefGuid",
    "REFIID": "RefInterfaceId",
    "RPC_IF_HANDLE": "RpcInterfaceHandle",
    "SECURITY_ATTRIBUTES": "SecurityAttributes",
    "SIZE_T": "Size",
    "UINT": "UInt",
    "UINT8": "UInt8",
    "UINT16": "UInt16",
    "UINT32": "UInt32",
    "UINT64": "UInt64",
    "ULONG": "ULong",
    "ULONG_PTR": "PointerSizedULong",
    "UUID": "Uuid",
    "WCHAR": "WChar",
    "size_t": "Size",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def newest_sdk_include() -> Path:
    root = Path(os.environ.get("WINDOWSSDKDIR", r"C:\Program Files (x86)\Windows Kits\10")) / "Include"
    versions = [path for path in root.iterdir() if path.is_dir() and re.fullmatch(r"\d+\.\d+\.\d+\.\d+", path.name)]
    if not versions:
        raise RuntimeError(f"no Windows SDK versions found under {root}")
    return max(versions, key=lambda path: tuple(map(int, path.name.split("."))))


def find_clang_cl(explicit: Path | None) -> Path:
    candidates = [
        explicit,
        Path(r"T:\toolchains\coca-toolchain-p2996\bin\clang-cl.exe"),
    ]
    for candidate in candidates:
        if candidate is not None and candidate.is_file():
            return candidate
    raise RuntimeError("clang-cl was not found; pass --clang-cl")


def source_text(sdk: Path) -> str:
    includes = "\n".join(f'#include "{(sdk / directory / name).as_posix()}"' for directory, name in HEADERS)
    return PRELUDE + includes + POSTLUDE


def preprocess(clang_cl: Path, sdk: Path) -> str:
    with tempfile.TemporaryDirectory(prefix="sora-d3d-api-") as directory:
        source = Path(directory) / "snapshot.cpp"
        source.write_text(source_text(sdk), encoding="utf-8")
        command = [
            str(clang_cl),
            "/nologo",
            "/std:c++latest",
            "/permissive-",
            "/Zc:__cplusplus",
            "/E",
            "/DWIN32_LEAN_AND_MEAN",
            "/DNOMINMAX",
            "/DINITGUID",
            str(source),
        ]
        completed = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8")

    allowed = {"snapshot.cpp", *(name.casefold() for _, name in HEADERS)}
    selected: list[str] = []
    current = ""
    marker = re.compile(r'^\s*#\s+\d+\s+"([^"]+)"')
    for line in completed.stdout.splitlines():
        match = marker.match(line)
        if match is not None:
            current = Path(match.group(1)).name.casefold()
            continue
        if current in allowed:
            selected.append(line)

    filtered = "\n".join(selected)
    start = filtered.find("namespace Sora::PAL::D3D::Detail {")
    if start < 0:
        raise RuntimeError("native snapshot begin marker was not emitted")
    body = filtered[start:]
    end = body.rfind("}")
    if end < 0:
        raise RuntimeError("native snapshot end marker was not emitted")
    return body[: end + 1]


def qualify_windows_types(snapshot: str) -> str:
    snapshot = re.sub(r"^\s*struct\s+(?:IStream|IPropertyBag|IMalloc)\s*;\s*$", "", snapshot, flags=re.MULTILINE)
    enum_flag = re.compile(r"\b_ENUM_FLAG_SIZED_INTEGER<([^<>]+)>::type\b")
    snapshot = enum_flag.sub(r"WindowsSystem::EnumFlagSizedInteger<\1>::Type", snapshot)
    names = "|".join(sorted(map(re.escape, WINDOWS_TYPE_NAMES), key=len, reverse=True))
    return re.sub(
        rf"\b(?:{names})\b",
        lambda match: f"WindowsSystem::{WINDOWS_TYPE_NAMES[match.group(0)]}",
        snapshot,
    )


def declaration_names(snapshot: str) -> list[str]:
    prefixes = ("D3D", "ID3D", "DXGI", "IDXGI", "DXCore", "IDXCore", "Dxc", "IDxc")
    declarations = r"\b(?:struct|enum|union|using|typedef\s+(?:struct|enum|union)?)\s+([A-Za-z_]\w*)"
    names = set(re.findall(declarations, snapshot))
    names.update(re.findall(r"\b(?:const\s+)?(?:GUID|IID|CLSID)\s+([A-Za-z_]\w*)\b", snapshot))
    for body in re.findall(r"\benum\s+[A-Za-z_]\w*\s*\{([^}]*)\}", snapshot, re.DOTALL):
        names.update(re.findall(r"(?:^|,)\s*([A-Za-z_]\w*)\s*(?:=|,|$)", body))
    return sorted(name for name in names if name.startswith(prefixes) or category(name) is not None)


def category(name: str) -> str | None:
    if name.startswith(("D3D12", "ID3D12", "IID_ID3D12", "CLSID_D3D12")):
        return "D3D12"
    if name.startswith(("DXGI", "IDXGI", "IID_IDXGI")):
        return "DXGI"
    if name.startswith(("DXCore", "IDXCore", "IID_IDXCore")):
        return "DXCore"
    if name.startswith(("Dxc", "IDxc", "IID_IDxc", "CLSID_Dxc")):
        return "DXC"
    if name.startswith(("D3D", "ID3D", "IID_ID3D10", "IID_ID3D11")):
        return "Compiler"
    return None


def render(sdk: Path, snapshot: str) -> str:
    hashes = "\n".join(
        f" * - {name}: {sha256(sdk / directory / name)}" for directory, name in HEADERS
    )
    aliases: dict[str, list[str]] = {name: [] for name in ("D3D12", "DXGI", "DXCore", "Compiler", "DXC")}
    for name in declaration_names(snapshot):
        owner = category(name)
        if owner is not None:
            aliases[owner].append(f"    using Detail::{name};")

    projections = []
    for owner, lines in aliases.items():
        projections.append(f"namespace Sora::PAL::D3D::{owner} {{\n\n" + "\n".join(lines) +
                           f"\n\n}} // namespace Sora::PAL::D3D::{owner}\n")

    return f"""/**
 * @file D3DNativeAPI.inc
 * @brief Generated self-contained Direct3D ABI snapshot.
 * @ingroup PAL
 * @details Source SDK: {sdk.name}. Input hashes:
{hashes}
 * Generated by tools/generate_d3d_api.py; do not edit manually.
 */

#if defined(PLATFORM_WINDOWS)

{snapshot}

{''.join(projections)}
#endif
"""


def format_generated(clang_cl: Path, generated: str) -> str:
    clang_format = clang_cl.with_name("clang-format.exe")
    if not clang_format.is_file():
        raise RuntimeError(f"clang-format was not found beside clang-cl: {clang_format}")
    completed = subprocess.run(
        [str(clang_format), "--style={BasedOnStyle: LLVM, ColumnLimit: 120}"],
        input=generated,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    return completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--clang-cl", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    sdk = args.sdk or newest_sdk_include()
    clang_cl = find_clang_cl(args.clang_cl)
    for directory, name in HEADERS:
        path = sdk / directory / name
        if not path.is_file():
            raise RuntimeError(f"required SDK header is missing: {path}")

    generated = format_generated(clang_cl, render(sdk, qualify_windows_types(preprocess(clang_cl, sdk))))
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != generated:
            raise SystemExit(f"generated output differs: {args.output}")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
