/**
 * @file Functional.h
 * @brief Functional facade for callable, range, record, association, and monadic operations.
 * @ingroup Core
 *
 * @details Range algorithms are lazy and owned by @c Sora::View. Root-level mixed-domain names are retained as a
 * compact compatibility facade for tuple and public-record dispatch.
 */
#pragma once

#include <Sora/Core/Functional/Association.h>
#include <Sora/Core/Functional/Callable.h>
#include <Sora/Core/Functional/Monadic.h>
#include <Sora/Core/Functional/Record.h>
#include <Sora/Core/Functional/Views.h>

#include <concepts>
#include <cstddef>
#include <functional>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Detail {

        template<size_t Index, size_t Size, typename F, typename Accumulator, typename Tuple>
        constexpr auto FoldTupleFrom(F& function, Accumulator accumulator, Tuple&& tuple) {
            if constexpr (Index == Size) {
                return accumulator;
            } else {
                return FoldTupleFrom<Index + 1, Size>(
                    function,
                    std::invoke(function, std::move(accumulator), std::get<Index>(std::forward<Tuple>(tuple))),
                    std::forward<Tuple>(tuple));
            }
        }

        template<size_t Index, typename F, typename... Tuples>
        constexpr decltype(auto) InvokeTupleColumn(F& function, Tuples&&... tuples) {
            return std::invoke(function, std::get<Index>(std::forward<Tuples>(tuples))...);
        }

        template<typename F, typename... Tuples, size_t... Indices>
        [[nodiscard]] constexpr auto MapTupleColumns(
            F& function, std::index_sequence<Indices...>, Tuples&&... tuples) {
            return std::tuple{
                InvokeTupleColumn<Indices>(function, std::forward<Tuples>(tuples)...)...};
        }

        template<typename Self, typename Tuple, size_t... Indices>
        consteval bool TupleMappable(std::index_sequence<Indices...>) {
            using Function = decltype((std::declval<Self&>().function));
            return (std::invocable<Function, decltype(std::get<Indices>(std::declval<Tuple>()))> && ...);
        }

        template<typename Self, typename Record>
        consteval bool RecordMappable() {
            using T = std::remove_cvref_t<Record>;
            using Function = decltype((std::declval<Self&>().function));
            bool result = true;
            template for (constexpr auto member : Traits::PublicDataMembers<T>) {
                using Reference = decltype(std::declval<Record>().[:member:]);
                using Member = std::remove_cvref_t<Reference>;
                if constexpr (!std::invocable<Function, Reference>) {
                    result = false;
                } else if constexpr (!std::constructible_from<Member, std::invoke_result_t<Function, Reference>>) {
                    result = false;
                }
            }
            return result;
        }

        template<typename F>
        struct MapFunction {
            SORA_NO_UNIQUE_ADDRESS F function;

            template<typename Self, std::ranges::viewable_range R>
                requires requires(Self&& self, R&& range) {
                    std::views::transform(
                        std::forward<R>(range), std::forward<Self>(self).function);
                }
            constexpr auto operator()(this Self&& self, R&& range) {
                return std::views::transform(
                    std::forward<R>(range), std::forward<Self>(self).function);
            }

            template<typename Self, typename Tuple>
                requires Concept::TupleLikeClass<std::remove_cvref_t<Tuple>> &&
                         (!std::ranges::input_range<Tuple>) &&
                         (TupleMappable<Self, Tuple>(
                             std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<Tuple>>>{}))
            constexpr auto operator()(this Self&& self, Tuple&& tuple) {
                return std::apply(
                    [&](auto&&... elements) {
                        return std::tuple{
                            std::invoke(self.function, std::forward<decltype(elements)>(elements))...};
                    },
                    std::forward<Tuple>(tuple));
            }

            template<typename Self, Concept::PublicRecord Record>
                requires (RecordMappable<Self, Record>())
            constexpr auto operator()(this Self&& self, Record&& record) {
                using T = std::remove_cvref_t<Record>;
                return MapRecord(self.function, std::forward<Record>(record),
                                 std::make_index_sequence<Traits::PublicDataMembersCount<T>>{});
            }
        };

        template<typename F, typename Initial>
        struct FoldFunction {
            SORA_NO_UNIQUE_ADDRESS F function;
            SORA_NO_UNIQUE_ADDRESS Initial initial;

            template<typename Self, typename Tuple>
                requires Concept::TupleLikeClass<std::remove_cvref_t<Tuple>> &&
                         std::constructible_from<
                             Initial, decltype((std::forward<Self>(std::declval<Self>()).initial))>
            constexpr auto operator()(this Self&& self, Tuple&& tuple) {
                auto accumulator = std::forward<Self>(self).initial;
                constexpr size_t size = std::tuple_size_v<std::remove_cvref_t<Tuple>>;
                return FoldTupleFrom<0, size>(self.function, std::move(accumulator), std::forward<Tuple>(tuple));
            }

            template<typename Self, std::ranges::input_range Range>
                requires std::constructible_from<
                             Initial, decltype((std::forward<Self>(std::declval<Self>()).initial))> &&
                         std::invocable<F&, Initial&&, std::ranges::range_reference_t<Range>> &&
                         std::assignable_from<
                             Initial&, std::invoke_result_t<F&, Initial&&, std::ranges::range_reference_t<Range>>>
            constexpr Initial operator()(this Self&& self, Range&& range) {
                auto accumulator = std::forward<Self>(self).initial;
                for (auto&& value : range) {
                    accumulator = std::invoke(
                        self.function, std::move(accumulator), std::forward<decltype(value)>(value));
                }
                return accumulator;
            }
        };

        template<typename F>
        struct ApplyFunction {
            SORA_NO_UNIQUE_ADDRESS F function;

            template<typename Self, typename Tuple>
                requires Concept::TupleLikeClass<std::remove_cvref_t<Tuple>> &&
                         requires(Self&& self, Tuple&& tuple) {
                             std::apply(std::forward<Self>(self).function, std::forward<Tuple>(tuple));
                         }
            constexpr decltype(auto) operator()(this Self&& self, Tuple&& tuple) {
                return std::apply(std::forward<Self>(self).function, std::forward<Tuple>(tuple));
            }
        };

    } // namespace Detail

    /**
     * @brief Map a callable over a range, tuple-like object, or public aggregate record.
     * @details Range inputs produce a lazy standard transform view. Tuple-like inputs produce a value tuple, while
     * public-record inputs produce a value of the same record type. Range lvalues are borrowed and viewable rvalues
     * are owned by the returned view.
     * @tparam F Mapping callable.
     * @param[in] function Callable applied to each element or field.
     * @return A lazy transform view for ranges, a value tuple for tuple-like inputs, or a mapped record value.
     * @code{.cpp}
     * auto values = std::array{1, 2, 3};
     * auto doubled = values | Sora::Map([](int value) { return value * 2; });
     * auto output = std::ranges::to<std::vector>(doubled); // [2, 4, 6]
     * auto fields = Sora::Map([](int value) { return value + 1; })(std::tuple{1, 2}); // (2, 3)
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto Map(F&& function) {
        return Fn(Detail::MapFunction<std::decay_t<F>>{std::forward<F>(function)});
    }

    /**
     * @brief Compatibility facade for the lazy @ref View::Filter adapter.
     * @details This function has the same predicate, laziness, and range ownership semantics as
     * @ref View::Filter and exists at the root namespace for concise pipelines.
     * @tparam P Boolean-testable predicate.
     * @param[in] predicate Predicate receiving one range element.
     * @return A pipeable filter closure.
     * @code{.cpp}
     * auto values = std::array{-1, 2, -3, 4};
     * auto positives = values | Sora::Filter([](int value) { return value > 0; });
     * auto output = std::ranges::to<std::vector>(positives); // [2, 4]
     * @endcode
     */
    template<typename P>
    [[nodiscard]] constexpr auto Filter(P&& predicate) {
        return View::Filter(std::forward<P>(predicate));
    }

    /**
     * @brief Left-fold a tuple-like object or input range with an explicit initial accumulator.
     * @details Tuple-like inputs are consumed eagerly from left to right. Range inputs are traversed when the closure
     * is invoked and return the final accumulator value. The accumulator is copied or moved from the closure and each
     * step receives the previous accumulator as an rvalue.
     * @tparam F Binary accumulation callable.
     * @tparam Initial Accumulator type.
     * @param[in] function Callable receiving accumulator and current element.
     * @param[in] initial Initial accumulator value.
     * @return A pipeable fold closure.
     * @code{.cpp}
     * auto values = std::array{1, 2, 3};
     * auto total = values | Sora::Fold(std::plus{}, 0); // 6
     * @endcode
     */
    template<typename F, typename Initial>
    [[nodiscard]] constexpr auto Fold(F&& function, Initial&& initial) {
        return Fn(Detail::FoldFunction<std::decay_t<F>, std::decay_t<Initial>>{
            std::forward<F>(function), std::forward<Initial>(initial)});
    }

    /**
     * @brief Return a closure that spreads one tuple-like input into @p function.
     * @details The tuple is forwarded to @c std::apply when the closure is invoked; no intermediate tuple is created.
     * @tparam F Callable receiving one argument per tuple element.
     * @param[in] function Callable to invoke after expansion.
     * @return A pipeable apply closure.
     * @code{.cpp}
     * auto input = std::tuple{2, 3};
     * auto sum = input | Sora::Apply([](int left, int right) { return left + right; }); // 5
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto Apply(F&& function) {
        return Fn(Detail::ApplyFunction<std::decay_t<F>>{std::forward<F>(function)});
    }

    /**
     * @brief Immediately spread @p tuple into @p function.
     * @details The tuple-like input is forwarded directly to @c std::apply, preserving element value categories and
     * returning exactly the callable's result.
     * @tparam F Callable receiving one argument per tuple element.
     * @tparam T Tuple-like input type.
     * @param[in] function Callable to invoke.
     * @param[in] tuple Tuple-like arguments to expand.
     * @return The result of invoking @p function with the expanded elements.
     * @code{.cpp}
     * auto input = std::tuple{2, 3};
     * auto sum = Sora::Apply([](int left, int right) { return left + right; }, input); // 5
     * @endcode
     */
    template<typename F, typename T>
        requires Concept::TupleLikeClass<std::remove_cvref_t<T>>
    [[nodiscard]] constexpr decltype(auto) Apply(F&& function, T&& tuple) {
        return std::apply(std::forward<F>(function), std::forward<T>(tuple));
    }

    /**
     * @brief Lazily spread every tuple-like element of a range into @p function.
     * @details Each source element is passed through @ref Apply when dereferenced. The source range is borrowed for
     * lvalues and owned for viewable rvalues according to standard transform-view rules.
     * @tparam F Callable receiving the expanded tuple elements.
     * @param[in] function Callable to invoke for each tuple-like source element.
     * @return A lazy transformed range.
     * @code{.cpp}
     * auto pairs = std::array{std::pair{1, 10}, std::pair{2, 20}};
     * auto sums = pairs | Sora::MapApply([](int left, int right) { return left + right; });
     * auto output = std::ranges::to<std::vector>(sums); // [11, 22]
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto MapApply(F&& function) {
        return View::Transform(Apply(std::forward<F>(function)));
    }

    /**
     * @brief Apply @p function positionally across equal-length tuple-like inputs.
     * @details Element zero from each input is passed to the first invocation, element one to the second, and so on.
     * All inputs must be tuple-like and have the same compile-time length; the result is a value tuple.
     * @tparam F Callable accepting one element from each input tuple.
     * @tparam First First tuple-like input.
     * @tparam Tuples Remaining tuple-like inputs.
     * @param[in] function Element-wise callable.
     * @param[in] first First input tuple.
     * @param[in] tuples Remaining input tuples.
     * @return A value tuple containing one result per input position.
     * @code{.cpp}
     * auto sums = Sora::MapThread(std::plus{}, std::tuple{1, 2}, std::tuple{10, 20}); // (11, 22)
     * @endcode
     */
    template<typename F, typename First, typename... Tuples>
        requires Concept::TupleLikeClass<std::remove_cvref_t<First>> &&
                 (Concept::TupleLikeClass<std::remove_cvref_t<Tuples>> && ...) &&
                 ((std::tuple_size_v<std::remove_cvref_t<Tuples>> ==
                   std::tuple_size_v<std::remove_cvref_t<First>>) && ...)
    [[nodiscard]] constexpr auto MapThread(F&& function, First&& first, Tuples&&... tuples) {
        constexpr size_t size = std::tuple_size_v<std::remove_cvref_t<First>>;
        return Detail::MapTupleColumns(function, std::make_index_sequence<size>{}, std::forward<First>(first),
                                       std::forward<Tuples>(tuples)...);
    }

    /**
     * @brief Compatibility facade for the lazy seeded prefix @ref View::Scan adapter.
     * @details For @c [x0,x1,...], emits @c [f(init,x0), f(previous,x1), ...]. Construction is lazy and preserves
     * the source length; the returned view owns viewable rvalue sources.
     * @tparam F Binary accumulation callable.
     * @tparam Initial Seed accumulator type.
     * @param[in] function Accumulation callable.
     * @param[in] initial Seed value.
     * @return A pipeable seeded prefix adapter.
     * @code{.cpp}
     * auto values = std::array{1, 2, 3};
     * auto totals = values | Sora::Scan(std::plus{}, 0);
     * auto output = std::ranges::to<std::vector>(totals); // [1, 3, 6]
     * @endcode
     */
    template<typename F, typename Initial>
    [[nodiscard]] constexpr auto Scan(F&& function, Initial&& initial) {
        return View::Scan(std::forward<F>(function), std::forward<Initial>(initial));
    }

    /**
     * @brief Compatibility facade for the lazy unseeded prefix @ref View::PartialSum adapter.
     * @details The first output is the first source value; later outputs combine the previous output with the next
     * source value. The default operation is @c std::plus<>.
     * @tparam F Binary accumulation callable.
     * @param[in] function Accumulation callable.
     * @return A pipeable unseeded prefix adapter.
     * @code{.cpp}
     * auto values = std::array{1, 2, 3};
     * auto running = values | Sora::PartialSum();
     * auto output = std::ranges::to<std::vector>(running); // [1, 3, 6]
     * @endcode
     */
    template<typename F = std::plus<>>
    [[nodiscard]] constexpr auto PartialSum(F&& function = {}) {
        return View::PartialSum(std::forward<F>(function));
    }

    /**
     * @brief Compatibility facade for constructing a @ref View::CacheLatest view.
     * @details Repeated dereference of one iterator reuses the latest value until increment. The cache is local to
     * the resulting view and does not alter source ownership or iterator semantics.
     * @return A pipeable cache-latest adapter.
     * @code{.cpp}
     * auto values = std::array{1, 2};
     * auto cached = values | Sora::CacheLatest();
     * auto output = std::ranges::to<std::vector>(cached); // [1, 2]
     * @endcode
     */
    [[nodiscard]] constexpr auto CacheLatest() { return View::CacheLatest(); }

    /**
     * @brief Zip viewable ranges and stop when the shortest range ends.
     * @details This root-level name forwards to @ref View::Zip. Lvalue ranges are referenced and viewable rvalues are
     * owned by the standard zip view.
     * @tparam Ranges Viewable ranges.
     * @param[in] ranges Ranges traversed in lockstep.
     * @return A lazy tuple-like zip view.
     * @code{.cpp}
     * auto left = std::array{1, 2};
     * auto right = std::array{10, 20};
     * auto pairs = Sora::Zip(left, right);
     * auto output = std::ranges::to<std::vector>(pairs); // [(1, 10), (2, 20)]
     * @endcode
     */
    template<std::ranges::viewable_range... Ranges>
    [[nodiscard]] constexpr auto Zip(Ranges&&... ranges) {
        return View::Zip(std::forward<Ranges>(ranges)...);
    }

    /**
     * @brief Return a pipeable adapter that zips its input with additional ranges.
     * @details This root-level name forwards to @ref View::ZipWith. Additional lvalues are referenced and viewable
     * rvalues are owned by the resulting view.
     * @tparam Ranges Additional viewable ranges.
     * @param[in] ranges Ranges traversed alongside the piped input.
     * @return A pipeable zip adapter.
     * @code{.cpp}
     * auto values = std::array{1, 2};
     * auto other = std::array{10, 20};
     * auto pairs = values | Sora::ZipWith(other);
     * auto output = std::ranges::to<std::vector>(pairs); // [(1, 10), (2, 20)]
     * @endcode
     */
    template<std::ranges::viewable_range... Ranges>
    [[nodiscard]] constexpr auto ZipWith(Ranges&&... ranges) {
        return View::ZipWith(std::forward<Ranges>(ranges)...);
    }

    /**
     * @brief Return a pipeable adapter that zips a range with increasing indices.
     * @details The index starts at @p start and increments once per source element. The source range follows standard
     * borrowed-lvalue and owned-rvalue view rules.
     * @tparam I Integral index type.
     * @param[in] start First index value.
     * @return A pipeable enumerate adapter.
     * @code{.cpp}
     * auto values = std::array{'x', 'y'};
     * auto numbered = values | Sora::Enumerate(0);
     * auto output = std::ranges::to<std::vector>(numbered); // [(0, 'x'), (1, 'y')]
     * @endcode
     */
    template<std::integral I = size_t>
    [[nodiscard]] constexpr auto Enumerate(I start = {}) {
        return View::Enumerate(start);
    }

} // namespace Sora
