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
#include <expected>
#include <functional>
#include <meta>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Mashiro {

    namespace Traits {

        /// @brief An iterable that is *not* tuple-like — eligible for lazy view adaptors.
        template<typename T>
        concept LazyRange = std::ranges::input_range<T> && !Traits::TupleLike<std::remove_cvref_t<T>>;

        /// @brief A reflectable record: a class that is neither tuple-like nor a range.
        template<typename T>
        concept Record = std::is_class_v<std::remove_cvref_t<T>> &&
                         !Traits::TupleLike<std::remove_cvref_t<T>> && !std::ranges::range<T>;

    }

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

    namespace Traits {

        /** @cond INTERNAL */
        namespace Detail {

            template<typename T>
            consteval bool IsAdaptorOf() {
                constexpr auto r = ^^T;
                return std::meta::has_template_arguments(r) &&
                    std::meta::template_of(r) == ^^Adaptor;
            }

        } // namespace Detail
        /** @endcond */

        /// @brief A type that is not an `Adaptor` (guards the pipe against `adaptor | adaptor`).
        template<typename T>
        concept NonAdaptor = !Detail::IsAdaptorOf<std::remove_cvref_t<T>>();

    }

    /**
     * @brief Postfix application: `x | f` ≡ `f(x)` (Mathematica `x // f`).
     *
     * Found by ADL on the right-hand `Adaptor`. The left operand is constrained to
     * be a non-adaptor so closure composition stays the job of `Compose`/`Then`.
     */
    template<Traits::NonAdaptor X, typename F>
    [[nodiscard]] constexpr decltype(auto) operator|(X&& x, const Adaptor<F>& a) {
        return a(std::forward<X>(x));
    }

    /// @brief Rvalue overload: enables move-only closures in the pipe.
    template<Traits::NonAdaptor X, typename F>
    [[nodiscard]] constexpr decltype(auto) operator|(X&& x, Adaptor<F>&& a) {
        return std::move(a)(std::forward<X>(x));
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

        template<typename F, typename X>
        constexpr auto MapRecord(F& f, X&& xs) {
            using T = std::remove_cvref_t<X>;
            return [&]<size_t... I>(std::index_sequence<I...>) {
                return T{f(std::forward<X>(xs).[:Traits::Members<T>[I]:])...};
            }(std::make_index_sequence<Traits::Members<T>.size()>{});
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
            if constexpr (Traits::LazyRange<X>) {
                return std::views::transform(std::forward<decltype(xs)>(xs), f);
            } else if constexpr (Traits::TupleLike<X>) {
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
            if constexpr (Traits::TupleLike<X>) {
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
        requires Traits::TupleLike<std::remove_cvref_t<T>>
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
        requires Traits::TupleLike<std::remove_cvref_t<T0>>
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

    // =====================================================================
    // Fallible-type pipe adaptors (Result<T> / optional<T>)
    // =====================================================================

    /** @cond INTERNAL */
    namespace Detail {

        template<typename T>
        struct IsExpected : std::false_type {};
        template<typename V, typename E>
        struct IsExpected<std::expected<V, E>> : std::true_type {};

        template<typename T>
        struct IsOptional : std::false_type {};
        template<typename V>
        struct IsOptional<std::optional<V>> : std::true_type {};

    } // namespace Detail
    /** @endcond */

    /// @brief A type that supports monadic chaining (std::expected or std::optional).
    template<typename T>
    concept Fallible = Detail::IsExpected<std::remove_cvref_t<T>>::value ||
                       Detail::IsOptional<std::remove_cvref_t<T>>::value;

    /**
     * @brief Monadic bind: `result | AndThen(f)` ≡ `result.and_then(f)`.
     *
     * `f` receives the success value and must return a Fallible type.
     * On error/nullopt the original error propagates unchanged.
     *
     * @code
     * auto pipeline = LoadFile(path)
     *               | AndThen(Parse)
     *               | AndThen(Validate);
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto AndThen(F f) {
        return Adaptor{[f = std::move(f)](auto&& m) -> decltype(auto)
            requires Fallible<decltype(m)>
        {
            return std::forward<decltype(m)>(m).and_then(f);
        }};
    }

    /**
     * @brief Functor map: `result | Transform(f)` ≡ `result.transform(f)`.
     *
     * `f` receives the success value; its return is wrapped in the same Fallible container.
     */
    template<typename F>
    [[nodiscard]] constexpr auto Transform(F f) {
        return Adaptor{[f = std::move(f)](auto&& m) -> decltype(auto)
            requires Fallible<decltype(m)>
        {
            return std::forward<decltype(m)>(m).transform(f);
        }};
    }

    /**
     * @brief Error recovery: `result | OrElse(f)` ≡ `result.or_else(f)`.
     *
     * `f` receives the error and must return a Fallible of the same value type.
     * For `std::optional`, `f` takes no arguments.
     */
    template<typename F>
    [[nodiscard]] constexpr auto OrElse(F f) {
        return Adaptor{[f = std::move(f)](auto&& m) -> decltype(auto)
            requires Fallible<decltype(m)>
        {
            return std::forward<decltype(m)>(m).or_else(f);
        }};
    }

    /**
     * @brief Unwrap with default: `result | ValueOr(fallback)`.
     *
     * Returns the success value, or `fallback` if in error/nullopt state.
     */
    template<typename V>
    [[nodiscard]] constexpr auto ValueOr(V fallback) {
        return Adaptor{[fallback = std::move(fallback)](auto&& m)
            requires Fallible<decltype(m)>
        {
            return std::forward<decltype(m)>(m).value_or(fallback);
        }};
    }

    // =====================================================================
    // Scan (prefix accumulation / running fold)
    // =====================================================================

    /**
     * @brief Prefix accumulation: `range | Scan(f, init)`.
     *
     * Materialises a `std::vector` of running accumulations: element `i` is
     * `fold(range[0..i], f, init)`. The first output is `f(init, range[0])`.
     *
     * This is intentionally **eager** — scan requires stateful sequential
     * dependency (element `i` depends on `i-1`), which is inherently
     * non-parallelisable and cannot benefit from lazy view composition.
     * Materialising into a contiguous `vector` is the cache-optimal choice.
     *
     * @code
     * auto sums = std::vector{1,2,3,4} | Scan([](int a, int b){ return a+b; }, 0);
     * // yields: {1, 3, 6, 10}
     * @endcode
     */
    template<typename F, typename Init>
    [[nodiscard]] constexpr auto Scan(F f, Init init) {
        return Adaptor{[f = std::move(f), init = std::move(init)](auto&& xs) {
            using Acc = std::remove_cvref_t<Init>;
            std::vector<Acc> result;
            if constexpr (std::ranges::sized_range<std::remove_cvref_t<decltype(xs)>>) {
                result.reserve(std::ranges::size(xs));
            }
            Acc acc = init;
            for (auto&& elem : xs) {
                acc = f(std::move(acc), elem);
                result.push_back(acc);
            }
            return result;
        }};
    }

    // =====================================================================
    // Zip (multi-range co-iteration)
    // =====================================================================

    /**
     * @brief Zip two ranges into a view of pairs: `Zip(a, b)`.
     *
     * Returns `std::views::zip(a, b)`. Pipeable form: `a | ZipWith(b)`.
     *
     * @code
     * auto ids = std::vector{1, 2, 3};
     * auto names = std::vector{"a", "b", "c"};
     * for (auto [id, name] : Zip(ids, names)) { ... }
     * @endcode
     */
    template<std::ranges::input_range... Rs>
    [[nodiscard]] constexpr auto Zip(Rs&&... ranges) {
        return std::views::zip(std::forward<Rs>(ranges)...);
    }

    /**
     * @brief Pipeable zip: `range1 | ZipWith(range2)` → view of pairs.
     *
     * The right-hand range is captured; the left-hand range arrives via pipe.
     *
     * @code
     * auto paired = positions | ZipWith(velocities) | Map([](auto pv) { ... });
     * @endcode
     */
    template<std::ranges::input_range R>
    [[nodiscard]] constexpr auto ZipWith(R& other) {
        return Adaptor{[&other](auto&& lhs) {
            return std::views::zip(std::forward<decltype(lhs)>(lhs), other);
        }};
    }

    // =====================================================================
    // Enumerate (index + value)
    // =====================================================================

    /**
     * @brief Pipeable enumerate: `range | Enumerate()` → view of `(index, value)`.
     *
     * @code
     * for (auto [i, pos] : positions | Enumerate()) {
     *     fmt::print("[{}] = {}\n", i, pos);
     * }
     * @endcode
     */
    [[nodiscard]] constexpr auto Enumerate() {
        return Adaptor{[](auto&& xs) {
            return std::views::zip(
                std::views::iota(size_t{0}),
                std::forward<decltype(xs)>(xs));
        }};
    }

    /**
     * @brief Enumerate with a custom starting index.
     *
     * @code
     * for (auto [i, v] : data | Enumerate(100)) { ... } // i starts at 100
     * @endcode
     */
    template<std::integral I = size_t>
    [[nodiscard]] constexpr auto Enumerate(I start) {
        return Adaptor{[start](auto&& xs) {
            return std::views::zip(
                std::views::iota(start),
                std::forward<decltype(xs)>(xs));
        }};
    }

} // namespace Mashiro::Meta

