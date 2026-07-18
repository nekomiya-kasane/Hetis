/**
 * @file Handle.h
 * @brief Define compact, strongly typed handles for Render resources.
 * @details A handle occupies eight bytes with layout @c [generation:16|index:32|kind:8|backend:8]. The compile-time
 * tag determines the encoded resource kind, so unrelated handle types cannot be mixed accidentally. A default
 * constructed handle is null; non-null handles are created by @ref HandlePool rather than by application code.
 *
 * Include this lightweight header when an API only passes resource identities:
 * @code{.cpp}
 * void BindVertexBuffer(Sora::Render::BufferHandle buffer);
 *
 * Sora::Render::BufferHandle buffer;
 * if (!buffer) {
 *     // No buffer is bound.
 * }
 * @endcode
 *
 * Include @c <Sora/Render/HandlePool.h> when storage, lookup, or deferred reclamation is required.
 * @ingroup Render
 */

#pragma once

#include "Sora/Core/Traits/EnumTraits.h"
#include <Sora/Core/ToString.h>

#include <compare>
#include <cstddef>
#include <cstdint>

namespace Sora::Render {

    /** @brief Rendering backend encoded in the low eight bits of a resource handle. */
    enum class Backend : std::uint8_t {
        Unspecified = 0, /**< No concrete backend has been assigned. */
        Vulkan = 1,      /**< Vulkan backend. */
        Direct3D11 = 2,  /**< Direct3D 11 backend. */
        Direct3D12 = 3,  /**< Direct3D 12 backend. */
        Metal = 4,       /**< Metal backend. */
        WebGPU = 5,      /**< WebGPU backend. */
        OpenGL = 6       /**< OpenGL backend. */
    };

    /** @brief Render resource kind encoded in bits 8 through 15 of a resource handle. */
    enum class HandleKind : std::uint8_t {
        Unknown = 0,                /**< Unregistered or erased resource kind. */
        Buffer = 1,                 /**< Buffer resource. */
        Texture = 2,                /**< Texture resource. */
        TextureView = 3,            /**< Texture view resource. */
        Sampler = 4,                /**< Sampler resource. */
        Pipeline = 5,               /**< Pipeline resource. */
        PipelineLayout = 6,         /**< Pipeline layout resource. */
        PipelineCache = 7,          /**< Pipeline cache resource. */
        PipelineLibraryPart = 8,    /**< Pipeline-library component. */
        ShaderModule = 9,           /**< Shader module resource. */
        Fence = 10,                 /**< Fence resource. */
        Semaphore = 11,             /**< Semaphore resource. */
        QueryPool = 12,             /**< Query pool resource. */
        AccelerationStructure = 13, /**< Ray-tracing acceleration structure. */
        Swapchain = 14,             /**< Presentation swapchain. */
        DeviceMemory = 15,          /**< Explicit device-memory allocation. */
        DescriptorLayout = 16,      /**< Descriptor layout resource. */
        DescriptorSet = 17,         /**< Descriptor set resource. */
        CommandBuffer = 18,         /**< Command buffer resource. */
        CommandPool = 19            /**< Command pool resource. */
    };

    /**
     * @brief Expose the compile-time policy for @p Tag.
     * @tparam Tag Compile-time resource identity.
     */
    template<HandleKind Tag>
    struct HandleTraits {
        // clang-format off
        static constexpr std::pair<HandleKind, size_t> chunkSizeMap[]{
            {HandleKind::Texture,               256},
            {HandleKind::TextureView,           256},
            {HandleKind::Sampler,               256},
            {HandleKind::Pipeline,              256},
            {HandleKind::PipelineLayout,        256},
            {HandleKind::PipelineCache,         16},
            {HandleKind::PipelineLibraryPart,   256},
            {HandleKind::ShaderModule,          256},
            {HandleKind::Fence,                 64},
            {HandleKind::Semaphore,             64},
            {HandleKind::QueryPool,             32},
            {HandleKind::AccelerationStructure, 256},
            {HandleKind::Swapchain,             16},
            {HandleKind::DeviceMemory,          64},
            {HandleKind::DescriptorLayout,      256},
            {HandleKind::DescriptorSet,         256},
            {HandleKind::CommandBuffer,         64},
            {HandleKind::CommandPool,           16} 
        };
        // clang-format on

        // clang-format off
        static constexpr std::pair<HandleKind, size_t> capacityMap[]{
            {HandleKind::Texture,               1'6384},
            {HandleKind::TextureView,           3'2768},
            {HandleKind::Sampler,               2048},
            {HandleKind::Pipeline,              8192},
            {HandleKind::PipelineLayout,        4096},
            {HandleKind::PipelineCache,         16},
            {HandleKind::PipelineLibraryPart,   4096},
            {HandleKind::ShaderModule,          4096},
            {HandleKind::Fence,                 256},
            {HandleKind::Semaphore,             512},
            {HandleKind::QueryPool,             128},
            {HandleKind::AccelerationStructure, 8192},
            {HandleKind::Swapchain,             16},
            {HandleKind::DeviceMemory,          1024},
            {HandleKind::DescriptorLayout,      4096},
            {HandleKind::DescriptorSet,         3'2768},
            {HandleKind::CommandBuffer,         512},
            {HandleKind::CommandPool,           64},
        };
        // clang-format on

        // clang-format off
        static constexpr std::pair<HandleKind, size_t> initialCapacityMap[]{
            {HandleKind::Texture,               256},
            {HandleKind::TextureView,           256},
            {HandleKind::Sampler,               256},
            {HandleKind::Pipeline,              256},
            {HandleKind::PipelineLayout,        256},
            {HandleKind::PipelineCache,         16},
            {HandleKind::PipelineLibraryPart,   256},
            {HandleKind::ShaderModule,          256},
            {HandleKind::Fence,                 64},
            {HandleKind::Semaphore,             64},
            {HandleKind::QueryPool,             32},
            {HandleKind::AccelerationStructure, 256},
            {HandleKind::Swapchain,             16},
            {HandleKind::DeviceMemory,          64},
            {HandleKind::DescriptorLayout,      256},
            {HandleKind::DescriptorSet,         256},
            {HandleKind::CommandBuffer,         64},
            {HandleKind::CommandPool,           16}
        };
        // clang-format on

        static constexpr size_t GetProp(HandleKind kind, std::span<const std::pair<HandleKind, size_t>> map) {
            for (const auto& [k, v] : map) {
                if (k == kind) {
                    return v;
                }
            }
            return size_t{0};
        }

        inline static constexpr HandleKind kKind = Tag; /**< Encoded resource kind. */

        inline static constexpr size_t kInvalidIndex =
            std::numeric_limits<uint32_t>::max(); /**< Sentinel slot index. */
        inline static constexpr size_t kMaximumCount =
            GetProp(Tag, std::span{capacityMap}); /**< Default maximum slot count. */
        inline static constexpr size_t kAllocatedChunkSize =
            GetProp(Tag, std::span{chunkSizeMap}); /**< Default sparse chunk size. */
        inline static constexpr size_t kMaximumChunkCount =
            (kMaximumCount + kAllocatedChunkSize - 1) / kAllocatedChunkSize; /**< Maximum sparse chunk count. */
        inline static constexpr size_t kInitialCount =
            GetProp(Tag, std::span{initialCapacityMap}); /**< Default initial slot count. */

        inline static constexpr FixedString<32> kName =
            Sora::String::Wrap("<", ">", Sora::Traits::EnumDisplayName<Tag>()); /**< Diagnostic name. */
    };

    /**
     * @brief Eight-byte opaque identity for one statically selected Render resource kind.
     * @tparam Tag Compile-time resource identity registered by @ref DescribeHandle.
     *
     * @details Bit layout of the packed 64-bit @c value:
     * @code
     *  63                 48 47                 16 15        8 7         0
     * +----------------------+----------------------+----------+----------+
     * |     generation       |         index        |   kind   | backend  |
     * |      (16 bits)       |      (32 bits)       | (8 bits) | (8 bits) |
     * +----------------------+----------------------+----------+----------+
     * @endcode
     */
    template<HandleKind Tag>
    struct Handle {
        inline static constexpr uint64_t kGenerationBits = 16; /**< Generation field width. */
        inline static constexpr uint64_t kIndexBits = 32;      /**< Slot-index field width. */
        inline static constexpr uint64_t kTypeBits = 8;        /**< Resource-kind field width. */
        inline static constexpr uint64_t kBackendBits = 8;     /**< Backend field width. */

        union {
            uint64_t value = 0; /**< Packed identity; zero denotes the null handle. */

            struct {
                uint64_t backend : kBackendBits;       /**< Originating rendering backend. */
                uint64_t typeTag : kTypeBits;          /**< Encoded resource-kind tag. */
                uint64_t index : kIndexBits;           /**< Pool slot index. */
                uint64_t generation : kGenerationBits; /**< Slot generation. */
            } fields;
        };

        /** @brief Construct a null handle. */
        constexpr Handle() noexcept : value(0) {}

        /** @brief Construct from a packed 64-bit identity. */
        constexpr explicit Handle(uint64_t rawValue) noexcept : value(rawValue) {}

        constexpr Handle(const Handle&) noexcept = default;
        constexpr Handle& operator=(const Handle&) noexcept = default;

        /** @brief Return whether this handle is non-null. */
        [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }

        /** @brief Convert to @c true when this handle is non-null. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }

        /** @brief Return the generation captured when the slot was allocated. */
        [[nodiscard]] constexpr uint16_t GetGeneration() const noexcept {
            return static_cast<uint16_t>(fields.generation);
        }

        /** @brief Return the pool slot index. */
        [[nodiscard]] constexpr uint32_t GetIndex() const noexcept { return static_cast<uint32_t>(fields.index); }

        /** @brief Return the encoded resource kind. */
        [[nodiscard]] constexpr HandleKind GetKind() const noexcept { return static_cast<HandleKind>(fields.typeTag); }

        /** @brief Return the encoded rendering backend. */
        [[nodiscard]] constexpr Backend GetBackend() const noexcept { return static_cast<Backend>(fields.backend); }

        /** @brief Return a null handle of this resource type. */
        [[nodiscard]] static constexpr Handle Null() noexcept { return {}; }

        /**
         * @brief Pack a handle using the kind associated with @p Tag.
         * @param[in] generation Current nonzero slot generation.
         * @param[in] index Pool slot index.
         * @param[in] backend Originating rendering backend.
         * @return Packed handle.
         */
        [[nodiscard]] static constexpr Handle Pack(uint16_t generation, uint32_t index,
                                                   Backend backend = Backend::Unspecified) noexcept {
            Handle handle;
            handle.fields.backend = static_cast<uint64_t>(backend);
            handle.fields.typeTag = static_cast<uint64_t>(Tag);
            handle.fields.index = index;
            handle.fields.generation = generation;
            return handle;
        }

        friend constexpr bool operator==(const Handle& lhs, const Handle& rhs) noexcept {
            return lhs.value == rhs.value;
        }
        friend constexpr auto operator<=>(const Handle& lhs, const Handle& rhs) noexcept {
            return lhs.value <=> rhs.value;
        }
    };

    // clang-format off
    using BufferHandle              = Handle<HandleKind::Buffer>;                    /**< Buffer identity. */
    using TextureHandle             = Handle<HandleKind::Texture>;                   /**< Texture identity. */
    using TextureViewHandle         = Handle<HandleKind::TextureView>;               /**< Texture-view identity. */
    using SamplerHandle             = Handle<HandleKind::Sampler>;                   /**< Sampler identity. */
    using PipelineHandle            = Handle<HandleKind::Pipeline>;                  /**< Pipeline identity. */
    using PipelineLayoutHandle      = Handle<HandleKind::PipelineLayout>;            /**< Pipeline-layout identity. */
    using PipelineCacheHandle       = Handle<HandleKind::PipelineCache>;             /**< Pipeline-cache identity. */
    using PipelineLibraryPartHandle = Handle<HandleKind::PipelineLibraryPart>;       /**< Pipeline-library-part identity. */
    using ShaderModuleHandle        = Handle<HandleKind::ShaderModule>;              /**< Shader-module identity. */
    using FenceHandle               = Handle<HandleKind::Fence>;                     /**< Fence identity. */
    using SemaphoreHandle           = Handle<HandleKind::Semaphore>;                 /**< Semaphore identity. */
    using QueryPoolHandle           = Handle<HandleKind::QueryPool>;                 /**< Query-pool identity. */
    using AccelStructHandle         = Handle<HandleKind::AccelerationStructure>;     /**< Acceleration-structure identity. */
    using SwapchainHandle           = Handle<HandleKind::Swapchain>;                 /**< Swapchain identity. */
    using DeviceMemoryHandle        = Handle<HandleKind::DeviceMemory>;              /**< Device-memory identity. */
    using DescriptorLayoutHandle    = Handle<HandleKind::DescriptorLayout>;          /**< Descriptor-layout identity. */
    using DescriptorSetHandle       = Handle<HandleKind::DescriptorSet>;             /**< Descriptor-set identity. */
    using CommandBufferHandle       = Handle<HandleKind::CommandBuffer>;             /**< Command-buffer identity. */
    using CommandPoolHandle         = Handle<HandleKind::CommandPool>;               /**< Command-pool identity. */
    // clang-format on

    // clang-format off
    inline constexpr std::size_t kMaxBufferCount              = HandleTraits<HandleKind::Buffer>::kMaximumCount;
    inline constexpr std::size_t kMaxTextureCount             = HandleTraits<HandleKind::Texture>::kMaximumCount;
    inline constexpr std::size_t kMaxTextureViewCount         = HandleTraits<HandleKind::TextureView>::kMaximumCount;
    inline constexpr std::size_t kMaxSamplerCount             = HandleTraits<HandleKind::Sampler>::kMaximumCount;
    inline constexpr std::size_t kMaxShaderModuleCount        = HandleTraits<HandleKind::ShaderModule>::kMaximumCount;
    inline constexpr std::size_t kMaxFenceCount               = HandleTraits<HandleKind::Fence>::kMaximumCount;
    inline constexpr std::size_t kMaxSemaphoreCount           = HandleTraits<HandleKind::Semaphore>::kMaximumCount;
    inline constexpr std::size_t kMaxPipelineCount            = HandleTraits<HandleKind::Pipeline>::kMaximumCount;
    inline constexpr std::size_t kMaxPipelineLayoutCount      = HandleTraits<HandleKind::PipelineLayout>::kMaximumCount;
    inline constexpr std::size_t kMaxDescriptorLayoutCount    = HandleTraits<HandleKind::DescriptorLayout>::kMaximumCount;
    inline constexpr std::size_t kMaxDescriptorSetCount       = HandleTraits<HandleKind::DescriptorSet>::kMaximumCount;
    inline constexpr std::size_t kMaxPipelineCacheCount       = HandleTraits<HandleKind::PipelineCache>::kMaximumCount;
    inline constexpr std::size_t kMaxPipelineLibraryPartCount = HandleTraits<HandleKind::PipelineLibraryPart>::kMaximumCount;
    inline constexpr std::size_t kMaxQueryPoolCount           = HandleTraits<HandleKind::QueryPool>::kMaximumCount;
    inline constexpr std::size_t kMaxAccelStructCount         = HandleTraits<HandleKind::AccelerationStructure>::kMaximumCount;
    inline constexpr std::size_t kMaxSwapchainCount           = HandleTraits<HandleKind::Swapchain>::kMaximumCount;
    inline constexpr std::size_t kMaxCommandBufferCount       = HandleTraits<HandleKind::CommandBuffer>::kMaximumCount;
    inline constexpr std::size_t kMaxCommandPoolCount         = HandleTraits<HandleKind::CommandPool>::kMaximumCount;
    inline constexpr std::size_t kMaxDeviceMemoryCount        = HandleTraits<HandleKind::DeviceMemory>::kMaximumCount;
    // clang-format on

    static_assert(sizeof(BufferHandle) == sizeof(uint64_t));
    static_assert(alignof(BufferHandle) == alignof(uint64_t));

} // namespace Sora::Render
