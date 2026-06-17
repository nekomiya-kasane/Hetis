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

        /// @brief Stub — the real dispatch lookup lands with the Task 12 kernel rewrite.
        ///
        /// TODO(task-12): rewrite kernel against DispatchSnapshot. The previous body iterated
        /// `links->dispatch` as a `std::span<const DispatchEntry>` with a per-entry `offset` arm;
        /// Task 3 swapped that span out for an atomically-published @ref DispatchSnapshot and
        /// reshaped @c DispatchEntry around a payload union. Rather than half-port the kernel here,
        /// the lookup is stubbed to @c nullptr so the build stays green — Task 12 owns the rewrite
        /// of @c QueryDynamicRaw against @c Detail::LookupEntry plus the new payload arms.
        inline const DispatchEntry* FindDispatch(const MetaClass& /*mc*/, Iid /*iid*/) noexcept {
            return nullptr;
        }

        /// @brief Type-erased dynamic Query implementation; the public templates wrap it for type.
        inline RootObject* QueryDynamicRaw(RootObject* p, Iid iid) noexcept {
            while (p != nullptr) {
                const MetaClass& mc = p->MetaClassDynamic();

                // Identity short-circuit: a Query for the host's own metaclass-iid returns p.
                if (mc.iid() == iid) {
                    return p;
                }

                // TODO(task-12): rewrite kernel against DispatchSnapshot — call the stub for now so
                // the build stays green; the real per-kind dispatch lands with Task 12.
                (void) FindDispatch(mc, iid);

                switch (p->TypeDynamic()) {
                    case ClassType::Implementation: {
                        // Cold facade chain.
                        if (FacadeListHead* facades = RT::Facades(p)) {
                            if (RootObject* facade = facades->Lookup(iid)) {
                                return facade;
                            }
                        }
                        return nullptr;  // exhausted; no further redirection.
                    }
                    case ClassType::Interface:
                    case ClassType::Imposter:
                    case ClassType::Bridge:
                        // Facade arms are transparent — re-query through the underlying object.
                        p = RT::Target(p);
                        continue;
                    case ClassType::Extension:
                        // "Augment, do not hide": fall back to the extendee.
                        p = RT::Extendee(p);
                        continue;
                    case ClassType::None:
                    default:
                        return nullptr;
                }
            }
            return nullptr;
        }

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

    } // namespace RT

} // namespace Yuki
