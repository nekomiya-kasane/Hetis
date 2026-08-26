/**
 * @file VulkanAPITest.cpp
 * @brief Verify Vulkan PAL dispatch resolution without a native Vulkan header dependency.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/VulkanAPI.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <type_traits>

namespace {

    using namespace Sora::PAL::Vulkan;

#if defined(_WIN32)
#    define SORA_VULKAN_TEST_CALL __stdcall
#else
#    define SORA_VULKAN_TEST_CALL
#endif

    PFN_vkVoidFunction SORA_VULKAN_TEST_CALL ResolveInstance(VkInstance, const char* name) {
        if (std::string_view{name} == "vkDestroyInstance") {
            return reinterpret_cast<PFN_vkVoidFunction>(uintptr_t{1});
        }
        return nullptr;
    }

    PFN_vkVoidFunction SORA_VULKAN_TEST_CALL ResolveDevice(VkDevice, const char* name) {
        if (std::string_view{name} == "vkDestroyDevice") {
            return reinterpret_cast<PFN_vkVoidFunction>(uintptr_t{1});
        }
        return nullptr;
    }

#undef SORA_VULKAN_TEST_CALL

} // namespace

TEST_CASE("Vulkan PAL exposes the complete object-scoped dispatch model") {
    static_assert(std::is_standard_layout_v<VkApplicationInfo>);
    static_assert(std::is_same_v<decltype(DeviceAPI::vkCreateBuffer), PFN_vkCreateBuffer>);
    static_assert(std::is_same_v<decltype(InstanceAPI::vkCreateWin32SurfaceKHR), PFN_vkCreateWin32SurfaceKHR>);
    static_assert(std::is_same_v<decltype(InstanceAPI::vkCreateWaylandSurfaceKHR), PFN_vkCreateWaylandSurfaceKHR>);
    static_assert(VK_HEADER_VERSION == 350);

    const InstanceAPI instance = ResolveInstanceAPI(nullptr, ResolveInstance);
    const DeviceAPI device = ResolveDeviceAPI(nullptr, ResolveDevice);
    CHECK(instance.vkDestroyInstance != nullptr);
    CHECK(device.vkDestroyDevice != nullptr);
    CHECK(device.vkCreateBuffer == nullptr);
}
