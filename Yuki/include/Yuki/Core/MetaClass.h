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
     * sourced from one place.
     */
    template<typename I>
    inline constexpr Iid IidOf = [] consteval {
        constexpr bool overridden = !std::meta::annotations_of(^^I, ^^Iid).empty();
        if constexpr (overridden) {
            return std::meta::extract<Iid>(std::meta::annotations_of(^^I, ^^Iid)[0]);
        } else {
            return StableHash<I>;
        }
    }();

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

    // =========================================================================
    // Layer ② MetaLinks — the structural tail (load-time write, hot-path read)
    // =========================================================================

    /// @brief A perfect-hash dispatch entry: an interface IID mapped to its core and `this` offset.
    struct DispatchEntry {
        Iid iid{};                       ///< The interface's identity.
        const MetaCore* iface{nullptr};  ///< The interface's core.
        std::ptrdiff_t offset{0};        ///< `this`-adjustment to reach the subobject.
    };

    /**
     * @brief Back-references and the dispatch table — written once at load time, then read-only.
     *
     * Self types cannot know at compile time who will later extend them, so `extendedBy` /
     * `implementedBy` are folded in single-threaded by the loader and thereafter read RCU-style
     * with no locking on the hot path (QI / method dispatch).
     */
    struct MetaLinks {
        std::span<const MetaCore* const> extendedBy{};     ///< Extensions that extend this class.
        std::span<const MetaCore* const> implementedBy{};  ///< Classes implementing this interface.
        std::span<const DispatchEntry> dispatch{};         ///< Perfect-hash dispatch entries.
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
            c.iid = IidOf<T>;
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

} // namespace Yuki
