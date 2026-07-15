/**
 * @file PrimaryFunctions.h
 * @brief Extensible backend-dispatched arithmetic and transcendental customization-point objects.
 * @ingroup Math
 *
 * @details Each CPO directly selects a backend from the first operand's carrier type at compile time and calls the
 * corresponding static backend operation. Built-in arithmetic values use @ref Backend::ScalarCPU, fixed-width SIMD
 * vectors use @ref Backend::FixedSimdCPU, and user-defined carriers opt in through @ref Hook::Backend. The CPOs are
 * stateless regular callables, so @ref Sora::Compose and @ref Sora::Then build fully inlinable compound functions
 * without allocation, virtual dispatch, type erasure, or runtime backend selection.
 */
#pragma once

#include <type_traits>
#include <utility>

#include <Sora/Core/Functional.h>
#include <Sora/Math/Backends.h>

namespace Sora::Math {

    namespace CPO {

        /** @cond INTERNAL */
        template<typename Carrier>
        using BackendFor = Traits::BackendTypeOf<std::remove_cvref_t<Carrier>>;
        /** @endcond */

        /** @brief CPO implementing backend-dispatched addition. */
        struct AddFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First>::Add(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First>::Add(std::forward<First>(first), std::forward<Rest>(rest)...))) {
                return BackendFor<First>::Add(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /** @brief CPO implementing backend-dispatched subtraction. */
        struct SubFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First>::Sub(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First>::Sub(std::forward<First>(first), std::forward<Rest>(rest)...))) {
                return BackendFor<First>::Sub(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /** @brief CPO implementing backend-dispatched multiplication. */
        struct MulFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First>::Mul(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First>::Mul(std::forward<First>(first), std::forward<Rest>(rest)...))) {
                return BackendFor<First>::Mul(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /** @brief CPO implementing backend-dispatched division. */
        struct DivFn {
            template<typename First, typename... Rest>
                requires requires(First&& first, Rest&&... rest) {
                    BackendFor<First>::Div(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Rest&&... rest) const
                noexcept(noexcept(BackendFor<First>::Div(std::forward<First>(first), std::forward<Rest>(rest)...))) {
                return BackendFor<First>::Div(std::forward<First>(first), std::forward<Rest>(rest)...);
            }
        };

        /** @brief CPO implementing backend-dispatched negation. */
        struct NegFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Neg(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Neg(std::forward<First>(first)))) {
                return BackendFor<First>::Neg(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched multiplicative inversion. */
        struct InvFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Inv(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Inv(std::forward<First>(first)))) {
                return BackendFor<First>::Inv(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched squaring. */
        struct SquareFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Square(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Square(std::forward<First>(first)))) {
                return BackendFor<First>::Square(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched absolute value. */
        struct AbsFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Abs(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Abs(std::forward<First>(first)))) {
                return BackendFor<First>::Abs(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched sine. */
        struct SinFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Sin(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Sin(std::forward<First>(first)))) {
                return BackendFor<First>::Sin(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched cosine. */
        struct CosFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Cos(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Cos(std::forward<First>(first)))) {
                return BackendFor<First>::Cos(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched tangent. */
        struct TanFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Tan(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Tan(std::forward<First>(first)))) {
                return BackendFor<First>::Tan(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched arcsine. */
        struct AsinFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Asin(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Asin(std::forward<First>(first)))) {
                return BackendFor<First>::Asin(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched arccosine. */
        struct AcosFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Acos(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Acos(std::forward<First>(first)))) {
                return BackendFor<First>::Acos(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched arctangent. */
        struct AtanFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Atan(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Atan(std::forward<First>(first)))) {
                return BackendFor<First>::Atan(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched four-quadrant arctangent. */
        struct Atan2Fn {
            template<typename First, typename Second>
                requires requires(First&& first, Second&& second) {
                    BackendFor<First>::Atan2(std::forward<First>(first), std::forward<Second>(second));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second) const
                noexcept(noexcept(BackendFor<First>::Atan2(std::forward<First>(first),
                                                            std::forward<Second>(second)))) {
                return BackendFor<First>::Atan2(std::forward<First>(first), std::forward<Second>(second));
            }
        };

        /** @brief CPO implementing backend-dispatched natural exponential. */
        struct ExpFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Exp(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Exp(std::forward<First>(first)))) {
                return BackendFor<First>::Exp(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched natural logarithm. */
        struct LogFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Log(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Log(std::forward<First>(first)))) {
                return BackendFor<First>::Log(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched square root. */
        struct SqrtFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Sqrt(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Sqrt(std::forward<First>(first)))) {
                return BackendFor<First>::Sqrt(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched exponentiation. */
        struct PowFn {
            template<typename First, typename Second>
                requires requires(First&& first, Second&& second) {
                    BackendFor<First>::Pow(std::forward<First>(first), std::forward<Second>(second));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first, Second&& second) const
                noexcept(noexcept(BackendFor<First>::Pow(std::forward<First>(first), std::forward<Second>(second)))) {
                return BackendFor<First>::Pow(std::forward<First>(first), std::forward<Second>(second));
            }
        };

        /** @brief CPO implementing backend-dispatched @c a*b+c. */
        struct FmaFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First>::Fma(std::forward<First>(first), std::forward<Second>(second),
                                           std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
            operator()(First&& first, Second&& second, Third&& third) const
                noexcept(noexcept(BackendFor<First>::Fma(std::forward<First>(first), std::forward<Second>(second),
                                                         std::forward<Third>(third)))) {
                return BackendFor<First>::Fma(std::forward<First>(first), std::forward<Second>(second),
                                               std::forward<Third>(third));
            }
        };

        /** @brief CPO implementing backend-dispatched @c a*b-c. */
        struct MfsFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First>::Mfs(std::forward<First>(first), std::forward<Second>(second),
                                           std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
            operator()(First&& first, Second&& second, Third&& third) const
                noexcept(noexcept(BackendFor<First>::Mfs(std::forward<First>(first), std::forward<Second>(second),
                                                         std::forward<Third>(third)))) {
                return BackendFor<First>::Mfs(std::forward<First>(first), std::forward<Second>(second),
                                               std::forward<Third>(third));
            }
        };

        /** @brief CPO implementing backend-dispatched @c -a*b-c. */
        struct NmsFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First>::Nms(std::forward<First>(first), std::forward<Second>(second),
                                           std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
            operator()(First&& first, Second&& second, Third&& third) const
                noexcept(noexcept(BackendFor<First>::Nms(std::forward<First>(first), std::forward<Second>(second),
                                                         std::forward<Third>(third)))) {
                return BackendFor<First>::Nms(std::forward<First>(first), std::forward<Second>(second),
                                               std::forward<Third>(third));
            }
        };

        /** @brief CPO implementing backend-dispatched @c -a*b+c. */
        struct NmaFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First>::Nma(std::forward<First>(first), std::forward<Second>(second),
                                           std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
            operator()(First&& first, Second&& second, Third&& third) const
                noexcept(noexcept(BackendFor<First>::Nma(std::forward<First>(first), std::forward<Second>(second),
                                                         std::forward<Third>(third)))) {
                return BackendFor<First>::Nma(std::forward<First>(first), std::forward<Second>(second),
                                               std::forward<Third>(third));
            }
        };

        /** @brief CPO implementing backend-dispatched linear interpolation. */
        struct LerpFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First>::Lerp(std::forward<First>(first), std::forward<Second>(second),
                                            std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
            operator()(First&& first, Second&& second, Third&& third) const
                noexcept(noexcept(BackendFor<First>::Lerp(std::forward<First>(first), std::forward<Second>(second),
                                                          std::forward<Third>(third)))) {
                return BackendFor<First>::Lerp(std::forward<First>(first), std::forward<Second>(second),
                                                std::forward<Third>(third));
            }
        };

        /** @brief CPO implementing backend-dispatched clamping. */
        struct ClampFn {
            template<typename First, typename Second, typename Third>
                requires requires(First&& first, Second&& second, Third&& third) {
                    BackendFor<First>::Clamp(std::forward<First>(first), std::forward<Second>(second),
                                             std::forward<Third>(third));
                }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
            operator()(First&& first, Second&& second, Third&& third) const
                noexcept(noexcept(BackendFor<First>::Clamp(std::forward<First>(first), std::forward<Second>(second),
                                                           std::forward<Third>(third)))) {
                return BackendFor<First>::Clamp(std::forward<First>(first), std::forward<Second>(second),
                                                 std::forward<Third>(third));
            }
        };

        /** @brief CPO implementing backend-dispatched unit-interval clamping. */
        struct SaturateFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Saturate(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Saturate(std::forward<First>(first)))) {
                return BackendFor<First>::Saturate(std::forward<First>(first));
            }
        };

        /** @brief CPO implementing backend-dispatched sign extraction. */
        struct SignFn {
            template<typename First>
                requires requires(First&& first) { BackendFor<First>::Sign(std::forward<First>(first)); }
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(First&& first) const
                noexcept(noexcept(BackendFor<First>::Sign(std::forward<First>(first)))) {
                return BackendFor<First>::Sign(std::forward<First>(first));
            }
        };

    } // namespace CPO

    /** @brief Add one or more values using the backend selected from the first operand. */
    inline constexpr CPO::AddFn Add{};

    /** @brief Subtract the second operand from the first. */
    inline constexpr CPO::SubFn Sub{};

    /** @brief Multiply one or more values. */
    inline constexpr CPO::MulFn Mul{};

    /** @brief Divide the first operand by the second. */
    inline constexpr CPO::DivFn Div{};

    /** @brief Negate a value. */
    inline constexpr CPO::NegFn Neg{};

    /** @brief Compute the multiplicative inverse of a value. */
    inline constexpr CPO::InvFn Inv{};

    /** @brief Square a value. */
    inline constexpr CPO::SquareFn Square{};

    /** @brief Compute the absolute value. */
    inline constexpr CPO::AbsFn Abs{};

    /** @brief Compute the sine. */
    inline constexpr CPO::SinFn Sin{};

    /** @brief Compute the cosine. */
    inline constexpr CPO::CosFn Cos{};

    /** @brief Compute the tangent. */
    inline constexpr CPO::TanFn Tan{};

    /** @brief Compute the arcsine. */
    inline constexpr CPO::AsinFn Asin{};

    /** @brief Compute the arccosine. */
    inline constexpr CPO::AcosFn Acos{};

    /** @brief Compute the arctangent. */
    inline constexpr CPO::AtanFn Atan{};

    /** @brief Compute the four-quadrant arctangent of @c y and @c x. */
    inline constexpr CPO::Atan2Fn Atan2{};

    /** @brief Compute the natural exponential. */
    inline constexpr CPO::ExpFn Exp{};

    /** @brief Compute the natural logarithm. */
    inline constexpr CPO::LogFn Log{};

    /** @brief Compute the square root. */
    inline constexpr CPO::SqrtFn Sqrt{};

    /** @brief Raise the first operand to the power of the second. */
    inline constexpr CPO::PowFn Pow{};

    /** @brief Compute @c a*b+c, using a fused operation when supported. */
    inline constexpr CPO::FmaFn Fma{};

    /** @brief Compute @c a*b-c, using a fused operation when supported. */
    inline constexpr CPO::MfsFn Mfs{};

    /** @brief Compute @c -a*b-c, using a fused operation when supported. */
    inline constexpr CPO::NmsFn Nms{};

    /** @brief Compute @c -a*b+c, using a fused operation when supported. */
    inline constexpr CPO::NmaFn Nma{};

    /** @brief Linearly interpolate from the first operand to the second. */
    inline constexpr CPO::LerpFn Lerp{};

    /** @brief Clamp a value to an inclusive lower and upper bound. */
    inline constexpr CPO::ClampFn Clamp{};

    /** @brief Clamp a value to the inclusive unit interval. */
    inline constexpr CPO::SaturateFn Saturate{};

    /** @brief Return the signed unit value corresponding to the operand's sign. */
    inline constexpr CPO::SignFn Sign{};

} // namespace Sora::Math
