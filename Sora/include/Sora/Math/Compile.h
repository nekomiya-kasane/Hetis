/**
 * @file Compile.h
 * @brief Opt-in compile-time staging and fused evaluation for mathematical CPO expressions.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>

#include <concepts>
#include <cstddef>
#include <meta>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Math {

    namespace Detail {

        template<typename T>
        concept StagedExpression = requires { requires std::remove_cvref_t<T>::kIsStagedExpression; };

        template<std::size_t Index>
        struct StagedInput {
            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = false;
            static constexpr std::size_t kIndex = Index;
        };

        template<typename T>
        struct StagedConstant {
            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = true;

            T value;
        };

        template<std::size_t Position, StagedExpression E, bool Omit = !E::kStoresState>
        struct StagedOperand;

        template<std::size_t Position, StagedExpression E>
        struct StagedOperand<Position, E, false> {
            E value;

            constexpr explicit StagedOperand(E expression) noexcept(std::is_nothrow_move_constructible_v<E>)
                : value(std::move(expression)) {}

            [[nodiscard]] constexpr const E& Get() const noexcept { return value; }
        };

        template<std::size_t Position, StagedExpression E>
        struct StagedOperand<Position, E, true> {
            constexpr StagedOperand() = default;
            constexpr explicit StagedOperand(E) noexcept {}

            [[nodiscard]] static constexpr E Get() noexcept { return {}; }
        };

        template<typename Left, typename Right, bool OmitLeft = !Left::kStoresState,
                 bool OmitRight = !Right::kStoresState>
        struct StagedPair;

        template<typename Left, typename Right>
        struct StagedPair<Left, Right, false, false> {
            static constexpr bool kStoresState = true;

            Left left;
            Right right;

            constexpr StagedPair(Left first, Right second) noexcept(std::is_nothrow_move_constructible_v<Left> &&
                                                                    std::is_nothrow_move_constructible_v<Right>)
                : left(std::move(first)), right(std::move(second)) {}

            [[nodiscard]] constexpr const Left& LeftOperand() const noexcept { return left; }
            [[nodiscard]] constexpr const Right& RightOperand() const noexcept { return right; }
        };

        template<typename Left, typename Right>
        struct StagedPair<Left, Right, true, false> {
            static constexpr bool kStoresState = true;

            Right right;

            constexpr StagedPair(Left, Right second) noexcept(std::is_nothrow_move_constructible_v<Right>)
                : right(std::move(second)) {}

            [[nodiscard]] static constexpr Left LeftOperand() noexcept { return {}; }
            [[nodiscard]] constexpr const Right& RightOperand() const noexcept { return right; }
        };

        template<typename Left, typename Right>
        struct StagedPair<Left, Right, false, true> {
            static constexpr bool kStoresState = true;

            Left left;

            constexpr StagedPair(Left first, Right) noexcept(std::is_nothrow_move_constructible_v<Left>)
                : left(std::move(first)) {}

            [[nodiscard]] constexpr const Left& LeftOperand() const noexcept { return left; }
            [[nodiscard]] static constexpr Right RightOperand() noexcept { return {}; }
        };

        template<typename Left, typename Right>
        struct StagedPair<Left, Right, true, true> {
            static constexpr bool kStoresState = false;

            constexpr StagedPair() = default;
            constexpr StagedPair(Left, Right) noexcept {}

            [[nodiscard]] static constexpr Left LeftOperand() noexcept { return {}; }
            [[nodiscard]] static constexpr Right RightOperand() noexcept { return {}; }
        };

        template<auto Operation, StagedExpression E>
        struct StagedUnary : private StagedOperand<0, E> {
            using OperandStorage = StagedOperand<0, E>;

            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = E::kStoresState;
            static constexpr auto kOperation = Operation;

            constexpr StagedUnary()
                requires(!kStoresState)
            = default;

            constexpr explicit StagedUnary(E operand) noexcept(std::is_nothrow_move_constructible_v<E>)
                : OperandStorage(std::move(operand)) {}

            [[nodiscard]] constexpr decltype(auto) Operand() const noexcept { return OperandStorage::Get(); }
        };

        template<auto Operation, StagedExpression First, StagedExpression Second>
        struct StagedBinary : private StagedPair<First, Second> {
            using Storage = StagedPair<First, Second>;

            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = First::kStoresState || Second::kStoresState;
            static constexpr auto kOperation = Operation;

            constexpr StagedBinary()
                requires(!kStoresState)
            = default;

            constexpr StagedBinary(First first, Second second) noexcept(std::is_nothrow_move_constructible_v<First> &&
                                                                        std::is_nothrow_move_constructible_v<Second>)
                : Storage(std::move(first), std::move(second)) {}

            [[nodiscard]] constexpr decltype(auto) FirstOperand() const noexcept { return Storage::LeftOperand(); }
            [[nodiscard]] constexpr decltype(auto) SecondOperand() const noexcept { return Storage::RightOperand(); }
        };

        template<auto Operation, StagedExpression First, StagedExpression Second, StagedExpression Third>
        struct StagedTernary : private StagedPair<StagedPair<First, Second>, Third> {
            using FirstPair = StagedPair<First, Second>;
            using Storage = StagedPair<FirstPair, Third>;

            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = First::kStoresState || Second::kStoresState || Third::kStoresState;
            static constexpr auto kOperation = Operation;

            constexpr StagedTernary()
                requires(!kStoresState)
            = default;

            constexpr StagedTernary(First first, Second second,
                                    Third third) noexcept(std::is_nothrow_move_constructible_v<First> &&
                                                          std::is_nothrow_move_constructible_v<Second> &&
                                                          std::is_nothrow_move_constructible_v<Third>)
                : Storage(FirstPair(std::move(first), std::move(second)), std::move(third)) {}

            [[nodiscard]] constexpr decltype(auto) FirstOperand() const noexcept {
                if constexpr (!FirstPair::kStoresState) {
                    return First{};
                } else {
                    return Storage::LeftOperand().LeftOperand();
                }
            }

            [[nodiscard]] constexpr decltype(auto) SecondOperand() const noexcept {
                if constexpr (!FirstPair::kStoresState) {
                    return Second{};
                } else {
                    return Storage::LeftOperand().RightOperand();
                }
            }

            [[nodiscard]] constexpr decltype(auto) ThirdOperand() const noexcept { return Storage::RightOperand(); }
        };

        /** @brief Normalized @c Operation(x,x,c) node that evaluates @c x exactly once. */
        template<auto Operation, StagedExpression X, StagedExpression C>
        struct StagedFusedSquare : private StagedPair<X, C> {
            using Storage = StagedPair<X, C>;

            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = X::kStoresState || C::kStoresState;
            static constexpr auto kOperation = Operation;

            constexpr StagedFusedSquare()
                requires(!kStoresState)
            = default;

            constexpr StagedFusedSquare(X value, C addend) noexcept(std::is_nothrow_move_constructible_v<X> &&
                                                                    std::is_nothrow_move_constructible_v<C>)
                : Storage(std::move(value), std::move(addend)) {}

            [[nodiscard]] constexpr decltype(auto) ValueOperand() const noexcept { return Storage::LeftOperand(); }
            [[nodiscard]] constexpr decltype(auto) AddendOperand() const noexcept { return Storage::RightOperand(); }
        };

        template<typename T>
        [[nodiscard]] constexpr auto Stage(T&& value) {
            if constexpr (StagedExpression<T>) {
                return std::remove_cvref_t<T>(std::forward<T>(value));
            } else {
                return StagedConstant<std::remove_cvref_t<T>>{std::forward<T>(value)};
            }
        }

        template<auto Operation, typename T>
        [[nodiscard]] constexpr auto MakeUnary(T&& value) {
            auto operand = Stage(std::forward<T>(value));
            using E = decltype(operand);
            return StagedUnary<Operation, E>{std::move(operand)};
        }

        template<auto Operation, typename First, typename Second>
        [[nodiscard]] constexpr auto MakeBinary(First&& first, Second&& second) {
            auto left = Stage(std::forward<First>(first));
            auto right = Stage(std::forward<Second>(second));
            using L = decltype(left);
            using R = decltype(right);
            return StagedBinary<Operation, L, R>{std::move(left), std::move(right)};
        }

        template<auto Operation, typename First, typename Second, typename Third>
        [[nodiscard]] constexpr auto MakeTernary(First&& first, Second&& second, Third&& third) {
            auto left = Stage(std::forward<First>(first));
            auto middle = Stage(std::forward<Second>(second));
            auto right = Stage(std::forward<Third>(third));
            using L = decltype(left);
            using M = decltype(middle);
            using R = decltype(right);
            return StagedTernary<Operation, L, M, R>{std::move(left), std::move(middle), std::move(right)};
        }

        template<auto Operation, typename First, typename Second, typename... Rest>
        [[nodiscard]] constexpr auto FoldBinary(First&& first, Second&& second, Rest&&... rest) {
            auto accumulated = MakeBinary<Operation>(std::forward<First>(first), std::forward<Second>(second));
            if constexpr (sizeof...(Rest) == 0) {
                return accumulated;
            } else {
                return FoldBinary<Operation>(std::move(accumulated), std::forward<Rest>(rest)...);
            }
        }

        template<auto Operation>
        struct OperationTag {};

        template<auto Operation, StagedExpression E>
        [[nodiscard]] constexpr auto RewriteUnary(OperationTag<Operation>, E operand) {
            return StagedUnary<Operation, E>{std::move(operand)};
        }

        template<auto Operation, StagedExpression Left, StagedExpression Right>
        [[nodiscard]] constexpr auto RewriteBinary(OperationTag<Operation>, Left left, Right right) {
            return StagedBinary<Operation, Left, Right>{std::move(left), std::move(right)};
        }

        template<typename E>
        concept StagedMultiplication = StagedExpression<E> && requires(const std::remove_cvref_t<E>& expression) {
            requires std::same_as<std::remove_cv_t<decltype(std::remove_cvref_t<E>::kOperation)>,
                                  std::remove_cv_t<decltype(Math::Mul)>>;
            expression.FirstOperand();
            expression.SecondOperand();
        };

        template<typename E>
        concept StagedSquare = StagedExpression<E> && requires(const std::remove_cvref_t<E>& expression) {
            requires std::same_as<std::remove_cv_t<decltype(std::remove_cvref_t<E>::kOperation)>,
                                  std::remove_cv_t<decltype(Math::Square)>>;
            expression.Operand();
        };

        template<auto Operation, StagedMultiplication Product, StagedExpression C>
        [[nodiscard]] constexpr auto FuseProduct(const Product& product, C addend) {
            auto first = Stage(product.FirstOperand());
            auto second = Stage(product.SecondOperand());
            return StagedTernary<Operation, decltype(first), decltype(second), C>{std::move(first), std::move(second),
                                                                                  std::move(addend)};
        }

        template<auto Operation, StagedSquare Square, StagedExpression C>
        [[nodiscard]] constexpr auto FuseSquare(const Square& square, C addend) {
            auto value = Stage(square.Operand());
            return StagedFusedSquare<Operation, decltype(value), C>{std::move(value), std::move(addend)};
        }

        template<StagedExpression Left, StagedExpression Right>
        [[nodiscard]] constexpr auto RewriteBinary(OperationTag<Math::Add>, Left left, Right right) {
            if constexpr (StagedMultiplication<Left>) {
                return FuseProduct<Math::Fma>(left, std::move(right));
            } else if constexpr (StagedSquare<Left>) {
                return FuseSquare<Math::Fma>(left, std::move(right));
            } else if constexpr (StagedMultiplication<Right>) {
                return FuseProduct<Math::Fma>(right, std::move(left));
            } else if constexpr (StagedSquare<Right>) {
                return FuseSquare<Math::Fma>(right, std::move(left));
            } else {
                return StagedBinary<Math::Add, Left, Right>{std::move(left), std::move(right)};
            }
        }

        template<StagedExpression Left, StagedExpression Right>
        [[nodiscard]] constexpr auto RewriteBinary(OperationTag<Math::Sub>, Left left, Right right) {
            if constexpr (StagedMultiplication<Left>) {
                return FuseProduct<Math::Mfs>(left, std::move(right));
            } else if constexpr (StagedSquare<Left>) {
                return FuseSquare<Math::Mfs>(left, std::move(right));
            } else if constexpr (StagedMultiplication<Right>) {
                return FuseProduct<Math::Nma>(right, std::move(left));
            } else if constexpr (StagedSquare<Right>) {
                return FuseSquare<Math::Nma>(right, std::move(left));
            } else {
                return StagedBinary<Math::Sub, Left, Right>{std::move(left), std::move(right)};
            }
        }

        template<auto Operation, StagedExpression First, StagedExpression Second, StagedExpression Third>
        [[nodiscard]] constexpr auto RewriteTernary(OperationTag<Operation>, First first, Second second, Third third) {
            return StagedTernary<Operation, First, Second, Third>{std::move(first), std::move(second),
                                                                  std::move(third)};
        }

        template<StagedExpression E>
        struct StagedNormalizer;

        template<StagedExpression E>
        [[nodiscard]] constexpr auto Normalize(const E& expression) {
            return StagedNormalizer<E>::Apply(expression);
        }

        template<std::size_t Index>
        struct StagedNormalizer<StagedInput<Index>> {
            [[nodiscard]] static constexpr StagedInput<Index> Apply(StagedInput<Index> expression) noexcept {
                return expression;
            }
        };

        template<typename T>
        struct StagedNormalizer<StagedConstant<T>> {
            [[nodiscard]] static constexpr StagedConstant<T>
            Apply(const StagedConstant<T>& expression) noexcept(std::is_nothrow_copy_constructible_v<T>) {
                return expression;
            }
        };

        template<auto Operation, StagedExpression E>
        struct StagedNormalizer<StagedUnary<Operation, E>> {
            [[nodiscard]] static constexpr auto Apply(const StagedUnary<Operation, E>& expression) {
                return RewriteUnary(OperationTag<Operation>{}, Normalize(expression.Operand()));
            }
        };

        template<StagedExpression Left, StagedExpression Right>
        struct StagedNormalizer<StagedUnary<Math::Neg, StagedBinary<Math::Add, Left, Right>>> {
            using Sum = StagedBinary<Math::Add, Left, Right>;
            using Expression = StagedUnary<Math::Neg, Sum>;

            [[nodiscard]] static constexpr auto Apply(const Expression& expression) {
                const auto& sum = expression.Operand();
                if constexpr (StagedMultiplication<Left>) {
                    const auto& product = sum.FirstOperand();
                    return RewriteTernary(OperationTag<Math::Nms>{}, Normalize(product.FirstOperand()),
                                          Normalize(product.SecondOperand()), Normalize(sum.SecondOperand()));
                } else if constexpr (StagedMultiplication<Right>) {
                    const auto& product = sum.SecondOperand();
                    return RewriteTernary(OperationTag<Math::Nms>{}, Normalize(product.FirstOperand()),
                                          Normalize(product.SecondOperand()), Normalize(sum.FirstOperand()));
                } else {
                    return RewriteUnary(OperationTag<Math::Neg>{}, Normalize(sum));
                }
            }
        };

        template<auto Operation, StagedExpression First, StagedExpression Second>
        struct StagedNormalizer<StagedBinary<Operation, First, Second>> {
            [[nodiscard]] static constexpr auto Apply(const StagedBinary<Operation, First, Second>& expression) {
                return RewriteBinary(OperationTag<Operation>{}, Normalize(expression.FirstOperand()),
                                     Normalize(expression.SecondOperand()));
            }
        };

        template<auto Operation, StagedExpression First, StagedExpression Second, StagedExpression Third>
        struct StagedNormalizer<StagedTernary<Operation, First, Second, Third>> {
            [[nodiscard]] static constexpr auto
            Apply(const StagedTernary<Operation, First, Second, Third>& expression) {
                return RewriteTernary(OperationTag<Operation>{}, Normalize(expression.FirstOperand()),
                                      Normalize(expression.SecondOperand()), Normalize(expression.ThirdOperand()));
            }
        };

        template<auto Operation, StagedExpression X, StagedExpression C>
        struct StagedNormalizer<StagedFusedSquare<Operation, X, C>> {
            [[nodiscard]] static constexpr auto Apply(const StagedFusedSquare<Operation, X, C>& expression) {
                auto value = Normalize(expression.ValueOperand());
                auto addend = Normalize(expression.AddendOperand());
                return StagedFusedSquare<Operation, decltype(value), decltype(addend)>{std::move(value),
                                                                                       std::move(addend)};
            }
        };

        template<StagedExpression E>
        struct StagedEvaluator;

        template<StagedExpression E, typename... Args>
        [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
        Evaluate(const E& expression,
                 Args&... args) noexcept(noexcept(StagedEvaluator<E>::Evaluate(expression, args...))) {
            return StagedEvaluator<E>::Evaluate(expression, args...);
        }

        template<std::size_t Index>
        struct StagedEvaluator<StagedInput<Index>> {
            template<typename... Args>
                requires(Index < sizeof...(Args))
            [[nodiscard]] static constexpr auto
            Evaluate(const StagedInput<Index>&,
                     Args&... args) noexcept(noexcept(std::remove_cvref_t<decltype(args...[Index])>(args...[Index]))) {
                using T = std::remove_cvref_t<decltype(args...[Index])>;
                return T(args...[Index]);
            }
        };

        template<typename T>
        struct StagedEvaluator<StagedConstant<T>> {
            template<typename... Args>
            [[nodiscard]] static constexpr T Evaluate(const StagedConstant<T>& expression,
                                                      Args&...) noexcept(std::is_nothrow_copy_constructible_v<T>) {
                return expression.value;
            }
        };

        template<auto Operation, StagedExpression E>
        struct StagedEvaluator<StagedUnary<Operation, E>> {
            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto)
            Evaluate(const StagedUnary<Operation, E>& expression,
                     Args&... args) noexcept(noexcept(Operation(Detail::Evaluate(expression.Operand(), args...)))) {
                return Operation(Detail::Evaluate(expression.Operand(), args...));
            }
        };

        template<auto Operation, StagedExpression First, StagedExpression Second>
        struct StagedEvaluator<StagedBinary<Operation, First, Second>> {
            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(
                const StagedBinary<Operation, First, Second>& expression,
                Args&... args) noexcept(noexcept(Operation(Detail::Evaluate(expression.FirstOperand(), args...),
                                                           Detail::Evaluate(expression.SecondOperand(), args...)))) {
                return Operation(Detail::Evaluate(expression.FirstOperand(), args...),
                                 Detail::Evaluate(expression.SecondOperand(), args...));
            }
        };

        template<auto Operation, StagedExpression First, StagedExpression Second, StagedExpression Third>
        struct StagedEvaluator<StagedTernary<Operation, First, Second, Third>> {
            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(
                const StagedTernary<Operation, First, Second, Third>& expression,
                Args&... args) noexcept(noexcept(Operation(Detail::Evaluate(expression.FirstOperand(), args...),
                                                           Detail::Evaluate(expression.SecondOperand(), args...),
                                                           Detail::Evaluate(expression.ThirdOperand(), args...)))) {
                return Operation(Detail::Evaluate(expression.FirstOperand(), args...),
                                 Detail::Evaluate(expression.SecondOperand(), args...),
                                 Detail::Evaluate(expression.ThirdOperand(), args...));
            }
        };

        template<auto Operation, StagedExpression X, StagedExpression C>
        struct StagedEvaluator<StagedFusedSquare<Operation, X, C>> {
            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(
                const StagedFusedSquare<Operation, X, C>& expression,
                Args&... args) noexcept(noexcept(Operation(Detail::Evaluate(expression.ValueOperand(), args...),
                                                           Detail::Evaluate(expression.ValueOperand(), args...),
                                                           Detail::Evaluate(expression.AddendOperand(), args...)))) {
                auto value = Detail::Evaluate(expression.ValueOperand(), args...);
                return Operation(value, value, Detail::Evaluate(expression.AddendOperand(), args...));
            }
        };

        template<std::size_t Arity>
        [[nodiscard]] consteval std::meta::info StagedInputTuple() {
            std::vector<std::meta::info> inputs;
            inputs.reserve(Arity);
            for (std::size_t index = 0; index < Arity; ++index) {
                inputs.push_back(std::meta::substitute(^^StagedInput, {std::meta::reflect_constant(index)}));
            }
            return std::meta::substitute(^^std::tuple, inputs);
        }

    } // namespace Detail

    namespace Backend {

        /** @brief High-priority compile-time backend that records CPO calls as expression nodes. */
        struct Staged {
            static constexpr bool kIsScalar = false;
            static constexpr bool kIsSimd = false;
            static constexpr bool kIsAutodiff = false;
            static constexpr bool kIsStaged = true;
            static constexpr int kPriority = 4;

            template<typename First, typename... Rest>
            [[nodiscard]] static constexpr auto Add(First&& first, Rest&&... rest) {
                if constexpr (sizeof...(Rest) == 0) {
                    return Detail::Stage(std::forward<First>(first));
                } else {
                    return Detail::FoldBinary<Math::Add>(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Sub(First&& first, Second&& second) {
                return Detail::MakeBinary<Math::Sub>(std::forward<First>(first), std::forward<Second>(second));
            }

            template<typename First, typename... Rest>
            [[nodiscard]] static constexpr auto Mul(First&& first, Rest&&... rest) {
                if constexpr (sizeof...(Rest) == 0) {
                    return Detail::Stage(std::forward<First>(first));
                } else {
                    return Detail::FoldBinary<Math::Mul>(std::forward<First>(first), std::forward<Rest>(rest)...);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Div(First&& first, Second&& second) {
                return Detail::MakeBinary<Math::Div>(std::forward<First>(first), std::forward<Second>(second));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Neg(T&& value) {
                return Detail::MakeUnary<Math::Neg>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Inv(T&& value) {
                return Detail::MakeUnary<Math::Inv>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Square(T&& value) {
                return Detail::MakeUnary<Math::Square>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Abs(T&& value) {
                return Detail::MakeUnary<Math::Abs>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Sin(T&& value) {
                return Detail::MakeUnary<Math::Sin>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Cos(T&& value) {
                return Detail::MakeUnary<Math::Cos>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Tan(T&& value) {
                return Detail::MakeUnary<Math::Tan>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Asin(T&& value) {
                return Detail::MakeUnary<Math::Asin>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Acos(T&& value) {
                return Detail::MakeUnary<Math::Acos>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Atan(T&& value) {
                return Detail::MakeUnary<Math::Atan>(std::forward<T>(value));
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Atan2(First&& first, Second&& second) {
                return Detail::MakeBinary<Math::Atan2>(std::forward<First>(first), std::forward<Second>(second));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Exp(T&& value) {
                return Detail::MakeUnary<Math::Exp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Log(T&& value) {
                return Detail::MakeUnary<Math::Log>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Sqrt(T&& value) {
                return Detail::MakeUnary<Math::Sqrt>(std::forward<T>(value));
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Pow(First&& first, Second&& second) {
                return Detail::MakeBinary<Math::Pow>(std::forward<First>(first), std::forward<Second>(second));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Fma(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Math::Fma>(std::forward<First>(first), std::forward<Second>(second),
                                                      std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Mfs(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Math::Mfs>(std::forward<First>(first), std::forward<Second>(second),
                                                      std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nms(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Math::Nms>(std::forward<First>(first), std::forward<Second>(second),
                                                      std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nma(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Math::Nma>(std::forward<First>(first), std::forward<Second>(second),
                                                      std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Lerp(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Math::Lerp>(std::forward<First>(first), std::forward<Second>(second),
                                                       std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Clamp(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Math::Clamp>(std::forward<First>(first), std::forward<Second>(second),
                                                        std::forward<Third>(third));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Saturate(T&& value) {
                return Detail::MakeUnary<Math::Saturate>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Sign(T&& value) {
                return Detail::MakeUnary<Math::Sign>(std::forward<T>(value));
            }
        };

    } // namespace Backend

    namespace Hook {

        template<Detail::StagedExpression E>
        struct Backend<E> {
            using Type = Math::Backend::Staged;
        };

    } // namespace Hook

    /** @brief Stateless evaluator for one staged mathematical expression. */
    template<Detail::StagedExpression E, std::size_t Arity>
    class CompiledKernel {
    public:
        static constexpr std::size_t kArity = Arity;

        constexpr explicit CompiledKernel(E expression) noexcept(std::is_nothrow_move_constructible_v<E>)
            : expression_(std::move(expression)) {}

        template<typename... Args>
            requires(sizeof...(Args) == Arity)
        [[nodiscard, gnu::always_inline]] constexpr decltype(auto) operator()(Args&&... args) const
            noexcept(noexcept(Detail::Evaluate(expression_, args...))) {
            return Detail::Evaluate(expression_, args...);
        }

    private:
        E expression_;
    };

    /**
     * @brief Stage @p function at compile time and return a fused evaluator with @p Arity inputs.
     * @details The function must express its mathematics through the public Math CPOs. Staging is opt-in and may
     * replace local multiply-add/subtract patterns with explicitly fused primitives, changing intermediate rounding.
     */
    template<std::size_t Arity, typename Function>
    [[nodiscard]] consteval auto Compile(Function&& function) {
        using Inputs = [:Detail::StagedInputTuple<Arity>():];
        auto expression = Detail::Normalize(Detail::Stage(std::apply(std::forward<Function>(function), Inputs{})));
        return CompiledKernel<decltype(expression), Arity>{std::move(expression)};
    }

} // namespace Sora::Math
