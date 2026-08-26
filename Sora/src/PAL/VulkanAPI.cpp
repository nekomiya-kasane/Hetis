/**
 * @file VulkanAPI.cpp
 * @brief Dynamic-loader bootstrap and object-scoped Vulkan dispatch resolution.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/VulkanAPI.h>

#include <Sora/Core/PAL/Module.h>
#include <Sora/Platform.h>

#include <initializer_list>
#include <string_view>
#include <utility>

namespace Sora::PAL::Vulkan {

    namespace {

        [[nodiscard]] ModulePtr LoadVulkanModule(std::initializer_list<std::string_view> names) noexcept {
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

        [[nodiscard]] const ModulePtr& VulkanModule() noexcept {
#if defined(PLATFORM_WINDOWS)
            static const ModulePtr module = LoadVulkanModule({"vulkan-1.dll"});
#elif defined(__ANDROID__)
            static const ModulePtr module = LoadVulkanModule({"libvulkan.so"});
#elif defined(PLATFORM_LINUX)
            static const ModulePtr module = LoadVulkanModule({"libvulkan.so.1", "libvulkan.so"});
#elif defined(PLATFORM_MACOS)
            static const ModulePtr module = LoadVulkanModule({"libvulkan.1.dylib", "libvulkan.dylib"});
#else
            static const ModulePtr module;
#endif
            return module;
        }

    } // namespace

    const LoaderAPI& LoadVulkanAPI() noexcept {
        static const LoaderAPI api = [] {
            LoaderAPI loaded{};
            const ModulePtr& module = VulkanModule();
            if (module == nullptr) {
                return loaded;
            }
            loaded.Resolve([&module](const char* name) noexcept { return module->TryFindSymbol<void>(name); });
            const auto getProcAddress = loaded.vkGetInstanceProcAddr;
            if (getProcAddress != nullptr) {
                loaded.Resolve([&module, getProcAddress](const char* name) noexcept -> void* {
                    if (void* symbol = module->TryFindSymbol<void>(name); symbol != nullptr) {
                        return symbol;
                    }
                    return reinterpret_cast<void*>(getProcAddress(nullptr, name));
                });
            }
            return loaded;
        }();
        return api;
    }

    InstanceAPI ResolveInstanceAPI(VkInstance instance, PFN_vkGetInstanceProcAddr getProcAddress) noexcept {
        InstanceAPI api{};
        if (getProcAddress != nullptr) {
            api.Resolve([instance, getProcAddress](const char* name) noexcept {
                return reinterpret_cast<void*>(getProcAddress(instance, name));
            });
        }
        return api;
    }

    DeviceAPI ResolveDeviceAPI(VkDevice device, PFN_vkGetDeviceProcAddr getProcAddress) noexcept {
        DeviceAPI api{};
        if (getProcAddress != nullptr) {
            api.Resolve([device, getProcAddress](const char* name) noexcept {
                return reinterpret_cast<void*>(getProcAddress(device, name));
            });
        }
        return api;
    }

} // namespace Sora::PAL::Vulkan
