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

#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MetaClass.h>
#include <Yuki/Core/Registry.h>

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

} // namespace Yuki::RT
