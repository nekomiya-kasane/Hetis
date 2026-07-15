/**
 * @file Functional.h
 * @brief Pipeable functional combinators, association helpers, and reflection bridges.
 * @ingroup Core
 *
 * @details Provides a small point-free functional layer built around pipeable adaptor closures. A value can be piped
 * into an @ref Adaptor with @c operator|, while adaptor factories such as @ref Map, @ref Filter, @ref Fold, and
 * @ref Compose build reusable callables. Range operands use lazy standard views where possible; tuple and record
 * operands are handled eagerly because their shape is known at compile time.
 */
#pragma once

#include "Sora/Platform.h"
#include "Sora/Core/FixedString.h"
#include "Sora/Core/Traits.h"

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

namespace Sora {

    namespace Concept {

        /** @brief Input range that is not tuple-like and is therefore eligible for lazy view adaptors. */
        template<typename T>
        concept LazyRange = std::ranges::input_range<T> && !Concept::TupleLikeClass<std::remove_cvref_t<T>>;

        /** @brief Reflectable record: class type that is neither tuple-like nor a range. */
        template<typename T>
        concept Record = std::is_class_v<std::remove_cvref_t<T>> && !Concept::TupleLikeClass<std::remove_cvref_t<T>> &&
                         !std::ranges::range<T>;

    } // namespace Concept

    /**
     * @brief Pipeable, composable callable wrapper.
     *
     * @details Holds an arbitrary callable and forwards all invocations to it. Every combinator factory in this file
     * returns an @ref Adaptor so the resulting object can be used on the right-hand side of @c operator|.
     *
     * @tparam F Wrapped callable type.
     */
    template<typename F>
    struct Adaptor {
        /** @brief Wrapped callable. */
        NO_UNIQUE_ADDRESS F fn;

        /**
         * @brief Forward all arguments to the wrapped callable.
         * @tparam Self Deduced object type.
         * @tparam Xs Argument types.
         * @param[in] self Adaptor object.
         * @param[in] xs Arguments forwarded to @ref fn.
         * @return Result of invoking @ref fn.
         */
        template<typename Self, typename... Xs>
            requires requires(Self&& self, Xs&&... xs) {
                std::invoke(std::forward<Self>(self).fn, std::forward<Xs>(xs)...);
            }
        [[gnu::always_inline]] constexpr decltype(auto) operator()(this Self&& self, Xs&&... xs)
            noexcept(noexcept(std::invoke(std::forward<Self>(self).fn, std::forward<Xs>(xs)...))) {
            return std::invoke(std::forward<Self>(self).fn, std::forward<Xs>(xs)...);
        }
    };

    /** @brief Deduction guide for @ref Adaptor. */
    template<typename F>
    Adaptor(F) -> Adaptor<F>;

    namespace Concept {

        /** @brief Type that is an instantiation of @ref Sora::Adaptor. */
        template<typename T>
        concept Adaptor = std::meta::has_template_arguments(^^std::remove_cvref_t<T>) &&
                          std::meta::template_of(^^std::remove_cvref_t<T>) == ^^Sora::Adaptor;

        /** @brief Type that is not an @ref Sora::Adaptor. */
        template<typename T>
        concept NonAdaptor = !Adaptor<T>;

    } // namespace Concept

    /**
     * @brief Pipe value @p x into adaptor @p a.
     * @param[in] x Value supplied as the adaptor's first argument.
     * @param[in] a Adaptor to invoke.
     * @return Result of calling @p a with @p x.
     */
    template<Concept::NonAdaptor X, typename F>
    [[nodiscard]] constexpr decltype(auto) operator|(X&& x, const Adaptor<F>& a) {
        return a(std::forward<X>(x));
    }

    /**
     * @brief Pipe value @p x into rvalue adaptor @p a.
     * @param[in] x Value supplied as the adaptor's first argument.
     * @param[in] a Rvalue adaptor to invoke.
     * @return Result of moving @p a and calling it with @p x.
     */
    template<Concept::NonAdaptor X, typename F>
    [[nodiscard]] constexpr decltype(auto) operator|(X&& x, Adaptor<F>&& a) {
        return std::move(a)(std::forward<X>(x));
    }

    /** @brief Identity adaptor. */
    inline constexpr Adaptor Identity{[](auto&& x) -> decltype(auto) { return std::forward<decltype(x)>(x); }};

    /**
     * @brief Wrap a callable into a pipeable @ref Adaptor.
     * @param[in] f Callable object to wrap.
     * @return Pipeable adaptor that forwards to @p f.
     */
    template<typename F>
    [[nodiscard]] constexpr auto Fn(F&& f) {
        return Adaptor{std::forward<F>(f)};
    }

    /**
     * @brief Return an adaptor that ignores all arguments and returns @p v.
     * @param[in] v Constant value captured by the returned adaptor.
     */
    template<typename V>
    [[nodiscard]] constexpr auto Constant(V v) {
        return Adaptor{[v = std::move(v)](auto&&...) { return v; }};
    }

    /** @cond INTERNAL */
    namespace Detail {

        /** @brief Stateless-layout-preserving composition of an outer and inner callable. */
        template<typename Outer, typename Inner>
        struct Composition {
            NO_UNIQUE_ADDRESS Outer outer;
            NO_UNIQUE_ADDRESS Inner inner;

            template<typename Self, typename... Xs>
                requires requires(Self&& self, Xs&&... xs) {
                    std::invoke(std::forward<Self>(self).outer,
                                std::invoke(std::forward<Self>(self).inner, std::forward<Xs>(xs)...));
                }
            [[gnu::always_inline]] constexpr decltype(auto) operator()(this Self&& self, Xs&&... xs)
                noexcept(noexcept(std::invoke(
                    std::forward<Self>(self).outer,
                    std::invoke(std::forward<Self>(self).inner, std::forward<Xs>(xs)...)))) {
                return std::invoke(std::forward<Self>(self).outer,
                                   std::invoke(std::forward<Self>(self).inner, std::forward<Xs>(xs)...));
            }
        };

        template<typename Outer, typename Inner>
        Composition(Outer, Inner) -> Composition<Outer, Inner>;

        /** @brief Map a callable over the reflected members of a record and reconstruct the same record type. */
        template<typename F, typename X>
        constexpr auto MapRecord(F& f, X&& xs) {
            using T = std::remove_cvref_t<X>;
            return [&]<size_t... I>(std::index_sequence<I...>) {
                return T{f(std::forward<X>(xs).[:Traits::DataMembers<T>[I]:])...};
            }(std::make_index_sequence<Traits::DataMembers<T>.size()>{});
        }

        /** @brief Recursive heterogeneous tuple fold implementation. */
        template<size_t I, size_t N, typename F, typename Acc, typename Tuple>
        constexpr auto FoldFrom(F& f, Acc acc, Tuple&& t) {
            if constexpr (I == N) {
                return acc;
            } else {
                return FoldFrom<I + 1, N>(f, f(std::move(acc), std::get<I>(std::forward<Tuple>(t))),
                                          std::forward<Tuple>(t));
            }
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Map callable @p f over a range, tuple, or reflectable record.
     *
     * @details A range returns a lazy @c std::views::transform view. A tuple returns a @c std::tuple. A record is
     * reconstructed as its own type by applying @p f to every reflected non-static data member.
     */
    template<typename F>
    [[nodiscard]] constexpr auto Map(F f) {
        return Adaptor{[f = std::move(f)](auto&& xs) -> decltype(auto) {
            using X = std::remove_cvref_t<decltype(xs)>;
            if constexpr (Concept::LazyRange<X>) {
                return std::views::transform(std::forward<decltype(xs)>(xs), f);
            } else if constexpr (Concept::TupleLikeClass<X>) {
                return std::apply([&](auto&&... es) { return std::tuple{f(std::forward<decltype(es)>(es))...}; },
                                  std::forward<decltype(xs)>(xs));
            } else {
                return Detail::MapRecord(f, std::forward<decltype(xs)>(xs));
            }
        }};
    }

    /**
     * @brief Keep elements satisfying predicate @p pred.
     * @details Returns a lazy @c std::ranges::filter_view for range operands.
     */
    template<typename P>
    [[nodiscard]] constexpr auto Filter(P pred) {
        return Adaptor{
            [pred = std::move(pred)](auto&& xs) { return std::views::filter(std::forward<decltype(xs)>(xs), pred); }};
    }

    /** @brief Lazy standard-view helpers that intentionally return standard range adaptor closures. */
    namespace View {

        /**
         * @brief Return a filter closure keeping elements whose projected value compares with @p expected.
         *
         * @details The predicate is equivalent to @c comparator(proj(element), expected). With the default comparator,
         * this is the projected-value form of an equality filter.
         *
         * @tparam Expected Captured expected value type.
         * @tparam Proj Projection applied to every input element.
         * @tparam Comparator Binary predicate comparing projected element values against @p expected.
         * @param[in] expected Value captured by the returned view closure.
         * @param[in] proj Projection applied before comparison.
         * @param[in] comparator Binary comparator, defaulting to equality.
         * @return Pipeable @c std::views::filter closure.
         */
        template<typename Expected, typename Proj = std::identity, typename Comparator = std::ranges::equal_to>
        [[nodiscard]] constexpr auto Where(Expected expected, Proj proj = {}, Comparator comparator = {}) {
            return std::views::filter(
                [expected = std::move(expected), proj = std::move(proj),
                 comparator = std::move(comparator)]<typename T>(T&& value) mutable -> bool {
                    return std::invoke(comparator, std::invoke(proj, std::forward<T>(value)), expected);
                });
        }

    } // namespace View

    /**
     * @brief Left fold with an initial accumulator.
     *
     * @details Tuple folding is heterogeneous and resolved by recursive instantiation. Range folding uses a simple
     * loop and updates the accumulator in place.
     */
    template<typename F, typename Init>
    [[nodiscard]] constexpr auto Fold(F f, Init init) {
        return Adaptor{[f = std::move(f), init = std::move(init)](auto&& xs) {
            using X = std::remove_cvref_t<decltype(xs)>;
            if constexpr (Concept::TupleLikeClass<X>) {
                return Detail::FoldFrom<0, std::tuple_size_v<X>>(f, init, std::forward<decltype(xs)>(xs));
            } else {
                auto acc = init;
                for (auto&& e : xs) {
                    acc = f(std::move(acc), e);
                }
                return acc;
            }
        }};
    }

    /**
     * @brief Return an adaptor that applies callable @p f to a tuple-like value by spreading its elements.
     * @param[in] f Callable to invoke through @c std::apply.
     */
    template<typename F>
    [[nodiscard]] constexpr auto Apply(F f) {
        return Adaptor{
            [f = std::move(f)](auto&& t) -> decltype(auto) { return std::apply(f, std::forward<decltype(t)>(t)); }};
    }

    /**
     * @brief Apply callable @p f to tuple-like @p t by spreading its elements.
     * @param[in] f Callable to invoke.
     * @param[in] t Tuple-like value to spread.
     */
    template<typename F, typename T>
        requires Concept::TupleLikeClass<std::remove_cvref_t<T>>
    [[nodiscard]] constexpr decltype(auto) Apply(F&& f, T&& t) {
        return std::apply(std::forward<F>(f), std::forward<T>(t));
    }

    /**
     * @brief Map over a range of tuple-like values and apply @p f to each subtuple.
     * @param[in] f Callable applied to each spread subtuple.
     */
    template<typename F>
    [[nodiscard]] constexpr auto MapApply(F f) {
        return Map([f = std::move(f)](auto&& sub) -> decltype(auto) {
            return std::apply(f, std::forward<decltype(sub)>(sub));
        });
    }

    /**
     * @brief Zip-apply callable @p f across equal-length tuples.
     * @param[in] f Callable invoked once per tuple index.
     * @param[in] t0 First tuple-like input.
     * @param[in] ts Additional tuple-like inputs.
     * @return Tuple containing @p f applied to each index across all inputs.
     */
    template<typename F, typename T0, typename... Ts>
        requires Concept::TupleLikeClass<std::remove_cvref_t<T0>>
    [[nodiscard]] constexpr auto MapThread(F f, T0&& t0, Ts&&... ts) {
        constexpr size_t N = std::tuple_size_v<std::remove_cvref_t<T0>>;
        auto atI = [&]<size_t I>(std::integral_constant<size_t, I>) { return f(std::get<I>(t0), std::get<I>(ts)...); };
        return [&]<size_t... I>(std::index_sequence<I...>) {
            return std::tuple{atI(std::integral_constant<size_t, I>{})...};
        }(std::make_index_sequence<N>{});
    }

    /**
     * @brief Right-to-left composition.
     * @param[in] f Callable to wrap.
     */
    template<typename F>
    [[nodiscard]] constexpr auto Compose(F f) {
        return Adaptor{std::move(f)};
    }

    /**
     * @brief Right-to-left composition of two or more callables.
     * @param[in] f Outermost callable.
     * @param[in] fs Inner callables.
     */
    template<typename F, typename G, typename... Fs>
    [[nodiscard]] constexpr auto Compose(F f, G g, Fs... fs) {
        return Adaptor{Detail::Composition{std::move(f), Compose(std::move(g), std::move(fs)...)}};
    }

    /**
     * @brief Left-to-right composition.
     * @param[in] f Callable to wrap.
     */
    template<typename F>
    [[nodiscard]] constexpr auto Then(F f) {
        return Adaptor{std::move(f)};
    }

    /**
     * @brief Left-to-right composition of two or more callables.
     * @param[in] f First callable.
     * @param[in] fs Later callables.
     */
    template<typename F, typename G, typename... Fs>
    [[nodiscard]] constexpr auto Then(F f, G g, Fs... fs) {
        return Adaptor{Detail::Composition{Then(std::move(g), std::move(fs)...), std::move(f)}};
    }

    /**
     * @brief Partially apply leading arguments to callable @p f.
     * @param[in] f Callable to bind.
     * @param[in] a Leading arguments captured by the returned adaptor.
     */
    template<typename F, typename... A>
    [[nodiscard]] constexpr auto Bind(F&& f, A&&... a) {
        return Adaptor{std::bind_front(std::forward<F>(f), std::forward<A>(a)...)};
    }

    /**
     * @brief Immutable runtime key-value rule.
     * @tparam K Key type.
     * @tparam V Value type.
     */
    template<typename K, typename V>
    struct Rule {
        /** @brief Rule key. */
        K key;

        /** @brief Rule value. */
        V value;
    };

    /** @brief Deduction guide for @ref Rule. */
    template<typename K, typename V>
    Rule(K, V) -> Rule<K, V>;

    /** @brief Forward declaration for compile-time keyed fields. */
    template<auto K, typename V>
    struct Field;

    /**
     * @brief Compile-time key tag produced by @c _k literals.
     * @tparam K Compile-time key string.
     */
    template<auto K>
    struct KeyTag {
        /** @brief Carried compile-time key string. */
        static constexpr auto key = K;

        /**
         * @brief Bind this key to value @p v.
         * @param[in] v Value to store in the resulting field.
         * @return Field containing @p v under key @p K.
         */
        template<typename V>
        [[nodiscard]] constexpr auto operator=(V&& v) const {
            return Field<K, std::remove_cvref_t<V>>{std::forward<V>(v)};
        }
    };

    /**
     * @brief Construct a compile-time key tag from a string literal.
     * @tparam S Literal key string.
     */
    template<char... Cs>
    [[nodiscard]] consteval auto operator""_k() {
        constexpr char text[]{Cs..., '\0'};
        return KeyTag<FixedString<sizeof...(Cs)>{text}>{};
    }

    /**
     * @brief One compile-time keyed value inside an @ref Assoc.
     * @tparam K Compile-time key string.
     * @tparam V Stored value type.
     */
    template<auto K, typename V>
    struct Field {
        /** @brief Compile-time key string. */
        static constexpr auto key = K;

        /** @brief Stored field value. */
        V value;
    };

    /**
     * @brief Heterogeneous association keyed by compile-time strings.
     * @tparam Fs Field types contained by the association.
     */
    template<typename... Fs>
    struct Assoc {
        /** @brief Stored fields. */
        std::tuple<Fs...> fields;

        /** @brief Construct from fields. */
        constexpr explicit Assoc(Fs... fs) : fields{fs...} {}

        /**
         * @brief Return the index of key @p K within the field pack.
         * @return Matching index, or @c size_t(-1) when the key is absent.
         */
        template<auto K>
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

        /**
         * @brief Return field value for key @p K.
         * @tparam K Compile-time key string.
         */
        template<auto K>
        [[nodiscard]] constexpr decltype(auto) Get(this auto&& self) {
            constexpr size_t i = IndexOfKey<K>();
            static_assert(i != size_t(-1), "Assoc: key not found");
            return (std::get<i>(std::forward<decltype(self)>(self).fields).value);
        }

        /**
         * @brief Return field value for key tag @p KeyTag<K>.
         * @tparam K Compile-time key string.
         */
        template<auto K>
        [[nodiscard]] constexpr decltype(auto) operator[](this auto&& self, KeyTag<K>) {
            return std::forward<decltype(self)>(self).template Get<K>();
        }

        /** @brief Return whether key @p K exists in this association type. */
        template<auto K>
        static constexpr bool Contains() {
            return IndexOfKey<K>() != size_t(-1);
        }
    };

    /** @brief Deduction guide for @ref Assoc. */
    template<typename... Fs>
    Assoc(Fs...) -> Assoc<Fs...>;

    /**
     * @brief Convert a record into a tuple of its reflected member values.
     * @param[in] obj Record object to read.
     * @return Tuple containing the reflected member values of @p obj.
     */
    template<typename T>
    [[nodiscard]] constexpr auto ToTuple(T&& obj) {
        using U = std::remove_cvref_t<T>;
        return [&]<size_t... I>(std::index_sequence<I...>) {
            return std::tuple{std::forward<T>(obj).[:Traits::DataMembers<U>[I]:]...};
        }(std::make_index_sequence<Traits::DataMembers<U>.size()>{});
    }

    /**
     * @brief Invoke @p f for every reflected non-static data member of @p obj.
     * @param[in] obj Record object to visit.
     * @param[in] f Callable invoked as @c f(name, value).
     */
    template<typename T, typename F>
    constexpr void ForEachField(T&& obj, F&& f) {
        using U = std::remove_cvref_t<T>;
        template for (constexpr auto m : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^U, std::meta::access_context::unchecked()))) {
            f(std::meta::identifier_of(m), std::forward<T>(obj).[:m:]);
        }
    }

    /** @cond INTERNAL */
    namespace Detail {

        /** @brief Type trait detecting @c std::expected. */
        template<typename T>
        struct IsExpected : std::false_type {};

        /** @brief Type trait specialization for @c std::expected. */
        template<typename V, typename E>
        struct IsExpected<std::expected<V, E>> : std::true_type {};

        /** @brief Type trait detecting @c std::optional. */
        template<typename T>
        struct IsOptional : std::false_type {};

        /** @brief Type trait specialization for @c std::optional. */
        template<typename V>
        struct IsOptional<std::optional<V>> : std::true_type {};

    } // namespace Detail
    /** @endcond */

    /** @brief Type that supports monadic chaining through @c std::expected or @c std::optional. */
    template<typename T>
    concept Fallible =
        Detail::IsExpected<std::remove_cvref_t<T>>::value || Detail::IsOptional<std::remove_cvref_t<T>>::value;

    /**
     * @brief Return an adaptor that invokes @c and_then on a fallible value.
     * @param[in] f Callable passed to @c and_then.
     */
    template<typename F>
    [[nodiscard]] constexpr auto AndThen(F f) {
        return Adaptor{[f = std::move(f)](auto&& m) -> decltype(auto)
                           requires Fallible<decltype(m)>
                       { return std::forward<decltype(m)>(m).and_then(f); }};
    }

    /**
     * @brief Return an adaptor that invokes @c transform on a fallible value.
     * @param[in] f Callable passed to @c transform.
     */
    template<typename F>
    [[nodiscard]] constexpr auto Transform(F f) {
        return Adaptor{[f = std::move(f)](auto&& m) -> decltype(auto)
                           requires Fallible<decltype(m)>
                       { return std::forward<decltype(m)>(m).transform(f); }};
    }

    /**
     * @brief Return an adaptor that invokes @c or_else on a fallible value.
     * @param[in] f Callable passed to @c or_else.
     */
    template<typename F>
    [[nodiscard]] constexpr auto OrElse(F f) {
        return Adaptor{[f = std::move(f)](auto&& m) -> decltype(auto)
                           requires Fallible<decltype(m)>
                       { return std::forward<decltype(m)>(m).or_else(f); }};
    }

    /**
     * @brief Return an adaptor that unwraps a fallible value with fallback @p fallback.
     * @param[in] fallback Value returned when the fallible value is disengaged or erroneous.
     */
    template<typename V>
    [[nodiscard]] constexpr auto ValueOr(V fallback) {
        return Adaptor{[fallback = std::move(fallback)](auto&& m)
                           requires Fallible<decltype(m)>
                       { return std::forward<decltype(m)>(m).value_or(fallback); }};
    }

    /**
     * @brief Prefix accumulation over a range.
     *
     * @details Materializes a @c std::vector of running accumulations. Element @c i is the result of folding all input
     * elements through @c i with the supplied initial value.
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

    /**
     * @brief Zip input ranges into a standard zip view.
     * @param[in] ranges Ranges to zip together.
     */
    template<std::ranges::input_range... Rs>
    [[nodiscard]] constexpr auto Zip(Rs&&... ranges) {
        return std::views::zip(std::forward<Rs>(ranges)...);
    }

    /**
     * @brief Return a pipeable adaptor that zips the piped range with @p other.
     * @param[in] other Right-hand range captured by reference.
     */
    template<std::ranges::input_range R>
    [[nodiscard]] constexpr auto ZipWith(R& other) {
        return Adaptor{[&other](auto&& lhs) { return std::views::zip(std::forward<decltype(lhs)>(lhs), other); }};
    }

    /** @brief Return a pipeable adaptor that zips a range with zero-based indices. */
    [[nodiscard]] constexpr auto Enumerate() {
        return Adaptor{
            [](auto&& xs) { return std::views::zip(std::views::iota(size_t{0}), std::forward<decltype(xs)>(xs)); }};
    }

    /**
     * @brief Return a pipeable adaptor that zips a range with indices starting at @p start.
     * @param[in] start First index to emit.
     */
    template<std::integral I = size_t>
    [[nodiscard]] constexpr auto Enumerate(I start) {
        return Adaptor{
            [start](auto&& xs) { return std::views::zip(std::views::iota(start), std::forward<decltype(xs)>(xs)); }};
    }

    namespace Traits {

        template<typename T>
        auto&& AdaptorAppliedTarget(T&&);

        template<typename T>
            requires requires {
                { Traits::AdaptorAppliedTarget(std::declval<T>()) };
            }
        auto operator|(auto&& x, Concept::Adaptor auto&& adaptor) {
            return Traits::AdaptorAppliedTarget(std::forward<decltype(x)>(x)) |
                   std::forward<decltype(adaptor)>(adaptor);
        }

    } // namespace Traits

} // namespace Sora
