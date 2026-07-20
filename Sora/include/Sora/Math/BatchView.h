/**
 * @file BatchView.h
 * @brief Non-owning mdspan views for homogeneous point and vector batches.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/Point.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mdspan>
#include <meta>
#include <ranges>
#include <span>
#include <type_traits>

namespace Sora::Math {

    /** @brief Physical order of a homogeneous coordinate batch. */
    enum class BatchLayout : uint8_t {
        PointMajor,     /**< Coordinates of each sample are contiguous. */
        CoordinateMajor /**< One coordinate across all samples is contiguous, enabling unit-stride SIMD. */
    };

    namespace Detail {

        template<BatchLayout Layout>
        [[nodiscard]] consteval auto MdspanLayoutType() {
            if constexpr (Layout == BatchLayout::CoordinateMajor) {
                return std::type_identity<std::layout_right>{};
            } else {
                return std::type_identity<std::layout_left>{};
            }
        }

        template<BatchLayout Layout>
        using MdspanLayout = typename decltype(MdspanLayoutType<Layout>())::type;

    } // namespace Detail

    /**
     * @brief Non-owning two-dimensional view of homogeneous fixed-size values.
     * @tparam ValueTemplate Mathematical value template, such as @ref Vector or @ref Point.
     * @tparam T Possibly-const scalar element type stored in external memory.
     * @tparam N Compile-time coordinate dimension.
     * @tparam Layout Physical order of coordinates and samples.
     * @details The logical mdspan shape is always @c [coordinate,sample]. Layout changes physical strides only.
     */
    template<template<typename, size_t> typename ValueTemplate, typename T, size_t N,
             BatchLayout Layout = BatchLayout::CoordinateMajor>
        requires(N > 0 && std::is_object_v<T> && ValueCarrier<std::remove_const_t<T>>)
    class HomogeneousBatchView {
    public:
        using ElementType = T;                     /**< Possibly-const external scalar element type. */
        using ScalarType = std::remove_const_t<T>; /**< Unqualified scalar element type. */
        using Extents = std::extents<size_t, N, std::dynamic_extent>;
        using LayoutPolicy = Detail::MdspanLayout<Layout>;
        using ViewType = std::mdspan<T, Extents, LayoutPolicy>;
        static constexpr size_t kDimension = N;
        static constexpr BatchLayout kLayout = Layout;

        /** @brief Rebind the mathematical value to another carrier without changing its affine semantics. */
        template<typename Carrier>
        using Rebind = ValueTemplate<Carrier, N>;

        /** @brief Construct a view over @p sampleCount values. @pre @p data addresses @c N*sampleCount items. */
        constexpr HomogeneousBatchView(T* data, size_t sampleCount) noexcept : view_(data, sampleCount) {}

        /** @brief Construct from an mdspan with the exact logical shape and layout. */
        constexpr explicit HomogeneousBatchView(ViewType view) noexcept : view_(view) {}

        /** @brief Return the number of logical point or vector samples. */
        [[nodiscard]] constexpr size_t Size() const noexcept { return view_.extent(1); }

        /** @brief Return the underlying logical @c [coordinate,sample] mdspan. */
        [[nodiscard]] constexpr ViewType View() const noexcept { return view_; }

        /** @brief Access one external scalar element. */
        [[nodiscard]] constexpr T& operator[](size_t coordinate, size_t sample) const noexcept {
            return view_[coordinate, sample];
        }

        /** @brief Gather one logical scalar value. */
        [[nodiscard]] constexpr Rebind<ScalarType> Load(size_t sample) const noexcept {
            Rebind<ScalarType> result;
            template for (constexpr size_t coordinate : std::define_static_array(std::views::iota(size_t{0}, N))) {
                result[coordinate] = view_[coordinate, sample];
            }
            return result;
        }

        /** @brief Scatter one logical scalar value. */
        constexpr void Store(size_t sample, const Rebind<ScalarType>& value) const noexcept
            requires(!std::is_const_v<T>)
        {
            template for (constexpr size_t coordinate : std::define_static_array(std::views::iota(size_t{0}, N))) {
                view_[coordinate, sample] = value[coordinate];
            }
        }

        /** @brief Return one unit-stride coordinate channel. */
        [[nodiscard]] constexpr std::span<T> Coordinate(size_t coordinate) const noexcept
            requires(Layout == BatchLayout::CoordinateMajor)
        {
            return {view_.data_handle() + coordinate * Size(), Size()};
        }

    private:
        ViewType view_;
    };

    /** @brief Homogeneous free-vector batch view. */
    template<typename T, size_t N, BatchLayout Layout = BatchLayout::CoordinateMajor>
    using VectorBatchView = HomogeneousBatchView<Vector, T, N, Layout>;

    /** @brief Homogeneous affine-point batch view. */
    template<typename T, size_t N, BatchLayout Layout = BatchLayout::CoordinateMajor>
    using PointBatchView = HomogeneousBatchView<Point, T, N, Layout>;

    /** @brief Type satisfying the non-owning homogeneous batch interface. */
    template<typename T>
    concept HomogeneousBatch = requires(T batch, size_t sample) {
        typename T::ElementType;
        typename T::ScalarType;
        typename T::template Rebind<typename T::ScalarType>;
        { T::kDimension } -> std::convertible_to<size_t>;
        { T::kLayout } -> std::convertible_to<BatchLayout>;
        { batch.Size() } -> std::same_as<size_t>;
        batch.Load(sample);
    };

    /** @brief Homogeneous batch exposing unit-stride coordinate channels for SIMD loads and stores. */
    template<typename T>
    concept CoordinateMajorBatch =
        HomogeneousBatch<T> && (std::remove_cvref_t<T>::kLayout == BatchLayout::CoordinateMajor) &&
        requires(T batch, size_t coordinate) {
            { batch.Coordinate(coordinate) } -> std::same_as<std::span<typename std::remove_cvref_t<T>::ElementType>>;
        };

} // namespace Sora::Math
