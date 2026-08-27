/**
 * @file Sparse.h
 * @brief Canonical two-dimensional coordinate and compressed sparse matrices.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>
#include <Sora/Math/TensorCore.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <span>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace Sora::Math {

    namespace SparseDetail {

        template<typename T>
        [[nodiscard]] constexpr bool BuffersOverlap(std::span<const T> input, std::span<T> output) noexcept {
            if (input.empty() || output.empty()) {
                return false;
            }
            const std::less<const T*> less;
            return less(input.data(), output.data() + output.size()) &&
                   less(output.data(), input.data() + input.size());
        }

        template<typename T>
        concept SpmvValue = ZeroInitializable<T> && requires(const T& value, const T& input, T& accumulator) {
            Sora::Math::Mul(value, input);
            Sora::Math::Add(accumulator, Sora::Math::Mul(value, input));
            requires std::assignable_from<T&, decltype(Sora::Math::Add(
                accumulator, Sora::Math::Mul(value, input)))>;
        };

    } // namespace SparseDetail

    /** @brief Major index used by a compressed sparse matrix. */
    enum class SparseMajorAxis : uint8_t {
        Row,
        Column,
    };

    /** @brief Recoverable sparse-matrix construction or execution failure. */
    enum class SparseError : uint8_t {
        InvalidDimensions,
        IndexOutOfRange,
        IndexOverflow,
        DimensionMismatch,
        OverlappingBuffers,
    };

    template<typename T, typename Index, SparseMajorAxis Major>
    class CompressedMatrix;

    template<typename T, typename Index = uint32_t>
    using CsrMatrix = CompressedMatrix<T, Index, SparseMajorAxis::Row>;

    template<typename T, typename Index = uint32_t>
    using CscMatrix = CompressedMatrix<T, Index, SparseMajorAxis::Column>;

    /**
     * @brief Owns unordered sparse coordinates until one canonical compressed format is requested.
     * @tparam T Stored value type.
     * @tparam Index Unsigned index representation used by the compressed arrays.
     */
    template<typename T, typename Index = uint32_t>
        requires std::is_object_v<T> && std::is_unsigned_v<Index>
    class CoordinateMatrixBuilder {
    public:
        /** @brief Construct an empty builder for an @p rows by @p columns matrix. */
        constexpr CoordinateMatrixBuilder(size_t rows, size_t columns) noexcept
            : rows_(rows), columns_(columns) {}

        /** @brief Append one coordinate; duplicate coordinates are merged during @ref Build. */
        [[nodiscard]] std::expected<void, SparseError> Add(size_t row, size_t column, const T& value) {
            if (row >= rows_ || column >= columns_) {
                return std::unexpected(SparseError::IndexOutOfRange);
            }
            entries_.emplace_back(row, column, value);
            return {};
        }

        /** @brief Return the logical row count. */
        [[nodiscard]] constexpr size_t Rows() const noexcept { return rows_; }

        /** @brief Return the logical column count. */
        [[nodiscard]] constexpr size_t Columns() const noexcept { return columns_; }

        /** @brief Canonicalize coordinates into CSR or CSC with sorted indices and merged duplicates. */
        template<SparseMajorAxis Major>
        [[nodiscard]] std::expected<CompressedMatrix<T, Index, Major>, SparseError> Build() const {
            if (rows_ == 0 || columns_ == 0) {
                return std::unexpected(SparseError::InvalidDimensions);
            }
            const size_t majorCount = Major == SparseMajorAxis::Row ? rows_ : columns_;
            if (majorCount == (std::numeric_limits<size_t>::max)() ||
                majorCount > static_cast<size_t>((std::numeric_limits<Index>::max)())) {
                return std::unexpected(SparseError::IndexOverflow);
            }

            struct Entry {
                size_t major;
                size_t minor;
                T value;
            };
            std::vector<Entry> sorted;
            sorted.reserve(entries_.size());
            for (const auto& [row, column, value] : entries_) {
                sorted.push_back(Entry{Major == SparseMajorAxis::Row ? row : column,
                                       Major == SparseMajorAxis::Row ? column : row, value});
            }
            std::ranges::sort(sorted, [](const Entry& left, const Entry& right) {
                return left.major < right.major || (left.major == right.major && left.minor < right.minor);
            });

            std::vector<Entry> canonical;
            canonical.reserve(sorted.size());
            for (auto& entry : sorted) {
                if (!canonical.empty() && canonical.back().major == entry.major &&
                    canonical.back().minor == entry.minor) {
                    if constexpr (requires(T& left, const T& right) { Sora::Math::Add(left, right); }) {
                        canonical.back().value = Sora::Math::Add(canonical.back().value, entry.value);
                    } else {
                        canonical.back().value = canonical.back().value + entry.value;
                    }
                } else {
                    canonical.push_back(std::move(entry));
                }
            }

            if (canonical.size() > static_cast<size_t>((std::numeric_limits<Index>::max)())) {
                return std::unexpected(SparseError::IndexOverflow);
            }
            std::vector<Index> offsets(majorCount + 1);
            std::vector<Index> indices;
            std::vector<T> values;
            indices.reserve(canonical.size());
            values.reserve(canonical.size());
            size_t cursor = 0;
            for (size_t major = 0; major < majorCount; ++major) {
                offsets[major] = static_cast<Index>(cursor);
                while (cursor < canonical.size() && canonical[cursor].major == major) {
                    if (canonical[cursor].minor > static_cast<size_t>((std::numeric_limits<Index>::max)())) {
                        return std::unexpected(SparseError::IndexOverflow);
                    }
                    indices.push_back(static_cast<Index>(canonical[cursor].minor));
                    values.push_back(std::move(canonical[cursor].value));
                    ++cursor;
                }
            }
            offsets[majorCount] = static_cast<Index>(cursor);
            return CompressedMatrix<T, Index, Major>{rows_, columns_, std::move(offsets), std::move(indices),
                                                    std::move(values)};
        }

    private:
        size_t rows_;
        size_t columns_;
        std::vector<std::tuple<size_t, size_t, T>> entries_;
    };

    /**
     * @brief Canonical compressed sparse matrix with sorted minor indices in every major segment.
     * @details Explicit zero entries are retained as structural entries. Duplicate coordinates are merged by the
     * additive backend, but no value-based pruning is performed, so sparsity structure remains deterministic.
     */
    template<typename T, typename Index, SparseMajorAxis Major>
    class CompressedMatrix {
    public:
        using ValueType = T;
        using IndexType = Index;
        static constexpr SparseMajorAxis kMajorAxis = Major;

        /** @brief Return logical row count. */
        [[nodiscard]] constexpr size_t Rows() const noexcept { return rows_; }

        /** @brief Return logical column count. */
        [[nodiscard]] constexpr size_t Columns() const noexcept { return columns_; }

        /** @brief Return the number of stored canonical entries. */
        [[nodiscard]] constexpr size_t NonZeros() const noexcept { return values_.size(); }

        /** @brief Return compressed segment offsets. */
        [[nodiscard]] constexpr std::span<const Index> Offsets() const noexcept { return offsets_; }

        /** @brief Return sorted minor indices. */
        [[nodiscard]] constexpr std::span<const Index> Indices() const noexcept { return indices_; }

        /** @brief Return values aligned with @ref Indices. */
        [[nodiscard]] constexpr std::span<const T> Values() const noexcept { return values_; }

        /** @brief Read one logical entry, returning the additive identity for an absent coordinate. */
        [[nodiscard]] T At(size_t row, size_t column) const
            requires ZeroInitializable<T>
        {
            if (row >= rows_ || column >= columns_ ||
                (Major == SparseMajorAxis::Row ? column : row) >
                    static_cast<size_t>((std::numeric_limits<Index>::max)())) {
                return ZeroValue<T>();
            }
            if (indices_.empty()) {
                return ZeroValue<T>();
            }
            const size_t major = Major == SparseMajorAxis::Row ? row : column;
            const size_t minor = Major == SparseMajorAxis::Row ? column : row;
            const auto segment = Segment(major);
            const auto found = std::lower_bound(segment.first, segment.second, static_cast<Index>(minor));
            return found != segment.second && *found == minor
                       ? values_[static_cast<size_t>(found - indices_.data())]
                       : ZeroValue<T>();
        }

        /** @brief Convert to the opposite compressed major axis. */
        template<SparseMajorAxis OtherMajor>
            requires std::copy_constructible<T>
        [[nodiscard]] std::expected<CompressedMatrix<T, Index, OtherMajor>, SparseError> To() const {
            if constexpr (OtherMajor == Major) {
                return *this;
            }

            const size_t targetMajorCount = OtherMajor == SparseMajorAxis::Row ? rows_ : columns_;
            if (targetMajorCount == (std::numeric_limits<size_t>::max)() ||
                targetMajorCount > static_cast<size_t>((std::numeric_limits<Index>::max)())) {
                return std::unexpected(SparseError::IndexOverflow);
            }

            std::vector<Index> targetOffsets(targetMajorCount + 1, Index{0});
            for (const Index targetMajor : indices_) {
                ++targetOffsets[static_cast<size_t>(targetMajor) + 1];
            }
            for (size_t major = 0; major < targetMajorCount; ++major) {
                targetOffsets[major + 1] += targetOffsets[major];
            }

            std::vector<Index> targetIndices(values_.size());
            std::vector<size_t> sourcePositions(values_.size());
            std::vector<Index> targetCursors = targetOffsets;
            for (size_t sourceMajor = 0; sourceMajor < MajorCount(); ++sourceMajor) {
                for (Index sourceCursor = offsets_[sourceMajor]; sourceCursor < offsets_[sourceMajor + 1];
                     ++sourceCursor) {
                    const Index targetMajor = indices_[sourceCursor];
                    const size_t targetPosition = targetCursors[targetMajor]++;
                    targetIndices[targetPosition] = static_cast<Index>(sourceMajor);
                    sourcePositions[targetPosition] = sourceCursor;
                }
            }

            std::vector<T> targetValues;
            targetValues.reserve(values_.size());
            for (const size_t sourcePosition : sourcePositions) {
                targetValues.push_back(values_[sourcePosition]);
            }
            return CompressedMatrix<T, Index, OtherMajor>{rows_, columns_, std::move(targetOffsets),
                                                          std::move(targetIndices), std::move(targetValues)};
        }

        /** @brief Compute sparse matrix-vector multiplication in O(nnz) without allocating.
         * @return @ref SparseError::OverlappingBuffers when @p input and @p output share storage.
         */
        [[nodiscard]] std::expected<void, SparseError> SpMV(std::span<const T> input, std::span<T> output) const
            requires SparseDetail::SpmvValue<T>
        {
            if (input.size() != columns_ || output.size() != rows_) {
                return std::unexpected(SparseError::DimensionMismatch);
            }
            if (SparseDetail::BuffersOverlap(input, output)) {
                return std::unexpected(SparseError::OverlappingBuffers);
            }
            std::ranges::fill(output, ZeroValue<T>());
            AccumulateSpMV(input, output);
            return {};
        }

        /** @brief Compute sparse matrix-vector multiplication and return an owning result. */
        [[nodiscard]] std::expected<std::vector<T>, SparseError> SpMV(std::span<const T> input) const
            requires SparseDetail::SpmvValue<T>
        {
            if (input.size() != columns_) {
                return std::unexpected(SparseError::DimensionMismatch);
            }
            std::vector<T> output(rows_, ZeroValue<T>());
            AccumulateSpMV(input, output);
            return output;
        }

    private:
        void AccumulateSpMV(std::span<const T> input, std::span<T> output) const
            requires SparseDetail::SpmvValue<T>
        {
            if constexpr (Major == SparseMajorAxis::Row) {
                for (size_t row = 0; row < rows_; ++row) {
                    for (Index cursor = offsets_[row]; cursor < offsets_[row + 1]; ++cursor) {
                        output[row] = Sora::Math::Add(output[row],
                                                      Sora::Math::Mul(values_[cursor], input[indices_[cursor]]));
                    }
                }
            } else {
                for (size_t column = 0; column < columns_; ++column) {
                    for (Index cursor = offsets_[column]; cursor < offsets_[column + 1]; ++cursor) {
                        const size_t row = indices_[cursor];
                        output[row] = Sora::Math::Add(output[row],
                                                      Sora::Math::Mul(values_[cursor], input[column]));
                    }
                }
            }
        }

        friend class CoordinateMatrixBuilder<T, Index>;
        template<typename, typename, SparseMajorAxis>
        friend class CompressedMatrix;

        CompressedMatrix(size_t rows, size_t columns, std::vector<Index> offsets, std::vector<Index> indices,
                         std::vector<T> values)
            : rows_(rows), columns_(columns), offsets_(std::move(offsets)), indices_(std::move(indices)),
              values_(std::move(values)) {}

        [[nodiscard]] constexpr size_t MajorCount() const noexcept {
            return Major == SparseMajorAxis::Row ? rows_ : columns_;
        }

        [[nodiscard]] std::pair<const Index*, const Index*> Segment(size_t major) const noexcept {
            if (major >= MajorCount()) {
                return {indices_.data(), indices_.data()};
            }
            return {indices_.data() + offsets_[major], indices_.data() + offsets_[major + 1]};
        }

        size_t rows_;
        size_t columns_;
        std::vector<Index> offsets_;
        std::vector<Index> indices_;
        std::vector<T> values_;
    };

    /** @brief Zero-copy transpose view over a canonical compressed matrix. */
    template<typename T, typename Index, SparseMajorAxis Major>
    class SparseTransposeView {
    public:
        explicit constexpr SparseTransposeView(const CompressedMatrix<T, Index, Major>& source) noexcept
            : source_(source) {}

        SparseTransposeView(CompressedMatrix<T, Index, Major>&&) = delete;
        SparseTransposeView(const CompressedMatrix<T, Index, Major>&&) = delete;

        [[nodiscard]] constexpr size_t Rows() const noexcept { return source_.Columns(); }
        [[nodiscard]] constexpr size_t Columns() const noexcept { return source_.Rows(); }
        [[nodiscard]] constexpr size_t NonZeros() const noexcept { return source_.NonZeros(); }

        /** @brief Return the referenced source matrix without taking ownership. */
        [[nodiscard]] constexpr const CompressedMatrix<T, Index, Major>& Source() const noexcept { return source_; }

        [[nodiscard]] T At(size_t row, size_t column) const requires ZeroInitializable<T> {
            return source_.At(column, row);
        }

        /** @brief Compute multiplication by the transposed logical matrix in O(nnz) without allocating.
         * @return @ref SparseError::OverlappingBuffers when @p input and @p output share storage.
         */
        [[nodiscard]] std::expected<void, SparseError> SpMV(std::span<const T> input, std::span<T> output) const
            requires SparseDetail::SpmvValue<T>
        {
            if (input.size() != Columns() || output.size() != Rows()) {
                return std::unexpected(SparseError::DimensionMismatch);
            }
            if (SparseDetail::BuffersOverlap(input, output)) {
                return std::unexpected(SparseError::OverlappingBuffers);
            }
            std::ranges::fill(output, ZeroValue<T>());
            AccumulateSpMV(input, output);
            return {};
        }

        /** @brief Compute multiplication by the transposed logical matrix and return an owning result. */
        [[nodiscard]] std::expected<std::vector<T>, SparseError> SpMV(std::span<const T> input) const
            requires SparseDetail::SpmvValue<T>
        {
            if (input.size() != Columns()) {
                return std::unexpected(SparseError::DimensionMismatch);
            }
            std::vector<T> output(Rows(), ZeroValue<T>());
            AccumulateSpMV(input, output);
            return output;
        }

    private:
        void AccumulateSpMV(std::span<const T> input, std::span<T> output) const
            requires SparseDetail::SpmvValue<T>
        {
            const auto offsets = source_.Offsets();
            const auto indices = source_.Indices();
            const auto values = source_.Values();
            const size_t sourceMajorCount = Major == SparseMajorAxis::Row ? source_.Rows() : source_.Columns();
            for (size_t major = 0; major < sourceMajorCount; ++major) {
                for (Index cursor = offsets[major]; cursor < offsets[major + 1]; ++cursor) {
                    const size_t row = Major == SparseMajorAxis::Row ? major : indices[cursor];
                    const size_t column = Major == SparseMajorAxis::Row ? indices[cursor] : major;
                    output[column] = Sora::Math::Add(output[column], Sora::Math::Mul(values[cursor], input[row]));
                }
            }
        }

        const CompressedMatrix<T, Index, Major>& source_;
    };

    template<typename T, typename Index, SparseMajorAxis Major>
    SparseTransposeView(const CompressedMatrix<T, Index, Major>&) -> SparseTransposeView<T, Index, Major>;

} // namespace Sora::Math
