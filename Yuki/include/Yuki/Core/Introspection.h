/**
 * @file Introspection.h
 * @brief Metaclass-level introspection surface for the Yuki closure model — spec §1.2 / §6.1.
 *
 * Every helper here is a pure read of one of @ref Yuki::MetaLinks's atomic snapshot pointers; no
 * closure instance is consulted, no allocation runs on the read path, and the answer is the same
 * for every closure of the same metaclass. The reads are acquire-loads paired with the registrar's
 * release-stores, so the snapshot a caller sees is always fully-constructed and immutable for the
 * duration of the call.
 *
 * Three kinds of question land here:
 *  - "Which interface iids does @p m's closure resolve, and through which dispatch arm?"
 *    @ref IidsOf, @ref Provides, @ref ProviderClass, @ref ProviderDispatchKind. All read the
 *    @ref DispatchSnapshot.
 *  - "Which classes extend / implement @p m?" @ref Extensions, @ref Implementations. Both read
 *    the @ref ExtendedListSnapshot pair populated by the registrar (Tasks 7–8).
 *  - "Which Extensions does the construction hook eagerly materialise into a fresh nucleus of
 *    @p m?" @ref EagerExtensions. Reads the @ref EagerSetSnapshot.
 *
 * All views borrow into the snapshot — their lifetime is bounded by the metaclass's lifetime,
 * which is program-wide, so callers can forward the view without copying as long as the metaclass
 * is reachable. Returned spans / ranges are empty on a null snapshot or null @c MetaLinks; callers
 * never need a defensive nullptr check.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/FacadeList.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaClass.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>

namespace Yuki::RT {

    /**
     * @brief A range of @ref Iid values — every interface iid the closure of @p m can resolve.
     *
     * Implemented as a transform_view over the dispatch snapshot's entries projecting @c .iid out
     * of each @c DispatchEntry. The transform view borrows; it does not allocate, and the iteration
     * stride matches @c DispatchEntry (not @c Iid), so the bug the plan-literal Capabilities had —
     * a span striding over the wrong type — is structurally avoided here.
     *
     * Empty when @p m has no @c MetaLinks installed yet (interfaces that no Implementation has
     * registered against, common during early bring-up) or its dispatch snapshot is null.
     */
    [[nodiscard]] inline auto IidsOf(const MetaClass& m) noexcept {
        const MetaLinks* l = m.links();
        const DispatchSnapshot* s = (l != nullptr) ? l->dispatch.load(std::memory_order_acquire) : nullptr;
        std::span<const DispatchEntry> entries =
            (s != nullptr) ? std::span<const DispatchEntry>{s->entries, s->count} : std::span<const DispatchEntry>{};
        return entries | std::views::transform([](const DispatchEntry& e) noexcept { return e.iid; });
    }

    /// @brief Spec § 6.1 alias: @ref IidsOf is the lookup-language name for the same surface.
    [[nodiscard]] inline auto Capabilities(const MetaClass& m) noexcept {
        return IidsOf(m);
    }

    /**
     * @brief @c true iff @p m's closure has a @ref DispatchEntry for interface @p I.
     *
     * Acquire-loads the dispatch snapshot, runs the same binary search the dynamic kernel uses,
     * and reports presence. A miss is observation-only — no SideTableResolver runs, no facade is
     * materialised. Symmetric to the static-face @ref Yuki::RT::Query for "would Query<I> succeed
     * statically", except this answers "does Query<I> have an entry to take, dynamically".
     */
    template<InterfaceClass I>
    [[nodiscard]] inline bool Provides(const MetaClass& m) noexcept {
        const MetaLinks* l = m.links();
        if (l == nullptr) {
            return false;
        }
        const auto* s = l->dispatch.load(std::memory_order_acquire);
        return Detail::LookupEntry(s, IidOf<I>()) != nullptr;
    }

    /**
     * @brief The @c MetaClass that registered the @ref DispatchEntry for @p I, or @c nullptr.
     *
     * For @c DirectCast / @c InlineFacade entries this is @p m itself — the Implementation owns
     * the layout that yields the interface. For @c CodeExtensionSingleton / @c SideTableResolver
     * entries this is the Extension that overrode the entry. The information lets
     * diagnostics / tooling answer "which class is responsible for this interface on this
     * closure" without instantiating the closure or invoking the dispatch arm.
     */
    template<InterfaceClass I>
    [[nodiscard]] inline const MetaClass* ProviderClass(const MetaClass& m) noexcept {
        const MetaLinks* l = m.links();
        if (l == nullptr) {
            return nullptr;
        }
        const auto* s = l->dispatch.load(std::memory_order_acquire);
        const auto* e = Detail::LookupEntry(s, IidOf<I>());
        return (e != nullptr) ? e->providerClass : nullptr;
    }

    /**
     * @brief The @ref DispatchKind of the entry resolving @p I, or @c std::nullopt on a miss.
     *
     * Returns the same enum the dynamic kernel switches on — useful for tooling that wants to
     * report "interface I lands on the SideTableResolver arm of m's closure" without actually
     * driving Query. Acquire-loads the snapshot; binary-searches by iid.
     */
    template<InterfaceClass I>
    [[nodiscard]] inline std::optional<DispatchKind> ProviderDispatchKind(const MetaClass& m) noexcept {
        const MetaLinks* l = m.links();
        if (l == nullptr) {
            return std::nullopt;
        }
        const auto* s = l->dispatch.load(std::memory_order_acquire);
        const auto* e = Detail::LookupEntry(s, IidOf<I>());
        return (e != nullptr) ? std::optional{e->kind} : std::nullopt;
    }

    /**
     * @brief Span of @ref MetaClass back-pointers — every Extension class that extends @p m.
     *
     * Populated by @ref Detail::PublishExtensionEntries (Task 8) on each Install<E>: the
     * Extension's @c &MetaClassOf<E> is appended to @p m's @c extendedBy snapshot under
     * @ref Registry::WriterMutexFor. Returns @c MetaClass* — symmetric with @ref ProviderClass and
     * @ref EagerExtensions, so every back-reference accessor on the introspection surface speaks
     * the same currency. Callers that want the immutable core can read it through
     * @ref MetaClass::core at any time.
     *
     * Empty when @p m has no Extensions registered or no @c MetaLinks installed yet.
     */
    [[nodiscard]] inline std::span<const MetaClass* const> Extensions(const MetaClass& m) noexcept {
        const MetaLinks* l = m.links();
        if (l == nullptr) {
            return {};
        }
        const auto* s = l->extendedBy.load(std::memory_order_acquire);
        return (s != nullptr) ? std::span<const MetaClass* const>{s->classes, s->count}
                              : std::span<const MetaClass* const>{};
    }

    /**
     * @brief Span of @ref MetaClass back-pointers — every Implementation class implementing @p m.
     *
     * Symmetric to @ref Extensions for the @c implementedBy snapshot: each @ref Registry::Install
     * for an @ref ImplementationClass walks @c kImplementsInfos and appends @c &MetaClassOf<T> to
     * every advertised interface's @c implementedBy. Asking @c Implementations on an interface's
     * metaclass is the canonical "who implements this interface" query.
     */
    [[nodiscard]] inline std::span<const MetaClass* const> Implementations(const MetaClass& m) noexcept {
        const MetaLinks* l = m.links();
        if (l == nullptr) {
            return {};
        }
        const auto* s = l->implementedBy.load(std::memory_order_acquire);
        return (s != nullptr) ? std::span<const MetaClass* const>{s->classes, s->count}
                              : std::span<const MetaClass* const>{};
    }

    /**
     * @brief Range of @c MetaClass* — every Extension the construction hook eagerly materialises.
     *
     * Implemented as a transform_view over the eager-set snapshot's entries projecting
     * @c .extensionClass out of each @c EagerSetEntry. Stateless code-extensions and lazy stateful
     * Extensions do not appear here — the eager set is exactly the stateful + @c Anno::Eager
     * subset (spec §3.3 step 5). Empty on a null snapshot or null @c MetaLinks.
     */
    [[nodiscard]] inline auto EagerExtensions(const MetaClass& m) noexcept {
        const MetaLinks* l = m.links();
        const EagerSetSnapshot* s = (l != nullptr) ? l->eagerSet.load(std::memory_order_acquire) : nullptr;
        std::span<const EagerSetEntry> entries =
            (s != nullptr) ? std::span<const EagerSetEntry>{s->entries, s->count} : std::span<const EagerSetEntry>{};
        return entries | std::views::transform([](const EagerSetEntry& e) noexcept { return e.extensionClass; });
    }

    // =========================================================================
    // Instance-level introspection (Task 15, spec §6.1 / §6.3)
    // =========================================================================
    //
    // The metaclass-level surface above answers "what *can* a closure of M
    // resolve". The four entries below answer "what does *this particular*
    // closure carry, right now" — every one reads the runtime facade chain on
    // a @c RootObject (the cold-path arm @ref FacadeListHead manages), not the
    // dispatch snapshot. Nothing here ever fires a SideTableResolver; the
    // results are stable across calls until something else publishes a fresh
    // facade node (e.g. an explicit @ref Reify or a Materialize::Yes Query).

    /**
     * @brief Forward-iterable view over a closure's *currently materialised* facade pointers,
     *        deduplicated by @c RootObject identity.
     *
     * The chain head publishes the SAME Extension instance pointer under multiple FacadeNodes
     * (one for the Extension's own iid, one per advertised interface iid — see
     * @ref Detail::MaterializeIntoImpl). A naive forward walk would therefore yield the same
     * @c RootObject* multiple times; this view drops duplicates by keeping an inline
     * fixed-capacity "seen" set on the iterator (see @ref Iterator::kSeenCapacity). The capacity
     * is intentionally generous for typical closures and falls out of dedup on overflow —
     * closures with more than @ref Iterator::kSeenCapacity distinct facades are an outlier and
     * can be reshaped if the limit ever bites.
     *
     * All operations are @c noexcept and inline; iteration never allocates.
     */
    class MaterializedFacadesView : public std::ranges::view_interface<MaterializedFacadesView> {
        FacadeListHead* head_;

    public:
        constexpr MaterializedFacadesView() noexcept : head_(nullptr) {}
        explicit constexpr MaterializedFacadesView(FacadeListHead* h) noexcept : head_(h) {}

        class Sentinel {};

        class Iterator {
        public:
            /// Inline dedup capacity. Closures with more distinct facades than this lose dedup
            /// on the overflow tail — see @ref MaterializedFacadesView class doc for rationale.
            static constexpr std::size_t kSeenCapacity = 32;

        private:
            FacadeNode* cur_{nullptr};
            std::array<RootObject*, kSeenCapacity> seen_{};
            std::size_t seenCount_{0};

            bool SeenBefore(RootObject* p) const noexcept {
                for (std::size_t i = 0; i < seenCount_; ++i) {
                    if (seen_[i] == p) {
                        return true;
                    }
                }
                return false;
            }

            void SkipDupes() noexcept {
                while (cur_ != nullptr && SeenBefore(cur_->facade)) {
                    cur_ = cur_->next;
                }
                if (cur_ != nullptr && seenCount_ < seen_.size()) {
                    seen_[seenCount_++] = cur_->facade;
                }
            }

        public:
            using iterator_concept = std::input_iterator_tag;
            using value_type       = RootObject*;
            using difference_type  = std::ptrdiff_t;

            Iterator() noexcept = default;
            explicit Iterator(FacadeNode* start) noexcept : cur_(start) { SkipDupes(); }

            RootObject* operator*() const noexcept { return cur_->facade; }
            Iterator& operator++() noexcept {
                cur_ = cur_->next;
                SkipDupes();
                return *this;
            }
            Iterator operator++(int) noexcept {
                auto t = *this;
                ++*this;
                return t;
            }

            friend bool operator==(const Iterator& i, const Sentinel&) noexcept {
                return i.cur_ == nullptr;
            }
        };

        Iterator begin() const noexcept {
            return head_ != nullptr ? Iterator{head_->Head()} : Iterator{};
        }
        Sentinel end() const noexcept { return {}; }
    };

    /**
     * @brief View over the currently-materialised facades on @p n's closure — every facade pointer
     *        appears exactly once.
     *
     * Resolves @p n's facade chain head via @ref Facades (so @p n may be the nucleus itself or
     * any Implementation / Extension whose nucleus owns the chain) and wraps it in a
     * @ref MaterializedFacadesView. An Implementation that has never had a Cold facade
     * materialised yields an empty view; a non-Implementation @p n (Extension, Interface
     * facade, @c nullptr) also yields empty — the role-arm mismatch is silent so callers do not
     * need to pre-classify @p n.
     *
     * No SideTableResolver fires; the view is observation-only.
     */
    [[nodiscard]] inline MaterializedFacadesView MaterializedFacades(RootObject* n) noexcept {
        return MaterializedFacadesView{Facades(n)};
    }

    /**
     * @brief View filtered to facades whose dynamic role is @ref ClassType::Extension.
     *
     * Filters @ref MaterializedFacades by @c RootObject::TypeDynamic — the role bits in the
     * tagged payload, decoded without a vcall. Every Extension instance lands in the chain with
     * role @c Extension because @ref ExtensionNode (its CRTP base) makes the payload via
     * @c TaggedPayload::Make(ClassType::Extension, extendee). Implementations that have never
     * been extended yield an empty view.
     */
    [[nodiscard]] inline auto MaterializedExtensions(RootObject* n) noexcept {
        return MaterializedFacades(n) | std::views::filter([](RootObject* p) noexcept {
                   return p != nullptr && p->TypeDynamic() == ClassType::Extension;
               });
    }

    /**
     * @brief @c true iff @p anyNode and @p candidate share the same closure nucleus.
     *
     * The closure-identity test spec §6.3 calls for: every facade, Extension and Implementation
     * in one closure walks to the same @ref Nucleus, so identity at the nucleus is identity at
     * the closure. The body is one comparison after two @ref Nucleus walks; both walks accept
     * @c nullptr and return @c nullptr, so @c InClosure(nullptr, nullptr) is @c true — useful as
     * an identity element when composing closure-aware algorithms.
     */
    [[nodiscard]] inline bool InClosure(RootObject* anyNode, RootObject* candidate) noexcept {
        return Nucleus(anyNode) == Nucleus(candidate);
    }

    /**
     * @brief Visit the nucleus of @p anyNode and every materialised facade exactly once.
     *
     * @p visit is invoked first with @c (nucleus, nucleus->TypeDynamic()) — the
     * Implementation arm — and then with @c (facade, facade->TypeDynamic()) for each
     * facade yielded by @ref MaterializedFacades (which already dedups by pointer identity, so
     * no facade is visited twice even though the underlying chain publishes the same Extension
     * under multiple iids). Null @p anyNode is a no-op. No SideTableResolver fires; the walk
     * sees exactly the closure's current observable state.
     */
    template<std::invocable<RootObject*, ClassType> F>
    inline void WalkClosure(RootObject* anyNode, F&& visit) noexcept {
        RootObject* n = Nucleus(anyNode);
        if (n == nullptr) {
            return;
        }
        visit(n, n->TypeDynamic());
        for (RootObject* facade : MaterializedFacades(n)) {
            visit(facade, facade->TypeDynamic());
        }
    }

} // namespace Yuki::RT
