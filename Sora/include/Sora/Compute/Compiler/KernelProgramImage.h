/**
 * @file KernelProgramImage.h
 * @brief Owning dynamic representation of one canonical KernelProgram.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelProgramPackage.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Backend-independent resource budgets for decoding one canonical program. */
    struct KernelProgramLimits {
        uint64_t maximumBytes = uint64_t{64} * 1'024 * 1'024;
        uint32_t maximumArguments = 65'536;
        uint32_t maximumScalarParameters = 65'536;
        uint32_t maximumInstructions = 1'048'576;

        /** @brief Return whether the budgets admit at least one valid elementwise program. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return maximumBytes >= KernelProgram<1, 0, 1>::kHeaderWireSize + KernelProgram<1, 0, 1>::kArgumentWireSize +
                                       KernelModule<0, 1>::kWireCapacity &&
                   maximumBytes <= std::numeric_limits<size_t>::max() && maximumArguments != 0 &&
                   maximumInstructions != 0;
        }

        friend constexpr bool operator==(KernelProgramLimits, KernelProgramLimits) noexcept = default;
    };

    /** @brief Unvalidated owning fields used to construct one dynamic canonical program transactionally. */
    struct KernelProgramImageStorage {
        KernelPattern pattern = KernelPattern::Elementwise;
        IndexDomain domain{};
        PartitionContract partition{};
        NumericalPolicy numericalPolicy{};
        std::vector<KernelArgumentDesc> arguments{};
        std::vector<ScalarInputBinding> inputs{};
        std::vector<ParameterDesc> scalarParameters{};
        std::vector<Instruction> instructions{};
        ValueId result{};
        KernelArgumentId output{};
    };

    /** @brief Immutable owning semantic image plus its unique canonical wire representation. */
    class KernelProgramImage final {
    public:
        KernelProgramImage() noexcept = default;

        /** @brief Validate and canonicalize caller-owned semantic fields. */
        [[nodiscard]] static std::expected<KernelProgramImage, KernelProgramPackageError>
        Create(KernelProgramImageStorage storage, KernelProgramLimits limits = {}) noexcept;

        /** @brief Decode external canonical bytes into one validated owning semantic image. */
        [[nodiscard]] static std::expected<KernelProgramImage, KernelProgramPackageError>
        Decode(std::span<const std::byte> bytes, KernelProgramLimits limits = {}) noexcept;

        /** @brief Return whether this image owns one canonical program. */
        [[nodiscard]] explicit operator bool() const noexcept { return !bytes_.empty(); }

        /** @brief Borrow the program through the common representation-independent interface. */
        [[nodiscard]] KernelProgramView View() const noexcept;

        /** @brief Return canonical immutable program bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return bytes_; }

        /** @brief Return the semantic identity equal to HashBytes(Bytes()). */
        [[nodiscard]] SemanticHash Hash() const noexcept { return hash_; }

        /** @brief Transfer canonical bytes out of a consumed image without copying. */
        [[nodiscard]] std::vector<std::byte> ConsumeBytes() && noexcept { return std::move(bytes_); }

        /** @brief Derive canonical per-work-item resource demand. */
        [[nodiscard]] KernelResourceDemand ResourceDemand() const noexcept { return ResourceDemandOf(View()); }

    private:
        explicit KernelProgramImage(KernelProgramImageStorage storage, std::vector<std::byte> bytes,
                                    SemanticHash hash) noexcept;

        KernelPattern pattern_ = KernelPattern::Elementwise;
        IndexDomain domain_{};
        PartitionContract partition_{};
        NumericalPolicy numericalPolicy_{};
        std::vector<KernelArgumentDesc> arguments_{};
        std::vector<ScalarInputBinding> inputs_{};
        std::vector<ParameterDesc> scalarParameters_{};
        std::vector<Instruction> instructions_{};
        ValueId result_{};
        KernelArgumentId output_{};
        std::vector<std::byte> bytes_{};
        SemanticHash hash_{};
    };

    static_assert(std::is_trivially_copyable_v<KernelProgramLimits>);

} // namespace Sora::Compute
