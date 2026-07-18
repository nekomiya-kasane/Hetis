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

#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Core/Traits/EnumTraits.h"
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <limits>
#include <type_traits>

namespace Sora::Render {

    /** @brief Rendering backend encoded in the low eight bits of a resource handle. */
    enum class Backend : uint8_t {
        Unspecified = 0, /**< No concrete backend has been assigned. */
        Vulkan = 1,      /**< Vulkan backend. */
        Direct3D = 2,    /**< Direct3D backend. */
        Metal = 3,       /**< Metal backend. */
        WebGPU = 4,      /**< WebGPU backend. */
        OpenGL = 5       /**< OpenGL backend. */
    };

    /** @brief Compile-time allocation policy for one handle pool kind. */
    struct HandlePoolPolicy {
        size_t maximumCount{};       /**< Hard slot limit. */
        size_t allocatedChunkSize{}; /**< Number of slots in one stable storage chunk. */
        size_t initialCount{};       /**< Slots initialized by default construction. */

        /** @brief Return whether this policy can instantiate a pool directory and chunks. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumCount != 0 && maximumCount <= std::numeric_limits<uint32_t>::max() &&
                   allocatedChunkSize != 0 && allocatedChunkSize <= maximumCount && initialCount <= maximumCount;
        }

        /** @brief Return the maximum number of chunks required to store all slots. */
        [[nodiscard]] constexpr size_t MaximumChunkCount() const noexcept {
            return (maximumCount + allocatedChunkSize - 1) / allocatedChunkSize;
        }

        friend constexpr bool operator==(const HandlePoolPolicy&, const HandlePoolPolicy&) noexcept = default;
    };

    /** @brief Render resource kind encoded in bits 8 through 15 of a resource handle. */
    enum class HandleKind : uint8_t {
        // clang-format off
        Unknown               [[= HandlePoolPolicy{}]]                                                               = 0,  /**< Unregistered or erased resource kind. */
        Buffer                [[= HandlePoolPolicy{16'384, 256, 256}]]  = 1,  /**< Buffer resource. */
        Texture               [[= HandlePoolPolicy{16'384, 256, 256}]]  = 2,  /**< Texture resource. */
        TextureView           [[= HandlePoolPolicy{32'768, 256, 256}]]  = 3,  /**< Texture view resource. */
        Sampler               [[= HandlePoolPolicy{2'048, 256, 256}]]   = 4,  /**< Sampler resource. */
        Pipeline              [[= HandlePoolPolicy{8'192, 256, 256}]]   = 5,  /**< Pipeline resource. */
        PipelineLayout        [[= HandlePoolPolicy{4'096, 256, 256}]]   = 6,  /**< Pipeline layout resource. */
        PipelineCache         [[= HandlePoolPolicy{16, 16, 16}]]        = 7,  /**< Pipeline cache resource. */
        PipelineLibraryPart   [[= HandlePoolPolicy{4'096, 256, 256}]]   = 8,  /**< Pipeline-library component. */
        ShaderModule          [[= HandlePoolPolicy{4'096, 256, 256}]]   = 9,  /**< Shader module resource. */
        Fence                 [[= HandlePoolPolicy{256, 64, 64}]]       = 10, /**< Fence resource. */
        Semaphore             [[= HandlePoolPolicy{512, 64, 64}]]       = 11, /**< Semaphore resource. */
        QueryPool             [[= HandlePoolPolicy{128, 32, 32}]]       = 12, /**< Query pool resource. */
        AccelerationStructure [[= HandlePoolPolicy{8'192, 256, 256}]]   = 13, /**< Acceleration structure. */
        Swapchain             [[= HandlePoolPolicy{16, 16, 16}]]        = 14, /**< Presentation swapchain. */
        DeviceMemory          [[= HandlePoolPolicy{1'024, 64, 64}]]     = 15, /**< Explicit device-memory allocation. */
        DescriptorLayout      [[= HandlePoolPolicy{4'096, 256, 256}]]   = 16, /**< Descriptor layout resource. */
        DescriptorSet         [[= HandlePoolPolicy{32'768, 256, 256}]]  = 17, /**< Descriptor set resource. */
        CommandBuffer         [[= HandlePoolPolicy{512, 64, 64}]]       = 18, /**< Command buffer resource. */
        CommandPool           [[= HandlePoolPolicy{64, 16, 16}]]        = 19  /**< Command pool resource. */
        // clang-format on
    };

    namespace Traits {

        /**
         * @brief Return the allocation policy annotated on @p kind's enumerator.
         * @param[in] kind Handle kind whose pool policy is requested.
         * @return Annotated pool policy.
         */
        template<HandleKind kind>
        inline constexpr HandlePoolPolicy HandlePoolPolicyOf =
            Sora::$::GetSingle<HandlePoolPolicy>(Sora::Meta::GetEnumeratorMetaOf(kind));

    } // namespace Traits

    namespace Concept {

        /** @brief Resource kind that has a valid pool policy and may back a typed handle pool. */
        template<HandleKind Kind>
        concept PooledHandleKind =
            Kind != HandleKind::Unknown && Sora::Render::Traits::HandlePoolPolicyOf<Kind>.IsValid();

    } // namespace Concept

    /**
     * @brief Eight-byte opaque identity for one statically selected Render resource kind.
     * @tparam Tag Compile-time resource identity declared by @ref HandleKind.
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
        inline static constexpr uint64_t kBackendShift = 0;
        inline static constexpr uint64_t kKindShift = 8;
        inline static constexpr uint64_t kIndexShift = 16;
        inline static constexpr uint64_t kGenerationShift = 48;

        inline static constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();
        inline static constexpr uint16_t kInvalidGeneration = std::numeric_limits<uint16_t>::max();

        inline static constexpr HandleKind Type = Tag; /**< Unsigned integer type for @p Tag. */

        /** @brief Construct a null handle. */
        constexpr Handle() noexcept = default;

        /** @brief Construct from a packed 64-bit identity. */
        constexpr explicit Handle(uint64_t rawValue) noexcept : value_(rawValue) {}

        constexpr Handle(const Handle&) noexcept = default;
        constexpr Handle& operator=(const Handle&) noexcept = default;

        /** @brief Return whether this handle is non-null. */
        [[nodiscard]] constexpr bool IsValid() const noexcept { return Raw() != 0; }

        /** @brief Convert to @c true when this handle is non-null. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }

        /** @brief Return the packed 64-bit identity. */
        [[nodiscard]] constexpr uint64_t Raw() const noexcept { return value_; }

        /** @brief Return the generation captured when the slot was allocated. */
        [[nodiscard]] constexpr uint16_t GetGeneration() const noexcept {
            return static_cast<uint16_t>(value_ >> kGenerationShift);
        }

        /** @brief Return the pool slot index. */
        [[nodiscard]] constexpr uint32_t GetIndex() const noexcept {
            return static_cast<uint32_t>(value_ >> kIndexShift);
        }

        /** @brief Return the raw encoded resource-kind byte. */
        [[nodiscard]] constexpr uint8_t GetTypeTag() const noexcept {
            return static_cast<uint8_t>(value_ >> kKindShift);
        }

        /** @brief Return the encoded resource kind. */
        [[nodiscard]] constexpr HandleKind GetKind() const noexcept { return static_cast<HandleKind>(GetTypeTag()); }

        /** @brief Return the raw encoded backend byte. */
        [[nodiscard]] constexpr uint8_t GetBackendTag() const noexcept {
            return static_cast<uint8_t>(value_ >> kBackendShift);
        }

        /** @brief Return the encoded rendering backend. */
        [[nodiscard]] constexpr Backend GetBackend() const noexcept { return static_cast<Backend>(GetBackendTag()); }

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
            return Handle{(static_cast<uint64_t>(generation) << kGenerationShift) |
                          (static_cast<uint64_t>(index) << kIndexShift) | (static_cast<uint64_t>(Tag) << kKindShift) |
                          (static_cast<uint64_t>(backend) << kBackendShift)};
        }

        friend constexpr bool operator==(const Handle& lhs, const Handle& rhs) noexcept {
            return lhs.Raw() == rhs.Raw();
        }
        friend constexpr auto operator<=>(const Handle& lhs, const Handle& rhs) noexcept {
            return lhs.Raw() <=> rhs.Raw();
        }

    private:
        uint64_t value_ = 0;
    };

    namespace Concept {

        /** @brief Strongly typed Render handle instantiation. */
        template<typename T>
        concept PooledHandle = requires(T handle) {
            typename T::Type;
            { T::Pack(1, 0) } -> std::same_as<T>;
        } && std::same_as<std::remove_cvref_t<T>, Handle<T::Type>> && Concept::PooledHandleKind<T::Type>;

    } // namespace Concept

    namespace Traits {

        /** @brief Strong handle type for @p Kind. */
        template<HandleKind Kind>
        using HandleOf = Handle<Kind>;

        /** @brief Compile-time resource kind encoded by a @ref Render::Handle specialization. */
        template<Concept::PooledHandle T>
        inline constexpr HandleKind HandleKindOf = std::remove_cvref_t<T>::Type;

    } // namespace Traits

    /** @brief Buffer identity. */
    using BufferHandle = Handle<HandleKind::Buffer>;
    /** @brief Texture identity. */
    using TextureHandle = Handle<HandleKind::Texture>;
    /** @brief Texture-view identity. */
    using TextureViewHandle = Handle<HandleKind::TextureView>;
    /** @brief Sampler identity. */
    using SamplerHandle = Handle<HandleKind::Sampler>;
    /** @brief Pipeline identity. */
    using PipelineHandle = Handle<HandleKind::Pipeline>;
    /** @brief Pipeline-layout identity. */
    using PipelineLayoutHandle = Handle<HandleKind::PipelineLayout>;
    /** @brief Pipeline-cache identity. */
    using PipelineCacheHandle = Handle<HandleKind::PipelineCache>;
    /** @brief Pipeline-library-part identity. */
    using PipelineLibraryPartHandle = Handle<HandleKind::PipelineLibraryPart>;
    /** @brief Shader-module identity. */
    using ShaderModuleHandle = Handle<HandleKind::ShaderModule>;
    /** @brief Fence identity. */
    using FenceHandle = Handle<HandleKind::Fence>;
    /** @brief Semaphore identity. */
    using SemaphoreHandle = Handle<HandleKind::Semaphore>;
    /** @brief Query-pool identity. */
    using QueryPoolHandle = Handle<HandleKind::QueryPool>;
    /** @brief Acceleration-structure identity. */
    using AccelStructHandle = Handle<HandleKind::AccelerationStructure>;
    /** @brief Swapchain identity. */
    using SwapchainHandle = Handle<HandleKind::Swapchain>;
    /** @brief Device-memory identity. */
    using DeviceMemoryHandle = Handle<HandleKind::DeviceMemory>;
    /** @brief Descriptor-layout identity. */
    using DescriptorLayoutHandle = Handle<HandleKind::DescriptorLayout>;
    /** @brief Descriptor-set identity. */
    using DescriptorSetHandle = Handle<HandleKind::DescriptorSet>;
    /** @brief Command-buffer identity. */
    using CommandBufferHandle = Handle<HandleKind::CommandBuffer>;
    /** @brief Command-pool identity. */
    using CommandPoolHandle = Handle<HandleKind::CommandPool>;

    static_assert(sizeof(BufferHandle) == sizeof(uint64_t));
    static_assert(alignof(BufferHandle) == alignof(uint64_t));

} // namespace Sora::Render
