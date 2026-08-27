/**
 * @file PrimaryFunctions.h
 * @brief Extensible backend-dispatched arithmetic and transcendental customization-point objects.
 * @ingroup Math
 *
 * @details Each CPO joins the compile-time backends of all operands and calls the corresponding static operation.
 * Built-in arithmetic values use @ref Backend::ScalarCPU, fixed-width SIMD vectors use @ref Backend::FixedSimdCPU,
 * and user-defined carriers opt in through @ref Hook::Backend. The CPOs are stateless regular callables, so
 * @ref Sora::Compose and @ref Sora::Then build fully inlinable compound functions without allocation, virtual
 * dispatch, type erasure, or runtime backend selection.
 */
#pragma once

#include <meta>
#include <type_traits>
#include <utility>

#include <Sora/Core/Functional/Callable.h>
#include <Sora/Math/Backends.h>

namespace Sora::Math {

    namespace CPO {

        /** @cond INTERNAL */
        template<typename Left, typename Right>
        [[nodiscard]] consteval std::meta::info JoinBackend() {
            if constexpr (std::same_as<Left, Right>) {
                return ^^Left;
            } else if constexpr (requires {
                                     Left::kPriority;
                                     Right::kPriority;
                                 } && Left::kPriority != Right::kPriority) {
                if constexpr (Left::kPriority > Right::kPriority) {
                    return ^^Left;
                } else {
                    return ^^Right;
                }
            } else {
                throw "Sora::Math: no unambiguous backend join exists for these carriers";
            }
        }

        template<typename Current>
        [[nodiscard]] consteval std::meta::info JoinBackends() {
            return ^^Current;
        }

        template<typename Current, typename Next, typename... Rest>
        [[nodiscard]] consteval std::meta::info JoinBackends() {
            using Joined = [:JoinBackend<Current, Next>():];
            if constexpr (sizeof...(Rest) == 0) {
                return ^^Joined;
            } else {
                return JoinBackends<Joined, Rest...>();
            }
        }

        template<typename... Carriers>
        using BackendFor = [:JoinBackends<Traits::BackendTypeOf<std::remove_cvref_t<Carriers>>...>():];
        /** @endcond */

        /**
         * @brief CPO implementing backend-dispatched addition.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Add(input, 3.0F); // 5.0F
         * @endcode
         */
        struct AddFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First, Rest...>::Add(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First, Rest...>::Add(std::forward<First>(first),
                                                                  std::forward<Rest>(rest)...))) {
                return BackendFor<First, Rest...>::Add(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /**
         * @brief CPO implementing backend-dispatched subtraction.
         * @code{.cpp}
         * constexpr auto input = 5.0F;
         * constexpr auto output = Sora::Math::Sub(input, 2.0F); // 3.0F
         * @endcode
         */
        struct SubFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First, Rest...>::Sub(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First, Rest...>::Sub(std::forward<First>(first),
                                                                  std::forward<Rest>(rest)...))) {
                return BackendFor<First, Rest...>::Sub(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /**
         * @brief CPO implementing backend-dispatched multiplication.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Mul(input, 3.0F); // 6.0F
         * @endcode
         */
        struct MulFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First, Rest...>::Mul(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First, Rest...>::Mul(std::forward<First>(first),
                                                                  std::forward<Rest>(rest)...))) {
                return BackendFor<First, Rest...>::Mul(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /**
         * @brief CPO implementing backend-dispatched division.
         * @code{.cpp}
         * constexpr auto input = 6.0F;
         * constexpr auto output = Sora::Math::Div(input, 2.0F); // 3.0F
         * @endcode
         */
        struct DivFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First, Rest...>::Div(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First, Rest...>::Div(std::forward<First>(first),
                                                                  std::forward<Rest>(rest)...))) {
                return BackendFor<First, Rest...>::Div(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /**
         * @brief CPO implementing backend-dispatched negation.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Neg(input); // -2.0F
         * @endcode
         */
        struct NegFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Neg(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Neg(std::forward<First>(first)))) {
                return BackendFor<First>::Neg(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched multiplicative inversion.
         * @code{.cpp}
         * constexpr auto input = 4.0F;
         * constexpr auto output = Sora::Math::Inv(input); // 0.25F
         * @endcode
         */
        struct InvFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Inv(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Inv(std::forward<First>(first)))) {
                return BackendFor<First>::Inv(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched squaring.
         * @code{.cpp}
         * constexpr auto input = 3.0F;
         * constexpr auto output = Sora::Math::Square(input); // 9.0F
         * @endcode
         */
        struct SquareFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Square(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Square(std::forward<First>(first)))) {
                return BackendFor<First>::Square(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched absolute value.
         * @code{.cpp}
         * constexpr auto input = -3.0F;
         * constexpr auto output = Sora::Math::Abs(input); // 3.0F
         * @endcode
         */
        struct AbsFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Abs(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Abs(std::forward<First>(first)))) {
                return BackendFor<First>::Abs(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched sine.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Sin(input); // 0.0F
         * @endcode
         */
        struct SinFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Sin(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Sin(std::forward<First>(first)))) {
                return BackendFor<First>::Sin(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched cosine.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Cos(input); // 1.0F
         * @endcode
         */
        struct CosFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Cos(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Cos(std::forward<First>(first)))) {
                return BackendFor<First>::Cos(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched tangent.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Tan(input); // 0.0F
         * @endcode
         */
        struct TanFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Tan(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Tan(std::forward<First>(first)))) {
                return BackendFor<First>::Tan(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched arcsine.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Asin(input); // 0.0F
         * @endcode
         */
        struct AsinFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Asin(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Asin(std::forward<First>(first)))) {
                return BackendFor<First>::Asin(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched arccosine.
         * @code{.cpp}
         * constexpr auto input = 1.0F;
         * constexpr auto output = Sora::Math::Acos(input); // 0.0F
         * @endcode
         */
        struct AcosFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Acos(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Acos(std::forward<First>(first)))) {
                return BackendFor<First>::Acos(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched arctangent.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Atan(input); // 0.0F
         * @endcode
         */
        struct AtanFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Atan(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Atan(std::forward<First>(first)))) {
                return BackendFor<First>::Atan(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched four-quadrant arctangent.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Atan2(input, 1.0F); // 0.0F
         * @endcode
         */
        struct Atan2Fn {
            template<typename First, typename Second>
                requires requires(First&& first, Second&& second) {
                    BackendFor<First, Second>::Atan2(std::forward<First>(first), std::forward<Second>(second));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second) const
                noexcept(noexcept(BackendFor<First, Second>::Atan2(std::forward<First>(first),
                                                                   std::forward<Second>(second)))) {
                return BackendFor<First, Second>::Atan2(std::forward<First>(first), std::forward<Second>(second));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched natural exponential.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Exp(input); // 1.0F
         * @endcode
         */
        struct ExpFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Exp(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Exp(std::forward<First>(first)))) {
                return BackendFor<First>::Exp(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched natural logarithm.
         * @code{.cpp}
         * constexpr auto input = 1.0F;
         * constexpr auto output = Sora::Math::Log(input); // 0.0F
         * @endcode
         */
        struct LogFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Log(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Log(std::forward<First>(first)))) {
                return BackendFor<First>::Log(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched square root.
         * @code{.cpp}
         * constexpr auto input = 9.0F;
         * constexpr auto output = Sora::Math::Sqrt(input); // 3.0F
         * @endcode
         */
        struct SqrtFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Sqrt(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Sqrt(std::forward<First>(first)))) {
                return BackendFor<First>::Sqrt(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched exponentiation.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Pow(input, 3.0F); // 8.0F
         * @endcode
         */
        struct PowFn {
            template<typename First, typename Second>
                requires requires(First&& first, Second&& second) {
                    BackendFor<First, Second>::Pow(std::forward<First>(first), std::forward<Second>(second));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second) const
                noexcept(noexcept(BackendFor<First, Second>::Pow(std::forward<First>(first),
                                                                 std::forward<Second>(second)))) {
                return BackendFor<First, Second>::Pow(std::forward<First>(first), std::forward<Second>(second));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched @c a*b+c.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Fma(input, 3.0F, 1.0F); // 7.0F
         * @endcode
         */
        struct FmaFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First, Second, Third>::Fma(std::forward<First>(first), std::forward<Second>(second),
                                                          std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second,
                                                                                  Third&& third) const
                noexcept(noexcept(BackendFor<First, Second, Third>::Fma(std::forward<First>(first),
                                                                        std::forward<Second>(second),
                                                                        std::forward<Third>(third)))) {
                return BackendFor<First, Second, Third>::Fma(std::forward<First>(first), std::forward<Second>(second),
                                                             std::forward<Third>(third));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched @c a*b-c.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Mfs(input, 3.0F, 1.0F); // 5.0F
         * @endcode
         */
        struct MfsFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First, Second, Third>::Mfs(std::forward<First>(first), std::forward<Second>(second),
                                                          std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second,
                                                                                  Third&& third) const
                noexcept(noexcept(BackendFor<First, Second, Third>::Mfs(std::forward<First>(first),
                                                                        std::forward<Second>(second),
                                                                        std::forward<Third>(third)))) {
                return BackendFor<First, Second, Third>::Mfs(std::forward<First>(first), std::forward<Second>(second),
                                                             std::forward<Third>(third));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched @c -a*b-c.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Nms(input, 3.0F, 1.0F); // -7.0F
         * @endcode
         */
        struct NmsFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First, Second, Third>::Nms(std::forward<First>(first), std::forward<Second>(second),
                                                          std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second,
                                                                                  Third&& third) const
                noexcept(noexcept(BackendFor<First, Second, Third>::Nms(std::forward<First>(first),
                                                                        std::forward<Second>(second),
                                                                        std::forward<Third>(third)))) {
                return BackendFor<First, Second, Third>::Nms(std::forward<First>(first), std::forward<Second>(second),
                                                             std::forward<Third>(third));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched @c -a*b+c.
         * @code{.cpp}
         * constexpr auto input = 2.0F;
         * constexpr auto output = Sora::Math::Nma(input, 3.0F, 1.0F); // -5.0F
         * @endcode
         */
        struct NmaFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First, Second, Third>::Nma(std::forward<First>(first), std::forward<Second>(second),
                                                          std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second,
                                                                                  Third&& third) const
                noexcept(noexcept(BackendFor<First, Second, Third>::Nma(std::forward<First>(first),
                                                                        std::forward<Second>(second),
                                                                        std::forward<Third>(third)))) {
                return BackendFor<First, Second, Third>::Nma(std::forward<First>(first), std::forward<Second>(second),
                                                             std::forward<Third>(third));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched linear interpolation.
         * @code{.cpp}
         * constexpr auto input = 0.0F;
         * constexpr auto output = Sora::Math::Lerp(input, 10.0F, 0.25F); // 2.5F
         * @endcode
         */
        struct LerpFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First, Second, Third>::Lerp(std::forward<First>(first), std::forward<Second>(second),
                                                           std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second,
                                                                                  Third&& third) const
                noexcept(noexcept(BackendFor<First, Second, Third>::Lerp(std::forward<First>(first),
                                                                         std::forward<Second>(second),
                                                                         std::forward<Third>(third)))) {
                return BackendFor<First, Second, Third>::Lerp(std::forward<First>(first), std::forward<Second>(second),
                                                              std::forward<Third>(third));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched clamping.
         * @code{.cpp}
         * constexpr auto input = 12.0F;
         * constexpr auto output = Sora::Math::Clamp(input, 0.0F, 10.0F); // 10.0F
         * @endcode
         */
        struct ClampFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First, Second, Third>::Clamp(std::forward<First>(first), std::forward<Second>(second),
                                                            std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second,
                                                                                  Third&& third) const
                noexcept(noexcept(BackendFor<First, Second, Third>::Clamp(std::forward<First>(first),
                                                                          std::forward<Second>(second),
                                                                          std::forward<Third>(third)))) {
                return BackendFor<First, Second, Third>::Clamp(std::forward<First>(first), std::forward<Second>(second),
                                                               std::forward<Third>(third));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched lane selection.
         * @code{.cpp}
         * constexpr auto input = true;
         * constexpr auto output = Sora::Math::Select(input, 7.0F, 3.0F); // 7.0F
         * @endcode
         */
        struct SelectFn {
            template<typename Condition, typename First, typename Second>
                requires requires(Condition&& condition, First&& first, Second&& second) {
                    BackendFor<First, Second>::Select(std::forward<Condition>(condition),
                                                       std::forward<First>(first), std::forward<Second>(second));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
            operator()(Condition&& condition, First&& first, Second&& second) const noexcept(
                noexcept(BackendFor<First, Second>::Select(std::forward<Condition>(condition),
                                                           std::forward<First>(first), std::forward<Second>(second)))) {
                return BackendFor<First, Second>::Select(std::forward<Condition>(condition),
                                                         std::forward<First>(first), std::forward<Second>(second));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched unit-interval clamping.
         * @code{.cpp}
         * constexpr auto input = 1.25F;
         * constexpr auto output = Sora::Math::Saturate(input); // 1.0F
         * @endcode
         */
        struct SaturateFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Saturate(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Saturate(std::forward<First>(first)))) {
                return BackendFor<First>::Saturate(std::forward<First>(first));
            }
        };

        /**
         * @brief CPO implementing backend-dispatched sign extraction.
         * @code{.cpp}
         * constexpr auto input = -4.0F;
         * constexpr auto output = Sora::Math::Sign(input); // -1.0F
         * @endcode
         */
        struct SignFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Sign(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Sign(std::forward<First>(first)))) {
                return BackendFor<First>::Sign(std::forward<First>(first));
            }
        };

    } // namespace CPO

    /**
     * @brief Add one or more values using the compile-time join of all operand backends.
     * @code{.cpp}
     * auto input = std::pair{2.0F, 3.0F};
     * auto output = Sora::Math::Add(input.first, input.second); // 5.0F
     * @endcode
     */
    inline constexpr CPO::AddFn Add{};

    /**
     * @brief Subtract the second operand from the first.
     * @code{.cpp}
     * auto output = Sora::Math::Sub(5.0F, 2.0F); // 3.0F
     * @endcode
     */
    inline constexpr CPO::SubFn Sub{};

    /**
     * @brief Multiply one or more values.
     * @code{.cpp}
     * auto output = Sora::Math::Mul(2.0F, 3.0F); // 6.0F
     * @endcode
     */
    inline constexpr CPO::MulFn Mul{};

    /**
     * @brief Divide the first operand by the second.
     * @code{.cpp}
     * auto output = Sora::Math::Div(6.0F, 2.0F); // 3.0F
     * @endcode
     */
    inline constexpr CPO::DivFn Div{};

    /**
     * @brief Negate a value.
     * @code{.cpp}
     * auto output = Sora::Math::Neg(2.0F); // -2.0F
     * @endcode
     */
    inline constexpr CPO::NegFn Neg{};

    /**
     * @brief Compute the multiplicative inverse of a value.
     * @code{.cpp}
     * auto output = Sora::Math::Inv(4.0F); // 0.25F
     * @endcode
     */
    inline constexpr CPO::InvFn Inv{};

    /**
     * @brief Square a value.
     * @code{.cpp}
     * auto output = Sora::Math::Square(3.0F); // 9.0F
     * @endcode
     */
    inline constexpr CPO::SquareFn Square{};

    /**
     * @brief Compute the absolute value.
     * @code{.cpp}
     * auto output = Sora::Math::Abs(-3.0F); // 3.0F
     * @endcode
     */
    inline constexpr CPO::AbsFn Abs{};

    /**
     * @brief Compute the sine.
     * @code{.cpp}
     * auto output = Sora::Math::Sin(0.0F); // 0.0F
     * @endcode
     */
    inline constexpr CPO::SinFn Sin{};

    /**
     * @brief Compute the cosine.
     * @code{.cpp}
     * auto output = Sora::Math::Cos(0.0F); // 1.0F
     * @endcode
     */
    inline constexpr CPO::CosFn Cos{};

    /**
     * @brief Compute the tangent.
     * @code{.cpp}
     * auto output = Sora::Math::Tan(0.0F); // 0.0F
     * @endcode
     */
    inline constexpr CPO::TanFn Tan{};

    /**
     * @brief Compute the arcsine.
     * @code{.cpp}
     * auto output = Sora::Math::Asin(0.0F); // 0.0F
     * @endcode
     */
    inline constexpr CPO::AsinFn Asin{};

    /**
     * @brief Compute the arccosine.
     * @code{.cpp}
     * auto output = Sora::Math::Acos(1.0F); // 0.0F
     * @endcode
     */
    inline constexpr CPO::AcosFn Acos{};

    /**
     * @brief Compute the arctangent.
     * @code{.cpp}
     * auto output = Sora::Math::Atan(0.0F); // 0.0F
     * @endcode
     */
    inline constexpr CPO::AtanFn Atan{};

    /**
     * @brief Compute the four-quadrant arctangent of @c y and @c x.
     * @code{.cpp}
     * auto output = Sora::Math::Atan2(0.0F, 1.0F); // 0.0F
     * @endcode
     */
    inline constexpr CPO::Atan2Fn Atan2{};

    /**
     * @brief Compute the natural exponential.
     * @code{.cpp}
     * auto output = Sora::Math::Exp(0.0F); // 1.0F
     * @endcode
     */
    inline constexpr CPO::ExpFn Exp{};

    /**
     * @brief Compute the natural logarithm.
     * @code{.cpp}
     * auto output = Sora::Math::Log(1.0F); // 0.0F
     * @endcode
     */
    inline constexpr CPO::LogFn Log{};

    /**
     * @brief Compute the square root.
     * @code{.cpp}
     * auto output = Sora::Math::Sqrt(9.0F); // 3.0F
     * @endcode
     */
    inline constexpr CPO::SqrtFn Sqrt{};

    /**
     * @brief Raise the first operand to the power of the second.
     * @code{.cpp}
     * auto output = Sora::Math::Pow(2.0F, 3.0F); // 8.0F
     * @endcode
     */
    inline constexpr CPO::PowFn Pow{};

    /**
     * @brief Compute @c a*b+c, using a fused operation when supported.
     * @code{.cpp}
     * auto output = Sora::Math::Fma(2.0F, 3.0F, 1.0F); // 7.0F
     * @endcode
     */
    inline constexpr CPO::FmaFn Fma{};

    /**
     * @brief Compute @c a*b-c, using a fused operation when supported.
     * @code{.cpp}
     * auto output = Sora::Math::Mfs(2.0F, 3.0F, 1.0F); // 5.0F
     * @endcode
     */
    inline constexpr CPO::MfsFn Mfs{};

    /**
     * @brief Compute @c -a*b-c, using a fused operation when supported.
     * @code{.cpp}
     * auto output = Sora::Math::Nms(2.0F, 3.0F, 1.0F); // -7.0F
     * @endcode
     */
    inline constexpr CPO::NmsFn Nms{};

    /**
     * @brief Compute @c -a*b+c, using a fused operation when supported.
     * @code{.cpp}
     * auto output = Sora::Math::Nma(2.0F, 3.0F, 1.0F); // -5.0F
     * @endcode
     */
    inline constexpr CPO::NmaFn Nma{};

    /**
     * @brief Linearly interpolate from the first operand to the second.
     * @code{.cpp}
     * auto output = Sora::Math::Lerp(0.0F, 10.0F, 0.25F); // 2.5F
     * @endcode
     */
    inline constexpr CPO::LerpFn Lerp{};

    /**
     * @brief Clamp a value to an inclusive lower and upper bound.
     * @code{.cpp}
     * auto output = Sora::Math::Clamp(12.0F, 0.0F, 10.0F); // 10.0F
     * @endcode
     */
    inline constexpr CPO::ClampFn Clamp{};

    /**
     * @brief Clamp a value to the inclusive unit interval.
     * @code{.cpp}
     * auto output = Sora::Math::Saturate(1.25F); // 1.0F
     * @endcode
     */
    inline constexpr CPO::SaturateFn Saturate{};

    /**
     * @brief Select @p yes where @p condition is true and @p no elsewhere, lane-wise for SIMD carriers.
     * @code{.cpp}
     * auto output = Sora::Math::Select(true, 7.0F, 3.0F); // 7.0F
     * @endcode
     */
    inline constexpr CPO::SelectFn Select{};

    /**
     * @brief Return the signed unit value corresponding to the operand's sign.
     * @code{.cpp}
     * auto output = Sora::Math::Sign(-4.0F); // -1.0F
     * @endcode
     */
    inline constexpr CPO::SignFn Sign{};

} // namespace Sora::Math
