/**
 * @file FalseSharing.h
 * @brief Project-wide, reflection-driven compile-time false-sharing audit for concurrent types.
 *
 * One vocabulary, one analyzer, zero runtime cost. Every concurrent data structure tags each of its
 * non-static data members with a *contention domain* — the set of agents that may write it — and a
 * single `consteval` analyzer proves, via C++26 P2996 static reflection, that the physical layout
 * cannot exhibit false sharing. This supersedes the per-class audits previously hand-rolled in
 * `SpscRingBuffer.h`, `SeqLock.h` and `ConcurrentObjectPool.h`.
 *
 * @section model The model: false sharing has exactly two faces
 * - **Internal** — two members written by *distinct concurrent agents* land on one cache line, so
 *   each agent's write invalidates the other's cached line. Audited by @ref AuditFalseSharing:
 *   members of *different active domains* must never overlap a cache line.
 * - **External** — an object does not occupy whole cache lines, so neighbouring instances in an
 *   array straddle a shared boundary line. Audited by @ref OccupiesWholeLines.
 *
 * @section domains Contention domains
 * A *domain* is a type deriving from @ref ContentionDomain, used as a P3394 annotation on a member:
 * `[[=Concurrency::ProducerOwned{}]]`. Domain *identity is type identity*, so the vocabulary is
 * **open** — a new container may declare `struct Whatever : Concurrency::ContentionDomain {}` and the
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

#include "Mashiro/Core/TypeTraits.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <type_traits>

namespace Mashiro::Concurrency {

    // =========================================================================
    // Contention-domain vocabulary (open type set)
    // =========================================================================

    /// @brief Root of the contention-domain hierarchy. Derive to declare a write-ownership role.
    struct ContentionDomain {};

    /// @brief Domains whose members are never written concurrently (read-only after construction),
    ///        hence exempt from internal false-sharing conflict checks.
    struct InertContentionDomain : ContentionDomain {};

    /// @brief A type usable as a member contention-domain annotation.
    template<typename A>
    concept ContentionDomainTag = std::derived_from<A, ContentionDomain> && std::is_empty_v<A>;

    /// @brief Written by a single producer thread only.
    struct ProducerOwned : ContentionDomain {};
    /// @brief Written by a single consumer thread only.
    struct ConsumerOwned : ContentionDomain {};
    /// @brief Hot word hammered by all threads (CAS/RMW); must own its cache line.
    struct Contended : ContentionDomain {};
    /// @brief Bulk storage touched by several agents at disjoint offsets (e.g. a slot array).
    struct SharedStorage : ContentionDomain {};
    /// @brief Read-only after construction; co-locatable with anything (no concurrent write).
    struct Immutable : InertContentionDomain {};

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
        size_t cacheLine = 0;        ///< Cache-line granularity used (`Platform::kCacheLineSize`).
        size_t memberCount = 0;      ///< Non-static data members inspected.
        size_t objectSize = 0;       ///< `sizeof(T)`.
        size_t objectAlign = 0;      ///< `alignof(T)`.
        bool classifiedAll = true;   ///< Every member carries exactly one contention domain.
        bool hasConflict = false;    ///< Two distinct active domains overlap a cache line.
        bool occupiesWholeLines = false; ///< Aligned to, and a whole multiple of, the cache line.
        bool valid = false;          ///< `classifiedAll && !hasConflict && memberCount > 0`.
    };

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Per-member layout fact gathered by the reflective pass.
        struct MemberExtent {
            size_t firstLine = 0;      ///< Index of the first cache line the member occupies.
            size_t lastLine = 0;       ///< Index of the last cache line the member occupies.
            std::meta::info domain{};  ///< Reflection of the member's domain type (null if none).
            bool inert = false;        ///< Domain derives from @ref InertContentionDomain.
            bool classified = false;   ///< Exactly one contention domain was found.
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
        CacheLayoutReport r{};
        const size_t line = Platform::kCacheLineSize;
        r.cacheLine = line;
        r.memberCount = Traits::MembersCount<T>;
        r.objectSize = sizeof(T);
        r.objectAlign = alignof(T);
        r.occupiesWholeLines = (alignof(T) >= line) && (alignof(T) % line == 0) &&
                               (sizeof(T) % line == 0);

        std::array<Detail::MemberExtent, Traits::MembersCount<T>> ext{};
        size_t idx = 0;
        template for (constexpr auto m : Traits::Members<T>) {
            const size_t off = static_cast<size_t>(std::meta::offset_of(m).bytes);
            const size_t sz = std::meta::size_of(std::meta::type_of(m));
            Detail::MemberExtent e{};
            e.firstLine = off / line;
            e.lastLine = (off + (sz ? sz - 1 : 0)) / line;
            size_t domainCount = 0;
            template for (constexpr auto a : std::define_static_array(std::meta::annotations_of(m))) {
                using TA = typename [:std::meta::type_of(a):];
                if constexpr (std::derived_from<TA, ContentionDomain>) {
                    e.domain = std::meta::type_of(a);
                    e.inert = std::derived_from<TA, InertContentionDomain>;
                    ++domainCount;
                }
            }
            e.classified = (domainCount == 1);
            if (!e.classified) {
                r.classifiedAll = false;
            }
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

    /**
     * @brief `consteval`: @p T occupies whole cache lines, so array neighbours never straddle a line.
     *
     * The rigorous external-false-sharing condition: alignment is at least one line *and* the size is
     * an exact multiple of a line (the array stride is then a whole number of lines).
     */
    template<typename T>
    consteval bool OccupiesWholeLines() {
        const size_t line = Platform::kCacheLineSize;
        return (alignof(T) >= line) && (alignof(T) % line == 0) && (sizeof(T) % line == 0);
    }

    /**
     * @brief `consteval`: the lowest-offset member of domain @p D starts a cache line.
     * @tparam D An exact domain type (matched by annotation type, not by derivation).
     * @return true iff at least one @p D-tagged member exists and the first begins a line.
     */
    template<typename T, ContentionDomainTag D>
    consteval bool DomainStartsLine() {
        const size_t line = Platform::kCacheLineSize;
        bool found = false;
        size_t lowest = std::numeric_limits<size_t>::max();
        for (auto m : Traits::Members<T>) {
            if (Traits::Anno::Has<D>(m)) {
                found = true;
                const size_t off = static_cast<size_t>(std::meta::offset_of(m).bytes);
                if (off < lowest) {
                    lowest = off;
                }
            }
        }
        return found && (lowest % line == 0);
    }

    /**
     * @brief `consteval`: number of distinct cache lines spanned by all of domain @p D's members.
     * @tparam D An exact domain type (matched by annotation type, not by derivation).
     * @return The inclusive line span, or 0 if no @p D-tagged member exists. A result of 1 means the
     *         domain's fast path touches a single line (e.g. a seqlock's counter + small payload).
     */
    template<typename T, ContentionDomainTag D>
    consteval size_t DomainLineSpan() {
        const size_t line = Platform::kCacheLineSize;
        bool found = false;
        size_t lo = std::numeric_limits<size_t>::max();
        size_t hi = 0;
        for (auto m : Traits::Members<T>) {
            if (Traits::Anno::Has<D>(m)) {
                found = true;
                const size_t off = static_cast<size_t>(std::meta::offset_of(m).bytes);
                const size_t sz = std::meta::size_of(std::meta::type_of(m));
                const size_t first = off / line;
                const size_t last = (off + (sz ? sz - 1 : 0)) / line;
                if (first < lo) {
                    lo = first;
                }
                if (last > hi) {
                    hi = last;
                }
            }
        }
        return found ? (hi - lo + 1) : 0;
    }

} // namespace Mashiro::Concurrency
