/**
 * @file OpenGLAPI.cpp
 * @brief Dynamic-library bootstrap for the WGL, GLX, and EGL PAL dispatch tables.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/OpenGLAPI.h>

#include <Sora/Core/PAL/Module.h>
#include <Sora/Platform.h>

#include <initializer_list>
#include <string_view>
#include <utility>

namespace Sora::PAL::OpenGL {

    namespace {

        /** @brief Load an explicitly named graphics runtime and retain it for process lifetime. */
        [[nodiscard]] ModulePtr LoadGraphicsModule(std::initializer_list<std::string_view> names) noexcept {
            try {
                constexpr ModuleLoadOptions options{
                    .nameKind = ModuleNameKind::FileName,
                    .candidatePolicy = ModuleCandidatePolicy::ExactOnly,
                    .cachePolicy = ModuleCachePolicy::Private,
                    .unloadOnDestroy = false,
                };
                Result<ModulePtr> loaded = LoadModule(names, options);
                return loaded ? std::move(*loaded) : nullptr;
            } catch (...) {
                return nullptr;
            }
        }

        template<typename API>
        [[nodiscard]] API ResolveModuleExports(const ModulePtr& module) noexcept {
            API loaded{};
            if (module != nullptr) {
                loaded.Resolve([&module](const char* name) noexcept { return module->TryFindSymbol<void>(name); });
            }
            return loaded;
        }

#if defined(PLATFORM_WINDOWS)
        [[nodiscard]] const ModulePtr& WGLModule() noexcept {
            static const ModulePtr module = LoadGraphicsModule({"opengl32.dll"});
            return module;
        }

        [[nodiscard]] const ModulePtr& GDI32Module() noexcept {
            static const ModulePtr module = LoadGraphicsModule({"gdi32.dll"});
            return module;
        }
#endif

#if defined(PLATFORM_LINUX) && !defined(__ANDROID__)
        [[nodiscard]] const ModulePtr& GLXModule() noexcept {
            static const ModulePtr module = LoadGraphicsModule({"libGL.so.1", "libGL.so"});
            return module;
        }
#endif

        [[nodiscard]] const ModulePtr& EGLModule() noexcept {
#if defined(PLATFORM_WINDOWS)
            static const ModulePtr module = LoadGraphicsModule({"libEGL.dll", "EGL.dll"});
#elif defined(__ANDROID__)
            static const ModulePtr module = LoadGraphicsModule({"libEGL.so"});
#elif defined(PLATFORM_LINUX)
            static const ModulePtr module = LoadGraphicsModule({"libEGL.so.1", "libEGL.so"});
#elif defined(PLATFORM_MACOS)
            static const ModulePtr module = LoadGraphicsModule({"libEGL.1.dylib", "libEGL.dylib"});
#else
            static const ModulePtr module;
#endif
            return module;
        }

    } // namespace

    const WGLAPI& LoadWGLAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
        static const WGLAPI api = [] {
            const ModulePtr& openGLModule = WGLModule();
            const ModulePtr& gdiModule = GDI32Module();
            WGLAPI loaded{};
            loaded.Resolve([&openGLModule, &gdiModule](const char* name) noexcept -> void* {
                if (openGLModule != nullptr) {
                    if (void* symbol = openGLModule->TryFindSymbol<void>(name); symbol != nullptr) {
                        return symbol;
                    }
                }
                return gdiModule != nullptr ? gdiModule->TryFindSymbol<void>(name) : nullptr;
            });
            return loaded;
        }();
#else
        static const WGLAPI api{};
#endif
        return api;
    }

    const GLXAPI& LoadGLXAPI() noexcept {
#if defined(PLATFORM_LINUX) && !defined(__ANDROID__)
        static const GLXAPI api = [] {
            const ModulePtr& module = GLXModule();
            GLXAPI loaded = ResolveModuleExports<GLXAPI>(module);
            const auto getProcAddress = loaded.glXGetProcAddressARB != nullptr ? loaded.glXGetProcAddressARB
                                                                             : loaded.glXGetProcAddress;
            if (module != nullptr && getProcAddress != nullptr) {
                loaded.Resolve([&module, getProcAddress](const char* name) noexcept {
                    if (void* symbol = module->TryFindSymbol<void>(name); symbol != nullptr) {
                        return symbol;
                    }
                    return reinterpret_cast<void*>(getProcAddress(reinterpret_cast<const GLubyte*>(name)));
                });
            }
            return loaded;
        }();
#else
        static const GLXAPI api{};
#endif
        return api;
    }

    const EGLAPI& LoadEGLAPI() noexcept {
        static const EGLAPI api = [] {
            const ModulePtr& module = EGLModule();
            EGLAPI loaded = ResolveModuleExports<EGLAPI>(module);
            if (module != nullptr && loaded.eglGetProcAddress != nullptr) {
                const auto getProcAddress = loaded.eglGetProcAddress;
                loaded.Resolve([&module, getProcAddress](const char* name) noexcept {
                    if (void* symbol = module->TryFindSymbol<void>(name); symbol != nullptr) {
                        return symbol;
                    }
                    return reinterpret_cast<void*>(getProcAddress(name));
                });
            }
            return loaded;
        }();
        return api;
    }

} // namespace Sora::PAL::OpenGL
