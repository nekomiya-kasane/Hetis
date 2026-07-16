/**
 * @file Autodiff.h
 * @brief Zero-allocation forward automatic differentiation over unified mathematical primitives.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>

#include <concepts>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief A primal value paired with one directional derivative. */
    template<DifferentiableValue P>
    struct Dual {
        using PrimalType = P;

        P primal;
        P tangent;

        static constexpr bool kIsDual = true;
    };

    template<typename T>
    concept DualValue = requires {
        requires std::remove_cvref_t<T>::kIsDual;
        typename std::remove_cvref_t<T>::PrimalType;
    };

    template<DifferentiableValue P>
    [[nodiscard]] constexpr auto Variable(P primal) noexcept {
        using T = std::remove_cvref_t<P>;
        if constexpr (Simd::SimdVecType<T>) {
            return Dual<T>{std::move(primal), T(typename T::ValueType{1})};
        } else {
            return Dual<T>{std::move(primal), T{1}};
        }
    }

    namespace Backend {

        struct ForwardAD;

    } // namespace Backend

    namespace Hook {

        template<DifferentiableValue P>
        struct Backend<Dual<P>> {
            using Type = Math::Backend::ForwardAD;
        };

    } // namespace Hook

    namespace Backend {

        /** @brief Primitive JVP backend for @ref Dual carriers. */
        struct ForwardAD {
            static constexpr bool kIsScalar = false;
            static constexpr bool kIsSimd = false;
            static constexpr bool kIsAutodiff = true;
            static constexpr int kPriority = 2;

        private:
            template<typename T>
            [[nodiscard]] static constexpr decltype(auto) Primal(T&& value) noexcept {
                if constexpr (DualValue<T>) {
                    return std::forward<T>(value).primal;
                } else {
                    return std::forward<T>(value);
                }
            }

            template<typename P>
            [[nodiscard]] static constexpr auto ZeroLike(const P&) noexcept {
                using T = std::remove_cvref_t<P>;
                if constexpr (Simd::SimdVecType<T>) {
                    return T(typename T::ValueType{});
                } else {
                    return T{};
                }
            }

            template<typename P>
            [[nodiscard]] static constexpr auto OneLike(const P&) noexcept {
                using T = std::remove_cvref_t<P>;
                if constexpr (Simd::SimdVecType<T>) {
                    return T(typename T::ValueType{1});
                } else {
                    return T{1};
                }
            }

            template<typename T, typename P>
            [[nodiscard]] static constexpr decltype(auto) Tangent(T&& value, const P& resultPrototype) noexcept {
                if constexpr (DualValue<T>) {
                    return std::forward<T>(value).tangent;
                } else {
                    return ZeroLike(resultPrototype);
                }
            }

            template<typename Condition, typename T>
            [[nodiscard]] static constexpr T Select(const Condition& condition, const T& yes, const T& no) noexcept {
                if constexpr (std::same_as<std::remove_cvref_t<Condition>, bool>) {
                    return condition ? yes : no;
                } else {
                    return Simd::Select(condition, yes, no);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto MulTwo(First&& first, Second&& second) noexcept {
                auto primal = Math::Mul(Primal(first), Primal(second));
                auto tangent = Math::Add(Math::Mul(Tangent(first, primal), Primal(second)),
                                         Math::Mul(Primal(first), Tangent(second, primal)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

        public:
            template<DualValue T>
            [[nodiscard]] static constexpr auto Add(T&& value) noexcept {
                return std::remove_cvref_t<T>(std::forward<T>(value));
            }

            template<typename First, typename... Rest>
            [[nodiscard]] static constexpr auto Add(First&& first, Rest&&... rest) noexcept {
                auto primal = Math::Add(Primal(first), Primal(rest)...);
                auto tangent = Math::Add(Tangent(first, primal), Tangent(rest, primal)...);
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Mul(T&& value) noexcept {
                return std::remove_cvref_t<T>(std::forward<T>(value));
            }

            template<typename First, typename Second, typename... Rest>
            [[nodiscard]] static constexpr auto Mul(First&& first, Second&& second, Rest&&... rest) noexcept {
                auto product = MulTwo(std::forward<First>(first), std::forward<Second>(second));
                if constexpr (sizeof...(Rest) == 0) {
                    return product;
                } else {
                    return Mul(std::move(product), std::forward<Rest>(rest)...);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Sub(First&& first, Second&& second) noexcept {
                auto primal = Math::Sub(Primal(first), Primal(second));
                auto tangent = Math::Sub(Tangent(first, primal), Tangent(second, primal));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Div(First&& first, Second&& second) noexcept {
                auto primal = Math::Div(Primal(first), Primal(second));
                auto numerator = Math::Sub(Math::Mul(Tangent(first, primal), Primal(second)),
                                           Math::Mul(Primal(first), Tangent(second, primal)));
                auto tangent = Math::Div(numerator, Math::Square(Primal(second)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Neg(T&& x) noexcept {
                return Dual{Math::Neg(x.primal), Math::Neg(x.tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Inv(T&& x) noexcept {
                auto primal = Math::Inv(x.primal);
                auto tangent = Math::Neg(Math::Div(x.tangent, Math::Square(x.primal)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Square(T&& x) noexcept {
                auto primal = Math::Square(x.primal);
                auto tangent = Math::Mul(2, x.primal, x.tangent);
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Abs(T&& x) noexcept {
                auto primal = Math::Abs(x.primal);
                auto tangent = Math::Mul(Math::Sign(x.primal), x.tangent);
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Sin(T&& x) noexcept {
                auto primal = Math::Sin(x.primal);
                auto tangent = Math::Mul(Math::Cos(x.primal), x.tangent);
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Cos(T&& x) noexcept {
                auto primal = Math::Cos(x.primal);
                auto tangent = Math::Neg(Math::Mul(Math::Sin(x.primal), x.tangent));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Tan(T&& x) noexcept {
                auto primal = Math::Tan(x.primal);
                auto tangent = Math::Div(x.tangent, Math::Square(Math::Cos(x.primal)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Asin(T&& x) noexcept {
                auto primal = Math::Asin(x.primal);
                auto tangent = Math::Div(x.tangent, Math::Sqrt(Math::Sub(OneLike(x.primal), Math::Square(x.primal))));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Acos(T&& x) noexcept {
                auto primal = Math::Acos(x.primal);
                auto tangent =
                    Math::Neg(Math::Div(x.tangent, Math::Sqrt(Math::Sub(OneLike(x.primal), Math::Square(x.primal)))));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Atan(T&& x) noexcept {
                auto primal = Math::Atan(x.primal);
                auto tangent = Math::Div(x.tangent, Math::Add(OneLike(x.primal), Math::Square(x.primal)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Atan2(First&& y, Second&& x) noexcept {
                auto primal = Math::Atan2(Primal(y), Primal(x));
                auto numerator =
                    Math::Sub(Math::Mul(Primal(x), Tangent(y, primal)), Math::Mul(Primal(y), Tangent(x, primal)));
                auto denominator = Math::Add(Math::Square(Primal(x)), Math::Square(Primal(y)));
                auto tangent = Math::Div(numerator, denominator);
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Exp(T&& x) noexcept {
                auto primal = Math::Exp(x.primal);
                auto tangent = Math::Mul(primal, x.tangent);
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Log(T&& x) noexcept {
                auto primal = Math::Log(x.primal);
                auto tangent = Math::Div(x.tangent, x.primal);
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Sqrt(T&& x) noexcept {
                auto primal = Math::Sqrt(x.primal);
                auto tangent = Math::Div(x.tangent, Math::Mul(2, primal));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Pow(First&& base, Second&& exponent) noexcept {
                auto primal = Math::Pow(Primal(base), Primal(exponent));
                if constexpr (!DualValue<Second>) {
                    auto tangent = Math::Mul(Primal(exponent), Math::Pow(Primal(base), Math::Sub(Primal(exponent), 1)),
                                             Tangent(base, primal));
                    return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
                } else {
                    auto tangent = Math::Mul(
                        primal, Math::Add(Math::Mul(exponent.tangent, Math::Log(Primal(base))),
                                          Math::Div(Math::Mul(Primal(exponent), Tangent(base, primal)), Primal(base))));
                    return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
                }
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Fma(First&& a, Second&& b, Third&& c) noexcept {
                auto primal = Math::Fma(Primal(a), Primal(b), Primal(c));
                auto tangent = Math::Add(Math::Mul(Tangent(a, primal), Primal(b)),
                                         Math::Mul(Primal(a), Tangent(b, primal)), Tangent(c, primal));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Mfs(First&& a, Second&& b, Third&& c) noexcept {
                auto primal = Math::Mfs(Primal(a), Primal(b), Primal(c));
                auto tangent = Math::Sub(
                    Math::Add(Math::Mul(Tangent(a, primal), Primal(b)), Math::Mul(Primal(a), Tangent(b, primal))),
                    Tangent(c, primal));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nms(First&& a, Second&& b, Third&& c) noexcept {
                auto primal = Math::Nms(Primal(a), Primal(b), Primal(c));
                auto tangent = Math::Neg(Math::Add(Math::Mul(Tangent(a, primal), Primal(b)),
                                                   Math::Mul(Primal(a), Tangent(b, primal)), Tangent(c, primal)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nma(First&& a, Second&& b, Third&& c) noexcept {
                auto primal = Math::Nma(Primal(a), Primal(b), Primal(c));
                auto tangent = Math::Sub(Tangent(c, primal), Math::Add(Math::Mul(Tangent(a, primal), Primal(b)),
                                                                       Math::Mul(Primal(a), Tangent(b, primal))));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Lerp(First&& a, Second&& b, Third&& t) noexcept {
                auto primal = Math::Lerp(Primal(a), Primal(b), Primal(t));
                auto tangent = Math::Add(Tangent(a, primal),
                                         Math::Mul(Primal(t), Math::Sub(Tangent(b, primal), Tangent(a, primal))),
                                         Math::Mul(Math::Sub(Primal(b), Primal(a)), Tangent(t, primal)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<typename Value, typename Lower, typename Upper>
            [[nodiscard]] static constexpr auto Clamp(Value&& value, Lower&& lower, Upper&& upper) noexcept {
                auto primal = Math::Clamp(Primal(value), Primal(lower), Primal(upper));
                auto tangent =
                    Select(Primal(value) < Primal(lower), Tangent(lower, primal),
                           Select(Primal(upper) < Primal(value), Tangent(upper, primal), Tangent(value, primal)));
                return Dual<decltype(primal)>{std::move(primal), std::move(tangent)};
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Saturate(T&& x) noexcept {
                return Clamp(std::forward<T>(x), ZeroLike(x.primal), OneLike(x.primal));
            }

            template<DualValue T>
            [[nodiscard]] static constexpr auto Sign(T&& x) noexcept {
                auto primal = Math::Sign(x.primal);
                return Dual<decltype(primal)>{std::move(primal), ZeroLike(x.tangent)};
            }
        };

    } // namespace Backend

} // namespace Sora::Math

#include <Sora/Math/ReverseAutodiff.h>
