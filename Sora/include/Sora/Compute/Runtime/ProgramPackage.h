/**
 * @file ProgramPackage.h
 * @brief Validated shared ownership for one canonical serialized kernel program.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Compiler/KernelProgramImage.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable failure while validating or owning one canonical program package. */
    enum class ProgramPackageErrorCode : uint8_t {
        InvalidProgram,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Program-package failure with semantic identity and decoder location when available. */
    struct ProgramPackageError {
        ProgramPackageErrorCode code = ProgramPackageErrorCode::InvalidProgram;
        SemanticHash program{};
        uint64_t detail = 0;
        size_t offset = 0;

        friend constexpr bool operator==(ProgramPackageError, ProgramPackageError) noexcept = default;
    };

    /** @brief Shared immutable lease for one canonical serialized KernelProgram. */
    class ProgramPackage final {
        struct Storage {
            SemanticHash hash{};
            std::vector<std::byte> bytes{};
        };

    public:
        ProgramPackage() noexcept = default;

        /** @brief Serialize and own one semantically valid KernelProgram exactly once. */
        template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
        [[nodiscard]] static std::expected<ProgramPackage, ProgramPackageError>
        Create(const KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>& program) noexcept {
            const auto valid = ValidateKernelProgram(program);
            if (!valid) {
                return std::unexpected(ProgramPackageError{
                    ProgramPackageErrorCode::InvalidProgram, {}, static_cast<uint64_t>(valid.error().code)});
            }
            const auto serialized = program.Serialize();
            const SemanticHash hash = Detail::HashBytes(serialized.Bytes());
            try {
                auto storage = std::make_shared<Storage>();
                storage->hash = hash;
                storage->bytes.assign(serialized.Bytes().begin(), serialized.Bytes().end());
                return ProgramPackage{std::move(storage)};
            } catch (const std::length_error&) {
                return std::unexpected(ProgramPackageError{ProgramPackageErrorCode::CapacityExceeded, hash});
            } catch (const std::bad_alloc&) {
                return std::unexpected(ProgramPackageError{ProgramPackageErrorCode::OutOfMemory, hash});
            }
        }

        /** @brief Consume one already validated dynamic compiler image without re-decoding or copying bytes. */
        [[nodiscard]] static std::expected<ProgramPackage, ProgramPackageError>
        Create(KernelProgramImage image) noexcept;

        /** @brief Prove canonical external semantics under backend-independent resource budgets. */
        [[nodiscard]] static std::expected<SemanticHash, ProgramPackageError>
        Validate(std::span<const std::byte> bytes, KernelProgramLimits limits = {}) noexcept;

        /** @brief Validate external bytes without backend capacity assumptions and retain them exactly once. */
        [[nodiscard]] static std::expected<ProgramPackage, ProgramPackageError>
        Load(std::span<const std::byte> bytes, KernelProgramLimits limits = {}) noexcept;

        /** @brief Return whether this handle retains canonical program bytes. */
        [[nodiscard]] explicit operator bool() const noexcept { return storage_ != nullptr; }

        /** @brief Return the semantic identity equal to HashBytes(Bytes()). */
        [[nodiscard]] SemanticHash Hash() const noexcept {
            return storage_ != nullptr ? storage_->hash : SemanticHash{};
        }

        /** @brief Return canonical immutable package bytes while this lease remains alive. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return storage_ != nullptr ? std::span<const std::byte>{storage_->bytes} : std::span<const std::byte>{};
        }

        /** @brief Return the number of package leases, primarily for ownership tests and diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return storage_.use_count(); }

    private:
        explicit ProgramPackage(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

        std::shared_ptr<const Storage> storage_{};
    };

} // namespace Sora::Compute
