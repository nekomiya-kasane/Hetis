/**
 * @file DeviceFeatureTest.cpp
 * @brief Verify renderer device-feature taxonomy and its reflected fixed-capacity set.
 * @ingroup Testing
 */

#include <Sora/Render/DeviceFeature.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <meta>
#include <string_view>
#include <type_traits>

namespace Renderer = Sora::Renderer;

namespace {

    consteval bool HasFeatureNamed(std::string_view name) {
        for (std::meta::info feature : Sora::Meta::EnumeratorsOf(^^Renderer::DeviceFeature)) {
            if (std::meta::identifier_of(feature) == name) {
                return true;
            }
        }
        return false;
    }

    consteval bool ValidateRepresentativeCapabilities() {
        const Renderer::DeviceFeatureSet features{
            Renderer::DeviceFeature::DynamicRendering,     Renderer::DeviceFeature::MeshShader,
            Renderer::DeviceFeature::DescriptorBuffer,     Renderer::DeviceFeature::RayQuery,
            Renderer::DeviceFeature::CooperativeMatrix,    Renderer::DeviceFeature::TextureCompressionBC,
            Renderer::DeviceFeature::DeviceFaultReporting,
        };

        return features.Count() == 7 && features.Contains(Renderer::DeviceFeature::DynamicRendering) &&
               features.Contains(Renderer::DeviceFeature::RayQuery) &&
               !features.Contains(Renderer::DeviceFeature::TimelineSynchronization);
    }

    static_assert(Sora::Concept::OrdinalEnum<Renderer::DeviceFeature>);
    static_assert(Renderer::DeviceFeatureSet::kSize ==
                  Sora::Traits::OrdinaryEnumeratorsCountOf<Renderer::DeviceFeature>);
    static_assert(sizeof(Renderer::DeviceFeatureSet) == sizeof(Renderer::DeviceFeatureSet::Storage));
    static_assert(std::is_trivially_copyable_v<Renderer::DeviceFeatureSet>);
    static_assert(ValidateRepresentativeCapabilities());

    static_assert(
        Renderer::$::ExtensionRequirement{Renderer::$::NativeApi::Vulkan11, "VK_KHR_timeline_semaphore"}.IsValid());
    static_assert(!Renderer::$::ExtensionRequirement{Renderer::$::NativeApi::Vulkan14,
                                                     "VkPhysicalDeviceFeatures::geometryShader"}
                       .IsValid());
    static_assert(Renderer::Traits::ExtensionRequirementCountOf<Renderer::DeviceFeature::FineGrainedBarriers> == 0);
    static_assert(Renderer::Traits::ExtensionRequirementCountOf<Renderer::DeviceFeature::GeometryShader> == 0);
    static_assert(Renderer::Traits::ExtensionRequirementCountOf<Renderer::DeviceFeature::TimelineSynchronization> == 1);
    static_assert(Renderer::Traits::ExtensionRequirementsOf<Renderer::DeviceFeature::TimelineSynchronization>[0]
                      .extensionsRequired.view() == "VK_KHR_timeline_semaphore");

    static_assert(!HasFeatureNamed("Count"));
    static_assert(!HasFeatureNamed("Count_"));
    static_assert(!HasFeatureNamed("Present"));
    static_assert(!HasFeatureNamed("AsyncCompute"));
    static_assert(!HasFeatureNamed("ResizableBAR"));

} // namespace

TEST_CASE("DeviceFeatureSet composes capabilities across renderer domains", "[Sora.Renderer.DeviceFeature]") {
    const Renderer::DeviceFeatureSet graphics{
        Renderer::DeviceFeature::DynamicRendering,
        Renderer::DeviceFeature::MeshShader,
        Renderer::DeviceFeature::PerPrimitiveFragmentShadingRate,
    };
    const Renderer::DeviceFeatureSet resources{
        Renderer::DeviceFeature::RuntimeDescriptorArray,
        Renderer::DeviceFeature::DescriptorBuffer,
        Renderer::DeviceFeature::BufferDeviceAddress,
    };
    const Renderer::DeviceFeatureSet requested = graphics | resources;

    REQUIRE(requested.ContainsAll(graphics));
    REQUIRE(requested.ContainsAll(resources));
    REQUIRE(requested.Count() == graphics.Count() + resources.Count());
    REQUIRE((requested & graphics) == graphics);
    REQUIRE((requested - resources) == graphics);
}

TEST_CASE("DeviceFeatureSet preserves exact bitset representation", "[Sora.Renderer.DeviceFeature]") {
    Renderer::DeviceFeatureSet::Storage bits;
    bits.set(static_cast<std::size_t>(Renderer::DeviceFeature::RayTracingPipeline));
    bits.set(static_cast<std::size_t>(Renderer::DeviceFeature::DiagnosticBufferMarkers));

    const Renderer::DeviceFeatureSet features{bits};
    REQUIRE(features.Bits() == bits);
    REQUIRE(features.Contains(Renderer::DeviceFeature::RayTracingPipeline));
    REQUIRE(features.Contains(Renderer::DeviceFeature::DiagnosticBufferMarkers));
}
