/**
 * @file DeviceFeature.h
 * @brief Define backend-independent renderer device capabilities and their fixed-size set.
 * @ingroup Renderer
 *
 * @details A device feature is a Boolean capability that can change command encoding, shader lowering, resource
 * binding, or pipeline creation. Quantitative limits belong in a device-limits structure; queue counts and dedicated
 * queue availability belong in queue topology; presentation support belongs to a device-surface compatibility query.
 * Consequently, this enum intentionally contains neither @c Present, @c AsyncCompute, nor @c ResizableBar.
 *
 * Backends translate native feature structures, extension sets, and feature tiers into this semantic vocabulary.
 * Required OpenGL and Vulkan extension names are attached as @ref Sora::Renderer::$::ExtensionRequirement annotations.
 * Core feature fields, commands, enums, shader builtins, tiers, WebGPU feature names, fallback strategies, and
 * explanatory equivalences remain documentation because they are not native extension requirements.
 *
 * @sa https://registry.khronos.org/OpenGL/index_gl.php
 * @sa https://registry.khronos.org/vulkan/
 * @sa https://learn.microsoft.com/windows/win32/direct3d11/direct3d-11-features
 * @sa https://learn.microsoft.com/windows/win32/api/d3d12/ne-d3d12-d3d12_feature
 * @sa https://www.w3.org/TR/webgpu/
 * @sa https://gpuweb.github.io/types/types/GPUFeatureName.html
 */
#pragma once

#include <Sora/Core/FixedString.h>
#include <Sora/Core/Flags.h>
#include <Sora/Core/Traits/AnnotationTraits.h>

#include <cstddef>
#include <cstdint>
#include <meta>
#include <ranges>
#include <string_view>
#include <vector>

namespace Sora::Renderer {

    namespace $ {

        /**
         * @brief Graphics API baseline associated with an extension requirement.
         * @sa https://registry.khronos.org/OpenGL/specs/gl/glspec33.core.pdf
         * @sa https://registry.khronos.org/OpenGL/specs/gl/glspec43.core.pdf
         * @sa https://registry.khronos.org/vulkan/specs/1.1-extensions/html/vkspec.html
         * @sa https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html
         */
        enum class NativeApi : std::uint8_t {
            OpenGL33, /**< OpenGL 3.3 core profile. */
            OpenGL43, /**< OpenGL 4.3 core profile. */
            Vulkan11, /**< Vulkan 1.1 core API. */
            Vulkan14, /**< Vulkan 1.4 core API. */
            D3D11,    /**< D3D 11 core API */
            D3D12,    /**< D3D 12 core API */
            WebGPU,   /**< WebGPU core API */
        };

        /**
         * @brief Return whether @p extension follows an OpenGL or Vulkan extension-name grammar.
         * @param[in] api Graphics API baseline.
         * @param[in] extension Extension name to validate.
         */
        [[nodiscard]] consteval bool IsExtensionName(NativeApi api, std::string_view extension) {
            std::string_view prefix;
            switch (api) {
                case NativeApi::OpenGL33:
                case NativeApi::OpenGL43:
                    prefix = "GL_";
                    break;
                case NativeApi::Vulkan11:
                case NativeApi::Vulkan14:
                    prefix = "VK_";
                    break;
                default:
                    return false;
            }
            if (!extension.starts_with(prefix) || extension.contains(' ') || extension.contains('\t') ||
                extension.contains('/') || extension.contains('*')) {
                return false;
            }

            const size_t vendorEnd = extension.find('_', prefix.size());
            if (vendorEnd == std::string_view::npos || vendorEnd == prefix.size() ||
                vendorEnd + 1 == extension.size()) {
                return false;
            }

            bool hasLowercaseName = false;
            for (char c : extension.substr(vendorEnd + 1)) {
                const bool alphaNumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                if (!alphaNumeric && c != '_') {
                    return false;
                }
                hasLowercaseName = hasLowercaseName || c >= 'a' && c <= 'z';
            }
            return hasLowercaseName;
        }

        /**
         * @brief One compile-time extension requirement for a @ref DeviceFeature enumerator.
         * @details Attach one annotation per required extension. Absence means no extension requirement is recorded for
         * the API baseline; it does not imply that the semantic feature is unsupported.
         */
        struct ExtensionRequirement {
            NativeApi api{};                       /**< API baseline requiring the extension. */
            FixedString<128> extensionsRequired{}; /**< Required native extension name. */

            /** @brief Construct an empty requirement. */
            constexpr ExtensionRequirement() = default;

            /** @brief Construct a requirement for one native extension. */
            template<size_t N>
            consteval ExtensionRequirement(NativeApi mappedApi, const char (&requiredExtension)[N])
                : api(mappedApi), extensionsRequired(requiredExtension) {}

            /** @brief Return whether this requirement names a valid extension for its API. */
            [[nodiscard]] consteval bool IsValid() const noexcept {
                return IsExtensionName(api, extensionsRequired.view());
            }

            friend constexpr bool operator==(const ExtensionRequirement&,
                                             const ExtensionRequirement&) noexcept = default;
        };

    } // namespace $

    /**
     * @brief Boolean capabilities supported or enabled by one renderer device.
     * @note Ordinal positions are build-local. Persist feature identifiers through a versioned schema, not raw bits.
     */
    enum class DeviceFeature : std::uint8_t {
        // clang-format off
        /** @name Command Model and Synchronization @{ ----------------------------------------------------- */

        /**
         * @brief Encode rendering without predeclared render-pass compatibility objects.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan13Features::dynamicRendering.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan13Features.html
         * - D3D11      : @c OMSetRenderTargets.
         * @see https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargets
         * - D3D12      : @c OMSetRenderTargets.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12graphicscommandlist
         * - WebGPU     : @c GPURenderPassEncoder.
         * @see https://www.w3.org/TR/webgpu/#gpurenderpassencoder
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_framebuffer_object
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_framebuffer_object.txt
         * - OpenGL 4.3 @c GL_ARB_framebuffer_object
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_framebuffer_object.txt
         */
        DynamicRendering //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_framebuffer_object"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_framebuffer_object"}]],

        /**
         * @brief Express stage, access, and resource transitions with fine granularity.
         * @note D3D11 and WebGPU expose mostly implicit hazard tracking; that is a backend lowering rule, not a
         * native fine-grained barrier capability.
         * @par Native API mappings
         * - OpenGL 4.3 : @c glMemoryBarrier.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glMemoryBarrier.xhtml
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan13Features::synchronization2.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan13Features.html
         * - D3D12      : @c ID3D12GraphicsCommandList7::Barrier.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist7-barrier
         */
        FineGrainedBarriers,

        /**
         * @brief Signal and wait on monotonically increasing synchronization values.
         * @note Fence/semaphore emulation is a downgrade strategy and is intentionally not represented as a
         * @ref Sora::Renderer::$::ExtensionRequirement annotation.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::timelineSemaphore.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D11      : @c ID3D11Fence.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Fence.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12fence
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_KHR_timeline_semaphore
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_timeline_semaphore.html
         */
        TimelineSynchronization //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_timeline_semaphore"}]],

        /**
         * @brief Predicate rendering commands from GPU-visible state.
         * @par Native API mappings
         * - OpenGL 3.3 : @c glBeginConditionalRender.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBeginConditionalRender.xhtml
         * - OpenGL 4.3 : @c glBeginConditionalRender.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBeginConditionalRender.xhtml
         * - D3D11      : @c ID3D11DeviceContext::SetPredication.
         * @see https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-setpredication
         * - D3D12      : @c ID3D12GraphicsCommandList::SetPredication.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_conditional_rendering
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_conditional_rendering.html
         * - Vulkan 1.4 @c VK_EXT_conditional_rendering
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_conditional_rendering.html
         */
        ConditionalRendering //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_conditional_rendering"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_conditional_rendering"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Draw and Programmable Geometry @{ -------------------------------------------------------- */

        /**
         * @brief Source draw arguments from device memory.
         * @par Native API mappings
         * - OpenGL 4.3 : @c glDrawArraysIndirect.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDrawArraysIndirect.xhtml
         * - Vulkan 1.1 : @c vkCmdDrawIndirect.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirect.html
         * - Vulkan 1.4 : @c vkCmdDrawIndirect.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirect.html
         * - D3D11      : @c DrawInstancedIndirect.
         * @see https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-drawinstancedindirect
         * - D3D12      : @c ExecuteIndirect.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
         * - WebGPU     : @c GPURenderPassEncoder::drawIndirect.
         * @see https://www.w3.org/TR/webgpu/#dom-gpurenderpassencoder-drawindirect
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_draw_indirect
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_draw_indirect.txt
         */
        IndirectDrawing //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_draw_indirect"}]],

        /**
         * @brief Execute multiple indirect draws from one command.
         * @note D3D11 can issue repeated indirect draws, but a loop is an implementation strategy rather than a native
         * multi-draw-indirect token.
         * @par Native API mappings
         * - Vulkan 1.1 : @c vkCmdDrawIndirect::drawCount.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirect.html
         * - Vulkan 1.4 : @c vkCmdDrawIndirect::drawCount.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirect.html
         * - D3D12      : @c ExecuteIndirect.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_ARB_multi_draw_indirect
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_multi_draw_indirect.txt
         * - OpenGL 4.3 @c GL_ARB_indirect_parameters
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_indirect_parameters.txt
         */
        MultiDrawIndirect //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_multi_draw_indirect"}]],

        /**
         * @brief Source the number of indirect draws from device memory.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::drawIndirectCount.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D12      : @c ID3D12GraphicsCommandList::ExecuteIndirect.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-executeindirect
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_ARB_indirect_parameters
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_indirect_parameters.txt
         * - Vulkan 1.1 @c VK_KHR_draw_indirect_count
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_draw_indirect_count.html
         */
        IndirectDrawCount //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_indirect_parameters"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_draw_indirect_count"}]],

        /**
         * @brief Read base vertex, base instance, and draw index in shaders.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan11Features::shaderDrawParameters.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - D3D11      : @c SV_VertexID.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c SV_DrawID.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c vertex_index.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_ARB_shader_draw_parameters
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_draw_parameters.txt
         * - Vulkan 1.1 @c VK_KHR_shader_draw_parameters
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_draw_parameters.html
         */
        ShaderDrawParameters //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_shader_draw_parameters"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_shader_draw_parameters"}]],

        /**
         * @brief Capture vertex-processing outputs into buffers.
         * @par Native API mappings
         * - OpenGL 3.3 : @c glBeginTransformFeedback.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBeginTransformFeedback.xhtml
         * - OpenGL 4.3 : @c glBeginTransformFeedback.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBeginTransformFeedback.xhtml
         * - D3D11      : @c D3D11_SO_DECLARATION_ENTRY.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_STREAM_OUTPUT_DESC.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_transform_feedback
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_transform_feedback.html
         * - Vulkan 1.4 @c VK_EXT_transform_feedback
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_transform_feedback.html
         */
        TransformFeedback //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_transform_feedback"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_transform_feedback"}]],

        /**
         * @brief Render multiple views from one draw or render pass.
         * @par Native API mappings
         * - OpenGL 3.3 : @c gl_Layer.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/gl_Layer.xhtml
         * - OpenGL 4.3 : @c gl_Layer.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/gl_Layer.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceVulkan11Features::multiview.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan11Features::multiview.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - D3D11      : @c SV_RenderTargetArrayIndex.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_VIEW_INSTANCING_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         */
        Multiview,

        /**
         * @brief Execute a legacy D3D11_SHVER_GEOMETRY_SHADER stage.
         * @par Native API mappings
         * - OpenGL 3.3 : @c GL_GEOMETRY_SHADER.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_GEOMETRY_SHADER.xhtml
         * - OpenGL 4.3 : @c GL_GEOMETRY_SHADER.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_GEOMETRY_SHADER.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::geometryShader.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::geometryShader.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_SHVER_GEOMETRY_SHADER.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_SHVER_GEOMETRY_SHADER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         */
        GeometryShader,

        /**
         * @brief Execute tessellation control and evaluation stages.
         * @par Native API mappings
         * - OpenGL 4.3 : @c GL_PATCHES.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_PATCHES.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::tessellationShader.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::tessellationShader.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_SHVER_HULL_SHADER.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_SHVER_HULL_SHADER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_mesh_shader
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_mesh_shader.txt
         * - OpenGL 4.3 @c GL_NV_mesh_shader
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_mesh_shader.txt
         * - Vulkan 1.1 @c VK_EXT_mesh_shader
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
         * - Vulkan 1.4 @c VK_EXT_mesh_shader
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
         */
        TessellationShader,

        /**
         * @brief Generate mesh primitives through a mesh shader stage.
         * @par Native API mappings
         * - D3D12      : @c D3D12_FEATURE_DATA_D3D12_OPTIONS7::MeshShaderTier.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_mesh_shader
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_mesh_shader.txt
         * - OpenGL 4.3 @c GL_NV_mesh_shader
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_mesh_shader.txt
         * - Vulkan 1.1 @c VK_EXT_mesh_shader
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
         * - Vulkan 1.4 @c VK_EXT_mesh_shader
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
         */
        MeshShader //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_mesh_shader"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_mesh_shader"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_mesh_shader"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_mesh_shader"}]],

        /**
         * @brief Dispatch mesh work through a task or D3D12_SHVER_AMPLIFICATION_SHADER stage.
         * @par Native API mappings
         * - D3D12      : @c D3D12_SHVER_AMPLIFICATION_SHADER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_mesh_shader
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_mesh_shader.txt
         * - OpenGL 4.3 @c GL_NV_mesh_shader
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_mesh_shader.txt
         * - Vulkan 1.1 @c VK_EXT_mesh_shader
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
         * - Vulkan 1.4 @c VK_EXT_mesh_shader
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mesh_shader.html
         */
        TaskShader //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_mesh_shader"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_mesh_shader"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_mesh_shader"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_mesh_shader"}]],

        /**
         * @brief Schedule GPU work through device-generated graph nodes.
         * @par Native API mappings
         * - D3D12      : @c D3D12DDI_WORK_GRAPHS_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_AMDX_shader_enqueue
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMDX_shader_enqueue.html
         * - Vulkan 1.4 @c VK_AMDX_shader_enqueue
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMDX_shader_enqueue.html
         */
        WorkGraphs //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_AMDX_shader_enqueue"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_AMDX_shader_enqueue"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Rasterization and Fragment Processing @{ ------------------------------------------------- */

        /**
         * @brief Clamp depth instead of clipping primitives outside the depth range.
         * @par Native API mappings
         * - OpenGL 4.3 : @c GL_DEPTH_CLAMP.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_DEPTH_CLAMP.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::depthClamp.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::depthClamp.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c DepthClipEnable.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c DepthClipEnable.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c depth-clip-control.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_depth_clamp
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_depth_clamp.txt
         */
        DepthClamp //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_depth_clamp"}]],

        /**
         * @brief Reject fragments outside programmable depth bounds.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::depthBounds.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::depthBounds.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D12      : @c DepthBoundsTestEnable.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_depth_bounds_test
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_depth_bounds_test.txt
         * - OpenGL 4.3 @c GL_EXT_depth_bounds_test
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_depth_bounds_test.txt
         */
        DepthBounds //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_depth_bounds_test"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_depth_bounds_test"}]],

        /**
         * @brief Rasterize polygon edges or points instead of filled triangles.
         * @par Native API mappings
         * - OpenGL 3.3 : @c glPolygonMode.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glPolygonMode.xhtml
         * - OpenGL 4.3 : @c glPolygonMode.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glPolygonMode.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::fillModeNonSolid.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::fillModeNonSolid.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_FILL_WIREFRAME.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_FILL_MODE_WIREFRAME.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         */
        NonSolidFill,

        /**
         * @brief Rasterize lines wider than one pixel.
         * @par Native API mappings
         * - OpenGL 3.3 : @c glLineWidth.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glLineWidth.xhtml
         * - OpenGL 4.3 : @c glLineWidth.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glLineWidth.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::wideLines.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::wideLines.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         */
        WideLines,

        /**
         * @brief Configure blending independently for each color attachment.
         * @par Native API mappings
         * - OpenGL 4.3 : @c glBlendFunci.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glBlendFunci.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::independentBlend.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::independentBlend.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c IndependentBlendEnable.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c IndependentBlendEnable.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c GPUColorTargetState::blend.
         * @see https://www.w3.org/TR/webgpu/#dom-gpucolortargetstate-blend
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_draw_buffers_blend
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_draw_buffers_blend.txt
         */
        IndependentBlend //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_draw_buffers_blend"}]],

        /**
         * @brief Blend using two fragment-shader color outputs.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::dualSrcBlend.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::dualSrcBlend.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_BLEND_SRC1_COLOR.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_BLEND_SRC1_COLOR.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c dual-source-blending.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_blend_func_extended
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_blend_func_extended.txt
         * - OpenGL 4.3 @c GL_ARB_blend_func_extended
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_blend_func_extended.txt
         */
        DualSourceBlend //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_blend_func_extended"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_blend_func_extended"}]],

        /**
         * @brief Apply Boolean logic operations to color attachment writes.
         * @par Native API mappings
         * - OpenGL 3.3 : @c glLogicOp.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glLogicOp.xhtml
         * - OpenGL 4.3 : @c glLogicOp.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glLogicOp.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::logicOp.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::logicOp.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_FEATURE_DATA_D3D11_OPTIONS::OutputMergerLogicOp.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_LOGIC_OP.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         */
        LogicOperations,

        /**
         * @brief Execute fragment shading at sample frequency.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::sampleRateShading.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::sampleRateShading.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c SV_SampleIndex.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c SV_SampleIndex.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_sample_shading
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sample_shading.txt
         * - OpenGL 4.3 @c GL_ARB_sample_shading
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sample_shading.txt
         */
        SampleRateShading //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_sample_shading"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_sample_shading"}]],

        /**
         * @brief Control multisample locations within a pixel.
         * @par Native API mappings
         * - D3D11      : @c D3D11_STANDARD_MULTISAMPLE_PATTERN.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12GraphicsCommandList1::SetSamplePositions.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12graphicscommandlist1
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_ARB_sample_locations
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sample_locations.txt
         * - Vulkan 1.1 @c VK_EXT_sample_locations
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_sample_locations.html
         * - Vulkan 1.4 @c VK_EXT_sample_locations
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_sample_locations.html
         */
        ProgrammableSampleLocations //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_sample_locations"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_sample_locations"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_sample_locations"}]],

        /**
         * @brief Rasterize every pixel touched by a primitive conservatively.
         * @par Native API mappings
         * - D3D11      : @c D3D11_CONSERVATIVE_RASTERIZATION_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_CONSERVATIVE_RASTERIZATION_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_conservative_raster
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_conservative_raster.txt
         * - OpenGL 4.3 @c GL_NV_conservative_raster
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_conservative_raster.txt
         * - Vulkan 1.1 @c VK_EXT_conservative_rasterization
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_conservative_rasterization.html
         * - Vulkan 1.4 @c VK_EXT_conservative_rasterization
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_conservative_rasterization.html
         */
        ConservativeRasterization //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_conservative_raster"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_conservative_raster"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_conservative_rasterization"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_conservative_rasterization"}]],

        /**
         * @brief Read primitive barycentric coordinates in fragment shaders.
         * @par Native API mappings
         * - D3D12      : @c SV_Barycentrics.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_fragment_shader_barycentric
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_fragment_shader_barycentric.txt
         * - OpenGL 4.3 @c GL_NV_fragment_shader_barycentric
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_fragment_shader_barycentric.txt
         * - Vulkan 1.1 @c VK_EXT_fragment_shader_barycentric
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_shader_barycentric.html
         * - Vulkan 1.4 @c VK_KHR_fragment_shader_barycentric
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shader_barycentric.html
         */
        FragmentShaderBarycentrics //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_fragment_shader_barycentric"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_fragment_shader_barycentric"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_fragment_shader_barycentric"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_fragment_shader_barycentric"}]],

        /**
         * @brief Serialize overlapping fragment-shader critical sections.
         * @par Native API mappings
         * - D3D11      : @c D3D11_RASTERIZER_ORDERED_VIEW.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_RASTERIZER_ORDERED_VIEW.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_fragment_shader_interlock
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_fragment_shader_interlock.txt
         * - OpenGL 4.3 @c GL_ARB_fragment_shader_interlock
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_fragment_shader_interlock.txt
         * - Vulkan 1.1 @c VK_EXT_fragment_shader_interlock
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_shader_interlock.html
         * - Vulkan 1.4 @c VK_EXT_fragment_shader_interlock
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_shader_interlock.html
         */
        FragmentShaderInterlock //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_fragment_shader_interlock"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_fragment_shader_interlock"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_fragment_shader_interlock"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_fragment_shader_interlock"}]],

        /**
         * @brief Select a coarse shading rate per draw.
         * @par Native API mappings
         * - D3D12      : @c D3D12_VARIABLE_SHADING_RATE_TIER_1.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_shading_rate_image
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shading_rate_image.txt
         * - OpenGL 4.3 @c GL_NV_shading_rate_image
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shading_rate_image.txt
         * - Vulkan 1.1 @c VK_KHR_fragment_shading_rate
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shading_rate.html
         * - Vulkan 1.4 @c VK_KHR_fragment_shading_rate
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shading_rate.html
         */
        PerDrawFragmentShadingRate //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_shading_rate_image"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_shading_rate_image"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_fragment_shading_rate"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_fragment_shading_rate"}]],

        /**
         * @brief Select a coarse shading rate per primitive.
         * @par Native API mappings
         * - D3D12      : @c D3D12_VARIABLE_SHADING_RATE_TIER_2.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_shading_rate_image
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shading_rate_image.txt
         * - OpenGL 4.3 @c GL_NV_shading_rate_image
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shading_rate_image.txt
         * - Vulkan 1.1 @c VK_KHR_fragment_shading_rate
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shading_rate.html
         * - Vulkan 1.4 @c VK_KHR_fragment_shading_rate
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shading_rate.html
         */
        PerPrimitiveFragmentShadingRate //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_shading_rate_image"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_shading_rate_image"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_fragment_shading_rate"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_fragment_shading_rate"}]],

        /**
         * @brief Select a coarse shading rate from an image attachment.
         * @par Native API mappings
         * - D3D12      : @c ID3D12GraphicsCommandList5::RSSetShadingRateImage.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12graphicscommandlist5
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_shading_rate_image
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shading_rate_image.txt
         * - OpenGL 4.3 @c GL_NV_shading_rate_image
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shading_rate_image.txt
         * - Vulkan 1.1 @c VK_KHR_fragment_shading_rate
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shading_rate.html
         * - Vulkan 1.4 @c VK_KHR_fragment_shading_rate
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shading_rate.html
         * - Vulkan 1.1 @c VK_EXT_fragment_density_map
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_density_map.html
         */
        AttachmentFragmentShadingRate //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_shading_rate_image"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_shading_rate_image"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_fragment_shading_rate"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_fragment_shading_rate"}]],

        /**
         * @brief Drive fragment density from an image for foveated rendering.
         * @note D3D12 VRS images are related shading-rate controls, not fragment-density maps; treat any use as an
         * approximation in backend comments.
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_fragment_density_map
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_density_map.html
         * - Vulkan 1.4 @c VK_EXT_fragment_density_map
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_density_map.html
         */
        FragmentDensityMap //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_fragment_density_map"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_fragment_density_map"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Resource Binding @{ ---------------------------------------------------------------------- */

        /**
         * @brief Index resource arrays with dynamically non-uniform shader values.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::descriptorIndexing.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D11      : @c D3D11_SHADER_INPUT_FLAG_USERPACKED.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c NonUniformResourceIndex.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - OpenGL 4.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - Vulkan 1.1 @c VK_EXT_descriptor_indexing
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html
         */
        NonUniformResourceIndexing //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_descriptor_indexing"}]],

        /**
         * @brief Declare descriptor arrays whose length is selected outside shader source.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D12      : @c D3D12_DESCRIPTOR_RANGE1::NumDescriptors.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - OpenGL 4.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - Vulkan 1.1 @c VK_EXT_descriptor_indexing
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html
         */
        RuntimeDescriptorArray //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_descriptor_indexing"}]],

        /**
         * @brief Update descriptors after their containing table has been bound.
         * @par Native API mappings
         * - Vulkan 1.4 : @c descriptorBindingUpdateAfterBind.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingFeatures.html
         * - D3D12      : @c D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - OpenGL 4.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - Vulkan 1.1 @c VK_EXT_descriptor_indexing
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html
         * - OpenGL 3.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         */
        DescriptorUpdateAfterBind //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_descriptor_indexing"}]],

        /**
         * @brief Leave unused descriptor-array elements unbound.
         * @par Native API mappings
         * - Vulkan 1.4 : @c descriptorBindingPartiallyBound.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingFeatures.html
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - OpenGL 4.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - Vulkan 1.1 @c VK_EXT_descriptor_indexing
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html
         */
        PartiallyBoundDescriptors //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_descriptor_indexing"}]],

        /**
         * @brief Select a descriptor-array length when allocating a binding table.
         * @par Native API mappings
         * - Vulkan 1.4 : @c descriptorBindingVariableDescriptorCount.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingFeatures.html
         * - D3D12      : @c D3D12_DESCRIPTOR_RANGE1::NumDescriptors.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_descriptor_indexing
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html
         * - Vulkan 1.1 @c VK_EXT_descriptor_buffer
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_buffer.html
         * - Vulkan 1.4 @c VK_EXT_descriptor_buffer
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_buffer.html
         */
        VariableDescriptorCount //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_descriptor_indexing"}]],

        /**
         * @brief Store descriptor records directly in device-addressable buffers.
         * @par Native API mappings
         * - D3D12      : @c ID3D12DescriptorHeap.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12descriptorheap
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_descriptor_buffer
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_buffer.html
         * - Vulkan 1.4 @c VK_EXT_descriptor_buffer
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_buffer.html
         * - Vulkan 1.1 @c VK_KHR_push_descriptor
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_push_descriptor.html
         * - Vulkan 1.4 @c VK_KHR_push_descriptor
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_push_descriptor.html
         */
        DescriptorBuffer //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_descriptor_buffer"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_descriptor_buffer"}]],

        /**
         * @brief Encode a small descriptor table directly into a command stream.
         * @par Native API mappings
         * - D3D12      : @c D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_KHR_push_descriptor
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_push_descriptor.html
         * - Vulkan 1.4 @c VK_KHR_push_descriptor
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_push_descriptor.html
         * - OpenGL 3.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - OpenGL 4.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         */
        PushDescriptors //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_push_descriptor"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_push_descriptor"}]],

        /**
         * @brief Change the concrete resource kind represented by a descriptor slot.
         * @par Native API mappings
         * - D3D12      : @c D3D12_CPU_DESCRIPTOR_HANDLE.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - OpenGL 4.3 @c GL_ARB_bindless_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_bindless_texture.txt
         * - Vulkan 1.1 @c VK_VALVE_mutable_descriptor_type
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_VALVE_mutable_descriptor_type.html
         * - Vulkan 1.4 @c VK_EXT_mutable_descriptor_type
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_mutable_descriptor_type.html
         */
        MutableDescriptors //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_bindless_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_VALVE_mutable_descriptor_type"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_mutable_descriptor_type"}]],

        /**
         * @brief Address buffer storage through a stable device virtual address.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::bufferDeviceAddress.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D12      : @c D3D12_GPU_VIRTUAL_ADDRESS.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_shader_buffer_load
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_buffer_load.txt
         * - OpenGL 4.3 @c GL_NV_shader_buffer_load
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_buffer_load.txt
         * - Vulkan 1.1 @c VK_KHR_buffer_device_address
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_buffer_device_address.html
         */
        BufferDeviceAddress //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_shader_buffer_load"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_shader_buffer_load"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_buffer_device_address"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Memory and Interoperability @{ ----------------------------------------------------------- */

        /**
         * @brief Bind and unbind resource memory in independently managed regions.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::sparseBinding.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::sparseBinding.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_TILED_RESOURCES_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Device::CreateReservedResource.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_sparse_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_texture.txt
         * - OpenGL 4.3 @c GL_ARB_sparse_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_texture.txt
         */
        SparseBinding //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_sparse_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_sparse_texture"}]],

        /**
         * @brief Use partially resident sparse buffers.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::sparseResidencyBuffer.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::sparseResidencyBuffer.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_TILED_RESOURCES_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Device::CreateReservedResource.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_sparse_buffer
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_buffer.txt
         * - OpenGL 4.3 @c GL_ARB_sparse_buffer
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_buffer.txt
         */
        SparseBufferResidency //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_sparse_buffer"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_sparse_buffer"}]],

        /**
         * @brief Use partially resident two-dimensional sparse images.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::sparseResidencyImage2D.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::sparseResidencyImage2D.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_TILED_RESOURCES_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Device::CreateReservedResource.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_sparse_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_texture.txt
         * - OpenGL 4.3 @c GL_ARB_sparse_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_texture.txt
         */
        SparseImage2DResidency //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_sparse_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_sparse_texture"}]],

        /**
         * @brief Use partially resident three-dimensional sparse images.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::sparseResidencyImage3D.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::sparseResidencyImage3D.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_TILED_RESOURCES_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Device::CreateReservedResource.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-createreservedresource
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_sparse_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_texture.txt
         * - OpenGL 4.3 @c GL_ARB_sparse_texture
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_sparse_texture.txt
         */
        SparseImage3DResidency //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_sparse_texture"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_sparse_texture"}]],

        /**
         * @brief Alias one physical page into multiple sparse resource regions.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::sparseResidencyAliased.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::sparseResidencyAliased.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c ID3D11DeviceContext2::UpdateTileMappings.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_RESOURCE_ALIASING_BARRIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         */
        SparseAliasedResidency,

        /**
         * @brief Query current heap budgets and usage.
         * @par Native API mappings
         * - OpenGL 3.3 : @c GL_NVX_gpu_memory_info.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_NVX_gpu_memory_info.xhtml
         * - OpenGL 4.3 : @c GL_NVX_gpu_memory_info.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_NVX_gpu_memory_info.xhtml
         * - D3D11      : @c DXGI_QUERY_VIDEO_MEMORY_INFO.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c DXGI_QUERY_VIDEO_MEMORY_INFO.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_memory_budget
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html
         * - Vulkan 1.4 @c VK_EXT_memory_budget
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html
         */
        MemoryBudget //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_memory_budget"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_memory_budget"}]],

        /**
         * @brief Assign relative eviction priority to allocations.
         * @par Native API mappings
         * - D3D11      : @c IDXGIResource::SetEvictionPriority.
         * @see https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgiresource-setevictionpriority
         * - D3D12      : @c ID3D12Device1::SetResidencyPriority.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device1-setresidencypriority
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_memory_priority
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_priority.html
         * - Vulkan 1.4 @c VK_EXT_memory_priority
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_priority.html
         */
        MemoryPriority //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_memory_priority"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_memory_priority"}]],

        /**
         * @brief Explicitly make device-local allocations resident or pageable.
         * @par Native API mappings
         * - D3D11      : @c IDXGIDevice3::Trim.
         * @see https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgidevice3-trim
         * - D3D12      : @c ID3D12Device::MakeResident.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-makeresident
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_pageable_device_local_memory
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pageable_device_local_memory.html
         * - Vulkan 1.4 @c VK_EXT_pageable_device_local_memory
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_pageable_device_local_memory.html
         */
        PageableDeviceLocalMemory //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_pageable_device_local_memory"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_pageable_device_local_memory"}]],

        /**
         * @brief Allocate and execute against protected-content resources.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceVulkan11Features::protectedMemory.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan11Features::protectedMemory.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - D3D11      : @c ID3D11CryptoSession.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12ProtectedResourceSession.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12protectedresourcesession
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_protected_textures
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_protected_textures.txt
         * - OpenGL 4.3 @c GL_EXT_protected_textures
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_protected_textures.txt
         */
        ProtectedMemory //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_protected_textures"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_protected_textures"}]],

        /**
         * @brief Import and export resource memory across APIs or processes.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan11Features::externalMemory.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - D3D11      : @c ID3D11Device::OpenSharedResource.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Device::OpenSharedHandle.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12device-opensharedhandle
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_memory_object
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_memory_object.txt
         * - OpenGL 4.3 @c GL_EXT_memory_object
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_memory_object.txt
         * - Vulkan 1.1 @c VK_KHR_external_memory
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_external_memory.html
         */
        ExternalMemory //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_memory_object"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_memory_object"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_external_memory"}]],

        /**
         * @brief Import and export semaphores across APIs or processes.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan11Features::externalSemaphore.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - D3D11      : @c ID3D11Fence.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Fence.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12fence
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_semaphore
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_semaphore.txt
         * - OpenGL 4.3 @c GL_EXT_semaphore
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_semaphore.txt
         * - Vulkan 1.1 @c VK_KHR_external_semaphore
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_external_semaphore.html
         */
        ExternalSemaphore //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_semaphore"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_semaphore"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_external_semaphore"}]],

        /**
         * @brief Import and export fences across APIs or processes.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan11Features::externalFence.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - D3D11      : @c ID3D11Fence.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12Fence.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12fence
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_KHR_external_fence
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_external_fence.html
         * - Vulkan 1.1 @c VK_NV_memory_decompression
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_memory_decompression.html
         * - Vulkan 1.4 @c VK_NV_memory_decompression
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_memory_decompression.html
         */
        ExternalFence //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_external_fence"}]],

        /**
         * @brief Decompress supported streams directly into device memory.
         * @par Native API mappings
         * - D3D12      : @c IDStorageCompressionCodec.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_NV_memory_decompression
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_memory_decompression.html
         * - Vulkan 1.4 @c VK_NV_memory_decompression
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_memory_decompression.html
         */
        MemoryDecompression //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_NV_memory_decompression"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_NV_memory_decompression"}]],

        /**
         * @brief Copy image data between host memory and images without a command buffer.
         * @par Native API mappings
         * - OpenGL 3.3 : @c glTexSubImage2D.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glTexSubImage2D.xhtml
         * - OpenGL 4.3 : @c glTexSubImage2D.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/glTexSubImage2D.xhtml
         * - D3D11      : @c UpdateSubresource.
         * @see https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource
         * - D3D12      : @c ID3D12Resource::WriteToSubresource.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12resource-writetosubresource
         * - WebGPU     : @c GPUQueue::writeTexture.
         * @see https://www.w3.org/TR/webgpu/#dom-gpuqueue-writetexture
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_host_image_copy
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_host_image_copy.html
         * - Vulkan 1.4 @c VK_EXT_host_image_copy
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_host_image_copy.html
         */
        HostImageCopy //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_host_image_copy"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_host_image_copy"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Ray Tracing @{ --------------------------------------------------------------------------- */

        /**
         * @brief Build and manage ray-tracing acceleration structures.
         * @par Native API mappings
         * - D3D12      : @c D3D12_RAYTRACING_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_ray_tracing
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_ray_tracing.txt
         * - OpenGL 4.3 @c GL_NV_ray_tracing
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_ray_tracing.txt
         * - Vulkan 1.1 @c VK_KHR_acceleration_structure
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_acceleration_structure.html
         * - Vulkan 1.4 @c VK_KHR_acceleration_structure
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_acceleration_structure.html
         */
        AccelerationStructure //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_ray_tracing"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_ray_tracing"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_acceleration_structure"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_acceleration_structure"}]],

        /**
         * @brief Dispatch dedicated ray-generation and hit-group shader pipelines.
         * @par Native API mappings
         * - D3D12      : @c ID3D12StateObject.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12stateobject
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_ray_tracing
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_ray_tracing.txt
         * - OpenGL 4.3 @c GL_NV_ray_tracing
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_ray_tracing.txt
         * - Vulkan 1.1 @c VK_KHR_ray_tracing_pipeline
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_pipeline.html
         * - Vulkan 1.4 @c VK_KHR_ray_tracing_pipeline
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_pipeline.html
         */
        RayTracingPipeline //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_ray_tracing"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_ray_tracing"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_ray_tracing_pipeline"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_ray_tracing_pipeline"}]],

        /**
         * @brief Trace inline ray queries from ordinary shader stages.
         * @par Native API mappings
         * - D3D12      : @c RayQuery.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_KHR_ray_query
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_query.html
         * - Vulkan 1.4 @c VK_KHR_ray_query
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_query.html
         * - Vulkan 1.1 @c VK_NV_ray_tracing_motion_blur
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_ray_tracing_motion_blur.html
         * - Vulkan 1.4 @c VK_NV_ray_tracing_motion_blur
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_ray_tracing_motion_blur.html
         */
        RayQuery //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_ray_query"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_ray_query"}]],

        /**
         * @brief Trace against time-varying acceleration-structure geometry.
         * @par Native API mappings
         * - D3D12      : @c D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC::Transform3x4.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_NV_ray_tracing_motion_blur
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_ray_tracing_motion_blur.html
         * - Vulkan 1.4 @c VK_NV_ray_tracing_motion_blur
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_ray_tracing_motion_blur.html
         * - Vulkan 1.1 @c VK_KHR_ray_tracing_position_fetch
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_position_fetch.html
         * - Vulkan 1.4 @c VK_KHR_ray_tracing_position_fetch
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_position_fetch.html
         */
        RayTracingMotionBlur //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_NV_ray_tracing_motion_blur"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_NV_ray_tracing_motion_blur"}]],

        /**
         * @brief Fetch triangle positions from a ray-tracing intersection.
         * @note Manual vertex-buffer fetch in D3D12 is a shader lowering fallback, not a native position-fetch feature.
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_KHR_ray_tracing_position_fetch
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_position_fetch.html
         * - Vulkan 1.4 @c VK_KHR_ray_tracing_position_fetch
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_position_fetch.html
         * - Vulkan 1.1 @c VK_EXT_opacity_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html
         * - Vulkan 1.4 @c VK_EXT_opacity_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html
         */
        RayTracingPositionFetch //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_ray_tracing_position_fetch"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_ray_tracing_position_fetch"}]],

        /**
         * @brief Attach opacity micromaps to triangle geometry.
         * @par Native API mappings
         * - D3D12      : @c D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_opacity_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html
         * - Vulkan 1.4 @c VK_EXT_opacity_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html
         * - Vulkan 1.1 @c VK_NV_displacement_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_displacement_micromap.html
         * - Vulkan 1.4 @c VK_NV_displacement_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_displacement_micromap.html
         */
        OpacityMicromap //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_opacity_micromap"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_opacity_micromap"}]],

        /**
         * @brief Attach displacement micromaps to triangle geometry.
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_NV_displacement_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_displacement_micromap.html
         * - Vulkan 1.4 @c VK_NV_displacement_micromap
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_displacement_micromap.html
         */
        DisplacementMicromap //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_NV_displacement_micromap"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_NV_displacement_micromap"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Shader Numeric Types and Atomics @{ ------------------------------------------------------ */

        /**
         * @brief Use 16-bit floating-point arithmetic and storage in shaders.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::shaderFloat16.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D11      : @c min16float.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D_SHADER_MODEL_6_2.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c shader-f16.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_AMD_gpu_shader_half_float
         *   @see https://registry.khronos.org/OpenGL/extensions/AMD/AMD_gpu_shader_half_float.txt
         * - OpenGL 4.3 @c GL_AMD_gpu_shader_half_float
         *   @see https://registry.khronos.org/OpenGL/extensions/AMD/AMD_gpu_shader_half_float.txt
         * - Vulkan 1.1 @c VK_KHR_shader_float16_int8
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_float16_int8.html
         */
        ShaderFloat16 //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_AMD_gpu_shader_half_float"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_AMD_gpu_shader_half_float"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_shader_float16_int8"}]],

        /**
         * @brief Use 64-bit floating-point arithmetic in shaders.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::shaderFloat64.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::shaderFloat64.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c double.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D_SHADER_MODEL_6_0.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_gpu_shader_fp64
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_gpu_shader_fp64.txt
         * - OpenGL 4.3 @c GL_ARB_gpu_shader_fp64
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_gpu_shader_fp64.txt
         * - Vulkan 1.1 @c VK_KHR_shader_bfloat16
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_bfloat16.html
         * - Vulkan 1.4 @c VK_KHR_shader_bfloat16
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_bfloat16.html
         */
        ShaderFloat64 //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_gpu_shader_fp64"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_gpu_shader_fp64"}]],

        /**
         * @brief Use bfloat16 arithmetic or cooperative-matrix operands.
         * @note Vendor or cooperative-vector approximations are backend policy and are intentionally left out of
         * native-feature annotations.
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_KHR_shader_bfloat16
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_bfloat16.html
         * - Vulkan 1.4 @c VK_KHR_shader_bfloat16
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_bfloat16.html
         */
        ShaderBFloat16 //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_shader_bfloat16"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_shader_bfloat16"}]],

        /**
         * @brief Use 8-bit integer arithmetic and storage in shaders.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::shaderInt8.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D12      : @c D3D_SHADER_MODEL_6_6.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_EXT_shader_explicit_arithmetic_types_int8
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_explicit_arithmetic_types_int8.txt
         * - Vulkan 1.1 @c VK_KHR_8bit_storage
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_8bit_storage.html
         */
        ShaderInt8 //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_shader_explicit_arithmetic_types_int8"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_8bit_storage"}]],

        /**
         * @brief Use 16-bit integer arithmetic and storage in shaders.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan12Features::shaderInt16.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan12Features.html
         * - D3D11      : @c min16int.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D_SHADER_MODEL_6_2.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_EXT_shader_explicit_arithmetic_types_int16
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_explicit_arithmetic_types_int16.txt
         * - Vulkan 1.1 @c VK_KHR_16bit_storage
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_16bit_storage.html
         */
        ShaderInt16 //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_shader_explicit_arithmetic_types_int16"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_16bit_storage"}]],

        /**
         * @brief Use 64-bit integer arithmetic in shaders.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::shaderInt64.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::shaderInt64.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D12      : @c D3D_SHADER_MODEL_6_0.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_gpu_shader_int64
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_gpu_shader_int64.txt
         * - OpenGL 4.3 @c GL_ARB_gpu_shader_int64
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_gpu_shader_int64.txt
         * - OpenGL 3.3 @c GL_NV_shader_atomic_int64
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_atomic_int64.txt
         * - OpenGL 4.3 @c GL_NV_shader_atomic_int64
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_atomic_int64.txt
         */
        ShaderInt64 //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_gpu_shader_int64"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_gpu_shader_int64"}]],

        /**
         * @brief Perform 64-bit integer atomic operations in shaders.
         * @par Native API mappings
         * - D3D12      : @c D3D_SHADER_MODEL_6_6.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_shader_atomic_int64
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_atomic_int64.txt
         * - OpenGL 4.3 @c GL_NV_shader_atomic_int64
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_atomic_int64.txt
         * - Vulkan 1.1 @c VK_KHR_shader_atomic_int64
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_atomic_int64.html
         * - Vulkan 1.4 @c VK_KHR_shader_atomic_int64
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_atomic_int64.html
         */
        Int64Atomics //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_shader_atomic_int64"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_shader_atomic_int64"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_shader_atomic_int64"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_shader_atomic_int64"}]],

        /**
         * @brief Perform 32-bit floating-point atomic operations in shaders.
         * @par Native API mappings
         * - D3D12      : @c D3D_SHADER_MODEL_6_6.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_shader_atomic_float
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_atomic_float.txt
         * - OpenGL 4.3 @c GL_EXT_shader_atomic_float
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_atomic_float.txt
         * - Vulkan 1.1 @c VK_EXT_shader_atomic_float
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_atomic_float.html
         * - Vulkan 1.4 @c VK_EXT_shader_atomic_float
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_atomic_float.html
         */
        Float32Atomics //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_shader_atomic_float"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_shader_atomic_float"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_shader_atomic_float"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_shader_atomic_float"}]],

        /**
         * @brief Perform 64-bit floating-point atomic operations in shaders.
         * @par Native API mappings
         * - D3D12      : @c D3D_SHADER_MODEL_6_6.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_shader_atomic_float2
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_atomic_float2.txt
         * - OpenGL 4.3 @c GL_EXT_shader_atomic_float2
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_shader_atomic_float2.txt
         * - Vulkan 1.1 @c VK_EXT_shader_atomic_float2
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_atomic_float2.html
         * - Vulkan 1.4 @c VK_EXT_shader_atomic_float2
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_atomic_float2.html
         */
        Float64Atomics //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_shader_atomic_float2"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_shader_atomic_float2"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_shader_atomic_float2"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_shader_atomic_float2"}]],

        /**
         * @brief Execute packed integer dot-product instructions.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan13Features::integerDotProduct.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan13Features.html
         * - D3D12      : @c dot4add_u8packed.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c dot4U8Packed.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_INTEL_shader_integer_functions2
         *   @see https://registry.khronos.org/OpenGL/extensions/INTEL/INTEL_shader_integer_functions2.txt
         * - OpenGL 4.3 @c GL_INTEL_shader_integer_functions2
         *   @see https://registry.khronos.org/OpenGL/extensions/INTEL/INTEL_shader_integer_functions2.txt
         * - Vulkan 1.1 @c VK_KHR_shader_integer_dot_product
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_integer_dot_product.html
         * - OpenGL 3.3 @c GL_NV_cooperative_matrix
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_cooperative_matrix.txt
         */
        IntegerDotProduct //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_INTEL_shader_integer_functions2"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_INTEL_shader_integer_functions2"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_shader_integer_dot_product"}]],

        /**
         * @brief Execute subgroup-D3D12DDI_COOPERATIVE_VECTOR_TIER operations.
         * @par Native API mappings
         * - D3D12      : @c D3D12DDI_COOPERATIVE_VECTOR_TIER.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_cooperative_matrix
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_cooperative_matrix.txt
         * - OpenGL 4.3 @c GL_NV_cooperative_matrix
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_cooperative_matrix.txt
         * - Vulkan 1.1 @c VK_KHR_cooperative_matrix
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_cooperative_matrix.html
         * - Vulkan 1.4 @c VK_KHR_cooperative_matrix
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_cooperative_matrix.html
         */
        CooperativeMatrix //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_cooperative_matrix"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_cooperative_matrix"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_cooperative_matrix"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_cooperative_matrix"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Subgroup Operations @{ ------------------------------------------------------------------- */

        /**
         * @brief Query subgroup identity and elect one invocation.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VK_SUBGROUP_FEATURE_BASIC_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_BASIC_BIT.html
         * - Vulkan 1.4 : @c VK_SUBGROUP_FEATURE_BASIC_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_BASIC_BIT.html
         * - D3D12      : @c D3D12_FEATURE_DATA_D3D12_OPTIONS1::WaveOps.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroups.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_ARB_shader_group_vote
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_group_vote.txt
         */
        SubgroupBasic //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_shader_group_vote"}]],

        /**
         * @brief Evaluate Boolean votes across a subgroup.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VK_SUBGROUP_FEATURE_VOTE_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_VOTE_BIT.html
         * - Vulkan 1.4 : @c VK_SUBGROUP_FEATURE_VOTE_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_VOTE_BIT.html
         * - D3D12      : @c WaveActiveAllTrue.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroups.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_shader_group_vote
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_group_vote.txt
         * - OpenGL 4.3 @c GL_ARB_shader_group_vote
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_group_vote.txt
         */
        SubgroupVote //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_shader_group_vote"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_shader_group_vote"}]],

        /**
         * @brief Perform subgroup reductions and scans.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VK_SUBGROUP_FEATURE_ARITHMETIC_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_ARITHMETIC_BIT.html
         * - Vulkan 1.4 : @c VK_SUBGROUP_FEATURE_ARITHMETIC_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_ARITHMETIC_BIT.html
         * - D3D12      : @c WaveActiveSum.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroups.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_KHR_shader_subgroup
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_shader_subgroup.txt
         */
        SubgroupArithmetic //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_KHR_shader_subgroup"}]],

        /**
         * @brief Produce and consume subgroup invocation masks.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VK_SUBGROUP_FEATURE_BALLOT_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_BALLOT_BIT.html
         * - Vulkan 1.4 : @c VK_SUBGROUP_FEATURE_BALLOT_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_BALLOT_BIT.html
         * - D3D12      : @c WaveActiveBallot.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroups.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_shader_ballot
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_ballot.txt
         * - OpenGL 4.3 @c GL_ARB_shader_ballot
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_ballot.txt
         */
        SubgroupBallot //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_shader_ballot"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_shader_ballot"}]],

        /**
         * @brief Exchange register values among subgroup invocations.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VK_SUBGROUP_FEATURE_SHUFFLE_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_SHUFFLE_BIT.html
         * - Vulkan 1.4 : @c VK_SUBGROUP_FEATURE_SHUFFLE_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_SHUFFLE_BIT.html
         * - D3D12      : @c WaveReadLaneAt.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroups.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_NV_shader_thread_shuffle
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_thread_shuffle.txt
         * - OpenGL 4.3 @c GL_NV_shader_thread_shuffle
         *   @see https://registry.khronos.org/OpenGL/extensions/NV/NV_shader_thread_shuffle.txt
         */
        SubgroupShuffle //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_NV_shader_thread_shuffle"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_NV_shader_thread_shuffle"}]],

        /**
         * @brief Perform subgroup operations within fixed-size clusters.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VK_SUBGROUP_FEATURE_CLUSTERED_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_CLUSTERED_BIT.html
         * - Vulkan 1.4 : @c VK_SUBGROUP_FEATURE_CLUSTERED_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_CLUSTERED_BIT.html
         * - D3D12      : @c WaveActiveSum.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroups.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_KHR_shader_subgroup
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_shader_subgroup.txt
         */
        SubgroupClustered //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_KHR_shader_subgroup"}]],

        /**
         * @brief Exchange values within two-by-two invocation quads.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VK_SUBGROUP_FEATURE_QUAD_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_QUAD_BIT.html
         * - Vulkan 1.4 : @c VK_SUBGROUP_FEATURE_QUAD_BIT.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_SUBGROUP_FEATURE_QUAD_BIT.html
         * - D3D12      : @c QuadReadLaneAt.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroups.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_KHR_shader_subgroup
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_shader_subgroup.txt
         */
        SubgroupQuad //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_KHR_shader_subgroup"}]],

        /**
         * @brief Request or require a subgroup width for a shader stage.
         * @par Native API mappings
         * - Vulkan 1.4 : @c subgroupSizeControl.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceSubgroupSizeControlFeatures.html
         * - D3D12      : @c [WaveSize].
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c subgroup-size-control.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_subgroup_size_control
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_subgroup_size_control.html
         * - OpenGL 3.3 @c GL_ARB_shader_clock
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_clock.txt
         * - OpenGL 4.3 @c GL_ARB_shader_clock
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_clock.txt
         * - Vulkan 1.1 @c VK_KHR_shader_clock
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_clock.html
         * - Vulkan 1.4 @c VK_KHR_shader_clock
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_clock.html
         */
        SubgroupSizeControl //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_subgroup_size_control"}]],

        /**
         * @brief Read device or subgroup clock values from shaders.
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_shader_clock
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_clock.txt
         * - OpenGL 4.3 @c GL_ARB_shader_clock
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_shader_clock.txt
         * - Vulkan 1.1 @c VK_KHR_shader_clock
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_clock.html
         * - Vulkan 1.4 @c VK_KHR_shader_clock
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_shader_clock.html
         */
        ShaderClock //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_shader_clock"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_shader_clock"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_shader_clock"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_shader_clock"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Texture Formats and Sampling @{ ---------------------------------------------------------- */

        /**
         * @brief Sample BC1 through BC7 block-compressed textures.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::textureCompressionBC.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::textureCompressionBC.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c DXGI_FORMAT_BC1_UNORM.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c DXGI_FORMAT_BC1_UNORM.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c texture-compression-bc.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_texture_compression_s3tc
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt
         * - OpenGL 4.3 @c GL_EXT_texture_compression_s3tc
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt
         */
        TextureCompressionBC //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_texture_compression_s3tc"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_texture_compression_s3tc"}]],

        /**
         * @brief Sample ETC2 and EAC block-compressed textures.
         * @par Native API mappings
         * - OpenGL 4.3 : @c GL_COMPRESSED_RGB8_ETC2.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_COMPRESSED_RGB8_ETC2.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::textureCompressionETC2.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::textureCompressionETC2.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - WebGPU     : @c texture-compression-etc2.
         * @see https://www.w3.org/TR/webgpu/
         */
        TextureCompressionETC2,

        /**
         * @brief Sample ASTC low-dynamic-range block-compressed textures.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::textureCompressionASTC_LDR.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::textureCompressionASTC_LDR.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - WebGPU     : @c texture-compression-astc.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_KHR_texture_compression_astc_ldr
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_texture_compression_astc_ldr.txt
         * - OpenGL 4.3 @c GL_KHR_texture_compression_astc_ldr
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_texture_compression_astc_ldr.txt
         * - OpenGL 3.3 @c GL_KHR_texture_compression_astc_hdr
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_texture_compression_astc_hdr.txt
         * - OpenGL 4.3 @c GL_KHR_texture_compression_astc_hdr
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_texture_compression_astc_hdr.txt
         * - Vulkan 1.1 @c VK_EXT_texture_compression_astc_hdr
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_texture_compression_astc_hdr.html
         * - Vulkan 1.4 @c VK_EXT_texture_compression_astc_hdr
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_texture_compression_astc_hdr.html
         */
        TextureCompressionASTCLdr //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_KHR_texture_compression_astc_ldr"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_KHR_texture_compression_astc_ldr"}]],

        /**
         * @brief Sample ASTC high-dynamic-range block-compressed textures.
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_KHR_texture_compression_astc_hdr
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_texture_compression_astc_hdr.txt
         * - OpenGL 4.3 @c GL_KHR_texture_compression_astc_hdr
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_texture_compression_astc_hdr.txt
         * - Vulkan 1.1 @c VK_EXT_texture_compression_astc_hdr
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_texture_compression_astc_hdr.html
         * - Vulkan 1.4 @c VK_EXT_texture_compression_astc_hdr
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_texture_compression_astc_hdr.html
         */
        TextureCompressionASTCHdr //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_KHR_texture_compression_astc_hdr"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_KHR_texture_compression_astc_hdr"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_texture_compression_astc_hdr"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_texture_compression_astc_hdr"}]],

        /**
         * @brief Sample PVRTC block-compressed textures.
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_IMG_texture_compression_pvrtc
         *   @see https://registry.khronos.org/OpenGL/extensions/IMG/IMG_texture_compression_pvrtc.txt
         * - OpenGL 4.3 @c GL_IMG_texture_compression_pvrtc
         *   @see https://registry.khronos.org/OpenGL/extensions/IMG/IMG_texture_compression_pvrtc.txt
         * - Vulkan 1.1 @c VK_IMG_format_pvrtc
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_IMG_format_pvrtc.html
         * - Vulkan 1.4 @c VK_IMG_format_pvrtc
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_IMG_format_pvrtc.html
         */
        TextureCompressionPVRTC //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_IMG_texture_compression_pvrtc"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_IMG_texture_compression_pvrtc"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_IMG_format_pvrtc"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_IMG_format_pvrtc"}]],

        /**
         * @brief Apply anisotropic texture filtering.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::samplerAnisotropy.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::samplerAnisotropy.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c MaxAnisotropy.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c MaxAnisotropy.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c maxAnisotropy.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_EXT_texture_filter_anisotropic
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_filter_anisotropic.txt
         * - OpenGL 4.3 @c GL_EXT_texture_filter_anisotropic
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_filter_anisotropic.txt
         * - OpenGL 4.3 @c GL_EXT_texture_filter_minmax
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_filter_minmax.txt
         * - Vulkan 1.1 @c VK_EXT_sampler_filter_minmax
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_sampler_filter_minmax.html
         */
        SamplerAnisotropy //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_EXT_texture_filter_anisotropic"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_texture_filter_anisotropic"}]],

        /**
         * @brief Reduce filtered samples using minimum or maximum operations.
         * @par Native API mappings
         * - D3D12      : @c D3D12_FILTER_REDUCTION_TYPE_MINIMUM.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_EXT_texture_filter_minmax
         *   @see https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_filter_minmax.txt
         * - Vulkan 1.1 @c VK_EXT_sampler_filter_minmax
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_sampler_filter_minmax.html
         * - Vulkan 1.4 @c VK_EXT_sampler_filter_minmax
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_sampler_filter_minmax.html
         * - OpenGL 4.3 @c GL_IMG_texture_filter_cubic
         *   @see https://registry.khronos.org/OpenGL/extensions/IMG/IMG_texture_filter_cubic.txt
         * - Vulkan 1.1 @c VK_EXT_filter_cubic
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_filter_cubic.html
         * - Vulkan 1.4 @c VK_EXT_filter_cubic
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_filter_cubic.html
         */
        SamplerMinMax //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_EXT_texture_filter_minmax"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_sampler_filter_minmax"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_sampler_filter_minmax"}]],

        /**
         * @brief Apply cubic texture filtering.
         * @par Required extension documentation
         * - OpenGL 4.3 @c GL_IMG_texture_filter_cubic
         *   @see https://registry.khronos.org/OpenGL/extensions/IMG/IMG_texture_filter_cubic.txt
         * - Vulkan 1.1 @c VK_EXT_filter_cubic
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_filter_cubic.html
         * - Vulkan 1.4 @c VK_EXT_filter_cubic
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_filter_cubic.html
         */
        FilterCubic //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_IMG_texture_filter_cubic"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_filter_cubic"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_filter_cubic"}]],

        /**
         * @brief Supply arbitrary sampler border colors.
         * @par Native API mappings
         * - OpenGL 4.3 : @c GL_TEXTURE_BORDER_COLOR.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_TEXTURE_BORDER_COLOR.xhtml
         * - D3D11      : @c D3D11_SAMPLER_DESC::BorderColor.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_SAMPLER_DESC::BorderColor.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_texture_border_clamp
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_border_clamp.txt
         * - Vulkan 1.1 @c VK_EXT_custom_border_color
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_custom_border_color.html
         * - Vulkan 1.4 @c VK_EXT_custom_border_color
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_custom_border_color.html
         */
        CustomBorderColor //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_texture_border_clamp"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_custom_border_color"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_custom_border_color"}]],

        /**
         * @brief Convert multi-planar YCbCr images during sampling.
         * @par Native API mappings
         * - Vulkan 1.4 : @c VkPhysicalDeviceVulkan11Features::samplerYcbcrConversion.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html
         * - D3D11      : @c ID3D11VideoProcessor.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_VIDEO_PROCESS_INPUT_STREAM.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_KHR_sampler_ycbcr_conversion
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_sampler_ycbcr_conversion.html
         */
        YCbCrConversion //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_sampler_ycbcr_conversion"}]],

        /**
         * @brief Sample arrays of cube maps.
         * @par Native API mappings
         * - OpenGL 4.3 : @c GL_TEXTURE_CUBE_MAP_ARRAY.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_TEXTURE_CUBE_MAP_ARRAY.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::imageCubeArray.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::imageCubeArray.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_SRV_DIMENSION_TEXTURECUBEARRAY.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_SRV_DIMENSION_TEXTURECUBEARRAY.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c GPUTextureViewDimension::cube-array.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_texture_cube_map_array
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_cube_map_array.txt
         * - Vulkan 1.1 @c VK_EXT_image_view_min_lod
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_view_min_lod.html
         * - Vulkan 1.4 @c VK_EXT_image_view_min_lod
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_view_min_lod.html
         */
        CubeMapArray //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_texture_cube_map_array"}]],

        /**
         * @brief Clamp the minimum visible mip level per image view.
         * @par Native API mappings
         * - D3D12      : @c D3D12_SHADER_RESOURCE_VIEW_DESC::ResourceMinLODClamp.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_image_view_min_lod
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_view_min_lod.html
         * - Vulkan 1.4 @c VK_EXT_image_view_min_lod
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_view_min_lod.html
         */
        ImageViewMinimumLod //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_image_view_min_lod"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_image_view_min_lod"}]],

        /** @} ------------------------------------------------------------------------------------------------ */

        /** @name Queries, Pipelines, and Diagnostics @{ --------------------------------------------------- */

        /**
         * @brief Count samples passing depth and stencil tests.
         * @par Native API mappings
         * - OpenGL 3.3 : @c GL_ANY_SAMPLES_PASSED.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_ANY_SAMPLES_PASSED.xhtml
         * - OpenGL 4.3 : @c GL_ANY_SAMPLES_PASSED.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_ANY_SAMPLES_PASSED.xhtml
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::occlusionQuery.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::occlusionQuery.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_QUERY_OCCLUSION.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_QUERY_TYPE_OCCLUSION.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c GPUQueryType::occlusion.
         * @see https://www.w3.org/TR/webgpu/
         */
        OcclusionQuery,

        /**
         * @brief Query primitive, shader, and clipping invocation statistics.
         * @par Native API mappings
         * - Vulkan 1.1 : @c VkPhysicalDeviceFeatures::pipelineStatisticsQuery.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - Vulkan 1.4 : @c VkPhysicalDeviceFeatures::pipelineStatisticsQuery.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html
         * - D3D11      : @c D3D11_QUERY_PIPELINE_STATISTICS.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_QUERY_TYPE_PIPELINE_STATISTICS.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_pipeline_statistics_query
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_pipeline_statistics_query.txt
         * - OpenGL 4.3 @c GL_ARB_pipeline_statistics_query
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_pipeline_statistics_query.txt
         */
        PipelineStatisticsQuery //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_pipeline_statistics_query"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_pipeline_statistics_query"}]],

        /**
         * @brief Write device-domain timestamps from command streams.
         * @par Native API mappings
         * - OpenGL 4.3 : @c GL_TIMESTAMP.
         * @see https://registry.khronos.org/OpenGL-Refpages/gl4/html/GL_TIMESTAMP.xhtml
         * - Vulkan 1.1 : @c vkCmdWriteTimestamp.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html
         * - Vulkan 1.4 : @c vkCmdWriteTimestamp.
         * @see https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html
         * - D3D11      : @c D3D11_QUERY_TIMESTAMP.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_QUERY_TYPE_TIMESTAMP.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * - WebGPU     : @c timestamp-query.
         * @see https://www.w3.org/TR/webgpu/
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_timer_query
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_timer_query.txt
         * - Vulkan 1.1 @c VK_EXT_calibrated_timestamps
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_calibrated_timestamps.html
         */
        TimestampQuery //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_timer_query"}]],

        /**
         * @brief Correlate device timestamps with a host time domain.
         * @par Native API mappings
         * - D3D11      : @c IDXGISwapChain::GetFrameStatistics.
         * @see https://learn.microsoft.com/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics
         * - D3D12      : @c ID3D12CommandQueue::GetClockCalibration.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-getclockcalibration
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_calibrated_timestamps
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_calibrated_timestamps.html
         * - Vulkan 1.4 @c VK_EXT_calibrated_timestamps
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_calibrated_timestamps.html
         * - OpenGL 3.3 @c GL_INTEL_performance_query
         *   @see https://registry.khronos.org/OpenGL/extensions/INTEL/INTEL_performance_query.txt
         * - OpenGL 4.3 @c GL_INTEL_performance_query
         *   @see https://registry.khronos.org/OpenGL/extensions/INTEL/INTEL_performance_query.txt
         */
        CalibratedTimestamps //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_calibrated_timestamps"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_calibrated_timestamps"}]],

        /**
         * @brief Query implementation-defined hardware performance counters.
         * @par Native API mappings
         * - D3D11      : @c ID3D11Counter.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_INTEL_performance_query
         *   @see https://registry.khronos.org/OpenGL/extensions/INTEL/INTEL_performance_query.txt
         * - OpenGL 4.3 @c GL_INTEL_performance_query
         *   @see https://registry.khronos.org/OpenGL/extensions/INTEL/INTEL_performance_query.txt
         * - Vulkan 1.1 @c VK_KHR_performance_query
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_performance_query.html
         * - Vulkan 1.4 @c VK_KHR_performance_query
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_performance_query.html
         */
        PerformanceQuery //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_INTEL_performance_query"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_INTEL_performance_query"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_performance_query"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_performance_query"}]],

        /**
         * @brief Attach hierarchical labels to command streams and resources.
         * @par Native API mappings
         * - D3D11      : @c ID3DUserDefinedAnnotation.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12GraphicsCommandList::BeginEvent.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-beginevent
         * - WebGPU     : @c GPUObjectBase::label.
         * @see https://www.w3.org/TR/webgpu/#dom-gpuobjectbase-label
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_KHR_debug
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_debug.txt
         * - OpenGL 4.3 @c GL_KHR_debug
         *   @see https://registry.khronos.org/OpenGL/extensions/KHR/KHR_debug.txt
         * - Vulkan 1.1 @c VK_EXT_debug_utils
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_debug_utils.html
         * - Vulkan 1.4 @c VK_EXT_debug_utils
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_debug_utils.html
         */
        DebugLabels //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_KHR_debug"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_KHR_debug"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_debug_utils"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_debug_utils"}]],

        /**
         * @brief Retrieve structured device-fault addresses and vendor diagnostics.
         * @par Native API mappings
         * - D3D11      : @c DXGI_ERROR_DEVICE_REMOVED.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12DeviceRemovedExtendedData.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12deviceremovedextendeddata
         * - WebGPU     : @c GPUDevice::lost.
         * @see https://www.w3.org/TR/webgpu/#dom-gpudevice-lost
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_EXT_device_fault
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html
         * - Vulkan 1.4 @c VK_EXT_device_fault
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html
         * - Vulkan 1.1 @c VK_NV_device_diagnostic_checkpoints
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_device_diagnostic_checkpoints.html
         * - Vulkan 1.4 @c VK_NV_device_diagnostic_checkpoints
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_device_diagnostic_checkpoints.html
         */
        DeviceFaultReporting //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_device_fault"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_device_fault"}]],

        /**
         * @brief Record command-stream checkpoints for device-loss analysis.
         * @par Native API mappings
         * - D3D12      : @c D3D12_AUTO_BREADCRUMB_NODE.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_NV_device_diagnostic_checkpoints
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_device_diagnostic_checkpoints.html
         * - Vulkan 1.4 @c VK_NV_device_diagnostic_checkpoints
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_device_diagnostic_checkpoints.html
         * - Vulkan 1.1 @c VK_AMD_buffer_marker
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMD_buffer_marker.html
         * - Vulkan 1.4 @c VK_AMD_buffer_marker
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMD_buffer_marker.html
         */
        DiagnosticCheckpoints //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_NV_device_diagnostic_checkpoints"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_NV_device_diagnostic_checkpoints"}]],

        /**
         * @brief Write progress markers to buffers for post-fault command-stream analysis.
         * @par Native API mappings
         * - D3D12      : @c D3D12_DRED_PAGE_FAULT_OUTPUT.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - Vulkan 1.1 @c VK_AMD_buffer_marker
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMD_buffer_marker.html
         * - Vulkan 1.4 @c VK_AMD_buffer_marker
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMD_buffer_marker.html
         */
        DiagnosticBufferMarkers //
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_AMD_buffer_marker"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_AMD_buffer_marker"}]],

        /**
         * @brief Inspect backend pipeline executables and compiler statistics.
         * @par Native API mappings
         * - D3D11      : @c ID3D11ShaderReflection.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c ID3D12ShaderReflection.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12shaderreflection
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_get_program_binary
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_get_program_binary.txt
         * - OpenGL 4.3 @c GL_ARB_get_program_binary
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_get_program_binary.txt
         * - Vulkan 1.1 @c VK_KHR_pipeline_executable_properties
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_pipeline_executable_properties.html
         * - Vulkan 1.4 @c VK_KHR_pipeline_executable_properties
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_pipeline_executable_properties.html
         */
        PipelineExecutableInfo //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_get_program_binary"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_get_program_binary"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_pipeline_executable_properties"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_pipeline_executable_properties"}]],

        /**
         * @brief Compile and link reusable graphics-pipeline partitions.
         * @par Native API mappings
         * - D3D12      : @c ID3D12PipelineLibrary.
         * @see https://learn.microsoft.com/windows/win32/api/d3d12/nn-d3d12-id3d12pipelinelibrary
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_separate_shader_objects
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_separate_shader_objects.txt
         * - OpenGL 4.3 @c GL_ARB_separate_shader_objects
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_separate_shader_objects.txt
         * - Vulkan 1.1 @c VK_EXT_graphics_pipeline_library
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_graphics_pipeline_library.html
         * - Vulkan 1.4 @c VK_EXT_graphics_pipeline_library
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_graphics_pipeline_library.html
         */
        GraphicsPipelineLibrary //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_separate_shader_objects"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_separate_shader_objects"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_graphics_pipeline_library"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_graphics_pipeline_library"}]],

        /**
         * @brief Import and export implementation-native compiled pipeline binaries.
         * @par Native API mappings
         * - D3D11      : @c ID3DBlob.
         * @see https://learn.microsoft.com/windows/win32/direct3d11/d3d11-graphics-reference
         * - D3D12      : @c D3D12_CACHED_PIPELINE_STATE.
         * @see https://learn.microsoft.com/windows/win32/direct3d12/direct3d-12-graphics
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_get_program_binary
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_get_program_binary.txt
         * - OpenGL 4.3 @c GL_ARB_get_program_binary
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_get_program_binary.txt
         * - Vulkan 1.1 @c VK_KHR_pipeline_binary
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_pipeline_binary.html
         * - Vulkan 1.4 @c VK_KHR_pipeline_binary
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_pipeline_binary.html
         */
        PipelineBinary //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_get_program_binary"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_get_program_binary"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_KHR_pipeline_binary"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_KHR_pipeline_binary"}]],

        /**
         * @brief Bind independently compiled shader stages without a monolithic pipeline.
         * @note D3D12 dynamic shader linkage is not the same object model as Vulkan shader objects; represent it only
         * in backend-specific lowering notes.
         * @par Native API mappings
         * - D3D11      : @c ID3D11DeviceContext::VSSetShader.
         * @see https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-vssetshader
         * @par Required extension documentation
         * - OpenGL 3.3 @c GL_ARB_separate_shader_objects
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_separate_shader_objects.txt
         * - OpenGL 4.3 @c GL_ARB_separate_shader_objects
         *   @see https://registry.khronos.org/OpenGL/extensions/ARB/ARB_separate_shader_objects.txt
         * - Vulkan 1.1 @c VK_EXT_shader_object
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_object.html
         * - Vulkan 1.4 @c VK_EXT_shader_object
         *   @see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_object.html
         */
        ShaderObject //
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL33, "GL_ARB_separate_shader_objects"}]]
            [[= $::ExtensionRequirement{$::NativeApi::OpenGL43, "GL_ARB_separate_shader_objects"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan11, "VK_EXT_shader_object"}]]
            [[= $::ExtensionRequirement{$::NativeApi::Vulkan14, "VK_EXT_shader_object"}]],

        /** @} ------------------------------------------------------------------------------------------------ */
        // clang-format on
    };

    namespace Traits {

        /** @brief Compile-time extension requirements annotated on @p feature. */
        template<DeviceFeature feature>
        inline constexpr auto ExtensionRequirementsOf = [] consteval {
            using namespace std::views;
            const auto requirements = Sora::Meta::AnnotationsOf(Sora::Meta::GetEnumeratorMetaOf(feature)) |
                                      filter([](const auto& annotation) {
                                          return std::meta::type_of(annotation) == ^^$::ExtensionRequirement;
                                      }) |
                                      transform([](const auto& annotation) {
                                          return std::meta::extract<$::ExtensionRequirement>(annotation);
                                      }) |
                                      std::ranges::to<std::vector>();
            return std::define_static_array(requirements);
        }();

        /** @brief Number of extension requirements annotated on @p feature. */
        template<DeviceFeature feature>
        inline constexpr std::size_t ExtensionRequirementCountOf = ExtensionRequirementsOf<feature>.size();

    } // namespace Traits

    /** @brief Fixed-size set of renderer device capabilities. */
    using DeviceFeatureSet = Sora::EnumSet<DeviceFeature>;

    static_assert(Sora::Concept::OrdinalEnum<DeviceFeature>);
    static_assert([] consteval {
        template for (constexpr std::meta::info enumerator : Sora::Meta::EnumeratorsOf(^^DeviceFeature)) {
            if constexpr (!Sora::Concept::SpecialEnumerator<enumerator>) {
                template for (constexpr std::meta::info annotation : Sora::Meta::AnnotationsOf(enumerator)) {
                    if constexpr (std::meta::type_of(annotation) == ^^$::ExtensionRequirement) {
                        constexpr auto requirement = std::meta::extract<$::ExtensionRequirement>(annotation);
                        if (!requirement.IsValid()) {
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }());
} // namespace Sora::Renderer

namespace Sora {

    namespace $::Renderer {

        using Sora::Renderer::$::ExtensionRequirement;
        using Sora::Renderer::$::NativeApi;

    } // namespace $::Renderer

    namespace Traits::Renderer {

        using Sora::Renderer::Traits::ExtensionRequirementCountOf;
        using Sora::Renderer::Traits::ExtensionRequirementsOf;

    } // namespace Traits::Renderer

} // namespace Sora
