/**
 * @file ProgramPackage.cpp
 * @brief Backend-independent validation and ownership of canonical portable programs.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/ProgramPackage.h>

#include <new>
#include <utility>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr ProgramPackageError Translate(KernelProgramPackageError error,
                                                              SemanticHash program = {}) noexcept {
            ProgramPackageErrorCode code = ProgramPackageErrorCode::InvalidProgram;
            if (error.code == KernelProgramPackageErrorCode::CapacityExceeded) {
                code = ProgramPackageErrorCode::CapacityExceeded;
            } else if (error.code == KernelProgramPackageErrorCode::OutOfMemory) {
                code = ProgramPackageErrorCode::OutOfMemory;
            }
            return {
                .code = code, .program = program, .detail = static_cast<uint64_t>(error.code), .offset = error.offset};
        }

    } // namespace

    std::expected<ProgramPackage, ProgramPackageError> ProgramPackage::Create(KernelProgramImage image) noexcept {
        if (!image) {
            return std::unexpected(ProgramPackageError{ProgramPackageErrorCode::InvalidProgram});
        }
        try {
            auto storage = std::make_shared<Storage>();
            storage->hash = image.Hash();
            storage->bytes = std::move(image).ConsumeBytes();
            return ProgramPackage{std::move(storage)};
        } catch (const std::length_error&) {
            return std::unexpected(ProgramPackageError{ProgramPackageErrorCode::CapacityExceeded, image.Hash()});
        } catch (const std::bad_alloc&) {
            return std::unexpected(ProgramPackageError{ProgramPackageErrorCode::OutOfMemory, image.Hash()});
        }
    }

    std::expected<SemanticHash, ProgramPackageError> ProgramPackage::Validate(std::span<const std::byte> bytes,
                                                                              KernelProgramLimits limits) noexcept {
        const SemanticHash encodedHash = Detail::HashBytes(bytes);
        auto image = KernelProgramImage::Decode(bytes, limits);
        if (!image) {
            return std::unexpected(Translate(image.error(), encodedHash));
        }
        return image->Hash();
    }

    std::expected<ProgramPackage, ProgramPackageError> ProgramPackage::Load(std::span<const std::byte> bytes,
                                                                            KernelProgramLimits limits) noexcept {
        const SemanticHash encodedHash = Detail::HashBytes(bytes);
        auto image = KernelProgramImage::Decode(bytes, limits);
        if (!image) {
            return std::unexpected(Translate(image.error(), encodedHash));
        }
        return Create(std::move(*image));
    }

} // namespace Sora::Compute
