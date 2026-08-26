#!/usr/bin/env python3
"""Generate Sora's self-contained Vulkan ABI and dispatch tables."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import xml.etree.ElementTree as ET

CORE_SHA256 = "2f7aa635db9a068294c70de0f90d74c869602c132c30f306cb73b973a3f9b502"
REGISTRY_SHA256 = "f6f43874297ce094769714de2fc51ec683c12eed9b7615da20b34ab4a2f81470"
GLOBAL = {"vkCreateInstance", "vkEnumerateInstanceExtensionProperties", "vkEnumerateInstanceLayerProperties",
          "vkEnumerateInstanceVersion", "vkGetInstanceProcAddr"}
DEVICE_HANDLES = {"VkDevice", "VkQueue", "VkCommandBuffer", "VkExternalComputeQueueNV"}


def flatten(path: pathlib.Path, visited: set[pathlib.Path]) -> str:
    path = path.resolve()
    if path in visited:
        return ""
    visited.add(path)
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"^#ifndef [^\n]+\n#define [^\n]+\n", "", text, count=1)
    text = re.sub(r"\n#ifdef __cplusplus\nextern \"C\" \{\n#endif\n", "\n", text, count=1)
    text = re.sub(r"\n#ifdef __cplusplus\n}\n#endif\n\n#endif\s*$", "\n", text, count=1)

    def include(match: re.Match[str]) -> str:
        child = (path.parent / match.group(1)).resolve()
        if not child.is_relative_to(path.parent.parent):
            raise ValueError(f"Vulkan include escapes include root: {child}")
        return flatten(child, visited)

    return re.sub(r'#include\s+"((?:vk_video/)?vulkan_video_[^"\n]+)"', include, text).strip()


def core_body(core: pathlib.Path) -> str:
    text = core.read_text(encoding="utf-8")
    start = text.index("#define VULKAN_CORE_H_ 1") + len("#define VULKAN_CORE_H_ 1")
    body = text[start:].replace('#include "vk_platform.h"', "")
    body = re.sub(r"\n#ifdef __cplusplus\nextern \"C\" \{\n#endif\n", "\n", body, count=1)
    visited: set[pathlib.Path] = set()
    paths = sorted(set(re.findall(r'#include\s+"(vk_video/[^"\n]+)"', body)))
    video = "\n\n".join([flatten(core.parent.parent / "vk_video/vulkan_video_codecs_common.h", visited)] +
                           [flatten(core.parent.parent / p, visited) for p in paths])
    body = re.sub(r'#include\s+"vk_video/[^"\n]+"', "", body)
    body = re.sub(r"\n#ifdef __cplusplus\n}\n#endif\n\n#endif\s*$", "\n", body, count=1)
    return (video + "\n\n" + body).strip()


def command_metadata(registry: pathlib.Path, body: str) -> dict[str, str | None]:
    root = ET.parse(registry).getroot()
    result: dict[str, str | None] = {}
    aliases: list[tuple[str, str]] = []
    for node in root.findall("./commands/command"):
        name = node.get("name") or node.findtext("proto/name")
        if not name:
            continue
        if node.get("alias"):
            aliases.append((name, node.get("alias") or ""))
        else:
            result[name] = node.findtext("param/type")
    for name, alias in aliases:
        result[name] = result.get(alias)
    available = set(re.findall(r"typedef\s+.+?\bPFN_(vk[A-Za-z0-9_]+)\s*\)", body))
    return {name: result[name] for name in sorted(available & result.keys())}


def dispatch_table(name: str, names: list[str]) -> str:
    fields = "\n".join(f"        PFN_{n} {n} = nullptr;" for n in names)
    loads = "\n".join(f'            {n} = reinterpret_cast<PFN_{n}>(resolve("{n}"));' for n in names)
    descriptions = {
        "Loader": ("Process-wide Vulkan loader dispatch table.",
                    "These commands are resolved from the Vulkan loader and do not depend on a particular instance."),
        "Instance": ("Instance-scoped Vulkan dispatch table generated from the registry.",
                      "Commands in this table require a live instance and are resolved through vkGetInstanceProcAddr."),
        "Device": ("Device-scoped Vulkan dispatch table generated from the registry.",
                    "Commands in this table require a live device and are resolved through vkGetDeviceProcAddr."),
    }
    brief, details = descriptions[name]
    resolve_brief = "Resolve every loader command" if name == "Loader" else f"Resolve every {name.lower()} command"
    return f'''    /**
     * @brief {brief}
     * @details {details}
     */
    struct {name}API {{
{fields}

        /** @brief {resolve_brief} through @p resolve. */
        template<typename Resolver>
        void Resolve(Resolver&& resolve) noexcept {{
{loads}
        }}
    }};
'''


def generate(core: pathlib.Path, registry: pathlib.Path) -> str:
    for path, expected in ((core, CORE_SHA256), (registry, REGISTRY_SHA256)):
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"Unexpected Khronos source SHA-256 for {path}: {actual}")
    body = core_body(core)
    visited: set[pathlib.Path] = set()
    platform_files = ["vulkan_android.h", "vulkan_beta.h", "vulkan_directfb.h", "vulkan_fuchsia.h",
                      "vulkan_ggp.h", "vulkan_ios.h", "vulkan_macos.h", "vulkan_metal.h", "vulkan_ohos.h",
                      "vulkan_screen.h", "vulkan_vi.h", "vulkan_wayland.h", "vulkan_win32.h", "vulkan_xcb.h",
                      "vulkan_xlib.h", "vulkan_xlib_xrandr.h"]
    body += "\n\n" + "\n\n".join(flatten(core.parent / p, visited) for p in platform_files)
    metadata = command_metadata(registry, body)
    object_handles = DEVICE_HANDLES | {"VkInstance", "VkPhysicalDevice"}
    unexpected = {n: owner for n, owner in metadata.items() if owner not in object_handles and n not in GLOBAL}
    if unexpected:
        raise ValueError(f"Unknown Vulkan dispatch roots: {unexpected}")
    global_names = [n for n in metadata if n in GLOBAL]
    instance_names = [
        n for n, owner in metadata.items() if n not in GLOBAL and owner in {"VkInstance", "VkPhysicalDevice"}
    ]
    device_names = [n for n, owner in metadata.items() if owner in DEVICE_HANDLES]
    preamble = '''/**
 * @file VulkanAPI.h
 * @brief Self-contained Vulkan ABI declarations and object-scoped dispatch tables.
 * @ingroup PAL
 * @details Generated by tools/generate_vulkan_api.py from pinned vendored Khronos sources; do not edit the registry
 * section manually. The PAL-owned dispatch tables at the end of the file are deliberately split by Vulkan dispatch
 * scope: loader exports, instance commands, and device commands. A table is a non-owning set of function pointers;
 * object lifetime and synchronization remain the caller's responsibility.
 *
 * @par Example
 * @code{.cpp}
 * Sora::PAL::Vulkan::VkInstance instance{};
 * Sora::PAL::Vulkan::VkDevice device{};
 * const auto& loader = Sora::PAL::Vulkan::LoadVulkanAPI();
 * if (loader.vkGetInstanceProcAddr != nullptr) {
 *     auto instanceApi = Sora::PAL::Vulkan::ResolveInstanceAPI(instance, loader.vkGetInstanceProcAddr);
 *     if (instanceApi.vkCreateDevice != nullptr) {
 *         auto deviceApi = Sora::PAL::Vulkan::ResolveDeviceAPI(device, instanceApi.vkGetDeviceProcAddr);
 *         // Use deviceApi.vkQueueSubmit, vkCmdDraw, ... after checking optional commands as needed.
 *     }
 * }
 * @endcode
 *
 * @note @p instance and @p device are application-owned handles. The resolver functions must remain valid for the
 * duration of table construction; the resulting tables should be discarded before their dispatch domain is destroyed.
 */
#pragma once
#include <Sora/Core/PAL/SystemAPI.h>
#include <stddef.h>
#include <stdint.h>
#if defined(VULKAN_CORE_H_) || defined(VULKAN_H_)
#  error "Include VulkanAPI.h without native Vulkan headers in the same translation unit."
#endif
#if defined(_WIN32)
#  define VKAPI_ATTR
#  define VKAPI_CALL __stdcall
#  define VKAPI_PTR VKAPI_CALL
#else
#  define VKAPI_ATTR
#  define VKAPI_CALL
#  define VKAPI_PTR
#endif
#define VK_NO_PROTOTYPES 1
namespace Sora::PAL::Vulkan {
    struct XDisplay; struct XVisual; struct wl_display; struct wl_surface; struct ANativeWindow; struct AHardwareBuffer;
    struct Win32Instance; struct Win32Window; struct Win32SecurityAttributes; struct IDirectFB; struct IDirectFBSurface;
    struct OH_NativeBuffer; struct NativeWindow; using Display = XDisplay; using Window = uintptr_t;
    using VisualID = uintptr_t;
    using Visual = XVisual; using RROutput = uint64_t; using HINSTANCE = Win32Instance*; using HWND = Win32Window*;
    using HMONITOR = void*; using HANDLE = void*; using LPCWSTR = const wchar_t*; using DWORD = uint32_t;
    using SECURITY_ATTRIBUTES = Win32SecurityAttributes; using xcb_connection_t = struct XcbConnection;
    using xcb_visualid_t = uint32_t; using xcb_window_t = uint32_t; using GgpStreamDescriptor = uint64_t;
    using GgpFrameToken = uint64_t; using zx_handle_t = uint32_t; using CAMetalLayer = void;
    using MTLDevice_id = void*; using MTLCommandQueue_id = void*; using MTLBuffer_id = void*;
    using MTLTexture_id = void*; using MTLSharedEvent_id = void*; using OHNativeWindow = NativeWindow;
'''
    epilogue = '''
    /** @brief Return the immutable process-wide loader dispatch table. */
    [[nodiscard]] const LoaderAPI& LoadVulkanAPI() noexcept;
    /**
     * @brief Build an instance-scoped dispatch table.
     * @param[in] instance Live Vulkan instance used for resolution.
     * @param[in] resolver Instance-procedure resolver, normally vkGetInstanceProcAddr.
     * @return Fully populated table; optional extension commands may remain null.
     */
    [[nodiscard]] InstanceAPI ResolveInstanceAPI(VkInstance instance, PFN_vkGetInstanceProcAddr resolver) noexcept;
    /**
     * @brief Build a device-scoped dispatch table.
     * @param[in] device Live Vulkan device used for resolution.
     * @param[in] resolver Device-procedure resolver, normally vkGetDeviceProcAddr.
     * @return Fully populated table; optional extension commands may remain null.
     */
    [[nodiscard]] DeviceAPI ResolveDeviceAPI(VkDevice device, PFN_vkGetDeviceProcAddr resolver) noexcept;
} // namespace Sora::PAL::Vulkan
#undef VK_NO_PROTOTYPES
#undef VKAPI_PTR
#undef VKAPI_CALL
#undef VKAPI_ATTR
'''
    tables = "\n".join((dispatch_table("Loader", global_names), dispatch_table("Instance", instance_names),
                         dispatch_table("Device", device_names)))
    return preamble + body + "\n\n" + tables + epilogue


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--core", type=pathlib.Path, required=True)
    parser.add_argument("--registry", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    generated = generate(args.core, args.registry)
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != generated:
            raise SystemExit(f"generated Vulkan API is stale: {args.output}")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(generated, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
