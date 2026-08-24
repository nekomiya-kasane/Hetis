/**
 * @file Kokkos.h
 * @brief Zero-copy interop between Sora homogeneous batches and host-accessible unmanaged Kokkos Views.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/BatchView.h>

#include <Kokkos_Core.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace Sora::Math::Interop::Kokkos {

    /** @brief Reason a Kokkos View cannot be represented by a Sora homogeneous batch view. */
    enum class BatchViewError : uint8_t {
        DimensionMismatch, /**< The first logical extent is not the requested mathematical dimension. */
        NonContiguous,     /**< The View mapping contains padding or strides that Sora cannot represent. */
        MissingStorage     /**< A non-empty View has no data pointer. */
    };

    namespace Detail {

        template<typename View>
        using ViewType = std::remove_cvref_t<View>;

        template<typename View>
        concept KokkosView = ::Kokkos::is_view_v<ViewType<View>>;

        template<typename View>
        concept SupportedBatchLayout = std::same_as<typename ViewType<View>::array_layout, ::Kokkos::LayoutLeft> ||
                                       std::same_as<typename ViewType<View>::array_layout, ::Kokkos::LayoutRight>;

        template<typename View>
        using ElementType = std::remove_pointer_t<typename ViewType<View>::pointer_type>;

        template<typename View>
        [[nodiscard]] consteval BatchLayout SoraLayout() {
            if constexpr (std::same_as<typename ViewType<View>::array_layout, ::Kokkos::LayoutRight>) {
                return BatchLayout::CoordinateMajor;
            } else {
                return BatchLayout::PointMajor;
            }
        }

        template<BatchLayout Layout>
        using KokkosLayout =
            std::conditional_t<Layout == BatchLayout::CoordinateMajor, ::Kokkos::LayoutRight, ::Kokkos::LayoutLeft>;

        template<template<typename, size_t> typename ValueTemplate, size_t N, typename View>
        [[nodiscard]] auto TryAsBatch(const View& view)
            -> std::expected<HomogeneousBatchView<ValueTemplate, ElementType<View>, N, SoraLayout<View>()>,
                             BatchViewError> {
            using Batch = HomogeneousBatchView<ValueTemplate, ElementType<View>, N, SoraLayout<View>()>;
            if (view.extent(0) != N) {
                return std::unexpected(BatchViewError::DimensionMismatch);
            }
            if (!view.span_is_contiguous()) {
                return std::unexpected(BatchViewError::NonContiguous);
            }
            if (view.extent(0) != 0 && view.extent(1) != 0 && view.data() == nullptr) {
                return std::unexpected(BatchViewError::MissingStorage);
            }
            return Batch(view.data(), view.extent(1));
        }

    } // namespace Detail

    /**
     * @brief Kokkos View that Sora may expose as a non-owning host batch without copying or extending ownership.
     * @details Managed Views are intentionally rejected: returning a non-owning Sora view from a temporary managed
     * View could silently discard the final allocation owner. Device-only spaces and general strided layouts are also
     * rejected because @ref HomogeneousBatchView promises host dereferenceability and one of two exact dense layouts.
     */
    template<typename View>
    concept HostAccessibleUnmanagedBatchView =
        Detail::KokkosView<View> && (Detail::ViewType<View>::rank() == 2) && Detail::SupportedBatchLayout<View> &&
        bool(Detail::ViewType<View>::memory_traits::is_unmanaged) &&
        bool(::Kokkos::SpaceAccessibility<::Kokkos::HostSpace,
                                          typename Detail::ViewType<View>::memory_space>::accessible) &&
        std::is_pointer_v<typename Detail::ViewType<View>::pointer_type> &&
        ValueCarrier<std::remove_const_t<Detail::ElementType<View>>>;

    /** @brief Expose an unmanaged Kokkos View as a Sora vector batch without copying. */
    template<size_t N, HostAccessibleUnmanagedBatchView View>
        requires(N > 0 &&
                 (Detail::ViewType<View>::static_extent(0) == 0 || Detail::ViewType<View>::static_extent(0) == N))
    [[nodiscard]] auto TryAsVectorBatch(const View& view)
        -> std::expected<VectorBatchView<Detail::ElementType<View>, N, Detail::SoraLayout<View>()>, BatchViewError> {
        return Detail::TryAsBatch<Vector, N>(view);
    }

    /** @brief Expose an unmanaged Kokkos View as a Sora affine-point batch without copying. */
    template<size_t N, HostAccessibleUnmanagedBatchView View>
        requires(N > 0 &&
                 (Detail::ViewType<View>::static_extent(0) == 0 || Detail::ViewType<View>::static_extent(0) == N))
    [[nodiscard]] auto TryAsPointBatch(const View& view)
        -> std::expected<PointBatchView<Detail::ElementType<View>, N, Detail::SoraLayout<View>()>, BatchViewError> {
        return Detail::TryAsBatch<Point, N>(view);
    }

    /**
     * @brief Expose a Sora homogeneous batch as a host-space unmanaged Kokkos View without copying.
     * @details The returned View does not own or extend the lifetime of @p batch storage.
     */
    template<template<typename, size_t> typename ValueTemplate, typename T, size_t N, BatchLayout Layout>
    [[nodiscard]] auto AsKokkosView(HomogeneousBatchView<ValueTemplate, T, N, Layout> batch) {
        using KokkosLayout = Detail::KokkosLayout<Layout>;
        using View =
            ::Kokkos::View<T**, KokkosLayout, ::Kokkos::HostSpace, ::Kokkos::MemoryTraits<::Kokkos::Unmanaged>>;
        return View(batch.View().data_handle(), N, batch.Size());
    }

} // namespace Sora::Math::Interop::Kokkos
