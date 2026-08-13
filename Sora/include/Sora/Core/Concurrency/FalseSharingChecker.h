/**
 * @file FalseSharingChecker.h
 * @brief Reflection-driven compile-time false-sharing audit for explicitly classified concurrent layouts.
 *
 * Every direct non-static data member is annotated with a contention domain describing its concurrent access policy.
 *
 * Written class, union, and array members additionally require an explicit region-coverage annotation: either the
 *
 * complete member is one cohesive protocol region, or its type independently guarantees internal isolation.
 * The
 * analyzer checks all cache-line placements permitted by the object's alignment, so an internal verdict remains valid
 * even when the object itself is not cache-line aligned. A complete audit additionally requires whole-line object
 * occupancy to isolate adjacent instances.
 *
 * @section model Audited layout boundaries
 * - **Internal**: two conflicting access regions, at least one of which is
 * written concurrently, land on one cache line. Audited by @ref AuditInternalFalseSharing.
 * - **External**: an object does not occupy whole cache lines, so neighbouring instances in an array straddle a shared
 *   boundary line. Audited by @ref AuditExternalFalseSharing.
 * - **Coverage**: active composite members do not hide
 * unmodeled subregions. Pointer targets and other external storage
 *   are outside the represented object's audit
 * boundary.
 *
 * @section domains Contention domains
 * A domain derives from exactly one access-policy root. Same-domain sharing is allowed only for explicitly cohesive
 *
 * single-writer or multi-writer protocols. Generic writer domains remain isolated even when their annotation types
 *
 * match. Read-only members may share with each other but not with written state, because write-read traffic also
 * causes
 * coherence invalidations. An @ref InactiveContentionDomain is exempt because it denotes no concurrent memory
 * access.
 *
 * @section cost Cost
 * Domain tags are empty types (zero size, zero runtime). `alignas` still creates the physical layout; this header only
 * verifies it. Every entry point is `consteval`.
 *
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/ContentionDomain.h>
#include <Sora/Core/Traits/AnnotationTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cstddef>
#include <concepts>
#include <limits>
#include <meta>
#include <numeric>
#include <type_traits>

namespace Sora {

    namespace Concurrency {

        namespace Meta {

            // =========================================================================
            // Layout report
            // =========================================================================

            /**
             * @brief Compile-time description of a concurrent type's cache-line layout.
             * Produced by @ref AnalyzeCacheLayout and surfaced via a container's `Layout()` for property assertions.
             * `valid` is the complete internal-and-external verdict.
             */
            struct CacheLayoutReport {
                size_t cacheLine = 0;             ///< Cache-line granularity used (`Platform::kCacheLineSize`).
                size_t memberCount = 0;           ///< Non-static data members inspected.
                size_t objectSize = 0;            ///< `sizeof(T)`.
                size_t objectAlign = 0;           ///< `alignof(T)`.
                bool layoutSupported = false;     ///< Type is a non-polymorphic class without base subobjects.
                bool classifiedAll = true;        ///< Every direct member carries exactly one contention domain.
                bool coversAllRegions = true;     ///< Every active composite member has one region-coverage proof.
                bool hasInternalConflict = false; ///< Conflicting access regions may occupy one cache line.
                bool occupiesWholeLines = false;  ///< Aligned to, and a whole multiple of, the cache line.
                bool directMembersValid = false;  ///< Classified direct members are mutually isolated.
                bool internalValid = false;       ///< Direct isolation and composite-region coverage both hold.
                bool externalValid = false;       ///< Adjacent complete objects cannot share a cache line.
                bool valid = false;               ///< Both the internal and external audits pass.
            };

            /** @cond INTERNAL */
            namespace Detail {

                /** @brief Per-member layout fact gathered by the reflective pass. */
                struct MemberExtent {
                    size_t firstBit = 0;               ///< First bit of the member relative to the complete object.
                    size_t bitCount = 0;               ///< Number of representation bits occupied by the member.
                    std::meta::info domain{};          ///< Reflection of the member's domain type.
                    bool readOnly = false;             ///< Domain never writes during concurrent operation.
                    bool mayShareWithinDomain = false; ///< Same-domain regions are controlled by one cohesive protocol.
                    bool inactive = false;             ///< Region is not accessed during concurrent operation.
                    bool classified = false;           ///< Exactly one valid access-policy domain was found.
                    bool coverageProven = false;       ///< Scalar leaf or explicitly covered composite region.
                };

                [[nodiscard]] consteval bool SharesCacheLineForSomePlacement(const MemberExtent& lhs,
                                                                             const MemberExtent& rhs,
                                                                             size_t objectAlign, size_t cacheLine) {
                    if (lhs.bitCount == 0 || rhs.bitCount == 0) {
                        return false;
                    }

                    const size_t lineBits = cacheLine * CHAR_BIT;
                    const size_t placementStep = std::gcd(objectAlign, cacheLine);
                    for (size_t placement = 0; placement < cacheLine; placement += placementStep) {
                        const size_t placementBits = placement * CHAR_BIT;
                        const size_t lhsFirst = (placementBits + lhs.firstBit) / lineBits;
                        const size_t lhsLast = (placementBits + lhs.firstBit + lhs.bitCount - 1) / lineBits;
                        const size_t rhsFirst = (placementBits + rhs.firstBit) / lineBits;
                        const size_t rhsLast = (placementBits + rhs.firstBit + rhs.bitCount - 1) / lineBits;
                        if (!(lhsLast < rhsFirst || rhsLast < lhsFirst)) {
                            return true;
                        }
                    }
                    return false;
                }

                [[nodiscard]] consteval bool AccessesConflict(const MemberExtent& lhs, const MemberExtent& rhs) {
                    if (lhs.inactive || rhs.inactive || (lhs.readOnly && rhs.readOnly)) {
                        return false;
                    }
                    return lhs.domain != rhs.domain || !lhs.mayShareWithinDomain || !rhs.mayShareWithinDomain;
                }

            } // namespace Detail
            /** @endcond */

            // =========================================================================
            // Core analyzer
            // =========================================================================

            /**
             * @brief Reflect over @p T's data members and compute its @ref CacheLayoutReport.
             * @tparam T Any complete, reflectable class (typically a concurrent container).
             */
            template<typename T>
                requires std::is_class_v<T>
            consteval CacheLayoutReport AnalyzeCacheLayout() {
                const size_t line = Platform::kCacheLineSize;
                static_assert(std::has_single_bit(Platform::kCacheLineSize),
                              "Platform::kCacheLineSize must be a non-zero power of two");

                constexpr bool layoutSupported = [] consteval {
                    if constexpr (std::is_polymorphic_v<T>) {
                        return false;
                    } else {
                        return std::meta::bases_of(^^T, std::meta::access_context::unchecked()).empty();
                    }
                }();

                CacheLayoutReport r{
                    .cacheLine = line,
                    .memberCount = Sora::Traits::DataMembersCount<T>,
                    .objectSize = sizeof(T),
                    .objectAlign = alignof(T),
                    .layoutSupported = layoutSupported,
                    .classifiedAll = true,
                    .coversAllRegions = true,
                    .hasInternalConflict = false,
                    .occupiesWholeLines = (alignof(T) >= line) && (alignof(T) % line == 0) && (sizeof(T) % line == 0),
                    .directMembersValid = false,
                    .internalValid = false,
                    .externalValid = false,
                    .valid = false,
                };

                std::array<Detail::MemberExtent, Traits::DataMembersCount<T>> ext{};
                size_t idx = 0;
                template for (constexpr auto m : Traits::DataMembers<T>) {
                    const auto offset = std::meta::offset_of(m).total_bits();
                    if (offset < 0) {
                        throw std::define_static_string("False-sharing audit received a negative member offset.");
                    }

                    Detail::MemberExtent e{
                        .firstBit = static_cast<size_t>(offset),
                        .bitCount = std::meta::bit_size_of(m),
                        .domain = {},
                        .readOnly = false,
                        .mayShareWithinDomain = false,
                        .inactive = false,
                        .classified = false,
                        .coverageProven = false,
                    };

                    size_t domainCount = 0;
                    size_t coverageCount = 0;
                    template for (constexpr auto annotation : Sora::Meta::AnnotationsOf(m)) {
                        constexpr auto type =
                            std::meta::dealias(std::meta::remove_cvref(std::meta::type_of(annotation)));
                        if constexpr (std::meta::is_class_type(type)) {
                            using Annotation = [:type:];
                            if constexpr (std::derived_from<Annotation, Sora::Concurrency::$::ContentionDomain>) {
                                if constexpr (!Concept::ContentionDomainTag<Annotation>) {
                                    throw std::define_static_string(
                                        "A contention domain must be empty and derive from exactly one access-policy "
                                        "root.");
                                }
                                if (++domainCount > 1) {
                                    throw std::define_static_string(
                                        "A data member cannot have more than one contention-domain annotation.");
                                }
                                e.domain = type;
                                e.readOnly = std::derived_from<Annotation, $::ReadOnlyContentionDomain>;
                                const bool cohesiveSingleWriter =
                                    std::derived_from<Annotation, $::CohesiveSingleWriterContentionDomain>;
                                const bool cohesiveMultiWriter =
                                    std::derived_from<Annotation, $::CohesiveMultiWriterContentionDomain>;
                                e.mayShareWithinDomain = cohesiveSingleWriter || cohesiveMultiWriter;
                                e.inactive = std::derived_from<Annotation, $::InactiveContentionDomain>;
                                e.classified = true;
                            } else if constexpr (std::derived_from<Annotation, $::ContentionRegionCoverage>) {
                                if constexpr (!Concept::ContentionRegionCoverageTag<Annotation>) {
                                    throw std::define_static_string("A contention-region coverage tag must be empty.");
                                }
                                if (++coverageCount > 1) {
                                    throw std::define_static_string(
                                        "A data member cannot have more than one contention-region coverage "
                                        "annotation.");
                                }
                                e.coverageProven = true;
                            }
                        }
                    }

                    const auto memberType = std::meta::dealias(std::meta::remove_cvref(std::meta::type_of(m)));
                    e.coverageProven |= e.inactive || e.readOnly || std::meta::is_scalar_type(memberType) ||
                                        std::meta::is_enum_type(memberType);
                    r.classifiedAll &= e.classified;
                    r.coversAllRegions &= e.coverageProven;
                    ext[idx++] = e;
                }

                // Test every base-address residue permitted by alignof(T), not only a cache-line-aligned placement.
                for (size_t i = 0; i < idx; ++i) {
                    for (size_t j = i + 1; j < idx; ++j) {
                        if (!ext[i].classified || !ext[j].classified) {
                            continue; // already flagged by classifiedAll
                        }
                        if (Detail::AccessesConflict(ext[i], ext[j]) &&
                            Detail::SharesCacheLineForSomePlacement(ext[i], ext[j], alignof(T), line)) {
                            r.hasInternalConflict = true;
                        }
                    }
                }

                r.directMembersValid =
                    r.layoutSupported && r.classifiedAll && !r.hasInternalConflict && r.memberCount > 0;
                r.internalValid = r.directMembersValid && r.coversAllRegions;
                r.externalValid = r.occupiesWholeLines;
                r.valid = r.internalValid && r.externalValid;
                return r;
            }

            // =========================================================================
            // Audit predicates and targeted layout queries
            // =========================================================================

            /** @brief Return whether @p T's direct members are classified and mutually isolated. */
            template<typename T>
                requires std::is_class_v<T>
            consteval bool AuditDirectMemberFalseSharing() {
                return AnalyzeCacheLayout<T>().directMembersValid;
            }

            /** @brief Return whether direct-member isolation and all active composite-region proofs hold for @p T. */
            template<typename T>
                requires std::is_class_v<T>
            consteval bool AuditInternalFalseSharing() {
                return AnalyzeCacheLayout<T>().internalValid;
            }

            /** @brief Return whether adjacent complete @p T objects cannot share a cache line. */
            template<typename T>
                requires std::is_class_v<T>
            consteval bool AuditExternalFalseSharing() {
                return AnalyzeCacheLayout<T>().externalValid;
            }

            /** @brief Return whether both internal member isolation and external object isolation are proven. */
            template<typename T>
                requires std::is_class_v<T>
            consteval bool AuditFalseSharing() {
                return AnalyzeCacheLayout<T>().valid;
            }

        } // namespace Meta

        namespace Traits {

            /**
             * @brief Return whether @p T occupies whole cache lines.
             *
             * This requires line alignment and a size that is an exact multiple of the line size. Consequently, array
             * neighbours cannot straddle a shared cache line.
             */
            template<typename T>
            inline constexpr bool IsOccupyingWholeLines = [] {
                const size_t line = Platform::kCacheLineSize;
                return (alignof(T) >= line) && (alignof(T) % line == 0) && (sizeof(T) % line == 0);
            };

            /**
             * @brief Return whether the lowest-offset member of domain @p D always starts a cache line.
             *
             * @tparam D An exact domain type (matched by annotation type, not by derivation).
             * @return
             * true iff @p T guarantees a cache-line-aligned base and the first member begins a line.
             */
            template<typename T, Concept::ContentionDomainTag D>
            constexpr bool IsDomainStartsAlignedToCacheLine = [] {
                bool found = false;
                size_t lowestBit = std::numeric_limits<size_t>::max();
                for (auto m : Sora::Traits::DataMembers<T>) {
                    if (!Sora::$::Has<D>(m)) {
                        continue;
                    }

                    const auto offset = std::meta::offset_of(m).total_bits();
                    if (offset < 0) {
                        throw std::define_static_string("False-sharing query received a negative member offset.");
                    }
                    found = true;
                    lowestBit = std::min(static_cast<size_t>(offset), lowestBit);
                }
                const size_t line = Platform::kCacheLineSize;
                return found && alignof(T) >= line && alignof(T) % line == 0 && lowestBit % (line * CHAR_BIT) == 0;
            }();

            /**
             * @brief Return the maximum cache-line span of domain @p D over every permitted object placement.
             * @tparam D Exact domain type, matched by annotation identity rather than derivation.
             * @return Maximum inclusive span, or 0 when no @p D-tagged member occupies representation bits.
             */
            template<typename T, Concept::ContentionDomainTag D>
            constexpr size_t DomainLineSpanOf = [] {
                const size_t line = Platform::kCacheLineSize;
                const size_t lineBits = line * CHAR_BIT;
                bool found = false;
                size_t firstBit = std::numeric_limits<size_t>::max();
                size_t lastBit = 0;
                for (auto m : Sora::Traits::DataMembers<T>) {
                    if (!Sora::$::Has<D>(m)) {
                        continue;
                    }

                    const auto offset = std::meta::offset_of(m).total_bits();
                    if (offset < 0) {
                        throw std::define_static_string("False-sharing query received a negative member offset.");
                    }
                    const size_t bitCount = std::meta::bit_size_of(m);
                    if (bitCount == 0) {
                        continue;
                    }
                    found = true;
                    firstBit = std::min(firstBit, static_cast<size_t>(offset));
                    lastBit = std::max(lastBit, static_cast<size_t>(offset) + bitCount - 1);
                }
                if (!found) {
                    return size_t{0};
                }

                size_t maximumSpan = 0;
                const size_t placementStep = std::gcd(alignof(T), line);
                for (size_t placement = 0; placement < line; placement += placementStep) {
                    const size_t placementBits = placement * CHAR_BIT;
                    const size_t firstLine = (placementBits + firstBit) / lineBits;
                    const size_t lastLine = (placementBits + lastBit) / lineBits;
                    maximumSpan = std::max(maximumSpan, lastLine - firstLine + 1);
                }
                return maximumSpan;
            }();

        } // namespace Traits

    } // namespace Concurrency

    namespace Meta {

        inline namespace Concurrency {

            using Sora::Concurrency::Meta::AnalyzeCacheLayout;
            using Sora::Concurrency::Meta::AuditDirectMemberFalseSharing;
            using Sora::Concurrency::Meta::AuditExternalFalseSharing;
            using Sora::Concurrency::Meta::AuditFalseSharing;
            using Sora::Concurrency::Meta::AuditInternalFalseSharing;
            using Sora::Concurrency::Meta::CacheLayoutReport;

        } // namespace Concurrency

    } // namespace Meta

    namespace Traits {

        inline namespace Concurrency {

            using Sora::Concurrency::Traits::DomainLineSpanOf;
            using Sora::Concurrency::Traits::IsDomainStartsAlignedToCacheLine;
            using Sora::Concurrency::Traits::IsOccupyingWholeLines;

        } // namespace Concurrency

    } // namespace Traits

} // namespace Sora
