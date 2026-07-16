/**
 * @file ReverseAutodiff.h
 * @brief Fixed-capacity, allocation-free reverse automatic differentiation tape.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    template<DifferentiableValue P, std::size_t Capacity>
    class ReverseTape;

    namespace Detail {

        template<std::size_t Capacity>
        using ReverseIndex = std::conditional_t<(Capacity <= std::numeric_limits<std::uint32_t>::max()), std::uint32_t,
                                                std::uint64_t>;

        template<DifferentiableValue P, std::size_t Capacity, bool CompactScalar = (alignof(P) <= alignof(void*))>
        struct ReverseStorage;

        template<DifferentiableValue P, std::size_t Capacity>
        struct ReverseStorage<P, Capacity, true> {
            using IndexType = ReverseIndex<Capacity>;
            using TapeType = ReverseTape<P, Capacity>;

            TapeType* tape;
            P primal;
            IndexType node;

            constexpr ReverseStorage(P value, TapeType* owner, IndexType index) noexcept
                : tape(owner), primal(std::move(value)), node(index) {}
        };

        template<DifferentiableValue P, std::size_t Capacity>
        struct ReverseStorage<P, Capacity, false> {
            using IndexType = ReverseIndex<Capacity>;
            using TapeType = ReverseTape<P, Capacity>;

            P primal;
            TapeType* tape;
            IndexType node;

            constexpr ReverseStorage(P value, TapeType* owner, IndexType index) noexcept
                : primal(std::move(value)), tape(owner), node(index) {}
        };

    } // namespace Detail

    /** @brief A primal value and node reference into a fixed-capacity reverse tape. */
    template<DifferentiableValue P, std::size_t Capacity>
    struct Reverse : Detail::ReverseStorage<P, Capacity> {
        using Storage = Detail::ReverseStorage<P, Capacity>;
        using PrimalType = P;
        using TapeType = ReverseTape<P, Capacity>;
        using IndexType = Detail::ReverseIndex<Capacity>;

        static constexpr bool kIsReverse = true;
        static constexpr std::size_t kCapacity = Capacity;

        constexpr Reverse(P primal, TapeType* tape, IndexType node) noexcept
            : Storage(std::move(primal), tape, node) {}
    };

    template<typename T>
    concept ReverseValue = requires {
        requires std::remove_cvref_t<T>::kIsReverse;
        typename std::remove_cvref_t<T>::PrimalType;
        typename std::remove_cvref_t<T>::TapeType;
    };

    /** @brief Static reverse-mode graph whose construction and sweep perform no dynamic allocation. */
    template<DifferentiableValue P, std::size_t Capacity>
    class ReverseTape {
    public:
        using ValueType = Reverse<P, Capacity>;
        using IndexType = typename ValueType::IndexType;
        static constexpr IndexType kNoNode = std::numeric_limits<IndexType>::max();

        static_assert(Capacity <= std::numeric_limits<IndexType>::max(),
                      "ReverseTape capacity must leave one index value for the no-node sentinel");

    private:
        struct Node {
            IndexType left;
            IndexType right;
            P leftWeight;
            P rightWeight;
        };

        std::array<Node, Capacity> nodes_;
        std::array<P, Capacity> adjoints_;
        IndexType size_ = 0;
        bool hasAdjoints_ = false;

        [[nodiscard]] static constexpr P Zero() {
            if constexpr (Simd::SimdVecType<P>) {
                return P(typename P::ValueType{});
            } else {
                return P{};
            }
        }

        [[nodiscard]] static constexpr P One() {
            if constexpr (Simd::SimdVecType<P>) {
                return P(typename P::ValueType{1});
            } else {
                return P{1};
            }
        }

        [[nodiscard]] constexpr IndexType Append(Node node) {
            if (size_ == Capacity) {
                throw std::length_error("ReverseTape capacity exceeded");
            }
            hasAdjoints_ = false;
            nodes_[size_] = std::move(node);
            return size_++;
        }

    public:
        constexpr ReverseTape() = default;
        ReverseTape(const ReverseTape&) = delete;
        ReverseTape& operator=(const ReverseTape&) = delete;
        ReverseTape(ReverseTape&&) = delete;
        ReverseTape& operator=(ReverseTape&&) = delete;

        [[nodiscard]] constexpr ValueType Variable(P primal) {
            const IndexType node = Append({kNoNode, kNoNode, Zero(), Zero()});
            return {std::move(primal), this, node};
        }

        [[nodiscard]] constexpr ValueType Constant(P primal) { return {std::move(primal), this, kNoNode}; }

        [[nodiscard]] constexpr ValueType RecordUnary(P primal, const ValueType& input, P derivative) {
            if (input.tape != this) {
                throw std::invalid_argument("ReverseTape::RecordUnary requires a value from this tape");
            }
            if (input.node == kNoNode) {
                return Constant(std::move(primal));
            }
            return {std::move(primal), this, Append({input.node, kNoNode, std::move(derivative), Zero()})};
        }

        [[nodiscard]] constexpr ValueType RecordBinary(P primal, const ValueType& left, P leftDerivative,
                                                       const ValueType& right, P rightDerivative) {
            if (left.tape != this || right.tape != this) {
                throw std::invalid_argument("ReverseTape::RecordBinary requires values from this tape");
            }
            if (left.node == kNoNode && right.node == kNoNode) {
                return Constant(std::move(primal));
            }
            return {std::move(primal), this,
                    Append({left.node, right.node, std::move(leftDerivative), std::move(rightDerivative)})};
        }

        [[nodiscard]] constexpr ValueType RecordTernary(P primal, const ValueType& first, P firstDerivative,
                                                        const ValueType& second, P secondDerivative,
                                                        const ValueType& third, P thirdDerivative) {
            if (first.tape != this || second.tape != this || third.tape != this) {
                throw std::invalid_argument("ReverseTape::RecordTernary requires values from this tape");
            }
            if (first.node == kNoNode) {
                return RecordBinary(std::move(primal), second, std::move(secondDerivative), third,
                                    std::move(thirdDerivative));
            }
            if (second.node == kNoNode) {
                return RecordBinary(std::move(primal), first, std::move(firstDerivative), third,
                                    std::move(thirdDerivative));
            }
            if (third.node == kNoNode) {
                return RecordBinary(std::move(primal), first, std::move(firstDerivative), second,
                                    std::move(secondDerivative));
            }

            const P retainedPrimal = primal;
            ValueType pair =
                RecordBinary(std::move(primal), first, std::move(firstDerivative), second, std::move(secondDerivative));
            return RecordBinary(retainedPrimal, pair, One(), third, std::move(thirdDerivative));
        }

        constexpr void Backward(const ValueType& output, P seed = One()) {
            if (output.tape != this || output.node == kNoNode || output.node >= size_) {
                throw std::invalid_argument("ReverseTape::Backward requires a recorded value from this tape");
            }
            for (std::size_t i = 0; i < size_; ++i) {
                adjoints_[i] = Zero();
            }
            adjoints_[output.node] = std::move(seed);
            for (std::size_t i = size_; i-- > 0;) {
                const Node& node = nodes_[i];
                if (node.left != kNoNode) {
                    adjoints_[node.left] = Math::Fma(adjoints_[i], node.leftWeight, adjoints_[node.left]);
                }
                if (node.right != kNoNode) {
                    adjoints_[node.right] = Math::Fma(adjoints_[i], node.rightWeight, adjoints_[node.right]);
                }
            }
            hasAdjoints_ = true;
        }

        [[nodiscard]] constexpr const P& Gradient(const ValueType& variable) const {
            if (variable.tape != this || variable.node == kNoNode || variable.node >= size_) {
                throw std::invalid_argument("ReverseTape::Gradient requires a recorded value from this tape");
            }
            if (!hasAdjoints_) {
                throw std::logic_error("ReverseTape::Gradient requires Backward after the most recent recording");
            }
            return adjoints_[variable.node];
        }

        [[nodiscard]] constexpr std::size_t Size() const noexcept { return static_cast<std::size_t>(size_); }
    };

    namespace Backend {

        template<std::size_t Capacity>
        struct ReverseAD;

    } // namespace Backend

    namespace Hook {

        template<DifferentiableValue P, std::size_t Capacity>
        struct Backend<Reverse<P, Capacity>> {
            using Type = Sora::Math::Backend::ReverseAD<Capacity>;
        };

    } // namespace Hook

    namespace Backend {

        /** @brief Primitive VJP recorder for fixed-capacity reverse values. */
        template<std::size_t Capacity>
        struct ReverseAD {
            static constexpr bool kIsScalar = false;
            static constexpr bool kIsSimd = false;
            static constexpr bool kIsAutodiff = true;
            static constexpr int kPriority = 3;

        private:
            template<typename First, typename... Rest>
            [[nodiscard]] static consteval auto ReverseCarrier() {
                if constexpr (ReverseValue<First>) {
                    static_assert(std::remove_cvref_t<First>::kCapacity == Capacity);
                    return std::type_identity<std::remove_cvref_t<First>>{};
                } else {
                    return ReverseCarrier<Rest...>();
                }
            }

            template<typename... Args>
            using Carrier = typename decltype(ReverseCarrier<Args...>())::type;

            template<typename... Args>
            using PrimalType = typename Carrier<Args...>::PrimalType;

            template<typename T>
            [[nodiscard]] static constexpr decltype(auto) Primal(T&& value) {
                if constexpr (ReverseValue<T>) {
                    return std::forward<T>(value).primal;
                } else {
                    return std::forward<T>(value);
                }
            }

            template<typename... Args>
            [[nodiscard]] static constexpr auto* Tape(Args&&... args) {
                using R = Carrier<Args...>;
                typename R::TapeType* result = nullptr;
                auto inspect = [&](auto&& value) {
                    if constexpr (ReverseValue<decltype(value)>) {
                        if (result != nullptr && result != value.tape) {
                            throw std::invalid_argument("reverse operation combines values from different tapes");
                        }
                        result = value.tape;
                    }
                };
                (inspect(std::forward<Args>(args)), ...);
                return result;
            }

            template<typename R, typename T>
            [[nodiscard]] static constexpr R Lift(T&& value, typename R::TapeType* tape) {
                if constexpr (ReverseValue<T>) {
                    return std::forward<T>(value);
                } else {
                    return tape->Constant(typename R::PrimalType(std::forward<T>(value)));
                }
            }

            template<typename P>
            [[nodiscard]] static constexpr P Zero() {
                if constexpr (Simd::SimdVecType<P>) {
                    return P(typename P::ValueType{});
                } else {
                    return P{};
                }
            }

            template<typename P>
            [[nodiscard]] static constexpr P One() {
                if constexpr (Simd::SimdVecType<P>) {
                    return P(typename P::ValueType{1});
                } else {
                    return P{1};
                }
            }

            template<typename Condition, typename P>
            [[nodiscard]] static constexpr P Select(const Condition& condition, const P& yes, const P& no) {
                if constexpr (std::same_as<std::remove_cvref_t<Condition>, bool>) {
                    return condition ? yes : no;
                } else {
                    return Simd::Select(condition, yes, no);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto AddTwo(First&& first, Second&& second) {
                using R = Carrier<First, Second>;
                using P = typename R::PrimalType;
                auto* tape = Tape(first, second);
                R left = Lift<R>(std::forward<First>(first), tape);
                R right = Lift<R>(std::forward<Second>(second), tape);
                return tape->RecordBinary(Math::Add(left.primal, right.primal), left, One<P>(), right, One<P>());
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto MulTwo(First&& first, Second&& second) {
                using R = Carrier<First, Second>;
                auto* tape = Tape(first, second);
                R left = Lift<R>(std::forward<First>(first), tape);
                R right = Lift<R>(std::forward<Second>(second), tape);
                return tape->RecordBinary(Math::Mul(left.primal, right.primal), left, right.primal, right, left.primal);
            }

        public:
            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Add(T&& value) {
                return std::remove_cvref_t<T>(std::forward<T>(value));
            }

            template<typename First, typename Second, typename... Rest>
            [[nodiscard]] static constexpr auto Add(First&& first, Second&& second, Rest&&... rest) {
                auto accumulated = AddTwo(std::forward<First>(first), std::forward<Second>(second));
                if constexpr (sizeof...(Rest) == 0) {
                    return accumulated;
                } else {
                    return Add(std::move(accumulated), std::forward<Rest>(rest)...);
                }
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Mul(T&& value) {
                return std::remove_cvref_t<T>(std::forward<T>(value));
            }

            template<typename First, typename Second, typename... Rest>
            [[nodiscard]] static constexpr auto Mul(First&& first, Second&& second, Rest&&... rest) {
                auto accumulated = MulTwo(std::forward<First>(first), std::forward<Second>(second));
                if constexpr (sizeof...(Rest) == 0) {
                    return accumulated;
                } else {
                    return Mul(std::move(accumulated), std::forward<Rest>(rest)...);
                }
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Sub(First&& first, Second&& second) {
                using R = Carrier<First, Second>;
                using P = typename R::PrimalType;
                auto* tape = Tape(first, second);
                R left = Lift<R>(std::forward<First>(first), tape);
                R right = Lift<R>(std::forward<Second>(second), tape);
                return tape->RecordBinary(Math::Sub(left.primal, right.primal), left, One<P>(), right,
                                          Math::Neg(One<P>()));
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Div(First&& first, Second&& second) {
                using R = Carrier<First, Second>;
                auto* tape = Tape(first, second);
                R left = Lift<R>(std::forward<First>(first), tape);
                R right = Lift<R>(std::forward<Second>(second), tape);
                auto primal = Math::Div(left.primal, right.primal);
                auto rightWeight = Math::Neg(Math::Div(left.primal, Math::Square(right.primal)));
                return tape->RecordBinary(std::move(primal), left, Math::Inv(right.primal), right,
                                          std::move(rightWeight));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Neg(T&& x) {
                using P = typename std::remove_cvref_t<T>::PrimalType;
                return x.tape->RecordUnary(Math::Neg(x.primal), x, Math::Neg(One<P>()));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Inv(T&& x) {
                return x.tape->RecordUnary(Math::Inv(x.primal), x, Math::Neg(Math::Inv(Math::Square(x.primal))));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Square(T&& x) {
                return x.tape->RecordUnary(Math::Square(x.primal), x, Math::Mul(2, x.primal));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Abs(T&& x) {
                return x.tape->RecordUnary(Math::Abs(x.primal), x, Math::Sign(x.primal));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Sin(T&& x) {
                return x.tape->RecordUnary(Math::Sin(x.primal), x, Math::Cos(x.primal));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Cos(T&& x) {
                return x.tape->RecordUnary(Math::Cos(x.primal), x, Math::Neg(Math::Sin(x.primal)));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Tan(T&& x) {
                return x.tape->RecordUnary(Math::Tan(x.primal), x, Math::Inv(Math::Square(Math::Cos(x.primal))));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Asin(T&& x) {
                using P = typename std::remove_cvref_t<T>::PrimalType;
                auto derivative = Math::Inv(Math::Sqrt(Math::Sub(One<P>(), Math::Square(x.primal))));
                return x.tape->RecordUnary(Math::Asin(x.primal), x, std::move(derivative));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Acos(T&& x) {
                using P = typename std::remove_cvref_t<T>::PrimalType;
                auto derivative = Math::Neg(Math::Inv(Math::Sqrt(Math::Sub(One<P>(), Math::Square(x.primal)))));
                return x.tape->RecordUnary(Math::Acos(x.primal), x, std::move(derivative));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Atan(T&& x) {
                using P = typename std::remove_cvref_t<T>::PrimalType;
                auto derivative = Math::Inv(Math::Add(One<P>(), Math::Square(x.primal)));
                return x.tape->RecordUnary(Math::Atan(x.primal), x, std::move(derivative));
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Atan2(First&& y, Second&& x) {
                using R = Carrier<First, Second>;
                auto* tape = Tape(y, x);
                R yValue = Lift<R>(std::forward<First>(y), tape);
                R xValue = Lift<R>(std::forward<Second>(x), tape);
                auto denominator = Math::Add(Math::Square(xValue.primal), Math::Square(yValue.primal));
                return tape->RecordBinary(Math::Atan2(yValue.primal, xValue.primal), yValue,
                                          Math::Div(xValue.primal, denominator), xValue,
                                          Math::Neg(Math::Div(yValue.primal, denominator)));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Exp(T&& x) {
                auto primal = Math::Exp(x.primal);
                return x.tape->RecordUnary(primal, x, primal);
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Log(T&& x) {
                return x.tape->RecordUnary(Math::Log(x.primal), x, Math::Inv(x.primal));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Sqrt(T&& x) {
                auto primal = Math::Sqrt(x.primal);
                return x.tape->RecordUnary(primal, x, Math::Inv(Math::Mul(2, primal)));
            }

            template<typename First, typename Second>
            [[nodiscard]] static constexpr auto Pow(First&& base, Second&& exponent) {
                using R = Carrier<First, Second>;
                auto* tape = Tape(base, exponent);
                R baseValue = Lift<R>(std::forward<First>(base), tape);
                R exponentValue = Lift<R>(std::forward<Second>(exponent), tape);
                auto primal = Math::Pow(baseValue.primal, exponentValue.primal);
                auto baseWeight =
                    Math::Mul(exponentValue.primal, Math::Pow(baseValue.primal, Math::Sub(exponentValue.primal, 1)));
                auto exponentWeight = [&] {
                    if constexpr (ReverseValue<Second>) {
                        return Math::Mul(primal, Math::Log(baseValue.primal));
                    } else {
                        return Zero<typename R::PrimalType>();
                    }
                }();
                return tape->RecordBinary(std::move(primal), baseValue, std::move(baseWeight), exponentValue,
                                          std::move(exponentWeight));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Fma(First&& a, Second&& b, Third&& c) {
                using R = Carrier<First, Second, Third>;
                using P = typename R::PrimalType;
                auto* tape = Tape(a, b, c);
                R left = Lift<R>(std::forward<First>(a), tape);
                R right = Lift<R>(std::forward<Second>(b), tape);
                R addend = Lift<R>(std::forward<Third>(c), tape);
                return tape->RecordTernary(Math::Fma(left.primal, right.primal, addend.primal), left, right.primal,
                                           right, left.primal, addend, One<P>());
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Mfs(First&& a, Second&& b, Third&& c) {
                using R = Carrier<First, Second, Third>;
                using P = typename R::PrimalType;
                auto* tape = Tape(a, b, c);
                R left = Lift<R>(std::forward<First>(a), tape);
                R right = Lift<R>(std::forward<Second>(b), tape);
                R subtrahend = Lift<R>(std::forward<Third>(c), tape);
                return tape->RecordTernary(Math::Mfs(left.primal, right.primal, subtrahend.primal), left, right.primal,
                                           right, left.primal, subtrahend, Math::Neg(One<P>()));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nms(First&& a, Second&& b, Third&& c) {
                using R = Carrier<First, Second, Third>;
                using P = typename R::PrimalType;
                auto* tape = Tape(a, b, c);
                R left = Lift<R>(std::forward<First>(a), tape);
                R right = Lift<R>(std::forward<Second>(b), tape);
                R subtrahend = Lift<R>(std::forward<Third>(c), tape);
                return tape->RecordTernary(Math::Nms(left.primal, right.primal, subtrahend.primal), left,
                                           Math::Neg(right.primal), right, Math::Neg(left.primal), subtrahend,
                                           Math::Neg(One<P>()));
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Nma(First&& a, Second&& b, Third&& c) {
                using R = Carrier<First, Second, Third>;
                using P = typename R::PrimalType;
                auto* tape = Tape(a, b, c);
                R left = Lift<R>(std::forward<First>(a), tape);
                R right = Lift<R>(std::forward<Second>(b), tape);
                R addend = Lift<R>(std::forward<Third>(c), tape);
                return tape->RecordTernary(Math::Nma(left.primal, right.primal, addend.primal), left,
                                           Math::Neg(right.primal), right, Math::Neg(left.primal), addend, One<P>());
            }

            template<typename First, typename Second, typename Third>
            [[nodiscard]] static constexpr auto Lerp(First&& a, Second&& b, Third&& t) {
                using R = Carrier<First, Second, Third>;
                using P = typename R::PrimalType;
                auto* tape = Tape(a, b, t);
                R left = Lift<R>(std::forward<First>(a), tape);
                R right = Lift<R>(std::forward<Second>(b), tape);
                R weight = Lift<R>(std::forward<Third>(t), tape);
                return tape->RecordTernary(Math::Lerp(left.primal, right.primal, weight.primal), left,
                                           Math::Sub(One<P>(), weight.primal), right, weight.primal, weight,
                                           Math::Sub(right.primal, left.primal));
            }

            template<typename Value, typename Lower, typename Upper>
            [[nodiscard]] static constexpr auto Clamp(Value&& value, Lower&& lower, Upper&& upper) {
                using R = Carrier<Value, Lower, Upper>;
                using P = typename R::PrimalType;
                auto* tape = Tape(value, lower, upper);
                R input = Lift<R>(std::forward<Value>(value), tape);
                R lo = Lift<R>(std::forward<Lower>(lower), tape);
                R hi = Lift<R>(std::forward<Upper>(upper), tape);
                P inputWeight =
                    Select(input.primal < lo.primal, Zero<P>(), Select(hi.primal < input.primal, Zero<P>(), One<P>()));
                P loWeight = Select(input.primal < lo.primal, One<P>(), Zero<P>());
                auto first = tape->RecordBinary(Math::Clamp(input.primal, lo.primal, hi.primal), input,
                                                std::move(inputWeight), lo, std::move(loWeight));
                P hiWeight = Select(hi.primal < input.primal, One<P>(), Zero<P>());
                return tape->RecordBinary(first.primal, first, One<P>(), hi, std::move(hiWeight));
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Saturate(T&& x) {
                using P = typename std::remove_cvref_t<T>::PrimalType;
                return Clamp(std::forward<T>(x), Zero<P>(), One<P>());
            }

            template<ReverseValue T>
            [[nodiscard]] static constexpr auto Sign(T&& x) {
                using P = typename std::remove_cvref_t<T>::PrimalType;
                return x.tape->RecordUnary(Math::Sign(x.primal), x, Zero<P>());
            }
        };

    } // namespace Backend

} // namespace Sora::Math
