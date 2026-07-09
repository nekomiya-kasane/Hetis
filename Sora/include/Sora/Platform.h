// ReSharper disable CppClangTidyPerformanceEnumSize
/**
 * @file Platform.h
 * @brief Compile-time description of the target operating system, ABI, toolchain, and platform object identifiers.
 * @ingroup Platform
 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#if defined(__APPLE__)
#    include <TargetConditionals.h>
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#    ifndef PLATFORM_EXPORT
#        define PLATFORM_EXPORT __declspec(dllexport)
#    endif
#    ifndef PLATFORM_IMPORT
#        define PLATFORM_IMPORT __declspec(dllimport)
#    endif
#    ifndef PLATFORM_LOCAL
#        define PLATFORM_LOCAL
#    endif
#elif defined(__GNUC__) || defined(__clang__)
#    ifndef PLATFORM_EXPORT
#        define PLATFORM_EXPORT __attribute__((visibility("default")))
#    endif
#    ifndef PLATFORM_IMPORT
#        define PLATFORM_IMPORT __attribute__((visibility("default")))
#    endif
#    ifndef PLATFORM_LOCAL
#        define PLATFORM_LOCAL __attribute__((visibility("hidden")))
#    endif
#else
#    ifndef PLATFORM_EXPORT
#        define PLATFORM_EXPORT
#    endif
#    ifndef PLATFORM_IMPORT
#        define PLATFORM_IMPORT
#    endif
#    ifndef PLATFORM_LOCAL
#        define PLATFORM_LOCAL
#    endif
#endif

#ifndef PLATFORM_API
#    if defined(PLATFORM_STATIC) || defined(PLATFORM_BUILD_STATIC)
#        define PLATFORM_API
#    elif defined(PLATFORM_BUILD_SHARED) || defined(PLATFORM_EXPORTS)
#        define PLATFORM_API PLATFORM_EXPORT
#    else
#        define PLATFORM_API PLATFORM_IMPORT
#    endif
#endif

namespace Sora {

    inline namespace Platform {

        /** @brief Operating-system family selected for the current translation unit. */
        enum class OperatingSystem : uint8_t {
            Unknown = 0,  /**< The operating system is not recognized by the compile-time detector. */
            Windows,      /**< Microsoft Windows NT family. */
            Linux,        /**< Linux kernel based systems other than Android. */
            macOS,        /**< Apple macOS. */
            Android,      /**< Android userland on the Linux kernel. */
            iOS,          /**< Apple iOS or iPadOS. */
            FreeBSD,      /**< FreeBSD. */
            OpenBSD,      /**< OpenBSD. */
            NetBSD,       /**< NetBSD. */
            DragonFlyBSD, /**< DragonFly BSD. */
            WASI,         /**< WebAssembly System Interface. */
            Emscripten,   /**< Emscripten browser or node-hosted environment. */
        };

        /** @brief Kernel or host substrate beneath the operating-system personality. */
        enum class KernelFamily : uint8_t {
            Unknown = 0, /**< The kernel family is not recognized. */
            WindowsNT,   /**< Windows NT kernel family. */
            Linux,       /**< Linux kernel family. */
            XNU,         /**< Apple XNU kernel family. */
            BSD,         /**< BSD kernel family. */
            WebAssembly, /**< WebAssembly host substrate. */
        };

        /** @brief Native windowing or display protocol used by a platform backend. */
        enum class WindowSystem : uint8_t {
            Unknown = 0,   /**< The windowing system is not known at compile time. */
            None,          /**< No native windowing system is available. */
            Win32,         /**< Win32 HWND / message-queue windowing. */
            X11,           /**< X11 / XCB windowing. */
            Wayland,       /**< Wayland compositor protocol. */
            Cocoa,         /**< macOS AppKit / Cocoa. */
            UIKit,         /**< iOS UIKit. */
            AndroidNative, /**< Android NativeActivity / ANativeWindow. */
            Headless,      /**< Deliberately headless runtime. */
        };

        /** @brief CPU architecture targeted by the current translation unit. */
        enum class CpuArchitecture : uint8_t {
            Unknown = 0, /**< The CPU architecture is not recognized. */
            X86,         /**< 32-bit x86. */
            X64,         /**< x86-64 / AMD64. */
            Arm32,       /**< 32-bit ARM. */
            Arm64,       /**< AArch64 / ARM64. */
            RiscV32,     /**< 32-bit RISC-V. */
            RiscV64,     /**< 64-bit RISC-V. */
            Wasm32,      /**< 32-bit WebAssembly. */
            Wasm64,      /**< 64-bit WebAssembly. */
        };

        /** @brief Native byte order of scalar object representations. */
        enum class ByteOrder : uint8_t {
            Unknown = 0, /**< The byte order is not known. */
            Little,      /**< Least significant byte first. */
            Big,         /**< Most significant byte first. */
            Mixed,       /**< Mixed endian representation. */
        };

        /** @brief C++ ABI family used for layout, name mangling, and exception/runtime conventions. */
        enum class AbiFamily : uint8_t {
            Unknown = 0, /**< The ABI family is not recognized. */
            MSVC,        /**< Microsoft C++ ABI. */
            Itanium,     /**< Itanium C++ ABI used by GCC/Clang-style targets. */
            Emscripten,  /**< Emscripten WebAssembly ABI. */
            WASI,        /**< WASI WebAssembly ABI. */
        };

        /** @brief Front-end compiler family compiling the current translation unit. */
        enum class CompilerFamily : uint8_t {
            Unknown = 0, /**< The compiler family is not recognized. */
            Clang,       /**< Upstream or downstream Clang. */
            AppleClang,  /**< Apple Clang. */
            MSVC,        /**< Microsoft Visual C++. */
            GCC,         /**< GNU Compiler Collection. */
        };

        /** @brief Standard-library implementation visible to the current translation unit. */
        enum class StandardLibrary : uint8_t {
            Unknown = 0, /**< The standard library is not recognized. */
            LibCpp,      /**< LLVM libc++. */
            LibStdCpp,   /**< GNU libstdc++. */
            MSVCSTL,     /**< Microsoft STL. */
        };

        /** @brief Native executable and shared-object container format. */
        enum class BinaryFormat : uint8_t {
            Unknown = 0, /**< The binary format is not recognized. */
            PE,          /**< Portable Executable / COFF. */
            ELF,         /**< Executable and Linkable Format. */
            MachO,       /**< Mach-O. */
            Wasm,        /**< WebAssembly module. */
        };

        /** @brief Optimization/debugging intent inferred from common build macros. */
        enum class BuildMode : uint8_t {
            Unknown = 0,    /**< The build mode is not explicitly known. */
            Debug,          /**< Debug build. */
            Release,        /**< Release build. */
            RelWithDebInfo, /**< Release build with debug information. */
            MinSizeRel,     /**< Size-optimized release build. */
        };

#if defined(_WIN32)
#    define PLATFORM_WINDOWS 1
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::Windows;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::WindowsNT;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Win32;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::PE;
#elif defined(__EMSCRIPTEN__)
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::Emscripten;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::WebAssembly;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::None;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::Wasm;
#elif defined(__wasi__)
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::WASI;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::WebAssembly;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::None;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::Wasm;
#elif defined(__ANDROID__)
#    define PLATFORM_LINUX 1
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::Android;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::Linux;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::AndroidNative;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::ELF;
#elif defined(__linux__)
#    define PLATFORM_LINUX 1
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::Linux;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::Linux;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Headless;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::ELF;
#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::iOS;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::XNU;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::UIKit;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::MachO;
#elif defined(__APPLE__)
#    define PLATFORM_MACOS 1
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::macOS;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::XNU;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Cocoa;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::MachO;
#elif defined(__FreeBSD__)
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::FreeBSD;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::BSD;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Headless;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::ELF;
#elif defined(__OpenBSD__)
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::OpenBSD;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::BSD;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Headless;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::ELF;
#elif defined(__NetBSD__)
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::NetBSD;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::BSD;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Headless;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::ELF;
#elif defined(__DragonFly__)
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::DragonFlyBSD;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::BSD;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Headless;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::ELF;
#else
        inline constexpr OperatingSystem kOperatingSystem = OperatingSystem::Unknown;
        inline constexpr KernelFamily kKernelFamily = KernelFamily::Unknown;
        inline constexpr WindowSystem kDefaultWindowSystem = WindowSystem::Unknown;
        inline constexpr BinaryFormat kBinaryFormat = BinaryFormat::Unknown;
#endif

#if defined(__x86_64__) || defined(_M_X64)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::X64;
#elif defined(__i386__) || defined(_M_IX86)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::X86;
#elif defined(__aarch64__) || defined(_M_ARM64)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::Arm64;
#elif defined(__arm__) || defined(_M_ARM)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::Arm32;
#elif defined(__riscv) && (__riscv_xlen == 64)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::RiscV64;
#elif defined(__riscv) && (__riscv_xlen == 32)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::RiscV32;
#elif defined(__wasm64__)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::Wasm64;
#elif defined(__wasm32__)
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::Wasm32;
#else
        inline constexpr CpuArchitecture kCpuArchitecture = CpuArchitecture::Unknown;
#endif

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        inline constexpr ByteOrder kByteOrder = ByteOrder::Big;
#elif defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        inline constexpr ByteOrder kByteOrder = ByteOrder::Little;
#elif defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_PDP_ENDIAN__)
        inline constexpr ByteOrder kByteOrder = ByteOrder::Mixed;
#elif defined(_WIN32)
        inline constexpr ByteOrder kByteOrder = ByteOrder::Little;
#elif defined(__cpp_lib_endian)
        inline constexpr ByteOrder kByteOrder = std::endian::native == std::endian::little ? ByteOrder::Little
                                                : std::endian::native == std::endian::big  ? ByteOrder::Big
                                                                                           : ByteOrder::Mixed;
#else
        inline constexpr ByteOrder kByteOrder = ByteOrder::Unknown;
#endif

#if defined(__EMSCRIPTEN__)
        inline constexpr AbiFamily kAbiFamily = AbiFamily::Emscripten;
#elif defined(__wasi__)
        inline constexpr AbiFamily kAbiFamily = AbiFamily::WASI;
#elif defined(_MSC_VER) && defined(_WIN32)
        inline constexpr AbiFamily kAbiFamily = AbiFamily::MSVC;
#elif defined(__clang__) && defined(_WIN32)
        inline constexpr AbiFamily kAbiFamily = AbiFamily::MSVC;
#elif defined(__GNUC__) || defined(__clang__)
        inline constexpr AbiFamily kAbiFamily = AbiFamily::Itanium;
#else
        inline constexpr AbiFamily kAbiFamily = AbiFamily::Unknown;
#endif

#if defined(__clang__) && defined(__apple_build_version__)
        inline constexpr CompilerFamily kCompilerFamily = CompilerFamily::AppleClang;
#elif defined(__clang__)
        inline constexpr CompilerFamily kCompilerFamily = CompilerFamily::Clang;
#elif defined(_MSC_VER)
        inline constexpr CompilerFamily kCompilerFamily = CompilerFamily::MSVC;
#elif defined(__GNUC__)
        inline constexpr CompilerFamily kCompilerFamily = CompilerFamily::GCC;
#else
        inline constexpr CompilerFamily kCompilerFamily = CompilerFamily::Unknown;
#endif

#if defined(_LIBCPP_VERSION)
        inline constexpr StandardLibrary kStandardLibrary = StandardLibrary::LibCpp;
#elif defined(__GLIBCXX__)
        inline constexpr StandardLibrary kStandardLibrary = StandardLibrary::LibStdCpp;
#elif defined(_MSVC_STL_VERSION)
        inline constexpr StandardLibrary kStandardLibrary = StandardLibrary::MSVCSTL;
#else
        inline constexpr StandardLibrary kStandardLibrary = StandardLibrary::Unknown;
#endif

#if defined(SORA_BUILD_DEBUG) || defined(_DEBUG)
        inline constexpr BuildMode kBuildMode = BuildMode::Debug;
#elif defined(SORA_BUILD_RELWITHDEBINFO)
        inline constexpr BuildMode kBuildMode = BuildMode::RelWithDebInfo;
#elif defined(SORA_BUILD_MINSIZEREL)
        inline constexpr BuildMode kBuildMode = BuildMode::MinSizeRel;
#elif defined(NDEBUG)
        inline constexpr BuildMode kBuildMode = BuildMode::Release;
#else
        inline constexpr BuildMode kBuildMode = BuildMode::Unknown;
#endif

        inline constexpr bool kIsWindows = kOperatingSystem == OperatingSystem::Windows;
        inline constexpr bool kIsLinux = kOperatingSystem == OperatingSystem::Linux;
        inline constexpr bool kIsMacOS = kOperatingSystem == OperatingSystem::macOS;
        inline constexpr bool kIsAndroid = kOperatingSystem == OperatingSystem::Android;
        inline constexpr bool kIsIOS = kOperatingSystem == OperatingSystem::iOS;
        inline constexpr bool kIsWASI = kOperatingSystem == OperatingSystem::WASI;
        inline constexpr bool kIsApple = kIsMacOS || kIsIOS;
        inline constexpr bool kIsBSD = kKernelFamily == KernelFamily::BSD;
        inline constexpr bool kIsWebAssembly = kKernelFamily == KernelFamily::WebAssembly;
        inline constexpr bool kIsDesktopOS = kIsWindows || kIsLinux || kIsMacOS || kIsBSD;
        inline constexpr bool kIsMobileOS = kIsAndroid || kIsIOS;
        inline constexpr bool kIsPosixLike = kIsLinux || kIsAndroid || kIsApple || kIsBSD || kIsWASI;
        inline constexpr bool kIsUnixLike = kIsPosixLike && !kIsWebAssembly;
        inline constexpr bool kIsLittleEndian = kByteOrder == ByteOrder::Little;
        inline constexpr bool kIsBigEndian = kByteOrder == ByteOrder::Big;
        inline constexpr bool kIs64Bit = sizeof(void*) == 8;
        inline constexpr bool kIs32Bit = sizeof(void*) == 4;

        inline constexpr char kPathSeparator_Windows = '\\';
        inline constexpr char kPathSeparator_Unix = '/';
        inline constexpr char kPathSeparator = kIsWindows ? kPathSeparator_Windows : kPathSeparator_Unix;
        inline constexpr std::string_view kSharedLibraryPrefix = kIsWindows ? "" : "lib";
        inline constexpr std::string_view kSharedLibrarySuffix = kIsWindows ? ".dll" : kIsMacOS ? ".dylib" : ".so";
        inline constexpr std::string_view kStaticLibraryPrefix = kIsWindows ? "" : "lib";
        inline constexpr std::string_view kStaticLibrarySuffix = kIsWindows ? ".lib" : ".a";
        inline constexpr std::string_view kExecutableSuffix = kIsWindows ? ".exe" : "";

    } // namespace Platform

    using std::size_t;
    using ssize_t = std::make_signed_t<size_t>;

    using std::int16_t;
    using std::int32_t;
    using std::int64_t;
    using std::int8_t;
    using std::uint16_t;
    using std::uint32_t;
    using std::uint64_t;
    using std::uint8_t;

} // namespace Sora