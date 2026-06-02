#pragma once

namespace nova {

#if defined(_WIN32) || defined(_WIN64)
#    define NOVA_PLATFORM_NAME "Windows"
#    define NOVA_PLATFORM_WINDOWS 1
#elif defined(__linux__)
#    define NOVA_PLATFORM_NAME "Linux"
#    define NOVA_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#    define NOVA_PLATFORM_NAME "Apple"
#    define NOVA_PLATFORM_APPLE 1
#elif defined(__EMSCRIPTEN__)
#    define NOVA_PLATFORM_NAME "Emscripten"
#    define NOVA_PLATFORM_EMSCRIPTEN 1
#else
#    error "Unsupported platform"
#endif

    consteval const char* GetPlatformName() {
        return NOVA_PLATFORM_NAME;
    }

#if defined(_WIN32) && !defined(_WIN64) && NOVA_PLATFORM_WINDOWS
#    define NOVA_PLATFORM_32BIT 1
#elif defined(_WIN64) && NOVA_PLATFORM_WINDOWS
#    define NOVA_PLATFORM_64BIT 1
#elif defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
#    define NOVA_PLATFORM_64BIT 1
#elif defined(__i386__) || defined(_M_IX86) || defined(__arm__)
#    define NOVA_PLATFORM_32BIT 1
#else
#    error "Unsupported architecture"
#endif

    consteval bool IsPlatform64Bit() {
#if defined(NOVA_PLATFORM_64BIT)
        return true;
#elif defined(NOVA_PLATFORM_32BIT)
        return false;
#else
#    error "Unsupported architecture"
#endif
    }

#if defined(__x86_64__) || defined(_M_X64)
#    define NOVA_ARCHITECTURE_NAME "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#    define NOVA_ARCHITECTURE_NAME "x86"
#elif defined(__aarch64__)
#    define NOVA_ARCHITECTURE_NAME "ARM64"
#elif defined(__arm__)
#    define NOVA_ARCHITECTURE_NAME "ARM"
#else
#    error "Unsupported architecture"
#endif

    consteval const char* GetArchitectureName() {
        return NOVA_ARCHITECTURE_NAME;
    }

#if defined(__clang__)
#    define NOVA_COMPILER_NAME "Clang"
#    define NOVA_COMPILER_CLANG 1
#elif defined(__GNUC__) || defined(__GNUG__)
#    define NOVA_COMPILER_NAME "GCC"
#    define NOVA_COMPILER_GCC 1
#elif defined(_MSC_VER)
#    define NOVA_COMPILER_NAME "MSVC"
#    define NOVA_COMPILER_MSVC 1
#else
#    error "Unsupported compiler"
#endif

    consteval const char* GetCompilerName() {
        return NOVA_COMPILER_NAME;
    }

#if defined(_DEBUG) || defined(DEBUG) || defined(NOVA_BUILD_DEBUG)
#    if defined(NDEBUG) || defined(_NDEBUG) || defined(NOVA_BUILD_RELEASE)
#        error "Inconsistent build configuration: both NDEBUG and _DEBUG/DEBUG are defined"
#    endif
#    define NOVA_BUILD_TYPE "Debug"
#    define NOVA_BUILD_DEBUG 1
#else
#    define NOVA_BUILD_TYPE "Release"
#    define NOVA_BUILD_RELEASE 1
#endif

    consteval bool IsDebug() {
#if defined(NOVA_BUILD_DEBUG)
        return true;
#elif defined(NOVA_BUILD_RELEASE)
        return false;
#else
#    error "Unsupported build configuration"
#endif
    }

    consteval bool IsRelease() {
        return !IsDebug();
    }

} // namespace nova
