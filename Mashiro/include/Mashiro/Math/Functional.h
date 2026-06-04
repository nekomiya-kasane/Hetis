/**
 * @file Functional.h
 * @brief Point-free functional combinators and type-level metaprogramming.
 *
 * A Wolfram-Language-inspired functional layer that maps Mathematica's terse
 * operators onto C++'s overloadable surface. Because C++ cannot mint new tokens
 * (`/@`, `@@`, `@@@`, …), every operator is realised through the 2025-idiomatic
 * *pipeable closure* (range-adaptor) pattern: a single universal `operator|`
 * feeds a value into a combinator closure, and combinator factories produce those
 * closures.
 *
 * **Operator dictionary**
 *
 * | Mathematica            | Mashiro                         | Semantics            |
 * |------------------------|---------------------------------|----------------------|
 * | `x // f` / `f @ x`     | `x \| f` / `Apply(f, x)`        | application          |
 * | `f /@ list`            | `list \| Map(f)`                | map                  |
 * | `f @@ args`            | `Apply(f, tuple)` / `t\|Apply(f)`| spread-apply        |
 * | `f @@@ list`           | `list \| MapApply(f)`           | map + spread         |
 * | `Select[l,p]`          | `list \| Filter(p)`             | filter (lazy)        |
 * | `Fold[f,x,l]`          | `list \| Fold(f, x)`            | left fold            |
 * | `f @* g`               | `Compose(f, g)`                 | right-to-left ∘      |
 * | `f /\* g`              | `Then(f, g)`                    | left-to-right ∘      |
 * | `a -> b`               | `Rule(a, b)` / `"a"_k = b`      | rule / field         |
 * | `<\| "k"->v \|>`       | `Assoc("k"_k = v, …)`           | association          |
 *
 * **Three evaluation domains**
 * - *Runtime values* — tuples, aggregates (`Vec`), ranges; zero overhead via
 *   inlined closures, `std::apply`, fold expressions.
 * - *Compile-time values* — the very same code is `constexpr`.
 * - *Type level* — `Mashiro::Traits::TypeList` and its combinators (`MapT`,
 *   `FilterT`, `FoldT`, `ApplyT`, …), with a P2996 reflection bridge.
 *
 * **Container policy.** `Map`/`Filter` are *lazy* on `std::ranges` (returning
 * `transform_view`/`filter_view`, maximally cache-friendly and composable) and
 * *eager* on fixed-size carriers: a tuple maps to a `std::tuple` (`List` head),
 * an aggregate reconstructs its **own type** (`f /@ g[a,b]` ⇒ `g[f a, f b]`).
 *
 * Namespaces: `Mashiro::Meta` (value combinators), `Mashiro::Traits` (type level).
 *
 * @ingroup Math
 */
#pragma once

#include "Mashiro/Core/FixedString.h"
#include "Mashiro/Core/TypeTraits.h"

#include <array>
#include <concepts>
#include <functional>
#include <meta>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Mashiro::Meta {

    // =====================================================================
    // Detection concepts
    // =====================================================================

    using Traits::TupleLike;

    /// @brief A class type usable as a fixed-size record (reconstructed by `Map`).
    template<typename T>
    concept Aggregate = std::is_aggregate_v<std::remove_cvref_t<T>>;

    /// @brief An iterable that is *not* tuple-like — eligible for lazy view adaptors.
    template<typename T>
    concept LazyRange = std::ranges::input_range<T> && !TupleLike<std::remove_cvref_t<T>>;

    /// @brief A reflectable record: a class that is neither tuple-like nor a range.
    template<typename T>
    concept Record = std::is_class_v<std::remove_cvref_t<T>> &&
                     !TupleLike<std::remove_cvref_t<T>> && !std::ranges::range<T>;

    /** @cond INTERNAL */
    namespace Detail {


    } // namespace Detail
    /** @endcond */

    // =====================================================================
    // Closure core & the pipe operator
    // =====================================================================

    /**
     * @brief A pipeable, composable callable wrapper.
     *
     * Holds an arbitrary callable @p F and forwards through a single deducing-this
     * `operator()`, eliminating the const/&/&& overload quartet. Every combinator
     * factory returns an `Adaptor`, making results uniformly pipeable via `operator|`.
     *
     * @tparam F Wrapped callable type.
     */
    template<typename F>
    struct Adaptor {
        F fn; ///< The wrapped callable.

        /// @brief Forward all arguments to the wrapped callable.
        template<typename Self, typename... Xs>
        constexpr decltype(auto) operator()(this Self&& self, Xs&&... xs) {
            return std::invoke(std::forward<Self>(self).fn, std::forward<Xs>(xs)...);
        }
    };
    template<typename F>
    Adaptor(F) -> Adaptor<F>;

    /** @cond INTERNAL */
    namespace Detail {

        template<typename T>
        struct IsAdaptor : std::false_type {};

        template<typename F>
        struct IsAdaptor<Adaptor<F>> : std::true_type {};

    } // namespace Detail
    /** @endcond */

    /// @brief A type that is not an `Adaptor` (guards the pipe against `adaptor | adaptor`).
    template<typename T>
    concept NonAdaptor = !Detail::IsAdaptor<std::remove_cvref_t<T>>::value;

    /**
     * @brief Postfix application: `x | f` ≡ `f(x)` (Mathematica `x // f`).
     *
     * Found by ADL on the right-hand `Adaptor`. The left operand is constrained to
     * be a non-adaptor so closure composition stays the job of `Compose`/`Then`.
     */
    template<NonAdaptor X, typename F>
    [[nodiscard]] constexpr decltype(auto) operator|(X&& x, const Adaptor<F>& a) {
        return a(std::forward<X>(x));
    }

    /// @brief The identity combinator: `x | Identity` ≡ `x`.
    inline constexpr Adaptor Identity{[](auto&& x) -> decltype(auto) {
        return std::forward<decltype(x)>(x);
    }};

    /// @brief Wrap any callable into a pipeable, composable `Adaptor`.
    template<typename F>
    [[nodiscard]] constexpr auto Fn(F&& f) {
        return Adaptor{std::forward<F>(f)};
    }

    /// @brief Constant function: ignores its arguments and returns @p v.
    template<typename V>
    [[nodiscard]] constexpr auto Constant(V v) {
        return Adaptor{[v = std::move(v)](auto&&...) { return v; }};
    }

    // =====================================================================
    // Map / Filter / Fold
    // =====================================================================

    /** @cond INTERNAL */
    namespace Detail {

        template<typename T, typename F, typename X, size_t... I>
        constexpr T MapRecordImpl(F& f, X&& xs, std::index_sequence<I...>) {
            return T{f(std::forward<X>(xs).[:Traits::Members<T>[I]:])...};
        }

        template<typename F, typename X>
        constexpr auto MapRecord(F& f, X&& xs) {
            using T = std::remove_cvref_t<X>;
            return MapRecordImpl<T>(f, std::forward<X>(xs),
                                    std::make_index_sequence<Traits::Members<T>.size()>{});
        }

        template<size_t I, size_t N, typename F, typename Acc, typename Tuple>
        constexpr auto FoldFrom(F& f, Acc acc, Tuple&& t) {
            if constexpr (I == N) {
                return acc;
            } else {
                return FoldFrom<I + 1, N>(
                    f, f(std::move(acc), std::get<I>(std::forward<Tuple>(t))),
                    std::forward<Tuple>(t));
            }
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Map @p f over a structure (Mathematica `f /@ expr`).
     *
     * Dispatch by the operand: a range yields a lazy `transform_view`; a tuple
     * yields a `std::tuple`; an aggregate reconstructs **its own type** by applying
     * @p f to each member (the head is preserved, à la `f /@ g[a,b] == g[f a,f b]`).
     */
    template<typename F>
    [[nodiscard]] constexpr auto Map(F f) {
        return Adaptor{[f = std::move(f)](auto&& xs) -> decltype(auto) {
            using X = std::remove_cvref_t<decltype(xs)>;
            if constexpr (LazyRange<X>) {
                return std::views::transform(std::forward<decltype(xs)>(xs), f);
            } else if constexpr (TupleLike<X>) {
                return std::apply(
                    [&](auto&&... es) {
                        return std::tuple{f(std::forward<decltype(es)>(es))...};
                    },
                    std::forward<decltype(xs)>(xs));
            } else {
                return Detail::MapRecord(f, std::forward<decltype(xs)>(xs));
            }
        }};
    }

    /**
     * @brief Keep elements satisfying @p pred (Mathematica `Select`).
     *
     * Returns a lazy `std::ranges::filter_view`. Filtering a heterogeneous tuple by
     * a runtime predicate is ill-typed; use `Traits::FilterT` for the type level.
     */
    template<typename P>
    [[nodiscard]] constexpr auto Filter(P pred) {
        return Adaptor{[pred = std::move(pred)](auto&& xs) {
            return std::views::filter(std::forward<decltype(xs)>(xs), pred);
        }};
    }

    /**
     * @brief Left fold with seed (Mathematica `Fold[f, init, list]`).
     *
     * Over a tuple the fold is heterogeneous (the accumulator type may change at
     * each step, resolved by recursion); over a range it is an in-place loop.
     */
    template<typename F, typename Init>
    [[nodiscard]] constexpr auto Fold(F f, Init init) {
        return Adaptor{[f = std::move(f), init = std::move(init)](auto&& xs) {
            using X = std::remove_cvref_t<decltype(xs)>;
            if constexpr (TupleLike<X>) {
                return Detail::FoldFrom<0, std::tuple_size_v<X>>(f, init,
                                                                 std::forward<decltype(xs)>(xs));
            } else {
                auto acc = init;
                for (auto&& e : xs) {
                    acc = f(std::move(acc), e);
                }
                return acc;
            }
        }};
    }

    // =====================================================================
    // Apply / MapApply / MapThread
    // =====================================================================

    /// @brief Spread a tuple as arguments: `t | Apply(f)` ≡ `std::apply(f, t)`.
    template<typename F>
    [[nodiscard]] constexpr auto Apply(F f) {
        return Adaptor{[f = std::move(f)](auto&& t) -> decltype(auto) {
            return std::apply(f, std::forward<decltype(t)>(t));
        }};
    }

    /// @brief Eager spread-apply: `Apply(f, t)` ≡ `f(get<0>(t), get<1>(t), …)`.
    template<typename F, typename T>
        requires TupleLike<std::remove_cvref_t<T>>
    [[nodiscard]] constexpr decltype(auto) Apply(F&& f, T&& t) {
        return std::apply(std::forward<F>(f), std::forward<T>(t));
    }

    /// @brief Map then spread (Mathematica `f @@@ list`): apply @p f to each sub-tuple.
    template<typename F>
    [[nodiscard]] constexpr auto MapApply(F f) {
        return Map([f = std::move(f)](auto&& sub) -> decltype(auto) {
            return std::apply(f, std::forward<decltype(sub)>(sub));
        });
    }

    /**
     * @brief Zip-apply across equal-length tuples (Mathematica `MapThread`).
     *
     * `MapThread(f, {a0,a1}, {b0,b1})` ⇒ `{f(a0,b0), f(a1,b1)}`.
     */
    template<typename F, typename T0, typename... Ts>
        requires TupleLike<std::remove_cvref_t<T0>>
    [[nodiscard]] constexpr auto MapThread(F f, T0&& t0, Ts&&... ts) {
        constexpr size_t N = std::tuple_size_v<std::remove_cvref_t<T0>>;
        auto atI = [&]<size_t I>(std::integral_constant<size_t, I>) {
            return f(std::get<I>(t0), std::get<I>(ts)...);
        };
        return [&]<size_t... I>(std::index_sequence<I...>) {
            return std::tuple{atI(std::integral_constant<size_t, I>{})...};
        }(std::make_index_sequence<N>{});
    }

    // =====================================================================
    // Composition & partial application
    // =====================================================================

    /// @brief Right-to-left composition (Mathematica `@*`): `Compose(f,g)(x) == f(g(x))`.
    template<typename F>
    [[nodiscard]] constexpr auto Compose(F f) {
        return Adaptor{std::move(f)};
    }
    template<typename F, typename... Fs>
    [[nodiscard]] constexpr auto Compose(F f, Fs... fs) {
        return Adaptor{[f = std::move(f), tail = Compose(std::move(fs)...)](
                           auto&&... xs) -> decltype(auto) {
            return f(tail(std::forward<decltype(xs)>(xs)...));
        }};
    }

    /// @brief Left-to-right composition (Mathematica `/*`): `Then(f,g)(x) == g(f(x))`.
    template<typename F>
    [[nodiscard]] constexpr auto Then(F f) {
        return Adaptor{std::move(f)};
    }
    template<typename F, typename... Fs>
    [[nodiscard]] constexpr auto Then(F f, Fs... fs) {
        return Adaptor{[f = std::move(f), tail = Then(std::move(fs)...)](
                           auto&&... xs) -> decltype(auto) {
            return tail(f(std::forward<decltype(xs)>(xs)...));
        }};
    }

    /// @brief Partial application (currying) of leading arguments.
    template<typename F, typename... A>
    [[nodiscard]] constexpr auto Bind(F&& f, A&&... a) {
        return Adaptor{std::bind_front(std::forward<F>(f), std::forward<A>(a)...)};
    }

    // =====================================================================
    // Rules, compile-time string keys, and Associations
    // =====================================================================

    using Mashiro::FixedString;

    /// @brief An immutable runtime key→value rule (Mathematica `a -> b`).
    template<typename K, typename V>
    struct Rule {
        K key;   ///< Left-hand side.
        V value; ///< Right-hand side.
    };
    template<typename K, typename V>
    Rule(K, V) -> Rule<K, V>;

    template<FixedString K, typename V>
    struct Field; // forward

    /**
     * @brief Compile-time key tag produced by the `_k` literal.
     *
     * `"x"_k = v` yields a `Field<"x", V>`; `assoc["x"_k]` resolves to a compile-time
     * index lookup.
     * @tparam K The key string.
     */
    template<FixedString K>
    struct KeyTag {
        static constexpr auto key = K; ///< The carried key.

        /// @brief Bind this key to a value, producing a `Field`.
        template<typename V>
        [[nodiscard]] constexpr auto operator=(V&& v) const {
            return Field<K, std::remove_cvref_t<V>>{std::forward<V>(v)};
        }
    };

    /// @brief String literal operator: `"name"_k` → `KeyTag<"name">`.
    template<FixedString S>
    [[nodiscard]] constexpr KeyTag<S> operator""_k() {
        return {};
    }

    /**
     * @brief A single keyed value within an `Assoc`.
     * @tparam K Compile-time key.
     * @tparam V Value type.
     */
    template<FixedString K, typename V>
    struct Field {
        static constexpr auto key = K; ///< Compile-time key.
        V value;                       ///< Stored value.
    };

    /**
     * @brief Heterogeneous association keyed by compile-time strings (Mathematica `<|…|>`).
     *
     * Construct with `Assoc("x"_k = 1, "y"_k = 2.0)`. Lookup is a fully compile-time
     * index resolution: `a["x"_k]` and `a.Get<"x">()` are zero-cost.
     *
     * @tparam Fs The `Field` members.
     */
    template<typename... Fs>
    struct Assoc {
        std::tuple<Fs...> fields; ///< The stored fields.

        constexpr explicit Assoc(Fs... fs) : fields{fs...} {}

        /// @brief Index of key @p K within the field pack, or `size_t(-1)`.
        template<FixedString K>
        static consteval size_t IndexOfKey() {
            if constexpr (sizeof...(Fs) == 0) {
                return size_t(-1);
            } else {
                const bool match[] = {(Fs::key == K)...};
                for (size_t i = 0; i < sizeof...(Fs); ++i) {
                    if (match[i]) {
                        return i;
                    }
                }
                return size_t(-1);
            }
        }

        /// @brief Compile-time keyed access.
        template<FixedString K>
        [[nodiscard]] constexpr decltype(auto) Get(this auto&& self) {
            constexpr size_t i = IndexOfKey<K>();
            static_assert(i != size_t(-1), "Assoc: key not found");
            return (std::get<i>(std::forward<decltype(self)>(self).fields).value);
        }

        /// @brief `a["k"_k]` sugar for `Get<"k">()`.
        template<FixedString K>
        [[nodiscard]] constexpr decltype(auto) operator[](this auto&& self, KeyTag<K>) {
            return std::forward<decltype(self)>(self).template Get<K>();
        }

        /// @brief Whether the association contains key @p K.
        template<FixedString K>
        static constexpr bool Contains() {
            return IndexOfKey<K>() != size_t(-1);
        }
    };
    template<typename... Fs>
    Assoc(Fs...) -> Assoc<Fs...>;

    // =====================================================================
    // Reflection bridges (record ⇄ tuple, field iteration)
    // =====================================================================

    /// @brief Convert an aggregate/record into a `std::tuple` of its member values.
    template<typename T>
    [[nodiscard]] constexpr auto ToTuple(T&& obj) {
        using U = std::remove_cvref_t<T>;
        return [&]<size_t... I>(std::index_sequence<I...>) {
            return std::tuple{std::forward<T>(obj).[:Traits::Members<U>[I]:]...};
        }(std::make_index_sequence<Traits::Members<U>.size()>{});
    }

    /**
     * @brief Apply @p f to every data member as `f(name, value)`.
     *
     * @p name is the member identifier (`std::string_view`); @p value is the member
     * reference. Reflection-driven and fully unrolled via `template for`.
     */
    template<typename T, typename F>
    constexpr void ForEachField(T&& obj, F&& f) {
        using U = std::remove_cvref_t<T>;
        template for (constexpr auto m : std::define_static_array(
                          std::meta::nonstatic_data_members_of(
                              ^^U, std::meta::access_context::unchecked()))) {
            f(std::meta::identifier_of(m), std::forward<T>(obj).[:m:]);
        }
    }

} // namespace Mashiro::Meta

namespace Mashiro::Traits {

    // =====================================================================
    // Type-level list and combinators
    // =====================================================================

    /// @brief A compile-time list of types — the type-level analogue of a tuple.
    template<typename... Ts>
    struct TypeList {
        static constexpr size_t size = sizeof...(Ts); ///< Number of elements.
    };

    /// @brief Number of elements in @p L.
    template<template<typename...> class L, typename... Ts>
    inline constexpr size_t Length = L<Ts...>::size;

    /** @cond INTERNAL */
    namespace Detail {

        template<typename L, size_t I>
        struct AtT;
        template<typename... Ts, size_t I>
        struct AtT<TypeList<Ts...>, I> {
            using type = Ts...[I];
        };

        template<typename L>
        struct TailT;
        template<typename T, typename... Ts>
        struct TailT<TypeList<T, Ts...>> {
            using type = TypeList<Ts...>;
        };

        template<typename...>
        struct ConcatT;
        template<>
        struct ConcatT<> {
            using type = TypeList<>;
        };
        template<typename... Ts>
        struct ConcatT<TypeList<Ts...>> {
            using type = TypeList<Ts...>;
        };
        template<typename... As, typename... Bs, typename... Rest>
        struct ConcatT<TypeList<As...>, TypeList<Bs...>, Rest...> {
            using type = typename ConcatT<TypeList<As..., Bs...>, Rest...>::type;
        };

        template<template<typename> typename F, typename L>
        struct MapTT;
        template<template<typename> typename F, typename... Ts>
        struct MapTT<F, TypeList<Ts...>> {
            using type = TypeList<F<Ts>...>;
        };

        template<template<typename> typename Pred, typename L>
        struct FilterTT;
        template<template<typename> typename Pred>
        struct FilterTT<Pred, TypeList<>> {
            using type = TypeList<>;
        };
        template<template<typename> typename Pred, typename T, typename... Ts>
        struct FilterTT<Pred, TypeList<T, Ts...>> {
            using tail = typename FilterTT<Pred, TypeList<Ts...>>::type;
            using type =
                std::conditional_t<Pred<T>::value, typename ConcatT<TypeList<T>, tail>::type, tail>;
        };

        template<template<typename...> typename F, typename L>
        struct ApplyTT;
        template<template<typename...> typename F, typename... Ts>
        struct ApplyTT<F, TypeList<Ts...>> {
            using type = F<Ts...>;
        };

        template<template<typename, typename> typename Op, typename Acc, typename L>
        struct FoldTT;
        template<template<typename, typename> typename Op, typename Acc>
        struct FoldTT<Op, Acc, TypeList<>> {
            using type = Acc;
        };
        template<template<typename, typename> typename Op, typename Acc, typename T, typename... Ts>
        struct FoldTT<Op, Acc, TypeList<T, Ts...>> {
            using type = typename FoldTT<Op, Op<Acc, T>, TypeList<Ts...>>::type;
        };

        template<typename T, typename... Ts>
        consteval size_t IndexOfImpl() {
            if constexpr (sizeof...(Ts) == 0) {
                return size_t(-1);
            } else {
                const bool match[] = {std::is_same_v<T, Ts>...};
                for (size_t i = 0; i < sizeof...(Ts); ++i) {
                    if (match[i]) {
                        return i;
                    }
                }
                return size_t(-1);
            }
        }
        template<typename L, typename T>
        struct IndexOfT;
        template<typename T, typename... Ts>
        struct IndexOfT<TypeList<Ts...>, T> {
            static constexpr size_t value = IndexOfImpl<T, Ts...>();
        };

        template<typename T>
        consteval std::meta::info TypeListReflOf() {
            std::vector<std::meta::info> types;
            for (auto m : std::meta::nonstatic_data_members_of(
                     ^^T, std::meta::access_context::unchecked())) {
                types.push_back(std::meta::type_of(m));
            }
            return std::meta::substitute(^^TypeList, types);
        }

    } // namespace Detail
    /** @endcond */

    /// @brief The @p I-th element of @p L (C++26 pack indexing).
    template<typename L, size_t I>
    using At = typename Detail::AtT<L, I>::type;

    /// @brief First element of @p L.
    template<typename L>
    using Head = At<L, 0>;

    /// @brief All but the first element of @p L.
    template<typename L>
    using Tail = typename Detail::TailT<L>::type;

    /// @brief Concatenate any number of `TypeList`s.
    template<typename... Ls>
    using Concat = typename Detail::ConcatT<Ls...>::type;

    /// @brief Apply unary metafunction @p F to each element: `TypeList<F<Ts>...>`.
    template<template<typename> typename F, typename L>
    using MapT = typename Detail::MapTT<F, L>::type;

    /// @brief Keep elements for which `Pred<T>::value` holds.
    template<template<typename> typename Pred, typename L>
    using FilterT = typename Detail::FilterTT<Pred, L>::type;

    /// @brief Instantiate variadic template @p F with the list elements: `F<Ts...>`.
    template<template<typename...> typename F, typename L>
    using ApplyT = typename Detail::ApplyTT<F, L>::type;

    /// @brief Left fold with binary metafunction `Op<Acc, T>` and seed @p Init.
    template<template<typename, typename> typename Op, typename Init, typename L>
    using FoldT = typename Detail::FoldTT<Op, Init, L>::type;

    /// @brief Index of the first occurrence of @p T in @p L, or `size_t(-1)`.
    template<typename L, typename T>
    inline constexpr size_t IndexOf = Detail::IndexOfT<L, T>::value;

    /// @brief Whether @p L contains type @p T.
    template<typename L, typename T>
    inline constexpr bool Contains = (IndexOf<L, T> != size_t(-1));

    /// @brief Reflection bridge: a `TypeList` of @p T's data-member types.
    template<typename T>
    using ToTypeList = [:Detail::TypeListReflOf<T>():];

} // namespace Mashiro::Traits
