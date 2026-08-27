/**
 * @file TensorCore.h
 * @brief Static tensor shapes, dense storage mappings, interpretations, and the shared value facade.
 * @ingroup Math
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <mdspan>
#include <type_traits>
#include <utility>

namespace Sora::Concept {

    /** @brief Trivially copyable value suitable for inline mathematical storage. */
    template<typename T>
    concept TensorElement = std::is_object_v<T> && std::same_as<T, std::remove_cv_t<T>> && std::copyable<T> &&
                            std::is_trivially_copyable_v<T>;

} // namespace Sora::Concept

namespace Sora::Math {

    /** @brief Public compatibility spelling for an inline mathematical carrier. */
    template<typename T>
    concept TensorElement = Sora::Concept::TensorElement<T>;

    /** @brief Mathematical carrier accepted by vectors, points, matrices, and tensors. */
    template<typename T>
    concept ValueCarrier = TensorElement<T>;

    /** @brief Generate one fixed-size array with an index-constant invocation per element. */
    template<typename R, size_t Count, typename Function, size_t... Indices>
    [[nodiscard, gnu::always_inline]] constexpr auto GenerateStaticArray(
        Function&& function, std::index_sequence<Indices...>)
        noexcept((std::is_nothrow_invocable_r_v<R, Function&, std::integral_constant<size_t, Indices>> && ...)) {
        return std::array<R, Count>{
            std::invoke(function, std::integral_constant<size_t, Indices>{})...};
    }

    /** @brief Generate one fixed-size array without exposing index-sequence scaffolding. */
    template<typename R, size_t Count, typename Function>
    [[nodiscard, gnu::always_inline]] constexpr auto GenerateStaticArray(Function&& function)
        noexcept(noexcept(GenerateStaticArray<R, Count>(std::declval<Function&>(),
                                                         std::make_index_sequence<Count>{}))) {
        return GenerateStaticArray<R, Count>(function, std::make_index_sequence<Count>{});
    }

    /** @brief Value constructible from the additive identity. */
    template<typename T>
    concept ZeroInitializable = std::constructible_from<T, int> || requires { T(typename T::ValueType{0}); } ||
                                requires {
                                    typename T::PrimalType;
                                    T{typename T::PrimalType{0}, typename T::PrimalType{0}};
                                };

    /** @brief Value constructible from the multiplicative identity. */
    template<typename T>
    concept OneInitializable = std::constructible_from<T, int> || requires { T(typename T::ValueType{1}); } ||
                               requires {
                                   typename T::PrimalType;
                                   T{typename T::PrimalType{1}, typename T::PrimalType{0}};
                               };

    template<typename T>
        requires ZeroInitializable<T>
    inline constexpr bool kNothrowZeroValue = [] {
        if constexpr (std::constructible_from<T, int>) {
            return std::is_nothrow_constructible_v<T, int>;
        } else if constexpr (requires {
                                 typename T::PrimalType;
                                 T{typename T::PrimalType{0}, typename T::PrimalType{0}};
                             }) {
            return std::is_nothrow_constructible_v<T, typename T::PrimalType,
                                                   typename T::PrimalType>;
        } else {
            return noexcept(T(typename T::ValueType{0}));
        }
    }();

    template<typename T>
        requires OneInitializable<T>
    inline constexpr bool kNothrowOneValue = [] {
        if constexpr (std::constructible_from<T, int>) {
            return std::is_nothrow_constructible_v<T, int>;
        } else if constexpr (requires {
                                 typename T::PrimalType;
                                 T{typename T::PrimalType{1}, typename T::PrimalType{0}};
                             }) {
            return std::is_nothrow_constructible_v<T, typename T::PrimalType,
                                                   typename T::PrimalType>;
        } else {
            return noexcept(T(typename T::ValueType{1}));
        }
    }();

    /** @brief Construct the additive identity of @p T. */
    template<typename T>
        requires ZeroInitializable<T>
    [[nodiscard]] constexpr T ZeroValue() noexcept(kNothrowZeroValue<T>) {
        if constexpr (std::constructible_from<T, int>) {
            return T{0};
        } else if constexpr (requires {
                                 typename T::PrimalType;
                                 T{typename T::PrimalType{0}, typename T::PrimalType{0}};
                             }) {
            return T{typename T::PrimalType{0}, typename T::PrimalType{0}};
        } else {
            return T(typename T::ValueType{0});
        }
    }

    /** @brief Construct the multiplicative identity of @p T. */
    template<typename T>
        requires OneInitializable<T>
    [[nodiscard]] constexpr T OneValue() noexcept(kNothrowOneValue<T>) {
        if constexpr (std::constructible_from<T, int>) {
            return T{1};
        } else if constexpr (requires {
                                 typename T::PrimalType;
                                 T{typename T::PrimalType{1}, typename T::PrimalType{0}};
                             }) {
            return T{typename T::PrimalType{1}, typename T::PrimalType{0}};
        } else {
            return T(typename T::ValueType{1});
        }
    }

    /** @brief Positive compile-time tensor extents. */
    template<size_t... Extents>
        requires(sizeof...(Extents) > 0 && ((Extents > 0) && ...))
    using StaticShape = std::extents<size_t, Extents...>;

    /** @brief Physical storage order retained by the public matrix facade. */
    enum class MatrixLayout : unsigned char {
        ColumnMajor,
        RowMajor,
    };

    /** @brief Dense mapping with the final logical axis contiguous. */
    struct RightMapping {
        using LayoutPolicy = std::layout_right;
        template<typename Shape>
        using Mapping = LayoutPolicy::mapping<Shape>;
        static constexpr MatrixLayout kMatrixLayout = MatrixLayout::RowMajor;
    };

    /** @brief Dense mapping with the first logical axis contiguous. */
    struct LeftMapping {
        using LayoutPolicy = std::layout_left;
        template<typename Shape>
        using Mapping = LayoutPolicy::mapping<Shape>;
        static constexpr MatrixLayout kMatrixLayout = MatrixLayout::ColumnMajor;
    };

    /** @brief Matrix mapping preserving the existing enum-based public layout contract. */
    template<MatrixLayout Layout>
    struct MatrixMapping;

    template<>
    struct MatrixMapping<MatrixLayout::ColumnMajor> : LeftMapping {};

    template<>
    struct MatrixMapping<MatrixLayout::RowMajor> : RightMapping {};

    /** @brief Tag selecting logical row-major input independently of physical dense mapping. */
    struct LogicalValuesTag {};
    inline constexpr LogicalValuesTag LogicalValues{};

    namespace Algebra {

        struct GeneralTensor {};
        struct FreeVector {};
        struct AffinePoint {};
        struct LinearMap {};

    } // namespace Algebra

} // namespace Sora::Math

namespace Sora::Math::Detail {

    template<typename Shape, size_t... Axes>
    consteval size_t StaticExtentProduct(std::index_sequence<Axes...>) noexcept {
        return (size_t{1} * ... * Shape::static_extent(Axes));
    }

    template<typename T, typename Shape>
    using DenseMappingType = typename T::template Mapping<Shape>;

    template<typename T, typename Shape, size_t... Axes>
    consteval bool SupportsDenseMapping(std::index_sequence<Axes...>) {
        using Mapping = DenseMappingType<T, Shape>;
        return requires(Mapping mapping) {
            { mapping((static_cast<void>(Axes), size_t{})...) } -> std::convertible_to<size_t>;
        };
    }

} // namespace Sora::Math::Detail

namespace Sora::Concept {

    template<typename T>
    concept StaticShape = requires {
        { T::rank() } -> std::convertible_to<size_t>;
        { T::rank_dynamic() } -> std::convertible_to<size_t>;
    } && (T::rank() > 0) && (T::rank_dynamic() == 0);

    template<typename T, typename Shape>
    concept DenseMapping = requires {
        typename T::LayoutPolicy;
        typename Sora::Math::Detail::DenseMappingType<T, Shape>;
        { T::kMatrixLayout } -> std::convertible_to<Sora::Math::MatrixLayout>;
        { Sora::Math::Detail::DenseMappingType<T, Shape>{}.required_span_size() } -> std::convertible_to<size_t>;
        { Sora::Math::Detail::DenseMappingType<T, Shape>{}.is_exhaustive() } -> std::same_as<bool>;
        { Sora::Math::Detail::DenseMappingType<T, Shape>{}.is_unique() } -> std::same_as<bool>;
    } && Sora::Math::Detail::DenseMappingType<T, Shape>{}.is_exhaustive() &&
        Sora::Math::Detail::DenseMappingType<T, Shape>{}.is_unique() &&
        (Sora::Math::Detail::DenseMappingType<T, Shape>{}.required_span_size() ==
         Sora::Math::Detail::StaticExtentProduct<Shape>(std::make_index_sequence<Shape::rank()>{})) &&
        Sora::Math::Detail::SupportsDenseMapping<T, Shape>(std::make_index_sequence<Shape::rank()>{});

} // namespace Sora::Concept

namespace Sora::Math {

    /**
     * @brief Owning fixed-shape dense storage with a compile-time index mapping.
     * @details Constructor arrays and variadic values are always logical row-major. @p Mapping controls physical
     * placement only. The object representation remains exactly one @c std::array<T,kElementCount>.
     */
    template<TensorElement T, Sora::Concept::StaticShape Shape, typename Mapping = RightMapping>
        requires Sora::Concept::DenseMapping<Mapping, Shape>
    class StaticDenseStorage {
    public:
        using ElementType = T;
        using ShapeType = Shape;
        using MappingPolicy = Mapping;
        using MappingType = typename Mapping::template Mapping<Shape>;
        static constexpr size_t kRank = Shape::rank();
        static constexpr size_t kElementCount = MappingType{}.required_span_size();
        static constexpr auto kExtents = [] {
            std::array<size_t, kRank> extents{};
            for (size_t axis = 0; axis < kRank; ++axis) {
                extents[axis] = Shape::static_extent(axis);
            }
            return extents;
        }();

        template<TensorElement U>
        using RebindElement = StaticDenseStorage<U, Shape, Mapping>;

        constexpr StaticDenseStorage() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::default_initializable<T>
            : values_{} {}

        constexpr explicit StaticDenseStorage(std::array<T, kElementCount> physicalValues)
            noexcept(std::is_nothrow_move_constructible_v<T>)
            : values_(std::move(physicalValues)) {}

        constexpr StaticDenseStorage(LogicalValuesTag, std::array<T, kElementCount> logicalValues)
            noexcept(std::is_nothrow_move_constructible_v<T>)
            : StaticDenseStorage(LogicalValues, std::move(logicalValues),
                                 std::make_index_sequence<kElementCount>{}) {}

        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) AtLogicalLinear(this Self&& self, size_t logicalIndex) noexcept {
            return std::forward_like<Self>(self.values_[StorageIndex(logicalIndex)]);
        }

        template<typename Self, std::convertible_to<size_t>... Indices>
            requires(sizeof...(Indices) == kRank)
        [[nodiscard]] constexpr decltype(auto) At(this Self&& self, Indices... indices) noexcept {
            return std::forward_like<Self>(self.values_[MappingType{}(static_cast<size_t>(indices)...)]);
        }

        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Values(this Self&& self) noexcept {
            return std::forward_like<Self>(self.values_);
        }

        template<typename Self>
            requires std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto Data(this Self&& self) noexcept {
            return self.values_.data();
        }

        template<typename Self>
            requires std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto View(this Self&& self) noexcept {
            using Element = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>, const T, T>;
            return std::mdspan<Element, Shape, typename MappingPolicy::LayoutPolicy>{self.values_.data()};
        }

    private:
        template<size_t... PhysicalIndices>
        constexpr StaticDenseStorage(LogicalValuesTag, std::array<T, kElementCount> logicalValues,
                                     std::index_sequence<PhysicalIndices...>)
            noexcept(std::is_nothrow_move_constructible_v<T>)
            : values_{T(std::move(logicalValues[LogicalIndex(PhysicalIndices)]))...} {}

        [[nodiscard]] static constexpr std::array<size_t, kRank> Coordinates(size_t logicalIndex) noexcept {
            std::array<size_t, kRank> coordinates{};
            for (size_t axis = kRank; axis-- > 0;) {
                coordinates[axis] = logicalIndex % kExtents[axis];
                logicalIndex /= kExtents[axis];
            }
            return coordinates;
        }

        template<size_t... Axes>
        [[nodiscard]] static constexpr size_t Map(const std::array<size_t, kRank>& coordinates,
                                                  std::index_sequence<Axes...>) noexcept {
            return MappingType{}(coordinates[Axes]...);
        }

        [[nodiscard]] static constexpr size_t StorageIndex(size_t logicalIndex) noexcept {
            return Map(Coordinates(logicalIndex), std::make_index_sequence<kRank>{});
        }

        [[nodiscard]] static consteval size_t LogicalIndex(size_t physicalIndex) noexcept {
            for (size_t logicalIndex = 0; logicalIndex < kElementCount; ++logicalIndex) {
                if (StorageIndex(logicalIndex) == physicalIndex) {
                    return logicalIndex;
                }
            }
            return 0;
        }

        std::array<T, kElementCount> values_;
    };

} // namespace Sora::Math

namespace Sora::Math::Detail {

    template<typename T, size_t... Axes>
    consteval bool SupportsTensorAccess(std::index_sequence<Axes...>) {
        return requires(T storage, const T constStorage) {
            storage.At((static_cast<void>(Axes), size_t{})...);
            constStorage.At((static_cast<void>(Axes), size_t{})...);
            storage.View();
            constStorage.View();
        };
    }

    template<typename T, size_t... Axes>
    consteval bool HasValidExtents(std::index_sequence<Axes...>) {
        return ((T::kExtents[Axes] > 0) && ...) &&
               ((size_t{1} * ... * T::kExtents[Axes]) == T::kElementCount);
    }

} // namespace Sora::Math::Detail

namespace Sora::Concept {

    template<typename T>
    concept TensorStorage = requires(T storage, size_t index) {
        typename T::ElementType;
        typename T::ShapeType;
        { T::kRank } -> std::convertible_to<size_t>;
        { T::kElementCount } -> std::convertible_to<size_t>;
        { T::kExtents.size() } -> std::convertible_to<size_t>;
        storage.AtLogicalLinear(index);
    } && (T::kRank > 0) && (T::kElementCount > 0) && (T::kExtents.size() == T::kRank) &&
        Sora::Math::Detail::HasValidExtents<T>(std::make_index_sequence<T::kRank>{});

    template<typename T>
    concept DenseTensorStorage = TensorStorage<T> && TensorElement<typename T::ElementType> &&
        requires(T storage, const T constStorage) {
            typename T::MappingPolicy;
            typename T::MappingType;
            typename T::template RebindElement<typename T::ElementType>;
            typename T::MappingPolicy::LayoutPolicy;
            { T::MappingPolicy::kMatrixLayout } -> std::convertible_to<Sora::Math::MatrixLayout>;
            storage.Values();
            constStorage.Values();
            { storage.Data() } -> std::same_as<typename T::ElementType*>;
            { constStorage.Data() } -> std::same_as<const typename T::ElementType*>;
            requires std::is_lvalue_reference_v<decltype(storage.Values())>;
            requires std::is_lvalue_reference_v<decltype(constStorage.Values())>;
        } && std::move_constructible<T> &&
        std::constructible_from<T, Sora::Math::LogicalValuesTag,
                                std::array<typename T::ElementType, T::kElementCount>> &&
        Sora::Math::Detail::SupportsTensorAccess<T>(std::make_index_sequence<T::kRank>{});

    /** @brief Dense storage that can be rebound to another mathematical element type. */
    template<typename Storage, typename Element>
    concept RebindableDenseTensorStorage = TensorElement<Element> && requires {
        typename Storage::template RebindElement<Element>;
    } && DenseTensorStorage<typename Storage::template RebindElement<Element>>;

} // namespace Sora::Concept

namespace Sora::Math {

    /**
     * @brief Semantic facade over one tensor storage object.
     * @details @p Interpretation is compile-time metadata and occupies no object storage. Algebraic operations are
     * constrained separately, so affine points cannot accidentally acquire vector or general-tensor arithmetic.
     */
    template<Sora::Concept::DenseTensorStorage StoragePolicy, typename Interpretation = Algebra::GeneralTensor>
    class BasicTensor {
    public:
        using ElementType = typename StoragePolicy::ElementType;
        using CarrierType = ElementType;
        using ShapeType = typename StoragePolicy::ShapeType;
        using StorageType = StoragePolicy;
        using InterpretationType = Interpretation;
        using MappingPolicy = typename StoragePolicy::MappingPolicy;
        static constexpr size_t kRank = StoragePolicy::kRank;
        static constexpr size_t kElementCount = StoragePolicy::kElementCount;
        static constexpr size_t kSize = kElementCount;
        static constexpr auto kExtents = StoragePolicy::kExtents;
        static constexpr size_t kDimension = kRank == 1 ? kExtents[0] : 0;
        static constexpr size_t kRows = kRank == 2 ? kExtents[0] : 0;
        static constexpr size_t kColumns = kRank == 2 ? kExtents[1] : 0;
        static constexpr MatrixLayout kLayout = MappingPolicy::kMatrixLayout;

        template<TensorElement U>
            requires Sora::Concept::RebindableDenseTensorStorage<StoragePolicy, U>
        using Rebind = BasicTensor<typename StoragePolicy::template RebindElement<U>, Interpretation>;

        template<TensorElement U>
            requires Sora::Concept::RebindableDenseTensorStorage<StoragePolicy, U>
        using RebindElement = Rebind<U>;

        constexpr BasicTensor() noexcept(kNothrowDefaultConstruction)
            requires(std::same_as<Interpretation, Algebra::GeneralTensor> ?
                         std::default_initializable<StoragePolicy> :
                         ZeroInitializable<ElementType>)
            : storage_(DefaultStorage()) {}

        constexpr explicit BasicTensor(std::array<ElementType, kElementCount> storageValues)
            noexcept(std::is_nothrow_constructible_v<StoragePolicy, LogicalValuesTag,
                                                     std::array<ElementType, kElementCount>>)
            : storage_(LogicalValues, std::move(storageValues)) {}

        template<typename... U>
            requires(sizeof...(U) == kElementCount && (std::constructible_from<ElementType, U&&> && ...))
        constexpr explicit(!(std::convertible_to<U&&, ElementType> && ...))
            BasicTensor(U&&... values) noexcept(
                (std::is_nothrow_constructible_v<ElementType, U&&> && ...) &&
                std::is_nothrow_constructible_v<StoragePolicy, LogicalValuesTag,
                                                std::array<ElementType, kElementCount>>)
            : storage_(LogicalValues,
                       std::array<ElementType, kElementCount>{ElementType(std::forward<U>(values))...}) {}

        constexpr explicit BasicTensor(StoragePolicy storage)
            noexcept(std::is_nothrow_move_constructible_v<StoragePolicy>)
            : storage_(std::move(storage)) {}

        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) operator[](this Self&& self, size_t logicalIndex) noexcept {
            return std::forward_like<Self>(self.storage_).AtLogicalLinear(logicalIndex);
        }

        template<typename Self, std::convertible_to<size_t>... Indices>
            requires(sizeof...(Indices) == kRank)
        [[nodiscard]] constexpr decltype(auto) operator()(this Self&& self, Indices... indices) noexcept {
            return std::forward_like<Self>(self.storage_).At(static_cast<size_t>(indices)...);
        }

        template<typename Self>
            requires Sora::Concept::DenseTensorStorage<StoragePolicy>
        [[nodiscard]] constexpr decltype(auto) Values(this Self&& self) noexcept {
            return std::forward_like<Self>(self.storage_).Values();
        }

        template<typename Self>
            requires Sora::Concept::DenseTensorStorage<StoragePolicy>
        [[nodiscard]] constexpr decltype(auto) Storage(this Self&& self) noexcept {
            return std::forward_like<Self>(self.storage_).Values();
        }

        template<typename Self>
            requires Sora::Concept::DenseTensorStorage<StoragePolicy> && std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto Data(this Self&& self) noexcept {
            return self.storage_.Data();
        }

        template<typename Self>
            requires Sora::Concept::DenseTensorStorage<StoragePolicy> && std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto View(this Self&& self) noexcept {
            return self.storage_.View();
        }

        template<typename Self>
            requires std::same_as<Interpretation, Algebra::FreeVector>
        [[nodiscard]] constexpr decltype(auto) Components(this Self&& self) noexcept {
            return std::forward_like<Self>(self.storage_).Values();
        }

        template<typename Self>
            requires std::same_as<Interpretation, Algebra::AffinePoint>
        [[nodiscard]] constexpr decltype(auto) Coordinates(this Self&& self) noexcept {
            return std::forward_like<Self>(self.storage_).Values();
        }

        template<size_t Axis>
            requires(Axis < kRank)
        [[nodiscard]] static consteval size_t Extent() noexcept {
            return kExtents[Axis];
        }

        [[nodiscard]] static constexpr BasicTensor Identity()
            noexcept(kNothrowZeroValue<ElementType> && kNothrowOneValue<ElementType> &&
                     std::is_nothrow_constructible_v<StoragePolicy, LogicalValuesTag,
                                                     std::array<ElementType, kElementCount>> &&
                     std::is_nothrow_move_constructible_v<StoragePolicy>)
            requires std::same_as<Interpretation, Algebra::LinearMap> && (kRank == 2) && (kRows == kColumns) &&
                     ZeroInitializable<ElementType> && OneInitializable<ElementType>
        {
            return BasicTensor{StoragePolicy{LogicalValues,
                                             GenerateStaticArray<ElementType, kElementCount>([](auto index) {
                constexpr size_t row = decltype(index)::value / kColumns;
                constexpr size_t column = decltype(index)::value % kColumns;
                if constexpr (row == column) {
                    return OneValue<ElementType>();
                } else {
                    return ZeroValue<ElementType>();
                }
            })}};
        }

    private:
        inline static constexpr bool kNothrowDefaultConstruction = [] {
            if constexpr (std::same_as<Interpretation, Algebra::GeneralTensor>) {
                return std::is_nothrow_default_constructible_v<StoragePolicy>;
            } else if constexpr (ZeroInitializable<ElementType>) {
                return kNothrowZeroValue<ElementType> &&
                       std::is_nothrow_constructible_v<StoragePolicy, LogicalValuesTag,
                                                       std::array<ElementType, kElementCount>>;
            } else {
                return false;
            }
        }();

        [[nodiscard]] static constexpr StoragePolicy DefaultStorage() noexcept(kNothrowDefaultConstruction)
            requires(std::same_as<Interpretation, Algebra::GeneralTensor> ?
                         std::default_initializable<StoragePolicy> :
                         ZeroInitializable<ElementType>)
        {
            if constexpr (std::same_as<Interpretation, Algebra::GeneralTensor>) {
                return StoragePolicy{};
            } else {
                return StoragePolicy{LogicalValues, GenerateStaticArray<ElementType, kElementCount>([](auto) {
                                          return ZeroValue<ElementType>();
                                      })};
            }
        }

        StoragePolicy storage_;
    };

    template<typename T, typename... U>
    BasicTensor(T, U...)
        -> BasicTensor<StaticDenseStorage<std::common_type_t<T, U...>, StaticShape<1 + sizeof...(U)>, RightMapping>,
                       Algebra::GeneralTensor>;

} // namespace Sora::Math

namespace Sora::Concept {

    template<typename T>
    concept TensorValue = requires(const T value, size_t index) {
        typename T::ElementType;
        typename T::CarrierType;
        typename T::ShapeType;
        typename T::StorageType;
        typename T::InterpretationType;
        typename T::template RebindElement<typename T::CarrierType>;
        { T::kRank } -> std::convertible_to<size_t>;
        { T::kElementCount } -> std::convertible_to<size_t>;
        { T::kDimension } -> std::convertible_to<size_t>;
        { T::kExtents.size() } -> std::convertible_to<size_t>;
        value[index];
    } && TensorElement<typename T::CarrierType>;

    template<typename T>
    concept DenseTensor = TensorValue<T> && DenseTensorStorage<typename T::StorageType>;

} // namespace Sora::Concept

namespace Sora::Math {

    namespace Traits {

        template<Sora::Concept::TensorValue T>
        using Element = typename T::ElementType;

        template<Sora::Concept::TensorValue T>
        using Shape = typename T::ShapeType;

        template<Sora::Concept::TensorValue T>
        using Storage = typename T::StorageType;

        template<Sora::Concept::TensorValue T>
        using Interpretation = typename T::InterpretationType;

        template<Sora::Concept::TensorValue T, TensorElement U>
        using RebindElement = typename T::template RebindElement<U>;

        template<Sora::Concept::TensorValue T>
        inline constexpr size_t Rank = T::kRank;

        template<Sora::Concept::TensorValue T>
        inline constexpr size_t ElementCount = T::kElementCount;

        template<Sora::Concept::TensorValue T, size_t Axis>
            requires(Axis < T::kRank)
        inline constexpr size_t Extent = T::kExtents[Axis];

    } // namespace Traits

} // namespace Sora::Math
