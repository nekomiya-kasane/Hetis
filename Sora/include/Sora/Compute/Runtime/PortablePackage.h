/**
 * @file PortablePackage.h
 * @brief Validated runtime projection of a portable program distribution
 * package.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/PackagedArtifact.h>
#include <Sora/Compute/Runtime/ProgramPackage.h>

#include <expected>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /**
     * @brief Owning canonical program plus optional exact-target AOT artifact
     * seeds.
     * @details This type is the validated runtime projection, not the external
     * signed sectioned wire format.
     */
    class PortablePackage final {
    public:
        PortablePackage() = delete;
        PortablePackage(const PortablePackage&) = delete;
        PortablePackage& operator=(const PortablePackage&) = delete;
        PortablePackage(PortablePackage&&) noexcept = default;
        PortablePackage& operator=(PortablePackage&&) noexcept = default;

        /** @brief Prove program identity and artifact dependency coherence before
         * publication. */
        [[nodiscard]] static std::expected<PortablePackage, PortablePackageError>
        Create(ProgramPackage program, std::vector<PackagedArtifact> artifacts) noexcept;

        /** @brief Return the canonical owning program lease. */
        [[nodiscard]] const ProgramPackage& Program() const noexcept { return program_; }

        /** @brief Return exact-key AOT artifact seeds coherent with Program(). */
        [[nodiscard]] const PackagedArtifactSet& Artifacts() const noexcept { return artifacts_; }

    private:
        PortablePackage(ProgramPackage program, PackagedArtifactSet artifacts) noexcept
            : program_(std::move(program)), artifacts_(std::move(artifacts)) {}

        ProgramPackage program_{};
        PackagedArtifactSet artifacts_;
    };

} // namespace Sora::Compute
