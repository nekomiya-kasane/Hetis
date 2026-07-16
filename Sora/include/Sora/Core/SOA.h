/**
 * @file SOA.h
 * @brief Reflection-driven Structure-of-Arrays container with compile-time layout generation.
 *
 * @details @ref Sora::SoA::Array transforms an aggregate AoS shape into a cache-friendly SoA runtime container. Each
 * reflected non-skipped data member becomes an independently allocated, cache-aligned dense array. Named field
 * access is resolved from compile-time string keys, and element reconstruction is generated from C++26 reflection and
 * expansion statements with no runtime schema lookup.
 *
 * Static @ref SoAType values also lift closed operations defined on their source aggregate. A closed operation returns
 * the source type and receives one or more source values plus optional broadcast parameters. @ref Adapt accepts a
 * compile-time function, structural function object, non-capturing lambda, or member pointer; @ref Transform also
 * accepts runtime callables. Arithmetic operators preserve source-type unary, same-type, and scalar-broadcast
 * semantics. The implementation reconstructs source values only in the abstract machine. Once the operation body is
 * visible, scalar replacement and loop vectorization operate directly on the generated field arrays without
 * materializing AoS temporaries.
 *
 * @ref AdaptedOperation::Simd and the default call operator request loop vectorization from Clang;
 * @ref AdaptedOperation::Scalar disables loop vectorization for reference execution and benchmarking. SIMD lifting
 * requires a visible, lane-independent operation body without cross-element side effects. Reflection adapts the type
 * and storage model, while LLVM transforms the operation body; an opaque separately compiled function cannot be
 * vectorized from reflection metadata alone.
 *
 * Source aggregate members can be annotated with @c [[=Sora::SoA::Skip{}]] to exclude a member from the SoA layout, or
 * @c [[=Sora::SoA::Align{N}]] to override a runtime @ref Array field allocation's alignment.
 *
 * @code{.cpp}
 * struct Particle {
 *     Vec3 position;
 *     Vec3 velocity;
 *     float lifetime;
 *     [[=Sora::SoA::Skip{}]] std::uint32_t debugId;
 * };
 *
 * Sora::SoA::Array<Particle> particles;
 * particles.Resize(1024);
 * particles.Get<Sora::FixedString{"position"}>(0) = Vec3{1.0f, 2.0f, 3.0f};
 * for (auto& position : particles.Field<Sora::FixedString{"position"}>()) {
 *     position.y += 1.0f;
 * }
 *
 * struct Kinematic {
 *     float position;
 *     float velocity;
 *     Kinematic Step(float deltaTime) const;
 * };
 * Kinematic Integrate(Kinematic state, Kinematic acceleration, float deltaTime);
 *
 * consteval { Sora::SoA::Define<Kinematic, 1024>(); }
 * using KinematicSoA = Sora::SoA::SoAType<Kinematic, 1024>;
 * inline constexpr auto IntegrateSoA = Sora::SoA::Adapt<&Integrate>;
 * KinematicSoA scalar = IntegrateSoA.Scalar(current, acceleration, deltaTime);
 * KinematicSoA simd = IntegrateSoA.Simd(current, acceleration, deltaTime);
 * KinematicSoA stepped = Sora::SoA::Adapt<&Kinematic::Step>.Simd(current, deltaTime);
 * KinematicSoA combined = current + acceleration;
 * @endcode
 *
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/FixedString.h"
#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Core/Traits/InheritanceTraits.h"
#include "Sora/Core/Traits/TypeTraits.h"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <meta>
#include <new>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora {

    namespace SoA {

        namespace Concept {

            /** @brief Type that can be used as a source aggregate for SoA generation. */
            template<typename T>
            concept SoATransformable = std::is_class_v<T> && std::is_aggregate_v<T> && Sora::Concept::RootClass<T>;

        } // namespace Concept

        namespace $ {

            /** @brief Annotation that excludes a reflected member from SoA layout generation. */
            struct Skip {
                constexpr bool operator==(const Skip&) const = default;
            };

            /** @brief Annotation that overrides a field array's allocation alignment in bytes. */
            struct Align {
                std::size_t value = 64;
                constexpr bool operator==(const Align&) const = default;
            };

        } // namespace $

        /** @brief Compile-time loop-vectorization policy for lifted source operations. */
        enum class Vectorization : std::uint8_t {
            Scalar,
            Simd,
        };

        namespace Traits {

            /** @brief Filtered list of non-skipped data members used for SoA generation. */
            template<typename T>
            consteval auto SoaMembers() {
                std::vector<std::meta::info> result;
                auto members = std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked());
                for (auto member : members) {
                    if (!Sora::$::Has<$::Skip>(member)) {
                        result.push_back(member);
                    }
                }
                return result;
            }

            /** @brief Number of SoA-eligible reflected fields in @p T. */
            template<typename T>
            inline constexpr std::size_t FieldCountOf = SoaMembers<T>().size();

            /** @brief Static array of SoA-eligible member reflections for expansion statements. */
            template<typename T>
            inline constexpr auto FieldsOf = std::define_static_array(SoaMembers<T>());

            /** @brief Whether every reflected field supports the assignments used by gather and scatter. */
            template<typename T>
            consteval bool FieldsReconstructible() {
                bool result = true;
                template for (constexpr auto member : FieldsOf<T>) {
                    using Field = typename [:std::meta::type_of(member):];
                    result =
                        result && std::is_assignable_v<Field&, const Field&> && std::is_assignable_v<Field&, Field&&>;
                }
                return result;
            }

        } // namespace Traits

        namespace Concept {

            /**
             * @brief Lossless source aggregate whose complete value can be reconstructed from its generated SoA.
             * @details Operations cannot be lifted automatically when @ref Skip removes state observed by @p T.
             */
            template<typename T>
            concept OperationLiftable =
                SoATransformable<T> && std::default_initializable<T> && Traits::FieldsReconstructible<T>() &&
                Traits::FieldCountOf<T> ==
                    std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()).size();

        } // namespace Concept

        namespace Meta {

            /** @brief Resolve and validate the allocation alignment for one runtime SoA field. */
            consteval std::size_t AllocationAlignment(std::meta::info member) {
                const std::size_t naturalAlignment = std::meta::alignment_of(std::meta::type_of(member));
                const std::size_t minimumAlignment =
                    naturalAlignment > alignof(void*) ? naturalAlignment : alignof(void*);
                const std::size_t defaultAlignment = minimumAlignment > 64 ? minimumAlignment : 64;
                const std::size_t alignment =
                    Sora::$::Has<$::Align>(member) ? Sora::$::GetSingle<$::Align>(member).value : defaultAlignment;
                if (!std::has_single_bit(alignment) || alignment < minimumAlignment) {
                    throw "SoA::Align must be a power of two preserving field and allocator alignment";
                }
                return alignment;
            }

            /** @brief Build @c data_member_spec reflections for the generated SoA aggregate. */
            template<typename T, std::size_t N>
            consteval auto BuildSoASpecs() {
                auto specs =
                    Sora::SoA::Traits::SoaMembers<T>() |
                    std::views::transform([C = std::meta::reflect_constant(N)](std::meta::info member) {
                        auto arrayType = std::meta::substitute(^^std::array, {std::meta::type_of(member), C});
                        return std::meta::data_member_spec(arrayType, {.name = std::meta::identifier_of(member)});
                    });
                return specs | std::ranges::to<std::vector>();
            }

        } // namespace Meta

        /** @cond INTERNAL */

        namespace Detail {

            /** @brief Allocate @p bytes with @p alignment. */
            inline void* AllocAligned(std::size_t bytes, std::size_t alignment) {
                if (bytes == 0) {
                    return nullptr;
                }
#ifdef _WIN32
                return _aligned_malloc(bytes, alignment);
#else
                return std::aligned_alloc(alignment, (bytes + alignment - 1) & ~(alignment - 1));
#endif
            }

            /** @brief Free memory allocated by @ref AllocAligned. */
            inline void FreeAligned(void* pointer) {
                if (pointer == nullptr) {
                    return;
                }
#ifdef _WIN32
                _aligned_free(pointer);
#else
                std::free(pointer);
#endif
            }

        } // namespace Detail

        /** @endcond */

        /**
         * @brief Incomplete aggregate completed by @ref Define through @c std::meta::define_aggregate.
         *
         * @details Given @c struct @c Particle @c { @c Vec3 @c position; @c float @c lifetime; @c };,
         * @c SoAType<Particle, @c 1024> becomes an aggregate containing @c std::array<Vec3, @c 1024> @c position and
         * @c std::array<float, @c 1024> @c lifetime. Members annotated with @ref Skip are excluded.
         *
         * @tparam T Source aggregate type.
         * @tparam N Number of elements per generated field array.
         */
        template<typename T, std::size_t N>
            requires Concept::SoATransformable<T>
        struct SoAType;

        /**
         * @brief Complete @ref SoAType for @p T and @p N using reflection-driven aggregate generation.
         *
         * @details This function must be called from a namespace-scope @c consteval block before @ref SoAType is used
         * as a complete type. Each non-skipped member of @p T is transformed into @c std::array<MemberType, @p N>.
         *
         * @tparam T Source aggregate type.
         * @tparam N Number of elements per generated field array.
         */
        template<typename T, std::size_t N>
        consteval void Define() {
            std::meta::define_aggregate(^^SoAType<T, N>, Sora::SoA::Meta::BuildSoASpecs<T, N>());
        }

        namespace Meta {

            /** @brief Find the generated SoA field corresponding to one reflected source member. */
            template<typename T, std::size_t N, std::meta::info SourceMember>
            consteval std::meta::info StorageMember() {
                auto members =
                    std::meta::nonstatic_data_members_of(^^SoAType<T, N>, std::meta::access_context::unchecked());
                for (auto member : members) {
                    if (std::meta::identifier_of(member) == std::meta::identifier_of(SourceMember)) {
                        return member;
                    }
                }
                throw "SoA::StorageMember: generated field not found";
            }

        } // namespace Meta

        /** @brief Reconstruct source element @p index from a generated static SoA value. */
        template<Concept::OperationLiftable T, std::size_t N>
        [[nodiscard, gnu::always_inline]] constexpr T Gather(const SoAType<T, N>& source, std::size_t index) {
            assert(index < N);
            T result{};
            template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                constexpr auto storageMember = Sora::SoA::Meta::StorageMember<T, N, member>();
                result.[:member:] = source.[:storageMember:][index];
            }
            return result;
        }

        /** @brief Scatter source element @p value into element @p index of a generated static SoA value. */
        template<Concept::OperationLiftable T, std::size_t N>
        [[gnu::always_inline]] constexpr void Scatter(SoAType<T, N>& destination, std::size_t index, T value) {
            assert(index < N);
            template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                constexpr auto storageMember = Sora::SoA::Meta::StorageMember<T, N, member>();
                destination.[:storageMember:][index] = std::move(value.[:member:]);
            }
        }

        namespace Detail {

            template<typename T, std::size_t N, typename Argument>
            inline constexpr bool kGeneratedSoAArgument = std::same_as<std::remove_cvref_t<Argument>, SoAType<T, N>>;

            template<typename T, std::size_t N, typename Argument>
            using LiftedArgument = std::conditional_t<kGeneratedSoAArgument<T, N, Argument>, T, const Argument&>;

            /** @brief Source operation whose exact result is @p T after element and broadcast argument lifting. */
            template<typename Operation, typename T, std::size_t N, typename... Rest>
            concept ClosedOperation = requires(Operation& operation) {
                {
                    std::invoke(operation, std::declval<T>(), std::declval<LiftedArgument<T, N, Rest>>()...)
                } -> std::same_as<T>;
            };

            template<Concept::OperationLiftable T, std::size_t N, typename Argument>
            [[nodiscard, gnu::always_inline]] constexpr decltype(auto) ElementArgument(const Argument& argument,
                                                                                       std::size_t index) {
                if constexpr (kGeneratedSoAArgument<T, N, Argument>) {
                    return Gather(argument, index);
                } else {
                    return argument;
                }
            }

            template<Concept::OperationLiftable T, std::size_t N, typename Operation, typename... Rest>
                requires ClosedOperation<Operation, T, N, Rest...>
            [[gnu::always_inline]] constexpr void TransformElement(SoAType<T, N>& output, Operation& operation,
                                                                   const SoAType<T, N>& first, std::size_t index,
                                                                   const Rest&... rest) {
                Scatter(output, index,
                        std::invoke(operation, Gather(first, index), ElementArgument<T, N>(rest, index)...));
            }

            /** @brief Apply a binary function with its source and broadcast argument order reversed. */
            template<typename Operation>
            struct ReverseBinary {
                template<typename Right, typename Left>
                constexpr decltype(auto) operator()(Right&& right, Left&& left) const
                    noexcept(std::is_nothrow_invocable_v<const Operation&, Left, Right>) {
                    return std::invoke(Operation{}, std::forward<Left>(left), std::forward<Right>(right));
                }
            };

            /** @brief Preserve the source aggregate's unary-plus semantics. */
            struct UnaryPlus {
                template<typename Value>
                constexpr auto operator()(Value&& value) const noexcept(noexcept(+std::forward<Value>(value)))
                    -> decltype(+std::forward<Value>(value)) {
                    return +std::forward<Value>(value);
                }
            };

        } // namespace Detail

        /**
         * @brief Apply an n-ary source operation elementwise and write directly into an existing generated SoA.
         * @tparam Policy Selects scalar reference execution or compiler-backed SIMD execution.
         * @details For reliable SIMD execution, the operation definition must be visible in the translation unit or
         * supplied through LTO, and each element invocation must be independent. Generated-SoA arguments are gathered
         * per index; ordinary arguments are broadcast.
         */
        template<Vectorization Policy = Vectorization::Simd, Concept::OperationLiftable T, std::size_t N,
                 typename Operation, typename... Rest>
            requires Detail::ClosedOperation<Operation, T, N, Rest...>
        constexpr void TransformTo(SoAType<T, N>& output, Operation&& operation, const SoAType<T, N>& first,
                                   const Rest&... rest) {
            if constexpr (Policy == Vectorization::Scalar) {
#pragma clang loop vectorize(disable) interleave(disable)
                for (std::size_t index = 0; index < N; ++index) {
                    Detail::TransformElement(output, operation, first, index, rest...);
                }
            } else {
#pragma clang loop vectorize(enable) interleave(enable)
                for (std::size_t index = 0; index < N; ++index) {
                    Detail::TransformElement(output, operation, first, index, rest...);
                }
            }
        }

        /** @brief Return the generated SoA result of an n-ary elementwise source operation. */
        template<Vectorization Policy = Vectorization::Simd, Concept::OperationLiftable T, std::size_t N,
                 typename Operation, typename... Rest>
            requires Detail::ClosedOperation<Operation, T, N, Rest...>
        [[nodiscard]] constexpr auto Transform(Operation&& operation, const SoAType<T, N>& first, const Rest&... rest)
            -> SoAType<T, N> {
            SoAType<T, N> output{};
            TransformTo<Policy>(output, std::forward<Operation>(operation), first, rest...);
            return output;
        }

        /** @brief Compile-time source-operation adaptor inferred from generated SoA arguments. */
        template<auto Operation>
        struct AdaptedOperation {
            template<Concept::OperationLiftable T, std::size_t N, typename... Rest>
                requires Detail::ClosedOperation<decltype(Operation), T, N, Rest...>
            [[nodiscard]] constexpr auto operator()(const SoAType<T, N>& first, const Rest&... rest) const
                -> SoAType<T, N> {
                return Transform<Vectorization::Simd>(Operation, first, rest...);
            }

            /** @brief Evaluate with loop vectorization disabled. */
            template<Concept::OperationLiftable T, std::size_t N, typename... Rest>
                requires Detail::ClosedOperation<decltype(Operation), T, N, Rest...>
            [[nodiscard]] constexpr auto Scalar(const SoAType<T, N>& first, const Rest&... rest) const
                -> SoAType<T, N> {
                return Transform<Vectorization::Scalar>(Operation, first, rest...);
            }

            /** @brief Evaluate with loop vectorization requested from the compiler backend. */
            template<Concept::OperationLiftable T, std::size_t N, typename... Rest>
                requires Detail::ClosedOperation<decltype(Operation), T, N, Rest...>
            [[nodiscard]] constexpr auto Simd(const SoAType<T, N>& first, const Rest&... rest) const -> SoAType<T, N> {
                return Transform<Vectorization::Simd>(Operation, first, rest...);
            }

            template<Concept::OperationLiftable T, std::size_t N, typename... Rest>
                requires Detail::ClosedOperation<decltype(Operation), T, N, Rest...>
            constexpr void To(SoAType<T, N>& output, const SoAType<T, N>& first, const Rest&... rest) const {
                TransformTo<Vectorization::Simd>(output, Operation, first, rest...);
            }

            /** @brief Evaluate into @p output with loop vectorization disabled. */
            template<Concept::OperationLiftable T, std::size_t N, typename... Rest>
                requires Detail::ClosedOperation<decltype(Operation), T, N, Rest...>
            constexpr void ScalarTo(SoAType<T, N>& output, const SoAType<T, N>& first, const Rest&... rest) const {
                TransformTo<Vectorization::Scalar>(output, Operation, first, rest...);
            }

            /** @brief Evaluate into @p output with loop vectorization requested from the compiler backend. */
            template<Concept::OperationLiftable T, std::size_t N, typename... Rest>
                requires Detail::ClosedOperation<decltype(Operation), T, N, Rest...>
            constexpr void SimdTo(SoAType<T, N>& output, const SoAType<T, N>& first, const Rest&... rest) const {
                TransformTo<Vectorization::Simd>(output, Operation, first, rest...);
            }
        };

        /** @brief Adapt a compile-time function, function object, lambda, or member pointer to generated SoA values. */
        template<auto Operation>
        inline constexpr AdaptedOperation<Operation> Adapt{};

        /** @brief Elementwise lifted unary plus using the original aggregate's @c operator+. */
        template<Concept::OperationLiftable T, std::size_t N>
            requires Detail::ClosedOperation<Detail::UnaryPlus, T, N>
        [[nodiscard]] constexpr auto operator+(const SoAType<T, N>& value) -> SoAType<T, N> {
            return Transform(Detail::UnaryPlus{}, value);
        }

        /** @brief Elementwise lifted unary minus using the original aggregate's @c operator-. */
        template<Concept::OperationLiftable T, std::size_t N>
            requires Detail::ClosedOperation<std::negate<>, T, N>
        [[nodiscard]] constexpr auto operator-(const SoAType<T, N>& value) -> SoAType<T, N> {
            return Transform(std::negate<>{}, value);
        }

        /** @brief Lift source addition with generated-SoA or broadcast right operands. */
        template<Concept::OperationLiftable T, std::size_t N, typename Right>
            requires Detail::ClosedOperation<std::plus<>, T, N, Right>
        [[nodiscard]] constexpr auto operator+(const SoAType<T, N>& left, const Right& right) -> SoAType<T, N> {
            return Transform(std::plus<>{}, left, right);
        }

        /** @brief Lift source addition with a broadcast left operand. */
        template<typename Left, Concept::OperationLiftable T, std::size_t N>
            requires(!Detail::kGeneratedSoAArgument<T, N, Left>) &&
                    Detail::ClosedOperation<Detail::ReverseBinary<std::plus<>>, T, N, Left>
        [[nodiscard]] constexpr auto operator+(const Left& left, const SoAType<T, N>& right) -> SoAType<T, N> {
            return Transform(Detail::ReverseBinary<std::plus<>>{}, right, left);
        }

        /** @brief Lift source subtraction with generated-SoA or broadcast right operands. */
        template<Concept::OperationLiftable T, std::size_t N, typename Right>
            requires Detail::ClosedOperation<std::minus<>, T, N, Right>
        [[nodiscard]] constexpr auto operator-(const SoAType<T, N>& left, const Right& right) -> SoAType<T, N> {
            return Transform(std::minus<>{}, left, right);
        }

        /** @brief Lift source subtraction with a broadcast left operand. */
        template<typename Left, Concept::OperationLiftable T, std::size_t N>
            requires(!Detail::kGeneratedSoAArgument<T, N, Left>) &&
                    Detail::ClosedOperation<Detail::ReverseBinary<std::minus<>>, T, N, Left>
        [[nodiscard]] constexpr auto operator-(const Left& left, const SoAType<T, N>& right) -> SoAType<T, N> {
            return Transform(Detail::ReverseBinary<std::minus<>>{}, right, left);
        }

        /** @brief Lift source multiplication with generated-SoA or broadcast right operands. */
        template<Concept::OperationLiftable T, std::size_t N, typename Right>
            requires Detail::ClosedOperation<std::multiplies<>, T, N, Right>
        [[nodiscard]] constexpr auto operator*(const SoAType<T, N>& left, const Right& right) -> SoAType<T, N> {
            return Transform(std::multiplies<>{}, left, right);
        }

        /** @brief Lift source multiplication with a broadcast left operand. */
        template<typename Left, Concept::OperationLiftable T, std::size_t N>
            requires(!Detail::kGeneratedSoAArgument<T, N, Left>) &&
                    Detail::ClosedOperation<Detail::ReverseBinary<std::multiplies<>>, T, N, Left>
        [[nodiscard]] constexpr auto operator*(const Left& left, const SoAType<T, N>& right) -> SoAType<T, N> {
            return Transform(Detail::ReverseBinary<std::multiplies<>>{}, right, left);
        }

        /** @brief Lift source division with generated-SoA or broadcast right operands. */
        template<Concept::OperationLiftable T, std::size_t N, typename Right>
            requires Detail::ClosedOperation<std::divides<>, T, N, Right>
        [[nodiscard]] constexpr auto operator/(const SoAType<T, N>& left, const Right& right) -> SoAType<T, N> {
            return Transform(std::divides<>{}, left, right);
        }

        /** @brief Lift source division with a broadcast left operand. */
        template<typename Left, Concept::OperationLiftable T, std::size_t N>
            requires(!Detail::kGeneratedSoAArgument<T, N, Left>) &&
                    Detail::ClosedOperation<Detail::ReverseBinary<std::divides<>>, T, N, Left>
        [[nodiscard]] constexpr auto operator/(const Left& left, const SoAType<T, N>& right) -> SoAType<T, N> {
            return Transform(Detail::ReverseBinary<std::divides<>>{}, right, left);
        }

        /**
         * @brief Runtime Structure-of-Arrays container generated from aggregate @p T.
         *
         * @details Stores each non-skipped data member of @p T as a separate dense array. The container supports named
         * field access via compile-time string keys, dense field spans, AoS push/gather conversion, per-element field
         * iteration, and unordered O(1) removal via swap-remove.
         *
         * @tparam T Source aggregate type describing the AoS element shape.
         */
        template<typename T>
            requires std::is_aggregate_v<T>
        class Array {
            static constexpr std::size_t kN = Sora::SoA::Traits::FieldCountOf<T>;

            void* arrays_[kN]{};
            std::size_t size_ = 0;
            std::size_t capacity_ = 0;

        public:
            /** @brief Construct an empty SoA container. */
            Array() = default;

            /** @brief Destroy all live elements and release all field arrays. */
            ~Array() {
                Clear();
                DeallocArrays();
            }

            Array(const Array&) = delete;
            Array& operator=(const Array&) = delete;

            /** @brief Move field-array ownership from @p other. */
            Array(Array&& other) noexcept : size_(other.size_), capacity_(other.capacity_) {
                for (std::size_t i = 0; i < kN; ++i) {
                    arrays_[i] = other.arrays_[i];
                    other.arrays_[i] = nullptr;
                }
                other.size_ = 0;
                other.capacity_ = 0;
            }

            /** @brief Move-assign field-array ownership from @p other. */
            Array& operator=(Array&& other) noexcept {
                if (this != &other) {
                    Clear();
                    DeallocArrays();
                    for (std::size_t i = 0; i < kN; ++i) {
                        arrays_[i] = other.arrays_[i];
                        other.arrays_[i] = nullptr;
                    }
                    size_ = other.size_;
                    capacity_ = other.capacity_;
                    other.size_ = 0;
                    other.capacity_ = 0;
                }
                return *this;
            }

            /** @brief Number of live AoS elements represented by the container. */
            [[nodiscard]] std::size_t Size() const noexcept { return size_; }

            /** @brief Number of elements that can be stored before reallocation. */
            [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

            /** @brief True when the container has no live elements. */
            [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }

            /** @brief Reserve storage for at least @p newCapacity elements. */
            void Reserve(std::size_t newCapacity) {
                if (newCapacity > capacity_) {
                    ReallocArrays(newCapacity);
                }
            }

            /** @brief Resize to @p newSize elements, default-constructing new entries and destroying removed entries.
             */
            void Resize(std::size_t newSize) {
                if (newSize > capacity_) {
                    ReallocArrays(GrowCapacity(newSize));
                }
                if (newSize > size_) {
                    ConstructRange(size_, newSize);
                } else if (newSize < size_) {
                    DestroyRange(newSize, size_);
                }
                size_ = newSize;
            }

            /** @brief Destroy all live elements without releasing field-array capacity. */
            void Clear() {
                DestroyRange(0, size_);
                size_ = 0;
            }

            /** @brief Access field @p Name of element @p index. */
            template<auto Name>
            [[nodiscard]] decltype(auto) Get(std::size_t index) {
                constexpr std::size_t fieldIndex = FieldIndex<Name>();
                using FieldType = FieldTypeAt<fieldIndex>;
                return static_cast<FieldType*>(arrays_[fieldIndex])[index];
            }

            /** @brief Access field @p Name of element @p index. */
            template<auto Name>
            [[nodiscard]] decltype(auto) Get(std::size_t index) const {
                constexpr std::size_t fieldIndex = FieldIndex<Name>();
                using FieldType = FieldTypeAt<fieldIndex>;
                return static_cast<const FieldType*>(arrays_[fieldIndex])[index];
            }

            /** @brief Dense mutable span over field @p Name. */
            template<auto Name>
            [[nodiscard]] auto Field() {
                constexpr std::size_t fieldIndex = FieldIndex<Name>();
                using FieldType = FieldTypeAt<fieldIndex>;
                return std::span<FieldType>{static_cast<FieldType*>(arrays_[fieldIndex]), size_};
            }

            /** @brief Dense const span over field @p Name. */
            template<auto Name>
            [[nodiscard]] auto Field() const {
                constexpr std::size_t fieldIndex = FieldIndex<Name>();
                using FieldType = FieldTypeAt<fieldIndex>;
                return std::span<const FieldType>{static_cast<const FieldType*>(arrays_[fieldIndex]), size_};
            }

            /** @brief Append @p element by copying each reflected field into its dense array. */
            void Push(const T& element) {
                if (size_ == capacity_) {
                    ReallocArrays(GrowCapacity(size_ + 1));
                }
                CopyElementIn(size_, element);
                ++size_;
            }

            /** @brief Append @p element by moving each reflected field into its dense array. */
            void Push(T&& element) {
                if (size_ == capacity_) {
                    ReallocArrays(GrowCapacity(size_ + 1));
                }
                MoveElementIn(size_, std::move(element));
                ++size_;
            }

            /** @brief Reconstruct the AoS element at @p index by gathering every reflected field. */
            [[nodiscard]] T Gather(std::size_t index) const {
                T result{};
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    result.[:member:] = static_cast<const FieldType*>(arrays_[fieldIndex])[index];
                }
                return result;
            }

            /** @brief Call @p function with @c (name, value) for each SoA field of element @p index. */
            template<typename Function>
            void ForEach(std::size_t index, Function&& function) {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    function(std::string_view(std::meta::identifier_of(member)),
                             static_cast<FieldType*>(arrays_[fieldIndex])[index]);
                }
            }

            /** @brief Call @p function with @c (name, value) for each SoA field of element @p index. */
            template<typename Function>
            void ForEach(std::size_t index, Function&& function) const {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    function(std::string_view(std::meta::identifier_of(member)),
                             static_cast<const FieldType*>(arrays_[fieldIndex])[index]);
                }
            }

            /** @brief Remove element @p index by moving the last element into its slot. */
            void SwapRemove(std::size_t index) {
                if (index >= size_) {
                    return;
                }

                const std::size_t last = size_ - 1;
                if (index != last) {
                    template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                        constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                        using FieldType = typename [:std::meta::type_of(member):];
                        auto* array = static_cast<FieldType*>(arrays_[fieldIndex]);
                        array[index] = std::move(array[last]);
                    }
                }

                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    std::destroy_at(&static_cast<FieldType*>(arrays_[fieldIndex])[last]);
                }
                --size_;
            }

            /** @brief Number of reflected fields stored by this SoA container. */
            static constexpr std::size_t FieldCount = kN;

        private:
            template<auto Name>
            static consteval std::size_t FieldIndex() {
                for (std::size_t i = 0; i < Sora::SoA::Traits::FieldsOf<T>.size(); ++i) {
                    if (std::meta::identifier_of(Sora::SoA::Traits::FieldsOf<T>[i]) == std::string_view(Name)) {
                        return i;
                    }
                }
                throw "SoA::Array: field name not found";
            }

            template<std::meta::info Member>
            static consteval std::size_t FieldIndexOfMember() {
                for (std::size_t i = 0; i < Sora::SoA::Traits::FieldsOf<T>.size(); ++i) {
                    if (Sora::SoA::Traits::FieldsOf<T>[i] == Member) {
                        return i;
                    }
                }
                return static_cast<std::size_t>(-1);
            }

            template<std::size_t I>
            using FieldTypeAt = typename [:std::meta::type_of(Sora::SoA::Traits::FieldsOf<T>[I]):];

            [[nodiscard]] std::size_t GrowCapacity(std::size_t required) const {
                std::size_t capacity = capacity_ > 0 ? capacity_ : 8;
                while (capacity < required) {
                    capacity *= 2;
                }
                return capacity;
            }

            void ReallocArrays(std::size_t newCapacity) {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];

                    constexpr std::size_t alignment = Sora::SoA::Meta::AllocationAlignment(member);

                    auto* newArray =
                        static_cast<FieldType*>(Detail::AllocAligned(newCapacity * sizeof(FieldType), alignment));
                    auto* oldArray = static_cast<FieldType*>(arrays_[fieldIndex]);

                    for (std::size_t i = 0; i < size_; ++i) {
                        ::new (&newArray[i]) FieldType(std::move(oldArray[i]));
                        std::destroy_at(&oldArray[i]);
                    }

                    Detail::FreeAligned(oldArray);
                    arrays_[fieldIndex] = newArray;
                }
                capacity_ = newCapacity;
            }

            void DeallocArrays() {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    Detail::FreeAligned(arrays_[fieldIndex]);
                    arrays_[fieldIndex] = nullptr;
                }
                capacity_ = 0;
            }

            void ConstructRange(std::size_t from, std::size_t to) {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    auto* array = static_cast<FieldType*>(arrays_[fieldIndex]);
                    for (std::size_t i = from; i < to; ++i) {
                        ::new (&array[i]) FieldType{};
                    }
                }
            }

            void DestroyRange(std::size_t from, std::size_t to) {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    auto* array = static_cast<FieldType*>(arrays_[fieldIndex]);
                    for (std::size_t i = from; i < to; ++i) {
                        std::destroy_at(&array[i]);
                    }
                }
            }

            void CopyElementIn(std::size_t index, const T& element) {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    ::new (&static_cast<FieldType*>(arrays_[fieldIndex])[index]) FieldType(element.[:member:]);
                }
            }

            void MoveElementIn(std::size_t index, T&& element) {
                template for (constexpr auto member : Sora::SoA::Traits::FieldsOf<T>) {
                    constexpr std::size_t fieldIndex = FieldIndexOfMember<member>();
                    using FieldType = typename [:std::meta::type_of(member):];
                    ::new (&static_cast<FieldType*>(arrays_[fieldIndex])[index])
                        FieldType(std::move(element.[:member:]));
                }
            }
        };

    } // namespace SoA

    namespace $::SoA {

        /** @brief Annotation that excludes a reflected member from SoA layout generation. */
        using Skip = Sora::SoA::$::Skip;

        /** @brief Annotation that overrides a field array's allocation alignment in bytes. */
        using Align = Sora::SoA::$::Align;

    } // namespace $::SoA

    namespace Meta::SoA {

        using Sora::SoA::Meta::AllocationAlignment;
        using Sora::SoA::Meta::BuildSoASpecs;
        using Sora::SoA::Meta::StorageMember;

    } // namespace Meta::SoA

    namespace Traits::SoA {

        using Sora::SoA::Traits::FieldCountOf;
        using Sora::SoA::Traits::FieldsOf;
        using Sora::SoA::Traits::FieldsReconstructible;

    } // namespace Traits::SoA

    namespace Concept {

        inline namespace SoA {

            using Sora::SoA::Concept::OperationLiftable;
            using Sora::SoA::Concept::SoATransformable;

        } // namespace SoA

    } // namespace Concept

} // namespace Sora
