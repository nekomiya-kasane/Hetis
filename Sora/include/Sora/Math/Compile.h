/**
 * @file Compile.h
 * @brief Opt-in compile-time staging and fused evaluation for mathematical CPO expressions.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <meta>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Math {

    namespace Detail {

        enum class StagedOperation : std::uint8_t {
            AddOp,
            SubOp,
            MulOp,
            DivOp,
            NegOp,
            InvOp,
            SquareOp,
            AbsOp,
            SinOp,
            CosOp,
            TanOp,
            ASinOp,
            ACosOp,
            ATanOp,
            ATan2Op,
            ExpOp,
            LogOp,
            SqrtOp,
            PowOp,
            FmaOp,
            MfsOp,
            NmsOp,
            NmaOp,
            LerpOp,
            ClampOp,
            SaturateOp,
            SignOp,
        };

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

        template<StagedOperation Operation, StagedExpression E>
        struct StagedUnary : private StagedOperand<0, E> {
            using OperandStorage = StagedOperand<0, E>;

            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = E::kStoresState;
            static constexpr StagedOperation kOperation = Operation;

            constexpr StagedUnary()
                requires(!kStoresState)
            = default;

            constexpr explicit StagedUnary(E operand) noexcept(std::is_nothrow_move_constructible_v<E>)
                : OperandStorage(std::move(operand)) {}

            [[nodiscard]] constexpr decltype(auto) Operand() const noexcept { return OperandStorage::Get(); }
        };

        template<StagedOperation Operation, StagedExpression First, StagedExpression Second>
        struct StagedBinary : private StagedPair<First, Second> {
            using Storage = StagedPair<First, Second>;

            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = First::kStoresState || Second::kStoresState;
            static constexpr StagedOperation kOperation = Operation;

            constexpr StagedBinary()
                requires(!kStoresState)
            = default;

            constexpr StagedBinary(First first, Second second) noexcept(std::is_nothrow_move_constructible_v<First> &&
                                                                        std::is_nothrow_move_constructible_v<Second>)
                : Storage(std::move(first), std::move(second)) {}

            [[nodiscard]] constexpr decltype(auto) FirstOperand() const noexcept { return Storage::LeftOperand(); }
            [[nodiscard]] constexpr decltype(auto) SecondOperand() const noexcept { return Storage::RightOperand(); }
        };

        template<StagedOperation Operation, StagedExpression First, StagedExpression Second, StagedExpression Third>
        struct StagedTernary : private StagedPair<StagedPair<First, Second>, Third> {
            using FirstPair = StagedPair<First, Second>;
            using Storage = StagedPair<FirstPair, Third>;

            static constexpr bool kIsStagedExpression = true;
            static constexpr bool kStoresState = First::kStoresState || Second::kStoresState || Third::kStoresState;
            static constexpr StagedOperation kOperation = Operation;

            constexpr StagedTernary()
                requires(!kStoresState)
            = default;

            constexpr StagedTernary(First first, Second second, Third third) noexcept(
                std::is_nothrow_move_constructible_v<First> && std::is_nothrow_move_constructible_v<Second> &&
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

        template<typename T>
        [[nodiscard]] constexpr auto Stage(T&& value) {
            if constexpr (StagedExpression<T>) {
                return std::remove_cvref_t<T>(std::forward<T>(value));
            } else {
                return StagedConstant<std::remove_cvref_t<T>>{std::forward<T>(value)};
            }
        }

        template<StagedOperation Operation, typename T>
        [[nodiscard]] constexpr auto MakeUnary(T&& value) {
            auto operand = Stage(std::forward<T>(value));
            using E = decltype(operand);
            return StagedUnary<Operation, E>{std::move(operand)};
        }

        template<StagedOperation Operation, typename First, typename Second>
        [[nodiscard]] constexpr auto MakeBinary(First&& first, Second&& second) {
            auto left = Stage(std::forward<First>(first));
            auto right = Stage(std::forward<Second>(second));
            using L = decltype(left);
            using R = decltype(right);
            return StagedBinary<Operation, L, R>{std::move(left), std::move(right)};
        }

        template<StagedOperation Operation, typename First, typename Second, typename Third>
        [[nodiscard]] constexpr auto MakeTernary(First&& first, Second&& second, Third&& third) {
            auto left = Stage(std::forward<First>(first));
            auto middle = Stage(std::forward<Second>(second));
            auto right = Stage(std::forward<Third>(third));
            using L = decltype(left);
            using M = decltype(middle);
            using R = decltype(right);
            return StagedTernary<Operation, L, M, R>{std::move(left), std::move(middle), std::move(right)};
        }

        template<StagedOperation Operation, typename First, typename Second, typename... Rest>
        [[nodiscard]] constexpr auto FoldBinary(First&& first, Second&& second, Rest&&... rest) {
            auto accumulated = MakeBinary<Operation>(std::forward<First>(first), std::forward<Second>(second));
            if constexpr (sizeof...(Rest) == 0) {
                return accumulated;
            } else {
                return FoldBinary<Operation>(std::move(accumulated), std::forward<Rest>(rest)...);
            }
        }

        template<StagedOperation Operation>
        inline constexpr auto kOperationFunction = [] { static_assert(Operation != Operation, "invalid operation"); };

        template<>
        inline constexpr auto kOperationFunction<StagedOperation::AddOp> = Math::Add;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::SubOp> = Math::Sub;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::MulOp> = Math::Mul;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::DivOp> = Math::Div;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::NegOp> = Math::Neg;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::InvOp> = Math::Inv;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::SquareOp> = Math::Square;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::AbsOp> = Math::Abs;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::SinOp> = Math::Sin;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::CosOp> = Math::Cos;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::TanOp> = Math::Tan;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::ASinOp> = Math::Asin;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::ACosOp> = Math::Acos;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::ATanOp> = Math::Atan;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::ATan2Op> = Math::Atan2;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::ExpOp> = Math::Exp;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::LogOp> = Math::Log;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::SqrtOp> = Math::Sqrt;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::PowOp> = Math::Pow;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::FmaOp> = Math::Fma;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::MfsOp> = Math::Mfs;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::NmsOp> = Math::Nms;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::NmaOp> = Math::Nma;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::LerpOp> = Math::Lerp;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::ClampOp> = Math::Clamp;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::SaturateOp> = Math::Saturate;
        template<>
        inline constexpr auto kOperationFunction<StagedOperation::SignOp> = Math::Sign;

        template<StagedExpression E>
        struct StagedEvaluator;

        template<StagedExpression E, typename... Args>
        [[nodiscard, gnu::always_inline]] constexpr decltype(auto) Evaluate(const E& expression, Args&... args)
            noexcept(noexcept(StagedEvaluator<E>::Evaluate(expression, args...))) {
            return StagedEvaluator<E>::Evaluate(expression, args...);
        }

        template<std::size_t Index>
        struct StagedEvaluator<StagedInput<Index>> {
            template<typename... Args>
                requires(Index < sizeof...(Args))
            [[nodiscard]] static constexpr auto Evaluate(const StagedInput<Index>&, Args&... args) noexcept {
                using T = std::remove_cvref_t<decltype(args...[Index])>;
                return T(args...[Index]);
            }
        };

        template<typename T>
        struct StagedEvaluator<StagedConstant<T>> {
            template<typename... Args>
            [[nodiscard]] static constexpr T Evaluate(const StagedConstant<T>& expression, Args&...) noexcept(
                std::is_nothrow_copy_constructible_v<T>) {
                return expression.value;
            }
        };

        template<StagedOperation Operation, StagedExpression E>
        struct StagedEvaluator<StagedUnary<Operation, E>> {
            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const StagedUnary<Operation, E>& expression,
                                                                   Args&... args) noexcept(noexcept(std::invoke(
                kOperationFunction<Operation>, Detail::Evaluate(expression.Operand(), args...)))) {
                return std::invoke(kOperationFunction<Operation>, Detail::Evaluate(expression.Operand(), args...));
            }
        };

        template<StagedOperation Operation, StagedExpression First, StagedExpression Second>
        struct StagedEvaluator<StagedBinary<Operation, First, Second>> {
            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(
                const StagedBinary<Operation, First, Second>& expression, Args&... args) noexcept(noexcept(std::invoke(
                kOperationFunction<Operation>, Detail::Evaluate(expression.FirstOperand(), args...),
                Detail::Evaluate(expression.SecondOperand(), args...)))) {
                return std::invoke(kOperationFunction<Operation>, Detail::Evaluate(expression.FirstOperand(), args...),
                                   Detail::Evaluate(expression.SecondOperand(), args...));
            }
        };

        template<StagedOperation Operation, StagedExpression First, StagedExpression Second, StagedExpression Third>
        struct StagedEvaluator<StagedTernary<Operation, First, Second, Third>> {
            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(
                const StagedTernary<Operation, First, Second, Third>& expression,
                Args&... args) noexcept(noexcept(std::invoke(
                kOperationFunction<Operation>, Detail::Evaluate(expression.FirstOperand(), args...),
                Detail::Evaluate(expression.SecondOperand(), args...),
                Detail::Evaluate(expression.ThirdOperand(), args...)))) {
                return std::invoke(kOperationFunction<Operation>, Detail::Evaluate(expression.FirstOperand(), args...),
                                   Detail::Evaluate(expression.SecondOperand(), args...),
                                   Detail::Evaluate(expression.ThirdOperand(), args...));
            }
        };

        template<StagedExpression A, StagedExpression B, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::AddOp, StagedBinary<StagedOperation::MulOp, A, B>, C>> {
            using Product = StagedBinary<StagedOperation::MulOp, A, B>;
            using Expression = StagedBinary<StagedOperation::AddOp, Product, C>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Fma(Detail::Evaluate(expression.FirstOperand().FirstOperand(), args...),
                                            Detail::Evaluate(expression.FirstOperand().SecondOperand(), args...),
                                            Detail::Evaluate(expression.SecondOperand(), args...)))) {
                const auto& product = expression.FirstOperand();
                return Math::Fma(Detail::Evaluate(product.FirstOperand(), args...),
                                 Detail::Evaluate(product.SecondOperand(), args...),
                                 Detail::Evaluate(expression.SecondOperand(), args...));
            }
        };

        template<StagedExpression A, StagedExpression B, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::AddOp, C, StagedBinary<StagedOperation::MulOp, A, B>>> {
            using Product = StagedBinary<StagedOperation::MulOp, A, B>;
            using Expression = StagedBinary<StagedOperation::AddOp, C, Product>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Fma(Detail::Evaluate(expression.SecondOperand().FirstOperand(), args...),
                                            Detail::Evaluate(expression.SecondOperand().SecondOperand(), args...),
                                            Detail::Evaluate(expression.FirstOperand(), args...)))) {
                const auto& product = expression.SecondOperand();
                return Math::Fma(Detail::Evaluate(product.FirstOperand(), args...),
                                 Detail::Evaluate(product.SecondOperand(), args...),
                                 Detail::Evaluate(expression.FirstOperand(), args...));
            }
        };

        template<StagedExpression X, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::AddOp, StagedUnary<StagedOperation::SquareOp, X>, C>> {
            using Square = StagedUnary<StagedOperation::SquareOp, X>;
            using Expression = StagedBinary<StagedOperation::AddOp, Square, C>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Fma(Detail::Evaluate(expression.FirstOperand().Operand(), args...),
                                            Detail::Evaluate(expression.FirstOperand().Operand(), args...),
                                            Detail::Evaluate(expression.SecondOperand(), args...)))) {
                auto value = Detail::Evaluate(expression.FirstOperand().Operand(), args...);
                return Math::Fma(value, value, Detail::Evaluate(expression.SecondOperand(), args...));
            }
        };

        template<StagedExpression X, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::AddOp, C, StagedUnary<StagedOperation::SquareOp, X>>> {
            using Square = StagedUnary<StagedOperation::SquareOp, X>;
            using Expression = StagedBinary<StagedOperation::AddOp, C, Square>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Fma(Detail::Evaluate(expression.SecondOperand().Operand(), args...),
                                            Detail::Evaluate(expression.SecondOperand().Operand(), args...),
                                            Detail::Evaluate(expression.FirstOperand(), args...)))) {
                auto value = Detail::Evaluate(expression.SecondOperand().Operand(), args...);
                return Math::Fma(value, value, Detail::Evaluate(expression.FirstOperand(), args...));
            }
        };

        template<StagedExpression A, StagedExpression B, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::SubOp, StagedBinary<StagedOperation::MulOp, A, B>, C>> {
            using Product = StagedBinary<StagedOperation::MulOp, A, B>;
            using Expression = StagedBinary<StagedOperation::SubOp, Product, C>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Mfs(Detail::Evaluate(expression.FirstOperand().FirstOperand(), args...),
                                            Detail::Evaluate(expression.FirstOperand().SecondOperand(), args...),
                                            Detail::Evaluate(expression.SecondOperand(), args...)))) {
                const auto& product = expression.FirstOperand();
                return Math::Mfs(Detail::Evaluate(product.FirstOperand(), args...),
                                 Detail::Evaluate(product.SecondOperand(), args...),
                                 Detail::Evaluate(expression.SecondOperand(), args...));
            }
        };

        template<StagedExpression A, StagedExpression B, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::SubOp, C, StagedBinary<StagedOperation::MulOp, A, B>>> {
            using Product = StagedBinary<StagedOperation::MulOp, A, B>;
            using Expression = StagedBinary<StagedOperation::SubOp, C, Product>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Nma(Detail::Evaluate(expression.SecondOperand().FirstOperand(), args...),
                                            Detail::Evaluate(expression.SecondOperand().SecondOperand(), args...),
                                            Detail::Evaluate(expression.FirstOperand(), args...)))) {
                const auto& product = expression.SecondOperand();
                return Math::Nma(Detail::Evaluate(product.FirstOperand(), args...),
                                 Detail::Evaluate(product.SecondOperand(), args...),
                                 Detail::Evaluate(expression.FirstOperand(), args...));
            }
        };

        template<StagedExpression X, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::SubOp, StagedUnary<StagedOperation::SquareOp, X>, C>> {
            using Square = StagedUnary<StagedOperation::SquareOp, X>;
            using Expression = StagedBinary<StagedOperation::SubOp, Square, C>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Mfs(Detail::Evaluate(expression.FirstOperand().Operand(), args...),
                                            Detail::Evaluate(expression.FirstOperand().Operand(), args...),
                                            Detail::Evaluate(expression.SecondOperand(), args...)))) {
                auto value = Detail::Evaluate(expression.FirstOperand().Operand(), args...);
                return Math::Mfs(value, value, Detail::Evaluate(expression.SecondOperand(), args...));
            }
        };

        template<StagedExpression X, StagedExpression C>
        struct StagedEvaluator<StagedBinary<StagedOperation::SubOp, C, StagedUnary<StagedOperation::SquareOp, X>>> {
            using Square = StagedUnary<StagedOperation::SquareOp, X>;
            using Expression = StagedBinary<StagedOperation::SubOp, C, Square>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Nma(Detail::Evaluate(expression.SecondOperand().Operand(), args...),
                                            Detail::Evaluate(expression.SecondOperand().Operand(), args...),
                                            Detail::Evaluate(expression.FirstOperand(), args...)))) {
                auto value = Detail::Evaluate(expression.SecondOperand().Operand(), args...);
                return Math::Nma(value, value, Detail::Evaluate(expression.FirstOperand(), args...));
            }
        };

        template<StagedExpression A, StagedExpression B, StagedExpression C>
        struct StagedEvaluator<StagedUnary<
            StagedOperation::NegOp,
            StagedBinary<StagedOperation::AddOp, StagedBinary<StagedOperation::MulOp, A, B>, C>>> {
            using Product = StagedBinary<StagedOperation::MulOp, A, B>;
            using Sum = StagedBinary<StagedOperation::AddOp, Product, C>;
            using Expression = StagedUnary<StagedOperation::NegOp, Sum>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Nms(
                    Detail::Evaluate(expression.Operand().FirstOperand().FirstOperand(), args...),
                    Detail::Evaluate(expression.Operand().FirstOperand().SecondOperand(), args...),
                    Detail::Evaluate(expression.Operand().SecondOperand(), args...)))) {
                const auto& sum = expression.Operand();
                const auto& product = sum.FirstOperand();
                return Math::Nms(Detail::Evaluate(product.FirstOperand(), args...),
                                 Detail::Evaluate(product.SecondOperand(), args...),
                                 Detail::Evaluate(sum.SecondOperand(), args...));
            }
        };

        template<StagedExpression A, StagedExpression B, StagedExpression C>
        struct StagedEvaluator<StagedUnary<
            StagedOperation::NegOp,
            StagedBinary<StagedOperation::AddOp, C, StagedBinary<StagedOperation::MulOp, A, B>>>> {
            using Product = StagedBinary<StagedOperation::MulOp, A, B>;
            using Sum = StagedBinary<StagedOperation::AddOp, C, Product>;
            using Expression = StagedUnary<StagedOperation::NegOp, Sum>;

            template<typename... Args>
            [[nodiscard]] static constexpr decltype(auto) Evaluate(const Expression& expression, Args&... args)
                noexcept(noexcept(Math::Nms(
                    Detail::Evaluate(expression.Operand().SecondOperand().FirstOperand(), args...),
                    Detail::Evaluate(expression.Operand().SecondOperand().SecondOperand(), args...),
                    Detail::Evaluate(expression.Operand().FirstOperand(), args...)))) {
                const auto& sum = expression.Operand();
                const auto& product = sum.SecondOperand();
                return Math::Nms(Detail::Evaluate(product.FirstOperand(), args...),
                                 Detail::Evaluate(product.SecondOperand(), args...),
                                 Detail::Evaluate(sum.FirstOperand(), args...));
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
                    return Detail::FoldBinary<Detail::StagedOperation::AddOp>(std::forward<First>(first),
                                                                           std::forward<Rest>(rest)...);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Sub(First&& first, Second&& second) {
                return Detail::MakeBinary<Detail::StagedOperation::SubOp>(std::forward<First>(first),
                                                                        std::forward<Second>(second));
            }

            template<typename First, typename... Rest>
            [[nodiscard]] static constexpr auto Mul(First&& first, Rest&&... rest) {
                if constexpr (sizeof...(Rest) == 0) {
                    return Detail::Stage(std::forward<First>(first));
                } else {
                    return Detail::FoldBinary<Detail::StagedOperation::MulOp>(std::forward<First>(first),
                                                                           std::forward<Rest>(rest)...);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Div(First&& first, Second&& second) {
                return Detail::MakeBinary<Detail::StagedOperation::DivOp>(std::forward<First>(first),
                                                                        std::forward<Second>(second));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Neg(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::NegOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Inv(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::InvOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Square(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::SquareOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Abs(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::AbsOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Sin(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::SinOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Cos(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::CosOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Tan(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::TanOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Asin(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::ASinOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Acos(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::ACosOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Atan(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::ATanOp>(std::forward<T>(value));
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Atan2(First&& first, Second&& second) {
                return Detail::MakeBinary<Detail::StagedOperation::ATan2Op>(std::forward<First>(first),
                                                                          std::forward<Second>(second));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Exp(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::ExpOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Log(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::LogOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Sqrt(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::SqrtOp>(std::forward<T>(value));
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Pow(First&& first, Second&& second) {
                return Detail::MakeBinary<Detail::StagedOperation::PowOp>(std::forward<First>(first),
                                                                        std::forward<Second>(second));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Fma(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Detail::StagedOperation::FmaOp>(
                    std::forward<First>(first), std::forward<Second>(second), std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Mfs(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Detail::StagedOperation::MfsOp>(
                    std::forward<First>(first), std::forward<Second>(second), std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nms(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Detail::StagedOperation::NmsOp>(
                    std::forward<First>(first), std::forward<Second>(second), std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nma(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Detail::StagedOperation::NmaOp>(
                    std::forward<First>(first), std::forward<Second>(second), std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Lerp(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Detail::StagedOperation::LerpOp>(
                    std::forward<First>(first), std::forward<Second>(second), std::forward<Third>(third));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Clamp(First&& first, Second&& second, Third&& third) {
                return Detail::MakeTernary<Detail::StagedOperation::ClampOp>(
                    std::forward<First>(first), std::forward<Second>(second), std::forward<Third>(third));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Saturate(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::SaturateOp>(std::forward<T>(value));
            }

            template<typename T>
            [[nodiscard]] static constexpr auto Sign(T&& value) {
                return Detail::MakeUnary<Detail::StagedOperation::SignOp>(std::forward<T>(value));
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
    [[nodiscard]] consteval auto Compile(Function function) {
        using Inputs = [:Detail::StagedInputTuple<Arity>():];
        auto expression = Detail::Stage(std::apply(function, Inputs{}));
        return CompiledKernel<decltype(expression), Arity>{std::move(expression)};
    }

} // namespace Sora::Math
