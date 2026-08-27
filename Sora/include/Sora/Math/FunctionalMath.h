/**
 * @file FunctionalMath.h
 * @brief Allocation-free materialization of functional element-wise kernels through Sora Math backends.
 * @ingroup Math
 *
 * @details Lazy composition remains owned by @ref Sora::Map and @ref Sora::View::ZipTransform. This header owns the
 * explicit contiguous-output boundary where a scalar callable can be lifted to the preferred SIMD carrier and
 * delegated to @ref TransformBatch without duplicating backend or load/store machinery.
 */
#pragma once

#include <Sora/Core/Contracts.h>
#include <Sora/Core/Functional.h>
#include <Sora/Math/Batch.h>

#include <concepts>
#include <cstddef>
#include <functional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    namespace Detail {

        template<typename... Arguments>
        inline constexpr size_t kListableRangeCount =
            (size_t{std::ranges::viewable_range<Arguments>} + ... + size_t{0});

        template<typename... Arguments>
        inline constexpr bool kListableRangeArguments =
            kListableRangeCount<Arguments...> > 0 &&
            (kListableRangeCount<Arguments...> == 1 ||
             ((!std::ranges::viewable_range<Arguments> || std::ranges::sized_range<Arguments>) && ...));

        template<typename... Arguments>
        constexpr void RequireEqualListableRangeSizes(Arguments&&... arguments) {
            if constexpr (kListableRangeCount<Arguments...> > 1) {
                size_t expectedSize = 0;
                bool foundFirstRange = false;
                auto requireSize = [&]<typename Argument>(Argument&& argument) {
                    if constexpr (std::ranges::viewable_range<Argument>) {
                        const size_t size = static_cast<size_t>(std::ranges::size(argument));
                        if (!foundFirstRange) {
                            expectedSize = size;
                            foundFirstRange = true;
                        } else {
                            SORA_PRE(size == expectedSize);
                        }
                    }
                };
                (requireSize(arguments), ...);
            }
        }

        template<typename Argument>
        [[nodiscard]] constexpr auto MakeListableView(Argument&& argument) {
            if constexpr (std::ranges::viewable_range<Argument>) {
                return std::views::all(std::forward<Argument>(argument));
            } else {
                return std::views::repeat(std::decay_t<Argument>(std::forward<Argument>(argument)));
            }
        }

        template<typename Function>
        struct ListableFunction {
            SORA_NO_UNIQUE_ADDRESS Function function;

            template<typename Self, typename... Arguments>
                requires((!std::ranges::viewable_range<Arguments>) && ...) &&
                        std::invocable<decltype((std::forward<Self>(std::declval<Self>()).function)), Arguments...>
            [[nodiscard]] constexpr decltype(auto) operator()(this Self&& self, Arguments&&... arguments) noexcept(
                std::is_nothrow_invocable_v<
                    decltype((std::forward<Self>(self).function)), Arguments...>) {
                return std::invoke(std::forward<Self>(self).function, std::forward<Arguments>(arguments)...);
            }

            template<typename Self, typename... Arguments>
                requires kListableRangeArguments<Arguments...> && requires(Self&& self, Arguments&&... arguments) {
                    View::ZipTransform(std::forward<Self>(self).function,
                                       MakeListableView(std::forward<Arguments>(arguments))...);
                }
            [[nodiscard]] constexpr auto operator()(this Self&& self, Arguments&&... arguments) {
                RequireEqualListableRangeSizes(arguments...);
                return View::ZipTransform(std::forward<Self>(self).function,
                                          MakeListableView(std::forward<Arguments>(arguments))...);
            }
        };

    } // namespace Detail

    /**
     * @brief Lift a scalar callable to direct scalar invocation or one-level lazy range threading.
     * @details With only scalar arguments, invocation forwards directly to @p function. With one range, scalar
     * arguments are decay-owned and broadcast lazily. With multiple ranges, every range must be sized and have the
     * same length; unlike @ref View::ZipTransform, this adapter never silently truncates unequal inputs. Range lvalues
     * are borrowed and viewable rvalues are owned by the returned view. No recursive tensor broadcasting is implied.
     * @tparam Function Callable type before decay.
     * @param[in] function Scalar or mathematical-carrier callable to lift.
     * @return An owning pipeable callable supporting scalar and lazy threaded invocation.
     * @code{.cpp}
     * constexpr auto add = Sora::Math::Listable(Sora::Math::Add);
     * constexpr auto scalarInput = std::pair{2.0F, 3.0F};
     * static_assert(add(scalarInput.first, scalarInput.second) == 5.0F);
     * auto rangeInput = std::array{1.0F, 2.0F, 3.0F};
     * auto rangeOutput = add(rangeInput, 10.0F);
     * assert(std::ranges::equal(rangeOutput, std::array{11.0F, 12.0F, 13.0F}));
     * @endcode
     */
    template<typename Function>
        requires std::constructible_from<std::decay_t<Function>, Function>
    [[nodiscard]] constexpr auto Listable(Function&& function) {
        return Fn(Detail::ListableFunction<std::decay_t<Function>>{std::forward<Function>(function)});
    }

    namespace Concept {

        /** @brief Scalar element-wise kernel returning exactly the output scalar type. */
        template<typename Function, typename T, size_t... InputExtents>
        concept ScalarMapKernel = std::invocable<
                                      Function&,
                                      std::conditional_t<
                                          true, const T&, std::integral_constant<size_t, InputExtents>>...> &&
                                  std::same_as<
                                      std::remove_cvref_t<std::invoke_result_t<
                                          Function&,
                                          std::conditional_t<
                                              true, const T&,
                                              std::integral_constant<size_t, InputExtents>>...>>,
                                      T>;

    } // namespace Concept

    namespace Detail {

        template<typename T>
        [[nodiscard]] constexpr bool MapInputAliasingAllowed(std::span<T> output,
                                                             std::span<const T> input) noexcept {
            if (output.empty() || input.empty()) {
                return true;
            }
            if (output.data() == input.data()) {
                return output.size() == input.size();
            }
            if consteval {
                for (T& outputValue : output) {
                    for (const T& inputValue : input) {
                        if (&outputValue == &inputValue) {
                            return false;
                        }
                    }
                }
                return true;
            }
            const std::less<const T*> less;
            return !less(output.data(), input.data() + input.size()) ||
                   !less(input.data(), output.data() + output.size());
        }

    } // namespace Detail

    /**
     * @brief Materialize an element-wise function over equal-length contiguous inputs.
     * @details When @p function accepts the preferred SIMD carrier and its SIMD invocation is @c noexcept, execution
     * delegates to @ref TransformBatch, including its unrolled full batches and masked tail. Otherwise a scalar loop
     * preserves the same element-wise semantics. The function allocates no storage and performs no runtime backend
     * selection.
     * @tparam Unroll Number of independent SIMD batches issued per full loop iteration.
     * @tparam T Shared input and output scalar type.
     * @tparam OutputExtent Static output span extent, or @c std::dynamic_extent.
     * @tparam Function Scalar element-wise callable.
     * @tparam InputExtents Static input span extents.
     * @param[out] output Destination span.
     * @param[in] function Callable receiving one scalar or SIMD carrier from each input.
     * @param[in] inputs Source spans, all equal in length to @p output.
     * @pre Every input has the same length as @p output.
     * @pre Every input is either exactly @p output or has storage disjoint from it.
     * @pre SIMD invocation of @p function is lane-wise equivalent to one scalar invocation per element and has no
     * externally observable per-lane effects.
     * @code{.cpp}
     * constexpr std::array left{1.0F, 2.0F, 3.0F};
     * constexpr std::array right{10.0F, 20.0F, 30.0F};
     * std::array<float, 3> output{};
     * Sora::Math::MapInto(std::span(output), Sora::Math::Add,
     *                     std::span<const float>(left), std::span<const float>(right));
     * assert(output == std::array{11.0F, 22.0F, 33.0F});
     * @endcode
     */
    template<size_t Unroll = 4, typename T, size_t OutputExtent, typename Function, size_t... InputExtents>
        requires(sizeof...(InputExtents) > 0) && (!std::is_const_v<T>) &&
                Concept::ScalarMapKernel<Function, T, InputExtents...>
    constexpr void MapInto(std::span<T, OutputExtent> output, Function&& function,
                           std::span<const T, InputExtents>... inputs) noexcept(
        std::is_nothrow_invocable_v<
            Function&,
            std::conditional_t<true, const T&, std::integral_constant<size_t, InputExtents>>...>) {
        static_assert(Unroll > 0, "MapInto requires a positive unroll factor");
        SORA_PRE(((inputs.size() == output.size()) && ...));
        SORA_PRE((Detail::MapInputAliasingAllowed(std::span<T>(output), std::span<const T>(inputs)) && ...));

        if constexpr (Simd::Vectorizable<T>) {
            using V = Simd::BasicVector<T, Simd::PreferredAbiT<T>>;
            if constexpr (requires {
                              TransformBatch<V, Unroll>(std::span<T>(output), function,
                                                        std::span<const T>(inputs)...);
                          }) {
                TransformBatch<V, Unroll>(std::span<T>(output), std::forward<Function>(function),
                                          std::span<const T>(inputs)...);
                return;
            }
        }

        for (size_t index = 0; index < output.size(); ++index) {
            output[index] = std::invoke(function, inputs[index]...);
        }
    }

} // namespace Sora::Math
