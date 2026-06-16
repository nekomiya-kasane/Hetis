/**
 * @file MetaClass.h
 * @brief Three-layer compile-time metaclass for the Yuki object model.
 *
 * Each object-model class gets one @ref MetaClass — a stable shell stitching three physically
 * separate layers:
 *   - @ref MetaCore    — the immutable core, baked into `.rodata` by reflection at compile time
 *                        (role, qualified name, IID, `extends` / `implements` / `omBase` edges).
 *                        Zero runtime construction cost.
 *   - @ref MetaLinks   — the structural tail: back-references and the dispatch table. Written once
 *                        at load time (single-threaded fold-in), read-only on every hot path.
 *   - @ref MetaDynamic — the cold tail: a lock-protected property bag and runtime statistics.
 *                        Lazily allocated; touched only by diagnostics / scripting, never hot paths.
 *
 * Two access surfaces share the single source of truth @ref MetaCore — no synonyms, no tautology:
 *   - **Static** (type known, folds at compile time): @ref ClassTypeOf, @ref IidOf, @ref NameOf,
 *     @ref BaseMetaOf — read "of *this type*".
 *   - **Dynamic** (type erased through a `RootObject*`, one vcall): @ref MetaClass::classType,
 *     @ref MetaClass::iid, @ref MetaClass::name, @ref MetaClass::baseMeta — read "of *this metaobject*".
 *
 * The compile-time pipeline (@ref Detail::BuildMetaCore) is fully automatic: it reads @ref Anno::Meta,
 * splices the `extends` / `implements` reflections back into `&MetaCoreOf<...>` pointers, computes the
 * IID (annotation override, else @ref StableHash), and resolves @c omBase to the nearest object-model
 * ancestor. No hand-written registration.
 *
 * @ingroup Core
 */
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <meta>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Mashiro/Core/Hash.h>
#include <Mashiro/Core/Polymorphism.h>

#include <Yuki/Core/Identity.h>

namespace Yuki {

    // Targeted re-exports — see Identity.h. Pull in only what this header names.
    using Mashiro::Uuid;
    using Mashiro::uint128_t;
    namespace Hashing = Mashiro::Hashing;
    namespace Polymorphism = Mashiro::Polymorphism;

    // =========================================================================
    // Iid — 128-bit interface/identity name; also an annotation carrier
    // =========================================================================

    /**
     * @brief A 128-bit object-model identity, wrapping an RFC-4122 @ref Mashiro::Uuid.
     *
     * Doubles as the annotation that overrides a class's computed identity:
     * `[[=Iid("3f2504e0-4f89-41d3-9a0c-0305e82c3301")]] struct IShape { ... };`. A malformed
     * literal makes @ref Mashiro::Uuid::ParseOrThrow throw inside the `consteval` constructor,
     * which the compiler turns into a diagnostic pinpointing the bad literal.
     */
    struct Iid {
        Uuid value{};

        constexpr Iid() noexcept = default;
        constexpr Iid(Uuid u) noexcept : value(u) {}
        constexpr Iid(uint128_t u) noexcept : value(Uuid::FromUint128(u)) {}
        consteval Iid(std::string_view s) : value(Uuid::ParseOrThrow(s)) {}
        consteval Iid(const char* s) : value(Uuid::ParseOrThrow(std::string_view{s})) {}

        constexpr bool operator==(const Iid&) const noexcept = default;
        constexpr auto operator<=>(const Iid&) const noexcept = default;
    };

    // =========================================================================
    // IID hasher — StableHash keyed on name + Polymorphism vtable ABI digest
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Feed a byte into a 128-bit FNV state (constexpr-safe, no reinterpret_cast).
        constexpr void FeedByte(Hashing::Fnv1a128State& h, uint8_t byte) noexcept {
            std::byte b = static_cast<std::byte>(byte);
            h.Feed(std::span<const std::byte>(&b, 1));
        }

        /**
         * @brief Compute a stable 128-bit identity for @p I from its name and vtable ABI shape.
         *
         * Folds the qualified name (uniqueness) and @ref Mashiro::Polymorphism::AbiDigest
         * (ABI sensitivity — the digest shifts whenever any selected method's signature changes)
         * into one FNV-1a128 digest, then stamps the RFC-4122 version/variant bits. A signature
         * change flips the IID, so a stale client that links the wrong contract is exposed.
         */
        template<typename I>
        consteval Uuid ComputeStableUuid() {
            Hashing::Fnv1a128State h;
            for (char c : std::meta::display_string_of(^^I)) {
                FeedByte(h, static_cast<uint8_t>(c));
            }
            FeedByte(h, 0u);  // separator: name | abi
            uint128_t abi = Mashiro::Polymorphism::AbiDigest<I>();
            for (int i = 0; i < 16; ++i) {
                FeedByte(h, static_cast<uint8_t>(abi >> (i * 8)));
            }
            return Uuid{h.Finalize()}.WithRfc4122();
        }

    } // namespace Detail
    /** @endcond */

    /// @brief The computed, ABI-sensitive identity of @p I (used when no `Iid` annotation overrides).
    template<typename I>
    inline constexpr Iid StableHash = Iid{ Detail::ComputeStableUuid<I>() };

    /**
     * @brief The identity of @p I — the `[[=Iid("…")]]` annotation if present, else @ref StableHash.
     *
     * A single switch (annotation present?), not a dual track: every class has exactly one identity,
     * sourced from one place. This is the *static* form, where the type is known at the call site.
     *
     * Spelled as a `consteval` function template (rather than a variable template) so it can share
     * the unqualified name @c IidOf with the dynamic-side overload below — the two surfaces are
     * peers, taking either a type (statically) or a @ref MetaCore (dynamically) and answering the
     * same identity question.
     *
     * @note Call sites that previously read @c IidOf<I> as a value must now write @c IidOf<I>()
     *       (one paren-pair); the value is still folded at compile time.
     */
    template<typename I>
    [[nodiscard]] consteval Iid IidOf() noexcept {
        constexpr bool overridden = [] consteval {
            return !std::meta::annotations_of(^^I, ^^Iid).empty();
        }();
        if constexpr (overridden) {
            return [] consteval {
                return std::meta::extract<Iid>(std::meta::annotations_of(^^I, ^^Iid)[0]);
            }();
        } else {
            return StableHash<I>;
        }
    }

    // =========================================================================
    // Layer ① MetaCore — the immutable core (inline constexpr, .rodata)
    // =========================================================================

    /**
     * @brief The compile-time-baked, immutable core of a metaclass.
     *
     * A literal type with static storage duration (see @ref MetaCoreOf). `extends` / `implements`
     * hold stable pointers to the cores of the object-model types this class extends / implements;
     * `omBase` points to the nearest object-model ancestor's core, or `nullptr` at a root.
     */
    struct MetaCore {
        ClassType type{ClassType::None};                  ///< Object-model role.
        std::string_view qualifiedName{};                 ///< `display_string_of(^^T)`, static pool.
        Iid iid{};                                        ///< Annotation override, else StableHash.
        std::span<const MetaCore* const> extends{};       ///< Resolved `Anno::Meta.extends` cores.
        std::span<const MetaCore* const> implements{};    ///< Resolved `Anno::Meta.implements` cores.
        const MetaCore* omBase{nullptr};                  ///< Nearest OM ancestor core, or nullptr.
    };

    /**
     * @brief Runtime form of @ref IidOf: read the IID off an already-built @ref MetaCore.
     *
     * Used on the dynamic dispatch path where the type-erased side of the system has a @ref MetaCore
     * (or @c MetaClass) in hand and just needs the identity it carries. The body is a single field
     * read — there is no separate registry, no hashing — so the two surfaces share a single source
     * of truth: the function template asks reflection for the IID *of @p I*, and this overload
     * reads it back through the same @ref MetaCore the pipeline baked it into.
     */
    [[nodiscard]] inline Iid IidOf(const MetaCore& core) noexcept { return core.iid; }

    // =========================================================================
    // Layer ② MetaLinks — the structural tail (load-time write, hot-path read)
    // =========================================================================

    // Forward decl: payload arms point at RootObject identity (singleton facade, resolver result).
    class RootObject;

    /**
     * @brief How a @ref DispatchEntry reaches the requested interface from a host pointer.
     *
     * The runtime contract of the QueryInterface fast path. Every interface offered by an impl
     * lands in exactly one bucket — and which bucket is decided at compile time, fully visible to
     * the optimiser. The `kind` field on @ref DispatchEntry selects which arm of @ref
     * DispatchEntry::Payload the entry carries; the static-face @ref Query folds the bucket away
     * and emits the matching pointer adjustment directly, while the dynamic face reads @c kind to
     * pick the matching @c case in its dispatch switch.
     *
     * @c DirectCast covers BOA — the impl C++-inherits the interface, so @c Query<I> is a fold to
     * `static_cast<I*>(p)` and the compiler emits the layout-required offset. @c InlineFacade
     * covers the Hot facade subobjects aggregated inside the impl (offset folded at compile time).
     * @c CodeExtensionSingleton covers stateless code-extensions (Anno::Extension with no NSDM):
     * a single facade lives in `.rodata` and is shared across instances, reached via the
     * @c singleton pointer-to-pointer. @c SideTableResolver covers DataExtensions whose storage
     * hangs off a per-instance side table; the registrar emits a small @c resolver function that
     * @c AttachUnique-installs (or finds) the per-closure facade and returns it. @c FacadeList
     * covers Cold/runtime-attached interfaces walked through @c RootObject's facade chain — these
     * entries exist for introspection only and carry no payload arm.
     */
    enum class DispatchKind : uint8_t {
        DirectCast,             ///< BOA: `static_cast<I*>(impl)` with C++-emitted offset.
        InlineFacade,           ///< Hot inline-aggregated facade subobject (offset known statically).
        CodeExtensionSingleton, ///< Stateless code-extension; shared facade in `.rodata`.
        SideTableResolver,      ///< DataExtension storage materialised by an emitted resolver fn.
        FacadeList,             ///< Cold/runtime-attached interface; walks the @c FacadeList chain.
    };
    static_assert(sizeof(DispatchKind) == 1);

    /**
     * @brief One row of an immutable @ref DispatchSnapshot — an interface IID with the recipe for
     *        reaching it from a host pointer.
     *
     * The active payload arm is selected by @c kind. @c DirectCast and @c InlineFacade store a
     * static byte offset folded by the metaclass pipeline; @c CodeExtensionSingleton stores a
     * pointer-to-pointer-to-singleton (the indirection lets a stateless extension's facade live in
     * `.rodata` and still be addressed uniformly); @c SideTableResolver stores a small emitted
     * function that materialises (and caches) the per-closure facade. @c FacadeList entries are
     * informational only — they're walked through @c RootObject's chain, not through this table —
     * so any payload arm is fine and unread for that kind.
     *
     * Entries within a snapshot are sorted by @c iid, which is what @ref Detail::LookupEntry's
     * binary search depends on. The registrar is the only writer of snapshots; once published,
     * the entry array is immutable for the lifetime of that snapshot.
     */
    struct DispatchEntry {
        Iid          iid{};                                ///< The interface's identity (sort key).
        DispatchKind kind{DispatchKind::DirectCast};       ///< Which arm of @c payload is live.
        union Payload {
            std::ptrdiff_t       staticOffset;             ///< DirectCast / InlineFacade byte offset.
            RootObject* const*   singleton;                ///< CodeExtensionSingleton facade in .rodata.
            RootObject* (*resolver)(RootObject* nucleus) noexcept;  ///< SideTableResolver materialiser.
        } payload{.staticOffset = 0};                      ///< Default-init the staticOffset arm.
    };

    /**
     * @brief An immutable, atomically-published view of one metaclass's dispatch table.
     *
     * Per spec §2.1 of the Yuki Closure Architecture. The snapshot is the unit of publish: a
     * registrar builds a fresh, sorted-by-iid entry array, wraps it in a snapshot, and CAS-installs
     * it onto @ref MetaLinks::dispatch with release semantics. Readers acquire-load the pointer and
     * treat the snapshot as immutable for the lifetime of their query — no locks on the hot path.
     *
     * Old snapshots chain via @c previous so the deferred-reclaim pass (Task 5) can walk back and
     * free what readers no longer reference once the global epoch advances. The pointed-to entry
     * array and the snapshot itself live on the registrar's arena; nothing here owns them, and the
     * snapshot's destructor is therefore trivial.
     */
    struct DispatchSnapshot {
        std::size_t              count;     ///< Number of entries; 0 is a valid empty snapshot.
        const DispatchEntry*     entries;   ///< Sorted-by-iid; may be @c nullptr iff @c count == 0.
        const DispatchSnapshot*  previous;  ///< Older retired snapshot in the epoch-retirement chain.
    };

    // The registrar's arena owns every snapshot and its entry array; no per-snapshot destructor
    // runs. Locking the invariant in via static_assert parallels the FacadeNode triviality guard
    // in FacadeList.h — any future field added with a non-trivial destructor will trip the build
    // here so the arena-only lifetime story stays sound.
    static_assert(std::is_trivially_destructible_v<DispatchEntry>,
                  "DispatchEntry array lives in the registrar's arena; entries are POD by contract.");
    static_assert(std::is_trivially_destructible_v<DispatchSnapshot>,
                  "DispatchSnapshot lives in the registrar's arena; no per-snapshot destructor runs.");

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Binary-search a @ref DispatchSnapshot for the entry whose @c iid matches @p id.
         *
         * Returns the matching entry, or @c nullptr on miss / empty / null snapshot. Entries must
         * be sorted ascending by @c iid (the registrar guarantees this when it publishes); we rely
         * on @c Iid::operator< for the comparison so the same total order used at sort time drives
         * the search. The loop is the canonical lower-bound shape: half-open `[lo, hi)` window,
         * mid biased toward `lo` to avoid overflow on hypothetical huge tables.
         */
        [[nodiscard]] inline const DispatchEntry* LookupEntry(
                const DispatchSnapshot* s, Iid id) noexcept {
            if (s == nullptr || s->count == 0) {
                return nullptr;
            }
            std::size_t lo = 0;
            std::size_t hi = s->count;
            while (lo < hi) {
                std::size_t mid = lo + (hi - lo) / 2;
                const DispatchEntry& e = s->entries[mid];
                if (e.iid < id) {
                    lo = mid + 1;
                } else if (id < e.iid) {
                    hi = mid;
                } else {
                    return &e;
                }
            }
            return nullptr;
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief An immutable, atomically-published list of stateful eager Extensions to materialise.
     *
     * Per spec §3.3 step 5. Each Implementation metaclass's @ref MetaLinks carries one of these:
     * the Extension metacores marked @c Anno::Eager whose state must be allocated up-front at
     * nucleus construction time (as opposed to @c Anno::Lazy Extensions, which materialise on
     * first query). The registrar (Tasks 7–8) builds and publishes the snapshot; the construction
     * hook in @c MetaNode (Task 10) walks @c entries and runs each Extension's @c MaterializeInto.
     *
     * Stateless code-extensions (zero NSDMs) are never listed here — they don't need per-instance
     * storage, so a singleton facade in `.rodata` covers them.
     *
     * Old snapshots chain via @c previous for the same epoch-retirement reason as
     * @ref DispatchSnapshot. The pointed-to entry array lives on the registrar's arena.
     */
    struct EagerSetSnapshot {
        std::size_t                  count;     ///< Number of eager extension cores; 0 means none.
        const MetaCore* const*       entries;   ///< Pointers to eager Extension metacores.
        const EagerSetSnapshot*      previous;  ///< Older retired snapshot in the epoch chain.
    };

    static_assert(std::is_trivially_destructible_v<EagerSetSnapshot>,
                  "EagerSetSnapshot lives in the registrar's arena; no per-snapshot destructor runs.");

    /**
     * @brief Back-references and the dispatch table — written once at load time, then read-only.
     *
     * Self types cannot know at compile time who will later extend them, so `extendedBy` /
     * `implementedBy` are folded in single-threaded by the loader and thereafter read RCU-style
     * with no locking on the hot path (QI / method dispatch).
     *
     * @c dispatch is an atomic pointer to an immutable @ref DispatchSnapshot. Registrars publish
     * fresh snapshots with release semantics (and chain the prior one via @c previous so the
     * epoch-retirement pass can free it later); readers acquire-load and binary-search. The field
     * is @c mutable so registrars holding a `const MetaLinks&` (the common shape on the QI hot
     * path) can still CAS-install a new snapshot when a Lazy interface first materialises.
     *
     * @c eagerSet mirrors @c dispatch's publish/retire discipline for the construction-time
     * Extension materialisation list: the @c MetaNode CRTP hook (Task 10) acquire-loads it during
     * nucleus construction and runs @c MaterializeInto for each entry. @c mutable for the same
     * registrar-on-const-MetaLinks reason.
     */
    struct MetaLinks {
        std::span<const MetaCore* const> extendedBy{};     ///< Extensions that extend this class.
        std::span<const MetaCore* const> implementedBy{};  ///< Classes implementing this interface.
        mutable std::atomic<const DispatchSnapshot*> dispatch{nullptr};  ///< Acquire-load, CAS-publish.
        mutable std::atomic<const EagerSetSnapshot*> eagerSet{nullptr};  ///< Stateful eager extensions.
    };

    // =========================================================================
    // Layer ③ MetaDynamic — the cold tail (concurrent write, never hot)
    // =========================================================================

    /**
     * @brief Lock-protected property bag and runtime statistics — diagnostics / scripting only.
     *
     * Lazily allocated by @ref MetaClass::dynamic and never touched on any hot path, so its
     * synchronisation cost never pollutes QI or dispatch.
     */
    struct MetaDynamic {
        mutable std::shared_mutex mutex;                 ///< Guards @c properties.
        std::map<std::string_view, std::string> properties;  ///< Arbitrary string properties.
        std::atomic<uint64_t> instanceCount{0};          ///< Live-instance counter (opt-in).
    };

    // =========================================================================
    // MetaClass — the stable shell stitching the three layers into one identity
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief `true` if @p target is reachable from @p from via omBase / extends / implements.
        constexpr bool IsCoreReachable(const MetaCore* from, const MetaCore* target) noexcept {
            if (from == nullptr) {
                return false;
            }
            if (from == target) {
                return true;
            }
            if (IsCoreReachable(from->omBase, target)) {
                return true;
            }
            for (const MetaCore* e : from->extends) {
                if (IsCoreReachable(e, target)) {
                    return true;
                }
            }
            for (const MetaCore* i : from->implements) {
                if (IsCoreReachable(i, target)) {
                    return true;
                }
            }
            return false;
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief The stable, per-class metaobject: an immutable core plus lazily-attached tails.
     *
     * Constructed `constexpr` from a @ref MetaCore reference, so @ref MetaClassOf is constant-
     * initialised (no static-init-order fiasco). The dynamic access surface reads straight through
     * the core, keeping a single source of truth with the static surface.
     */
    class MetaClass {
    public:
        constexpr explicit MetaClass(const MetaCore& core) noexcept : core_(&core) {}

        MetaClass(const MetaClass&) = delete;
        MetaClass& operator=(const MetaClass&) = delete;

        /// @name Dynamic access surface (source: @ref MetaCore)
        /// @{
        [[nodiscard]] ClassType classType() const noexcept { return core_->type; }
        [[nodiscard]] Iid iid() const noexcept { return core_->iid; }
        [[nodiscard]] std::string_view name() const noexcept { return core_->qualifiedName; }
        [[nodiscard]] const MetaCore* baseMeta() const noexcept { return core_->omBase; }
        [[nodiscard]] const MetaCore& core() const noexcept { return *core_; }
        /// @}

        /// @brief `true` if this class is-a-kind-of @p base (walks omBase / extends / implements).
        [[nodiscard]] bool isAKindOf(const MetaCore& base) const noexcept {
            return Detail::IsCoreReachable(core_, &base);
        }
        [[nodiscard]] bool isAKindOf(const MetaClass& base) const noexcept {
            return Detail::IsCoreReachable(core_, base.core_);
        }

        /// @name Structural tail (load-time fold-in, hot-path read)
        /// @{
        [[nodiscard]] MetaLinks* links() const noexcept { return links_.load(std::memory_order_acquire); }
        void setLinks(MetaLinks* l) noexcept { links_.store(l, std::memory_order_release); }
        /// @}

        /// @brief The cold tail, allocated on first touch (thread-safe, leaked at program end).
        [[nodiscard]] MetaDynamic& dynamic() const {
            MetaDynamic* d = dynamic_.load(std::memory_order_acquire);
            if (d == nullptr) [[unlikely]] {
                auto* fresh = new MetaDynamic{};
                if (dynamic_.compare_exchange_strong(d, fresh, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    d = fresh;
                } else {
                    delete fresh;  // lost the race; another thread installed first
                }
            }
            return *d;
        }

    private:
        const MetaCore* core_;
        mutable std::atomic<MetaLinks*> links_{nullptr};
        mutable std::atomic<MetaDynamic*> dynamic_{nullptr};
    };

    // =========================================================================
    // Compile-time pipeline: Anno::Meta → MetaCore (fully automatic)
    // =========================================================================
    //
    // Two-phase definition breaks the recursion: BuildMetaCore is *declared* first so MetaCoreOf
    // can name it; MetaCoreOf is then declared so the resolver helpers can take `&MetaCoreOf<E>`;
    // BuildMetaCore is *defined* last. Every `&MetaCoreOf<E>` is reached via std::meta::substitute
    // (P3687: template-argument splices are not in C++26, so we substitute then splice as an lvalue).

    /** @cond INTERNAL */
    namespace Detail {
        template<typename T> consteval MetaCore BuildMetaCore();
    }
    /** @endcond */

    /// @brief The immutable core of @p T — constant-initialised into `.rodata`, the single source
    ///        of truth for every static and dynamic query about @p T's identity.
    template<typename T>
    inline constexpr MetaCore MetaCoreOf = Detail::BuildMetaCore<T>();

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief All `extends` type-reflections for @p T, merged from every edge-carrying
        ///        annotation: the full @ref Anno::Meta `.extends` field plus each stacked
        ///        @ref Anno::Extends. Empty when @p T carries none. Order is Meta-first, then
        ///        stacked, preserving declaration order within each.
        template<typename T>
        consteval std::vector<std::meta::info> ExtendsInfos() {
            std::vector<std::meta::info> out;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Meta)) {
                auto meta = std::meta::extract<Anno::Meta>(a);
                out.insert(out.end(), meta.extends.begin(), meta.extends.end());
            }
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Extends)) {
                auto ext = std::meta::extract<Anno::Extends>(a);
                out.insert(out.end(), ext.bases.begin(), ext.bases.end());
            }
            return out;
        }

        /// @brief All `implements` type-reflections for @p T, merged from every edge-carrying
        ///        annotation: the full @ref Anno::Meta `.implements` field plus each stacked
        ///        @ref Anno::Implements. Empty when @p T carries none. Order is Meta-first, then
        ///        stacked, preserving declaration order within each.
        template<typename T>
        consteval std::vector<std::meta::info> ImplementsInfos() {
            std::vector<std::meta::info> out;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Meta)) {
                auto meta = std::meta::extract<Anno::Meta>(a);
                out.insert(out.end(), meta.implements.begin(), meta.implements.end());
            }
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Implements)) {
                auto impl = std::meta::extract<Anno::Implements>(a);
                out.insert(out.end(), impl.ifaces.begin(), impl.ifaces.end());
            }
            return out;
        }

        template<typename T>
        inline constexpr auto kExtendsInfos = std::define_static_array(ExtendsInfos<T>());
        template<typename T>
        inline constexpr auto kImplementsInfos = std::define_static_array(ImplementsInfos<T>());

        /// @brief Map each `extends` reflection to `&MetaCoreOf<that type>`.
        template<typename T>
        consteval std::vector<const MetaCore*> ResolveExtends() {
            std::vector<const MetaCore*> out;
            template for (constexpr auto e : kExtendsInfos<T>) {
                out.push_back(&[: std::meta::substitute(^^MetaCoreOf, {e}) :]);
            }
            return out;
        }

        /// @brief Map each `implements` reflection to `&MetaCoreOf<that type>`.
        template<typename T>
        consteval std::vector<const MetaCore*> ResolveImplements() {
            std::vector<const MetaCore*> out;
            template for (constexpr auto e : kImplementsInfos<T>) {
                out.push_back(&[: std::meta::substitute(^^MetaCoreOf, {e}) :]);
            }
            return out;
        }

        template<typename T>
        inline constexpr auto kExtendsCores = std::define_static_array(ResolveExtends<T>());
        template<typename T>
        inline constexpr auto kImplementsCores = std::define_static_array(ResolveImplements<T>());

        /// @brief Reflection of @p type's nearest object-model ancestor (first base, transitively
        ///        through unannotated bases like MetaNode/RootObject, that carries an object-model
        ///        annotation). Returns the null reflection at an object-model root.
        consteval std::meta::info FindOmBaseInfo(std::meta::info type) {
            for (auto b : std::meta::bases_of(type, std::meta::access_context::unchecked())) {
                auto bt = std::meta::type_of(b);
                if (IsObjectModelType(bt)) {
                    return bt;
                }
                auto nested = FindOmBaseInfo(bt);
                if (nested != std::meta::info{}) {
                    return nested;
                }
            }
            return std::meta::info{};
        }

        /// @brief `&MetaCoreOf<nearest OM ancestor>`, or `nullptr` at an object-model root.
        template<typename T>
        consteval const MetaCore* OmBaseCore() {
            constexpr std::meta::info b = FindOmBaseInfo(^^T);
            if constexpr (b == std::meta::info{}) {
                return nullptr;
            } else {
                return &[: std::meta::substitute(^^MetaCoreOf, {b}) :];
            }
        }

        template<typename T>
        consteval MetaCore BuildMetaCore() {
            MetaCore c;
            c.type = ClassTypeOf<T>;
            c.qualifiedName = std::define_static_string(std::meta::display_string_of(^^T));
            c.iid = IidOf<T>();
            c.extends = kExtendsCores<T>;
            c.implements = kImplementsCores<T>;
            c.omBase = OmBaseCore<T>();
            return c;
        }

    } // namespace Detail
    /** @endcond */

    /// @brief The per-class metaobject for @p T — constant-initialised (constexpr ctor, no SIOF).
    template<typename T>
    inline MetaClass MetaClassOf{ MetaCoreOf<T> };

    // =========================================================================
    // Static access surface — read "of *this type*", folds at compile time
    // =========================================================================

    /// @brief @p T's qualified name (display string). Source: @ref MetaCore::qualifiedName.
    template<typename T>
    inline constexpr std::string_view NameOf = MetaCoreOf<T>.qualifiedName;

    /// @brief @p T's nearest object-model ancestor core, or `nullptr`. Source: @ref MetaCore::omBase.
    template<typename T>
    inline constexpr const MetaCore* BaseMetaOf = MetaCoreOf<T>.omBase;

    // =========================================================================
    // Static-face Query<I>(C*) — the type-known fast path of QueryInterface
    // =========================================================================

    /**
     * @brief Compile-time @c QueryInterface for the case where the host's static type is known.
     *
     * The "static face" of the QI surface: when the caller already has a @c C* with @c C's complete
     * type visible, the answer is decidable at compile time. The function folds to one of:
     *
     *   - `static_cast<I*>(p)` — when @c C C++-inherits @p I (BOA / @c InlineFacade aggregated as a
     *     subobject of @c C);
     *   - the address of the inline-aggregated facade subobject — when the metaclass pipeline
     *     records an @c InlineFacade entry whose offset is a compile-time constant;
     *   - @c nullptr — when @p I cannot be reached statically (the dynamic face, taking
     *     @c RootObject*, must consult @c MetaClassDynamic and the runtime tie chain instead).
     *
     * No tag check, no vcall, no table lookup: the optimiser sees through the @c if @c constexpr and
     * emits the C++-language-level offset adjustment directly.
     *
     * @tparam I The interface to retrieve.
     * @param p  A pointer to a fully-typed host. May be @c nullptr.
     * @return A native @p I pointer when the cast is statically valid; @c nullptr otherwise.
     */
    template<InterfaceClass I, typename C>
    [[nodiscard]] constexpr I* Query(C* p) noexcept {
        if (p == nullptr) {
            return nullptr;
        }
        if constexpr (std::derived_from<C, I>) {
            return static_cast<I*>(p);
        } else {
            return nullptr;
        }
    }

    /// @copydoc Query
    template<InterfaceClass I, typename C>
    [[nodiscard]] constexpr const I* Query(const C* p) noexcept {
        if (p == nullptr) {
            return nullptr;
        }
        if constexpr (std::derived_from<C, I>) {
            return static_cast<const I*>(p);
        } else {
            return nullptr;
        }
    }

} // namespace Yuki
