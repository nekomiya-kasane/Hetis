/**
 * @file VulkanNativeABITest.cpp
 * @brief Verify selected Sora Vulkan PAL declarations against the vendored Khronos header ABI.
 * @ingroup PAL
 */

#include <Sora/Core/PAL/VulkanAPI.h>

#include <vulkan/vulkan_core.h>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

TEST_CASE("Vulkan PAL matches the native core ABI") {
    namespace PAL = Sora::PAL::Vulkan;
    static_assert(sizeof(PAL::VkApplicationInfo) == sizeof(::VkApplicationInfo));
    static_assert(alignof(PAL::VkPhysicalDeviceProperties) == alignof(::VkPhysicalDeviceProperties));
    static_assert(sizeof(PAL::VkMemoryRequirements) == sizeof(::VkMemoryRequirements));
    static_assert(std::is_same_v<PAL::VkDeviceSize, ::VkDeviceSize>);
    SUCCEED();
}
