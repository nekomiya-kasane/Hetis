/**
 * @file Query.h
 * @brief Static- and dynamic-face @c Query — the QueryInterface surface of the closure model.
 *
 * Two surfaces share one name and one contract:
 *
 *   - **Static face** (@ref Yuki::RT::Query): the host's concrete C++ type @p C is visible at the
 *     call site, so the answer is decidable at compile time. Folds to one of: `static_cast<I*>(p)`
 *     when @p C C++-inherits @p I (BOA — `static_cast` is the layout-required offset adjustment, no
 *     table lookup, no vcall); the address of an inline-aggregated @ref InterfaceFacade subobject
 *     when reflection over @p C's NSDMs locates one for @p I; otherwise the dynamic kernel
 *     (@ref Detail::QueryDynamicRaw, Task 12) on a @c RootObject* fallback. The static face is the
 *     hot QI path — every fold is a constant expression and the optimiser sees the chosen arm.
 *
 *   - **Dynamic face** (@ref Yuki::Query): the host is reached through a type-erased
 *     @c RootObject*. Walks facades, consults the metaclass's PHF dispatch table for Hot interfaces,
 *     and falls back to the runtime tie chain for Cold ones — one role decode + one dispatch lookup
 *     on the hot path; the cold path adds an acquire load on the tie chain head. The kernel body
 *     (Task 12) is currently stubbed; this header keeps the public template alive so callers in
 *     existing tests (RegistryTest etc.) compile while the kernel rewrite lands.
 *
 * Header layout note: the static-face @c Detail helpers (@c HasInlineFacadeFor etc.) appear *before*
 * the @c RootObject.h include because @c RootObject.h transitively pulls in @c Registry.h, whose
 * @c BuildImplDispatchEntries needs the helpers visible at template-definition time.
 *
 * @ingroup Core
 */
#pragma once

#include <concepts>
#include <meta>
#include <utility>

#include <Yuki/Core/Identity.h>
#include <Yuki/Core/InterfaceFacade.h>

namespace Yuki {

    /** @cond INTERNAL */
    namespace Detail {

        // =====================================================================
        // Static-face inline-facade probe (Task 11, spec §4.1)
        // =====================================================================
        //
        // The probe walks @p C's NSDMs (and each NSDM type's bases — the project's convention is
        // for an interface author to *subclass* @ref InterfaceFacade<I, Impl> and override the
        // virtuals there, not to instantiate @c InterfaceFacade<I, Impl> directly as the NSDM
        // type). A match is "this NSDM derives from @c InterfaceFacade<I, X> for some @c X that
        // @c C derives from" — the second clause keeps the rule sound when @c C inherits the
        // facade-bearing type from some intermediate base.

        /// @brief consteval predicate: is @p T (or one of its bases) an @c InterfaceFacade<I, X>
        ///        specialisation with @c std::derived_from<C, X>?
        ///
        /// Implemented as a function template parameterised on the candidate's reflection so the
        /// caller can write `MatchesInlineFacade<C, I, ...>()` inside a @c template @c for body
        /// where the loop element is constexpr — splicing a parameter-derived `std::meta::info` is
        /// not a constant expression, so we lift the reflection into a template parameter.
        template<class C, class I, std::meta::info T>
        consteval bool MatchesInlineFacade() {
            if constexpr (std::meta::has_template_arguments(T)
                          && std::meta::template_of(T) == ^^InterfaceFacade) {
                using Iarg = [: std::meta::template_arguments_of(T)[0] :];
                using Xarg = [: std::meta::template_arguments_of(T)[1] :];
                if constexpr (std::same_as<Iarg, I> && std::derived_from<C, Xarg>) {
                    return true;
                }
            }
            // Walk every base: catches the canonical authoring pattern where an interface author
            // subclasses InterfaceFacade<I, Impl> and overrides the virtuals on the subclass.
            template for (constexpr auto b : std::define_static_array(
                              std::meta::bases_of(T, std::meta::access_context::current()))) {
                if constexpr (MatchesInlineFacade<C, I, std::meta::type_of(b)>()) {
                    return true;
                }
            }
            return false;
        }

        /// @brief `true` if @p C carries an inline-facade NSDM for interface @p I.
        ///
        /// Walks @p C's own NSDMs; for each NSDM, the type itself and its bases are tested via
        /// @ref MatchesInlineFacade. The match contract — "an `InterfaceFacade<I, X>` where `C`
        /// derives from `X`" — keeps subclassing the canonical authoring pattern (an interface
        /// author subclasses @c InterfaceFacade and overrides the virtuals) and tolerates the
        /// degenerate case where @c X == C exactly.
        template<class C, class I>
        consteval bool HasInlineFacadeFor() {
            template for (constexpr auto m : std::define_static_array(
                              std::meta::nonstatic_data_members_of(
                                  ^^C, std::meta::access_context::current()))) {
                // Nested if constexpr so `MatchesInlineFacade` is only instantiated when the NSDM
                // is a class type. A flat `&&` does not short-circuit instantiation here — the
                // consteval call would still be substituted for non-class NSDMs (e.g. `int`),
                // and `bases_of` would then reject the non-class reflection.
                if constexpr (std::meta::is_class_type(std::meta::type_of(m))) {
                    if constexpr (MatchesInlineFacade<C, I, std::meta::type_of(m)>()) {
                        return true;
                    }
                }
            }
            return false;
        }

        /// @brief The address of @p C's inline-aggregated @c InterfaceFacade<I, X> subobject,
        ///        sliced down to @p I*.
        ///
        /// Mirrors the @ref HasInlineFacadeFor walk: pick the first NSDM whose type or one of its
        /// bases is @c InterfaceFacade<I, X> with @c std::derived_from<C, X>, then return
        /// `static_cast<I*>(&p->[:m:])`. The returned pointer is a real @p I subobject — every
        /// @c InterfaceFacade<I, X> publicly inherits @p I, so the upcast is well-formed and yields
        /// the layout-correct @p I vptr without a runtime offset table.
        ///
        /// Precondition: @ref HasInlineFacadeFor<C, I>() is @c true. @c std::unreachable() on an
        /// unmatched walk so the compiler can prove the function is total in its precondition.
        template<class I, class C>
        [[nodiscard]] constexpr I* InlineFacadeAddress(C* p) noexcept {
            template for (constexpr auto m : std::define_static_array(
                              std::meta::nonstatic_data_members_of(
                                  ^^C, std::meta::access_context::current()))) {
                // Same nested if-constexpr discipline as @ref HasInlineFacadeFor — only instantiate
                // @ref MatchesInlineFacade after confirming the NSDM is a class type, otherwise
                // bases_of fires on (e.g.) `int` and breaks the consteval call.
                if constexpr (std::meta::is_class_type(std::meta::type_of(m))) {
                    if constexpr (MatchesInlineFacade<C, I, std::meta::type_of(m)>()) {
                        // The NSDM's static type may be a *subclass* of InterfaceFacade<I, X>; the
                        // static_cast through I directly is sound because every InterfaceFacade<I, X>
                        // publicly inherits I, so any subclass also derives from I.
                        return static_cast<I*>(&p->[: m :]);
                    }
                }
            }
            std::unreachable();
        }

    } // namespace Detail
    /** @endcond */

} // namespace Yuki

// Pulls in @c RootObject (the dynamic-kernel pointer type) and, via its bottom include,
// @c Registry — the latter consumes @ref Yuki::Detail::HasInlineFacadeFor in its
// @c BuildImplDispatchEntries body, so the helpers above must already be visible.
#include <Yuki/Core/RootObject.h>

namespace Yuki {

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Type-erased dynamic Query implementation; the public templates wrap it for type.
        ///
        /// Forwards to @ref RT::QueryDynamicRaw — the real kernel body lives there with the
        /// rest of the @c RT namespace. This wrapper survives so the legacy public
        /// @c Yuki::Query<I>(RootObject*) overloads below keep their existing call shape.
        inline RootObject* QueryDynamicRaw(RootObject* p, Iid iid) noexcept;

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Dynamic-face @c Query — request interface @p I from a type-erased @c RootObject.
     *
     * Returns a native @p I pointer when the request succeeds, @c nullptr otherwise. Walks
     * facades, consults the metaclass's PHF table for Hot interfaces, and falls back to the
     * runtime tie chain for Cold ones. The hot path is one role decode + one dispatch lookup; the
     * cold path adds an acquire load on the tie chain head.
     *
     * @tparam I The interface to retrieve (must be an @ref InterfaceClass).
     * @param p  A pointer to a polymorphic @c RootObject — may be @c nullptr.
     */
    template<InterfaceClass I>
    [[nodiscard]] inline I* Query(RootObject* p) noexcept {
        if (RootObject* hit = Detail::QueryDynamicRaw(p, IidOf<I>())) {
            return static_cast<I*>(hit);
        }
        return nullptr;
    }

    /// @copydoc Query
    template<InterfaceClass I>
    [[nodiscard]] inline const I* Query(const RootObject* p) noexcept {
        if (const RootObject* hit = Detail::QueryDynamicRaw(const_cast<RootObject*>(p), IidOf<I>())) {
            return static_cast<const I*>(hit);
        }
        return nullptr;
    }

    namespace RT {

        // =====================================================================
        // Dynamic kernel (Task 12, spec §4.2 + §6.4)
        // =====================================================================
        //
        // One switch over @ref DispatchKind, reading the published @ref DispatchSnapshot through
        // an acquire load. The only arm that branches on the @ref Materialize policy is
        // @c SideTableResolver: under @c Materialize::No the kernel reads the facade chain
        // directly (so @ref Has answers @c false for an unmaterialised lazy Extension); under
        // @c Materialize::Yes it calls the resolver, which AttachUnique-publishes the per-closure
        // facade and returns it. Every other arm produces a real pointer regardless of policy.
        //
        // Steering through @ref Nucleus before the lookup makes the kernel total over the role
        // space: a query handed an Interface/Imposter/Bridge facade transparently re-targets to
        // the underlying object, and an Extension hops to its Extendee — the same role-walk
        // contract @ref Underlying / @ref Extendee already encode, but composed once at the top
        // of the kernel rather than spread across the switch.

        /// @brief Whether the dynamic kernel may materialise lazy state on miss.
        ///
        /// Spelled as a strongly-typed boolean so the policy parameter cannot be confused with a
        /// pointer/integer at the call site, while still bridging via @c bool's value space —
        /// the @c switch arm reads `M == Materialize::No` as the constexpr discriminator.
        enum class Materialize : bool { No = false, Yes = true };

        /// @brief Type-erased dynamic Query implementation, parameterised on the materialise
        ///        policy. @ref QueryDynamicRaw and @ref Has are the two named entry points; both
        ///        sites delegate here so the per-arm logic stays in one place.
        template<Materialize M = Materialize::Yes>
        [[nodiscard]] inline RootObject* QueryDynamicRawPolicy(RootObject* p, Iid id) noexcept {
            RootObject* n = Nucleus(p);
            if (n == nullptr) {
                return nullptr;
            }
            const MetaLinks* links = n->MetaClassDynamic().links();
            if (links == nullptr) {
                // No registrar has installed a snapshot for this metaclass — only the cold,
                // user-attached facade chain can produce an answer.
                if (FacadeListHead* head = Facades(n)) {
                    return FacadeListLookup(*head, id);
                }
                return nullptr;
            }
            const DispatchSnapshot* snap = links->dispatch.load(std::memory_order_acquire);
            const DispatchEntry*    e    = (snap != nullptr) ? Detail::LookupEntry(snap, id) : nullptr;
            if (e != nullptr) {
                switch (e->kind) {
                case DispatchKind::DirectCast:
                case DispatchKind::InlineFacade:
                    // BOA / inline-facade: the offset is a compile-time constant baked into the
                    // entry; one byte add lands on a real I-typed subobject of @c n.
                    return reinterpret_cast<RootObject*>(
                            reinterpret_cast<std::byte*>(n) + e->payload.staticOffset);

                case DispatchKind::CodeExtensionSingleton:
                    // Stateless extension: the entry holds a pointer-to-pointer-to-singleton so
                    // the registrar can update the slot without rewriting the snapshot. Until the
                    // singleton-publication path lands the slot may carry a sentinel @c nullptr.
                    return (e->payload.singleton != nullptr) ? *e->payload.singleton : nullptr;

                case DispatchKind::SideTableResolver:
                    // The only Materialize-sensitive arm. @c No probes the chain directly — a
                    // miss is the contract for "lazy and not yet materialised". @c Yes calls the
                    // resolver, which is the same lookup-then-materialise primitive @c Has would
                    // need to be promoted to a Query.
                    if constexpr (M == Materialize::No) {
                        if (FacadeListHead* head = Facades(n)) {
                            return FacadeListLookup(*head, id);
                        }
                        return nullptr;
                    } else {
                        return (e->payload.resolver != nullptr) ? e->payload.resolver(n) : nullptr;
                    }

                case DispatchKind::FacadeList:
                    // Cold/runtime-attached interface — the entry exists for introspection only;
                    // the actual answer lives on the facade chain.
                    if (FacadeListHead* head = Facades(n)) {
                        return FacadeListLookup(*head, id);
                    }
                    return nullptr;
                }
            }
            // No dispatch entry — fall through to user-attached facades. This is the path a Cold
            // interface that never made it onto the snapshot takes, so spec §4.2's "cold-path
            // facade lookup" stays reachable even after the snapshot rewrite.
            if (FacadeListHead* head = Facades(n)) {
                return FacadeListLookup(*head, id);
            }
            return nullptr;
        }

        /// @brief Type-erased dynamic Query — the @c Materialize::Yes entry point.
        [[nodiscard]] inline RootObject* QueryDynamicRaw(RootObject* p, Iid id) noexcept {
            return QueryDynamicRawPolicy<Materialize::Yes>(p, id);
        }

        /// @brief @c true iff @p p exposes interface @p I without materialising new state — the
        ///        @c Materialize::No entry into the dynamic kernel. Spec §6.4: lazy + unmaterialised
        ///        answers @c false; every other arm answers exactly as @ref QueryDynamicRaw would.
        template<InterfaceClass I>
        [[nodiscard]] inline bool Has(RootObject* p) noexcept {
            return QueryDynamicRawPolicy<Materialize::No>(p, IidOf<I>()) != nullptr;
        }

        /**
         * @brief Static-face @c Query — host's concrete type @p C is visible, so the answer folds.
         *
         * Three-way fold per spec §4.1:
         *   - @c nullptr if @p p is null;
         *   - `static_cast<I*>(p)` when @c std::derived_from<C, I> — BOA, layout-correct upcast;
         *   - the address of an inline-aggregated @c InterfaceFacade<I, X> subobject when one is
         *     reachable through @p C's NSDMs (and their bases) with @c std::derived_from<C, X>;
         *   - otherwise falls through to the dynamic kernel via @c QueryDynamicRaw on a
         *     @c RootObject* upcast — the safety net for cases the static probe cannot decide.
         *
         * No table lookup, no vcall, no atomic load on the BOA / inline-facade paths — every arm
         * is a constant expression the optimiser sees through the @c if @c constexpr ladder.
         */
        template<InterfaceClass I, class C>
        [[nodiscard]] constexpr I* Query(C* p) noexcept {
            if (p == nullptr) {
                return nullptr;
            }
            if constexpr (std::derived_from<C, I>) {
                return static_cast<I*>(p);
            } else if constexpr (Detail::HasInlineFacadeFor<C, I>()) {
                return Detail::InlineFacadeAddress<I, C>(p);
            } else if constexpr (std::derived_from<C, RootObject> && std::derived_from<I, RootObject>) {
                // Type-erased fall-through: the host is a RootObject (so the kernel can take it)
                // *and* I derives from RootObject (so the kernel's RootObject* return value can
                // safely static_cast back to I*). Plain interfaces that don't inherit RootObject
                // skip this arm — a future T13 convenience layer over @c Yuki::Query may close
                // that gap once the kernel reshapes its return contract.
                return static_cast<I*>(Detail::QueryDynamicRaw(
                        static_cast<RootObject*>(p), IidOf<I>()));
            } else {
                return nullptr;
            }
        }

        /// @copydoc Query
        template<InterfaceClass I, class C>
        [[nodiscard]] constexpr const I* Query(const C* p) noexcept {
            if (p == nullptr) {
                return nullptr;
            }
            if constexpr (std::derived_from<C, I>) {
                return static_cast<const I*>(p);
            } else if constexpr (Detail::HasInlineFacadeFor<C, I>()) {
                return Detail::InlineFacadeAddress<I, C>(const_cast<C*>(p));
            } else if constexpr (std::derived_from<C, RootObject> && std::derived_from<I, RootObject>) {
                return static_cast<const I*>(Detail::QueryDynamicRaw(
                        const_cast<RootObject*>(static_cast<const RootObject*>(p)), IidOf<I>()));
            } else {
                return nullptr;
            }
        }

        // =====================================================================
        // Convenience layer (Task 13, spec §1.3)
        // =====================================================================
        //
        // Thin wrappers over the dynamic kernel + role-walk helpers — no new
        // mechanism, just named projections of what @ref QueryDynamicRawPolicy,
        // @ref Underlying, @ref Nucleus, @ref Facades and @ref FacadeListLookup
        // already do. Spec §1.3 lists each as a one-liner; the bodies below
        // match those derivations modulo the post-T11/T12 plumbing names.

        /**
         * @brief @c Provider<I>(p) — the @c RootObject whose state actually answers @p I for @p p.
         *
         * Spec §4.2: `Provider := Underlying ∘ Query`. The kernel arm fixes which object backs the
         * answer — DirectCast/InlineFacade lands on a real subobject of the nucleus, the singleton
         * arm reads a published @c RootObject*, the resolver arm returns an Extension instance —
         * and @ref Underlying then walks any Interface/Imposter/Bridge facade layers down to the
         * underlying object the facade was built over. For the typical Implementation/Extension
         * answer this is identity (those roles are already "bottom"), so the call collapses to a
         * single kernel invocation.
         *
         * @c C is parameterised so call sites with a concrete host type compile without a manual
         * upcast, but the body only handles the @c std::derived_from<C, RootObject> branch — that
         * is the only case where the kernel can take the host and @ref Underlying can take its
         * return value. For non-RootObject hosts (e.g. plain @ref InterfaceFacade subobjects) the
         * call is intentionally @c nullptr; T11's static face does not project facade subobjects
         * back to their hosts, so there is no path to a @c RootObject* to drive @ref Underlying.
         */
        template<InterfaceClass I, class C = RootObject>
        [[nodiscard]] inline RootObject* Provider(C* p) noexcept {
            if constexpr (std::derived_from<C, RootObject>) {
                RootObject* hit = QueryDynamicRaw(static_cast<RootObject*>(p), IidOf<I>());
                return (hit != nullptr) ? Underlying(hit) : nullptr;
            } else {
                (void)p;
                return nullptr;
            }
        }

        /**
         * @brief @c IsMaterialized<E>(p) — has the lazy Extension @p E already been published onto
         *        @p p's facade chain?
         *
         * Spec §6.4: a single @ref FacadeListLookup keyed on @c IidOf<E>(), the self-iid the
         * resolver (and @ref Reify) stamp at materialise time. No kernel call — even the dispatch
         * snapshot is irrelevant; the question is purely about chain membership. Steers through
         * @ref Nucleus first so a query handed an Interface/Imposter/Bridge facade (or an
         * Extension on some other nucleus) re-targets to the right facade list.
         */
        template<ExtensionClass E>
        [[nodiscard]] inline bool IsMaterialized(RootObject* p) noexcept {
            RootObject* n = Nucleus(p);
            if (n == nullptr) {
                return false;
            }
            FacadeListHead* head = Facades(n);
            if (head == nullptr) {
                return false;
            }
            return FacadeListLookup(*head, IidOf<E>()) != nullptr;
        }

        /**
         * @brief @c Reify<E>(p) — force-materialise the lazy Extension @p E onto @p p's nucleus.
         *
         * Spec §6.2: the same @ref Detail::MaterializeIntoImpl primitive the
         * @c SideTableResolver kernel arm invokes on a miss, but called directly without going
         * through any specific Interface iid. Idempotent: @ref Detail::MaterializeIntoImpl bails
         * on the FacadeListLookup hit before allocating, so a second @c Reify is a no-op.
         * Stateless / aggregate Extensions silently no-op (the @c std::is_base_of_v<RootObject, E>
         * gate inside @ref Detail::MaterializeIntoImpl).
         */
        template<ExtensionClass E>
        inline void Reify(RootObject* p) noexcept {
            RootObject* n = Nucleus(p);
            if (n == nullptr) {
                return;
            }
            Detail::MaterializeIntoImpl<E>(*n);
        }

        /**
         * @brief @c Materialized<I>(p) — observation-only @c Query that never materialises.
         *
         * Spec §6.4: the @c Materialize::No instantiation of @ref QueryDynamicRawPolicy. Every
         * non-resolver arm answers exactly as @ref QueryDynamicRaw would (those arms have nothing
         * to materialise); the @c SideTableResolver arm probes the facade chain directly instead
         * of firing the resolver, so a lazy + unmaterialised Extension surfaces as @c nullptr.
         * Useful for diagnostics and for fast-path early-outs that must not pay the
         * materialisation cost.
         */
        template<InterfaceClass I>
        [[nodiscard]] inline RootObject* Materialized(RootObject* p) noexcept {
            return QueryDynamicRawPolicy<Materialize::No>(p, IidOf<I>());
        }

    } // namespace RT

    /** @cond INTERNAL */
    namespace Detail {

        // Definition of the forward declaration above. Lives at namespace scope (out of @c RT)
        // because the legacy public @c Yuki::Query<I>(RootObject*) overloads name it with the
        // @c Detail:: qualifier and we keep their call sites unchanged. The forward in this
        // header was needed so those overloads compiled before @c RT was even opened; the body
        // here closes that loop now that @ref RT::QueryDynamicRaw is defined.
        inline RootObject* QueryDynamicRaw(RootObject* p, Iid iid) noexcept {
            return RT::QueryDynamicRaw(p, iid);
        }

    } // namespace Detail
    /** @endcond */

} // namespace Yuki
