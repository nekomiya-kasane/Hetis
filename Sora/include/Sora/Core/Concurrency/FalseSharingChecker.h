/**
 * @file FalseSharing.h
 * @brief Project-wide, reflection-driven compile-time false-sharing audit for concurrent types.
 *
 * One vocabulary, one analyzer, zero runtime cost. Every concurrent data structure tags each of its
 * non-static data members with a *contention domain* â€” the set of agents that may write it â€” and a
 * single `consteval` analyzer proves, via C++26 P2996 static reflection, that the physical layout
 * cannot exhibit false sharing. This supersedes the per-class audits previously hand-rolled in
 * `SpscRingBuffer.h`, `SeqLock.h` and `ConcurrentObjectPool.h`.
 *
 * @section model The model: false sharing has exactly two faces
 * - **Internal** â€” two members written by *distinct concurrent agents* land on one cache line, so
 *   each agent's write invalidates the other's cached line. Audited by @ref AuditFalseSharing:
 *   members of *different active domains* must never overlap a cache line.
 * - **External** â€” an object does not occupy whole cache lines, so neighbouring instances in an
 *   array straddle a shared boundary line. Audited by @ref OccupiesWholeLines.
 *
 * @section domains Contention domains
 * A *domain* is a type deriving from @ref ContentionDomain, used as a P3394 annotation on a member:
 * `[[=Concurrency::ProducerOwned{}]]`. Domain *identity is type identity*, so the vocabulary is
 * **open** â€” a new container may declare `struct Whatever : Concurrency::ContentionDomain {}` and the
 * analyzer handles it with no central change. Two members in the *same* domain may freely share a
 * line (one writer, no conflict). A domain deriving from @ref InertContentionDomain (e.g.
 * @ref Immutable) is never written concurrently, so it is exempt from internal-conflict checks: the
 * audit models *write-write* false sharing between distinct writers, which is the only kind a static
 * layout check can soundly prove.
 *
 * @section cost Cost
 * Domain tags are empty types (zero size, zero runtime). `alignas` still creates the physical layout;
 * this header only *verifies* it. Every entry point is `consteval`.
 *
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Core/Traits/InheritanceTraits.h"
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Core/Traits/AnnotationTraits.h>
#include <Sora/Core/Concurrency/ContentionDomain.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <meta>
#include <ranges>
#include <type_traits>

namespace Sora {

    namespace Concurrency {

        namespace Meta {

            // =========================================================================
            // Contention-domain vocabulary (open type set)
            // =========================================================================

            // =========================================================================
            // Layout report
            // =========================================================================

            /**
             * @brief Compile-time description of a concurrent type's cache-line layout.
             *
             * Produced by @ref AnalyzeCacheLayout and surfaced via a container's `Layout()` so callers can
             * `static_assert` specific properties. `valid` is the headline internal-false-sharing verdict.
             */
            struct CacheLayoutReport {
                size_t cacheLine = 0;            ///< Cache-line granularity used (`Platform::kCacheLineSize`).
                size_t memberCount = 0;          ///< Non-static data members inspected.
                size_t objectSize = 0;           ///< `sizeof(T)`.
                size_t objectAlign = 0;          ///< `alignof(T)`.
                bool classifiedAll = true;       ///< Every member carries exactly one contention domain.
                bool hasConflict = false;        ///< Two distinct active domains overlap a cache line.
                bool occupiesWholeLines = false; ///< Aligned to, and a whole multiple of, the cache line.
                bool valid = false;              ///< `classifiedAll && !hasConflict && memberCount > 0`.
            };

            /** @cond INTERNAL */
            namespace Detail {

                /// @brief Per-member layout fact gathered by the reflective pass.
                struct MemberExtent {
                    size_t firstLine = 0;     ///< Index of the first cache line the member occupies.
                    size_t lastLine = 0;      ///< Index of the last cache line the member occupies.
                    std::meta::info domain{}; ///< Reflection of the member's domain type (null if none).
                    bool inert = false;       ///< Domain derives from @ref InertContentionDomain.
                    bool classified = false;  ///< Exactly one contention domain was found.
                };

            } // namespace Detail
            /** @endcond */

            // =========================================================================
            // Core analyzer
            // =========================================================================

            /**
             * @brief Reflect over @p T's data members and compute its @ref CacheLayoutReport.
             *
             * @tparam T Any complete, reflectable class (typically a concurrent container).
             */
            template<typename T>
            consteval CacheLayoutReport AnalyzeCacheLayout() {
                const size_t line = Platform::kCacheLineSize;

                CacheLayoutReport r{
                    .cacheLine = line,
                    .memberCount = Sora::Traits::DataMembersCount<T>,
                    .objectSize = sizeof(T),
                    .objectAlign = alignof(T),
                    .classifiedAll = true,
                    .hasConflict = false,
                    .occupiesWholeLines = (alignof(T) >= line) && (alignof(T) % line == 0) && (sizeof(T) % line == 0),
                    .valid = false,
                };

                std::array<Detail::MemberExtent, Traits::DataMembersCount<T>> ext{};
                size_t idx = 0;
                template for (constexpr auto m : Traits::DataMembers<T>) {
                    const size_t offset = static_cast<size_t>(std::meta::offset_of(m).bytes);
                    const size_t size = std::meta::size_of(std::meta::type_of(m));

                    Detail::MemberExtent e{
                        .firstLine = offset / line,
                        .lastLine = (offset + (size ? size - 1 : 0)) / line,
                        .domain = {},
                        .inert = {},
                        .classified = false,
                    };

                    size_t domainCount = 0;
                    template for (constexpr auto annotation : Sora::Meta::AnnotationsOf(m)) {
                        constexpr auto type = std::meta::type_of(annotation);
                        if constexpr (std::meta::is_class_type(type) &&
                                      Sora::Meta::DerivedFrom(type, ^^Sora::Concurrency::$::ContentionDomain)) {
                            if (++domainCount > 1) {
                                throw std::define_static_string(
                                    "A data member cannot have more than one contention-domain annotation.");
                            }
                            e.domain = type;
                            e.inert = Sora::Meta::DerivedFrom(type, ^^Sora::Concurrency::$::InertContentionDomain);
                            e.classified = true;
                        }
                    }

                    r.classifiedAll &= e.classified;
                    ext[idx++] = e;
                }

                // Internal false sharing: distinct *active* domains may not overlap a cache line.
                for (size_t i = 0; i < idx; ++i) {
                    for (size_t j = i + 1; j < idx; ++j) {
                        if (!ext[i].classified || !ext[j].classified) {
                            continue; // already flagged by classifiedAll
                        }
                        if (ext[i].inert || ext[j].inert) {
                            continue; // inert data is never concurrently written
                        }
                        if (ext[i].domain == ext[j].domain) {
                            continue; // same writer: sharing a line is fine
                        }
                        const bool disjoint = ext[i].lastLine < ext[j].firstLine || ext[j].lastLine < ext[i].firstLine;
                        if (!disjoint) {
                            r.hasConflict = true;
                        }
                    }
                }

                r.valid = r.classifiedAll && !r.hasConflict && r.memberCount > 0;
                return r;
            }

            // =========================================================================
            // Audit predicates and targeted layout queries
            // =========================================================================

            /**
             * @brief `consteval` verdict: @p T is provably free of internal false sharing.
             * @return true iff every member is classified and no two distinct active domains share a line.
             */
            template<typename T>
            consteval bool AuditFalseSharing() {
                return AnalyzeCacheLayout<T>().valid;
            }

        } // namespace Meta

        namespace Traits {

            /**
             * @brief `consteval`: @p T occupies whole cache lines, so array neighbours never straddle a line.
             *
             * The rigorous external-false-sharing condition: alignment is at least one line *and* the size is
             * an exact multiple of a line (the array stride is then a whole number of lines).
             */
            template<typename T>
            inline constexpr bool IsOccupyingWholeLines = [] {
                const size_t line = Platform::kCacheLineSize;
                return (alignof(T) >= line) && (alignof(T) % line == 0) && (sizeof(T) % line == 0);
            };

            /**
             * @brief `consteval`: the lowest-offset member of domain @p D starts a cache line.
             * @tparam D An exact domain type (matched by annotation type, not by derivation).
             * @return true iff at least one @p D-tagged member exists and the first begins a line.
             */
            template<typename T, Concept::ContentionDomainTag D>
            constexpr bool IsDomainStartsAlignedToCacheLine = [] {
                bool found = false;
                size_t lowest = std::numeric_limits<size_t>::max();
                for (auto m : Sora::Traits::DataMembers<T>) {
                    if (!Sora::$::Has<D>(m)) {
                        continue;
                    }

                    found = true;
                    lowest = std::min(static_cast<size_t>(std::meta::offset_of(m).bytes), lowest);
                }
                return found && (lowest % Platform::kCacheLineSize == 0);
            }();

            /**
             * @brief `consteval`: number of distinct cache lines spanned by all of domain @p D's members.
             * @tparam D An exact domain type (matched by annotation type, not by derivation).
             * @return The inclusive line span, or 0 if no @p D-tagged member exists. A result of 1 means the
             *         domain's fast path touches a single line (e.g. a seqlock's counter + small payload).
             */
            template<typename T, Concept::ContentionDomainTag D>
            constexpr size_t DomainLineSpanOf = [] {
                const size_t line = Platform::kCacheLineSize;
                bool found = false;
                size_t lo = std::numeric_limits<size_t>::max();
                size_t hi = 0;
                for (auto m : Sora::Traits::DataMembers<T>) {
                    if (!Sora::$::Has<D>(m)) {
                        continue;
                    }

                    found = true;
                    const size_t offset = static_cast<size_t>(std::meta::offset_of(m).bytes);
                    const size_t size = std::meta::size_of(std::meta::type_of(m));
                    const size_t first = offset / line, last = (offset + (size ? size - 1 : 0)) / line;
                    lo = std::min(lo, first);
                    hi = std::max(hi, last);
                }
                return found ? (hi - lo + 1) : 0;
            }();

        } // namespace Traits

    } // namespace Concurrency

    namespace Meta {

        inline namespace Concurrency {

            using Sora::Concurrency::Meta::AnalyzeCacheLayout;
            using Sora::Concurrency::Meta::AuditFalseSharing;
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
