/**
 * @file BatchView.h
 * @brief Non-owning static-value batch views over dense sample storage.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Contracts.h>
#include <Sora/Math/Point.h>
#include <Sora/Math/TensorCore.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mdspan>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief Physical order of a dense batch whose logical axes end in the sample axis. */
    enum class BatchLayout : uint8_t {
        PointMajor,
        CoordinateMajor
    };

    namespace Traits {

        template<BatchLayout Layout>
        using BatchMapping = std::conditional_t<Layout == BatchLayout::CoordinateMajor, std::layout_right,
                                                std::layout_left>;

    } // namespace Traits

    /**
     * @brief Non-owning batch view for one concrete static value type.
     * @tparam Value Mathematical value with @c CarrierType, @c RebindElement, and @c kElementCount.
     * @tparam T External scalar element type, optionally const-qualified.
     * @tparam Layout Dense physical order of value elements and samples.
     * @details The logical storage shape is @c [value-element,sample]. A coordinate-major view has unit sample stride;
     * a point-major view has unit value-element stride. SIMD execution is selected only for the former.
     */
    template<Sora::Concept::TensorValue Value, typename T, BatchLayout Layout = BatchLayout::CoordinateMajor>
        requires Sora::Concept::TensorElement<std::remove_const_t<T>> &&
                 std::same_as<typename Value::CarrierType, std::remove_const_t<T>>
    class StaticValueBatchView {
    public:
        using ValueType = Value;
        using ElementType = T;
        using ScalarType = std::remove_const_t<T>;
        using Extents = std::extents<size_t, Value::kElementCount, std::dynamic_extent>;
        using LayoutPolicy = typename Traits::BatchMapping<Layout>;
        using ViewType = std::mdspan<T, Extents, LayoutPolicy>;
        static constexpr size_t kElementCount = Value::kElementCount;
        static constexpr size_t kDimension = Value::kDimension;
        static constexpr BatchLayout kLayout = Layout;

        template<Sora::Concept::TensorElement Carrier>
        using RebindElement = typename Value::template RebindElement<Carrier>;

        template<Sora::Concept::TensorElement Carrier>
        using Rebind = RebindElement<Carrier>;

        /** @brief Construct a view over @p sampleCount values in external storage. */
        constexpr StaticValueBatchView(T* data, size_t sampleCount) noexcept : view_(MakeView(data, sampleCount)) {}

        /** @brief Construct from an mdspan with the exact logical shape and layout. */
        constexpr explicit StaticValueBatchView(ViewType view) noexcept : view_(view) {
            SORA_PRE(ValidView(view_.data_handle(), view_.extent(1)));
        }

        /** @brief Return the number of samples in the batch. */
        [[nodiscard]] constexpr size_t Size() const noexcept { return view_.extent(1); }

        /** @brief Return the underlying logical @c [element,sample] mdspan. */
        [[nodiscard]] constexpr ViewType View() const noexcept { return view_; }

        /** @brief Return the first address of the external physical storage. */
        [[nodiscard]] constexpr T* Data() const noexcept { return view_.data_handle(); }

        /** @brief Access one external value element by logical coordinates. */
        [[nodiscard]] constexpr T& operator[](size_t element, size_t sample) const noexcept {
            return view_[element, sample];
        }

        /** @brief Access one element-major logical scalar index. @pre @p logicalIndex is within the batch. */
        [[nodiscard]] constexpr T& operator[](size_t logicalIndex) const noexcept {
            const size_t sampleCount = Size();
            SORA_PRE(logicalIndex < kElementCount * sampleCount);
            return view_[logicalIndex / sampleCount, logicalIndex % sampleCount];
        }

        /** @brief Gather one complete mathematical value from a sample. */
        [[nodiscard]] constexpr Value Load(size_t sample) const noexcept(kNothrowLoad) {
            SORA_PRE(sample < Size());
            return Load(sample, std::make_index_sequence<kElementCount>{});
        }

        /** @brief Scatter one complete mathematical value into a sample. */
        constexpr void Store(size_t sample, const Value& value) const noexcept(kNothrowStore)
            requires(!std::is_const_v<T>)
        {
            SORA_PRE(sample < Size());
            for (size_t element = 0; element < kElementCount; ++element) {
                view_[element, sample] = value[element];
            }
        }

        /** @brief Return one unit-stride value-element channel for coordinate-major storage. */
        [[nodiscard]] constexpr std::span<T> ElementChannel(size_t element) const noexcept
            requires(Layout == BatchLayout::CoordinateMajor)
        {
            SORA_PRE(element < kElementCount);
            if (Size() == 0) {
                return {view_.data_handle(), size_t{0}};
            }
            return {view_.data_handle() + element * Size(), Size()};
        }

        [[nodiscard]] constexpr std::span<T> Coordinate(size_t coordinate) const noexcept
            requires(Layout == BatchLayout::CoordinateMajor && kDimension > 0 && kElementCount == kDimension)
        {
            return ElementChannel(coordinate);
        }

    private:
        [[nodiscard]] static constexpr bool ValidView(const T* data, size_t sampleCount) noexcept {
            return sampleCount <= (std::numeric_limits<size_t>::max)() / kElementCount &&
                   (sampleCount == 0 || data != nullptr);
        }

        [[nodiscard]] static constexpr ViewType MakeView(T* data, size_t sampleCount) noexcept {
            SORA_PRE(ValidView(data, sampleCount));
            return ViewType{data, sampleCount};
        }

        template<size_t... Elements>
        [[nodiscard]] static consteval bool NothrowLoad(std::index_sequence<Elements...>) {
            return noexcept(Value{(static_cast<void>(Elements), std::declval<T&>())...});
        }

        inline static constexpr bool kNothrowLoad = NothrowLoad(std::make_index_sequence<kElementCount>{});
        inline static constexpr bool kNothrowStore =
            noexcept(std::declval<T&>() = std::declval<const typename Value::CarrierType&>());

        template<size_t... Elements>
        [[nodiscard]] constexpr Value Load(size_t sample,
                                           std::index_sequence<Elements...>) const noexcept(kNothrowLoad) {
            return Value{view_[Elements, sample]...};
        }

        ViewType view_;
    };

    /** @brief Concrete-value alias retained for call sites that use homogeneous terminology. */
    template<Sora::Concept::TensorValue Value, typename T, BatchLayout Layout = BatchLayout::CoordinateMajor>
    using ValueBatchView = StaticValueBatchView<Value, T, Layout>;

    /** @brief Batch view of a concrete fixed-shape tensor value. */
    template<Sora::Concept::TensorValue Value, typename T, BatchLayout Layout = BatchLayout::CoordinateMajor>
    using TensorBatchView = StaticValueBatchView<Value, T, Layout>;

    /** @brief Compatibility view retaining the historical value-template spelling. */
    template<template<typename, size_t> typename ValueTemplate, typename T, size_t N,
             BatchLayout Layout = BatchLayout::CoordinateMajor>
        requires(N > 0 && Sora::Concept::TensorElement<std::remove_const_t<T>>)
    using HomogeneousBatchView = StaticValueBatchView<ValueTemplate<std::remove_const_t<T>, N>, T, Layout>;

    /** @brief Batch view of fixed-size free vectors. */
    template<typename T, size_t N, BatchLayout Layout = BatchLayout::CoordinateMajor>
    using VectorBatchView = HomogeneousBatchView<Vector, T, N, Layout>;

    /** @brief Batch view of fixed-size affine points. */
    template<typename T, size_t N, BatchLayout Layout = BatchLayout::CoordinateMajor>
    using PointBatchView = HomogeneousBatchView<Point, T, N, Layout>;

    /** @brief Static value batch protocol accepted by value execution kernels. */
    template<typename T>
    concept HomogeneousBatch = requires(T batch, size_t sample) {
        typename std::remove_cvref_t<T>::ValueType;
        typename std::remove_cvref_t<T>::ElementType;
        typename std::remove_cvref_t<T>::ScalarType;
        typename std::remove_cvref_t<T>::template RebindElement<typename std::remove_cvref_t<T>::ScalarType>;
        { std::remove_cvref_t<T>::kElementCount } -> std::convertible_to<size_t>;
        { std::remove_cvref_t<T>::kLayout } -> std::convertible_to<BatchLayout>;
        { batch.Size() } -> std::same_as<size_t>;
        batch.Load(sample);
    };

    /** @brief Batch exposing unit-stride element channels for explicit SIMD execution. */
    template<typename T>
    concept CoordinateMajorBatch = HomogeneousBatch<T> &&
                                   (std::remove_cvref_t<T>::kLayout == BatchLayout::CoordinateMajor) &&
                                   requires(T batch, size_t element) {
                                       { batch.ElementChannel(element) } -> std::same_as<
                                           std::span<typename std::remove_cvref_t<T>::ElementType>>;
                                       { batch.Data() } -> std::same_as<typename std::remove_cvref_t<T>::ElementType*>;
                                   };

} // namespace Sora::Math
