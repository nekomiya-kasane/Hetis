/**
 * @file D3DAPI.cpp
 * @brief Dynamic-library bootstrap for Direct3D PAL export tables.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/D3DAPI.h>

#include <Sora/Core/PAL/Module.h>
#include <Sora/Platform.h>

#include <initializer_list>
#include <string_view>
#include <utility>

namespace Sora::PAL::D3D {

    namespace {

        [[nodiscard]] ModulePtr LoadD3DModule(std::initializer_list<std::string_view> names) noexcept {
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
        [[nodiscard]] API ResolveExports(const ModulePtr& module) noexcept {
            API api{};
            if (module != nullptr) {
                api.Resolve([&module](const char* name) noexcept { return module->TryFindSymbol<void>(name); });
            }
            return api;
        }

#if defined(PLATFORM_WINDOWS)
        [[nodiscard]] const ModulePtr& D3D12Module() noexcept {
            static const ModulePtr module = LoadD3DModule({"d3d12.dll"});
            return module;
        }

        [[nodiscard]] const ModulePtr& DXGIModule() noexcept {
            static const ModulePtr module = LoadD3DModule({"dxgi.dll"});
            return module;
        }

        [[nodiscard]] const ModulePtr& DXGIDebugModule() noexcept {
            static const ModulePtr module = LoadD3DModule({"dxgidebug.dll"});
            return module;
        }

        [[nodiscard]] const ModulePtr& DXCoreModule() noexcept {
            static const ModulePtr module = LoadD3DModule({"dxcore.dll"});
            return module;
        }

        [[nodiscard]] const ModulePtr& CompilerModule() noexcept {
            static const ModulePtr module = LoadD3DModule({"d3dcompiler_47.dll"});
            return module;
        }

        [[nodiscard]] const ModulePtr& DXCModule() noexcept {
            static const ModulePtr module = LoadD3DModule({"dxcompiler.dll"});
            return module;
        }
#endif

    } // namespace

    const D3D12API& LoadD3D12API() noexcept {
#if defined(PLATFORM_WINDOWS)
        static const D3D12API api = ResolveExports<D3D12API>(D3D12Module());
#else
        static const D3D12API api{};
#endif
        return api;
    }

    const DXGIAPI& LoadDXGIAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
        static const DXGIAPI api = [] {
            const ModulePtr& module = DXGIModule();
            const ModulePtr& debugModule = DXGIDebugModule();
            DXGIAPI loaded{};
            loaded.Resolve(
                [&module](const char* name) noexcept {
                    return module != nullptr ? module->TryFindSymbol<void>(name) : nullptr;
                },
                [&module, &debugModule](const char* name) noexcept {
                    if (debugModule != nullptr) {
                        if (void* symbol = debugModule->TryFindSymbol<void>(name); symbol != nullptr) {
                            return symbol;
                        }
                    }
                    return module != nullptr ? module->TryFindSymbol<void>(name) : nullptr;
                });
            return loaded;
        }();
#else
        static const DXGIAPI api{};
#endif
        return api;
    }

    const DXCoreAPI& LoadDXCoreAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
        static const DXCoreAPI api = ResolveExports<DXCoreAPI>(DXCoreModule());
#else
        static const DXCoreAPI api{};
#endif
        return api;
    }

    const CompilerAPI& LoadD3DCompilerAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
        static const CompilerAPI api = ResolveExports<CompilerAPI>(CompilerModule());
#else
        static const CompilerAPI api{};
#endif
        return api;
    }

    const DXCAPI& LoadDXCAPI() noexcept {
#if defined(PLATFORM_WINDOWS)
        static const DXCAPI api = ResolveExports<DXCAPI>(DXCModule());
#else
        static const DXCAPI api{};
#endif
        return api;
    }

} // namespace Sora::PAL::D3D
