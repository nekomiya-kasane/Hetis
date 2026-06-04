/**
 * @file SOA.h
 * @brief Reflection-driven Structure-of-Arrays (SoA) container with compile-time layout generation.
 *
 * Transforms any aggregate ("AoS struct") into a cache-friendly SoA container
 * via P2996 reflection, P3289 consteval blocks, and P1306 expansion statements. Zero
 * runtime overhead: the SoA layout is generated entirely at compile time, and per-element
 * access compiles to a single pointer dereference + offset.
 *
 * Usage:
 * @code
 * struct Particle {
 *     vec3  position;
 *     vec3  velocity;
 *     float lifetime;
 *     [[=SoA::Skip{}]] uint32_t debugId; // excluded from SoA
 * };
 *
 * SoA::Array<Particle> particles;
 * particles.Resize(1024);
 * particles.Get<"position">(i) = {1.f, 2.f, 3.f};
 * particles.Get<"velocity">(i) = {0.f, 0.f, -9.8f};
 *
 * // Dense iteration over a single field (cache-linear):
 * for (auto& pos : particles.Field<"position">()) { ... }
 *
 * // ForEach over all fields of element i (tuple-like):
 * particles.ForEach(i, [](auto name, auto& value) { ... });
 * @endcode
 *
 * Annotations:
 * - `[[=SoA::Skip{}]]` - exclude a member from SoA layout.
 * - `[[=SoA::Align{N}]]` - override per-array alignment for a field.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/FixedString.h"
#include "Mashiro/Core/Meta.h"
#include "Mashiro/Core/TypeTraits.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <meta>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Mashiro::SoA {

    using Mashiro::FixedString;

    // =========================================================================
    // Annotations
    // =========================================================================

    /// @brief Exclude a member from SoA layout generation.
    struct Skip {
        constexpr bool operator==(const Skip&) const = default;
    };

    /// @brief Override per-field array alignment (bytes, must be power of 2).
    struct Align {
        size_t value = 64;
        constexpr bool operator==(const Align&) const = default;
    };

    // =========================================================================
    // Compile-time field descriptors
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Check if a member carries the Skip annotation.
        consteval bool IsSkipped(std::meta::info member) {
            return std::meta::annotations_of(member, ^^Skip).size() > 0;
        }

        /// @brief Get alignment override for a member, or default cache-line alignment.
        consteval size_t AlignmentFor(std::meta::info member) {
            auto annots = std::meta::annotations_of(member, ^^Align);
            if (annots.size() > 0)
                return std::meta::extract<Align>(annots[0]).value;
            return 64; // cache-line default
        }

        /// @brief Filtered list of non-skipped data members for SoA generation.
        template <typename T>
        consteval auto SoaMembers() {
            std::vector<std::meta::info> result;
            for (auto m : std::meta::nonstatic_data_members_of(^^T,
                     std::meta::access_context::unchecked())) {
                if (!IsSkipped(m))
                    result.push_back(m);
            }
            return result;
        }

        /// @brief Number of SoA-eligible fields in T.
        template <typename T>
        inline constexpr size_t kFieldCount = SoaMembers<T>().size();

        /// @brief Static array of SoA-eligible member infos for template for.
        template <typename T>
        inline constexpr auto kFields = std::define_static_array(SoaMembers<T>());

        /// @brief Aligned allocation (64B default for cache-line friendliness).
        inline void* AllocAligned(size_t bytes, size_t align) {
            if (bytes == 0) return nullptr;
#ifdef _WIN32
            return _aligned_malloc(bytes, align);
#else
            return std::aligned_alloc(align, (bytes + align - 1) & ~(align - 1));
#endif
        }

        inline void FreeAligned(void* ptr) {
            if (!ptr) return;
#ifdef _WIN32
            _aligned_free(ptr);
#else
            std::free(ptr);
#endif
        }

        /// @brief Build data_member_spec vector for the SoA aggregate type.
        template <typename T, size_t N>
        consteval auto BuildSoASpecs() {
            auto members = SoaMembers<T>();
            std::vector<std::meta::info> specs;
            for (auto m : members) {
                auto arrayType = std::meta::substitute(^^std::array,
                    {std::meta::type_of(m), std::meta::reflect_constant(N)});
                specs.push_back(std::meta::data_member_spec(
                    arrayType, {.name = std::meta::identifier_of(m)}));
            }
            return specs;
        }

    } // namespace Detail
    /** @endcond */

    // =========================================================================
    // ToSoA<T, N> -- compile-time SoA type generation via define_aggregate
    // =========================================================================

    /**
     * @brief Incomplete struct completed by `define_aggregate` at instantiation time.
     *
     * Given `struct Particle { vec3 pos; float life; };`, `SoAType<Particle, 1024>` becomes:
     * @code
     * struct SoAType<Particle, 1024> {
     *     std::array<vec3,  1024> pos;
     *     std::array<float, 1024> life;
     * };
     * @endcode
     *
     * Members annotated with `[[=SoA::Skip{}]]` are excluded.
     */
    template <typename T, size_t N>
        requires std::is_aggregate_v<T>
    struct SoAType;

    /**
     * @brief Complete a `SoAType<T, N>` aggregate via `define_aggregate`.
     *
     * **Must be called from a `consteval { }` block at namespace scope.**
     * After the call, `SoAType<T, N>` is a complete aggregate where each
     * non-skipped member of `T` is replaced by `std::array<MemberType, N>`.
     *
     * @tparam T Source aggregate type.
     * @tparam N Number of elements per array.
     *
     * @code
     * struct Particle { vec3 pos; vec3 vel; float life; };
     *
     * consteval { SoA::Define<Particle, 1024>(); }
     *
     * // Now SoAType<Particle, 1024> is complete:
     * // struct { std::array<vec3,1024> pos; std::array<vec3,1024> vel; std::array<float,1024> life; };
     * SoA::SoAType<Particle, 1024> data{};
     * data.pos[0] = {1, 2, 3};
     * @endcode
     */
    template <typename T, size_t N>
    consteval void Define() {
        std::meta::define_aggregate(^^SoAType<T, N>, Detail::BuildSoASpecs<T, N>());
    }

    // =========================================================================
    // SoA::Array<T> -- the runtime container
    // =========================================================================

    /**
     * @brief Structure-of-Arrays container generated from aggregate T via reflection.
     *
     * Stores each non-skipped data member of T as a separate, cache-aligned array.
     * Provides named field access via compile-time string keys, dense iteration
     * over individual fields, and element-wise tuple-like access.
     *
     * @tparam T Source aggregate type ("AoS shape").
     */
    template <typename T>
        requires std::is_aggregate_v<T>
    class Array {
        static constexpr size_t kN = Detail::kFieldCount<T>;

        void* arrays_[kN]{};
        size_t size_ = 0;
        size_t capacity_ = 0;

    public:
        Array() = default;

        ~Array() { Clear(); DeallocArrays(); }

        Array(const Array&) = delete;
        Array& operator=(const Array&) = delete;

        Array(Array&& other) noexcept
            : size_(other.size_), capacity_(other.capacity_) {
            for (size_t i = 0; i < kN; ++i) {
                arrays_[i] = other.arrays_[i];
                other.arrays_[i] = nullptr;
            }
            other.size_ = 0;
            other.capacity_ = 0;
        }

        Array& operator=(Array&& other) noexcept {
            if (this != &other) {
                Clear();
                DeallocArrays();
                for (size_t i = 0; i < kN; ++i) {
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

        // -----------------------------------------------------------------
        // Capacity
        // -----------------------------------------------------------------

        [[nodiscard]] size_t Size() const noexcept { return size_; }
        [[nodiscard]] size_t Capacity() const noexcept { return capacity_; }
        [[nodiscard]] bool Empty() const noexcept { return size_ == 0; }

        /// @brief Reserve storage for at least `newCap` elements.
        void Reserve(size_t newCap) {
            if (newCap <= capacity_) return;
            ReallocArrays(newCap);
        }

        /// @brief Resize to `newSize` elements (default-constructs new, destroys excess).
        void Resize(size_t newSize) {
            if (newSize > capacity_)
                ReallocArrays(GrowCap(newSize));

            if (newSize > size_) {
                ConstructRange(size_, newSize);
            } else if (newSize < size_) {
                DestroyRange(newSize, size_);
            }
            size_ = newSize;
        }

        /// @brief Destroy all elements (does not free memory).
        void Clear() {
            DestroyRange(0, size_);
            size_ = 0;
        }

        // -----------------------------------------------------------------
        // Element access by compile-time field name
        // -----------------------------------------------------------------

        /// @brief Access field `Name` of element `index`.
        template <FixedString Name>
        [[nodiscard]] decltype(auto) Get(size_t index) {
            constexpr size_t fi = FieldIndex<Name>();
            using FT = FieldTypeAt<fi>;
            return static_cast<FT*>(arrays_[fi])[index];
        }

        template <FixedString Name>
        [[nodiscard]] decltype(auto) Get(size_t index) const {
            constexpr size_t fi = FieldIndex<Name>();
            using FT = FieldTypeAt<fi>;
            return static_cast<const FT*>(arrays_[fi])[index];
        }

        /// @brief Get a span over the entire field array (dense, cache-linear).
        template <FixedString Name>
        [[nodiscard]] auto Field() {
            constexpr size_t fi = FieldIndex<Name>();
            using FT = FieldTypeAt<fi>;
            return std::span<FT>{static_cast<FT*>(arrays_[fi]), size_};
        }

        template <FixedString Name>
        [[nodiscard]] auto Field() const {
            constexpr size_t fi = FieldIndex<Name>();
            using FT = FieldTypeAt<fi>;
            return std::span<const FT>{static_cast<const FT*>(arrays_[fi]), size_};
        }

        // -----------------------------------------------------------------
        // Push from AoS element
        // -----------------------------------------------------------------

        /// @brief Append a full AoS element, distributing its fields into separate arrays.
        void Push(const T& element) {
            if (size_ == capacity_)
                ReallocArrays(GrowCap(size_ + 1));
            CopyElementIn(size_, element);
            ++size_;
        }

        void Push(T&& element) {
            if (size_ == capacity_)
                ReallocArrays(GrowCap(size_ + 1));
            MoveElementIn(size_, std::move(element));
            ++size_;
        }

        // -----------------------------------------------------------------
        // Reconstruct AoS element
        // -----------------------------------------------------------------

        /// @brief Reconstruct the AoS element at `index` (gather from SoA fields).
        [[nodiscard]] T Gather(size_t index) const {
            T result{};
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                result.[:m:] = static_cast<const FT*>(arrays_[fi])[index];
            }
            return result;
        }

        // -----------------------------------------------------------------
        // Per-element field iteration
        // -----------------------------------------------------------------

        /// @brief Call `fn(name, value)` for each SoA field of element `index`.
        template <typename Fn>
        void ForEach(size_t index, Fn&& fn) {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                fn(std::string_view(std::meta::identifier_of(m)),
                   static_cast<FT*>(arrays_[fi])[index]);
            }
        }

        template <typename Fn>
        void ForEach(size_t index, Fn&& fn) const {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                fn(std::string_view(std::meta::identifier_of(m)),
                   static_cast<const FT*>(arrays_[fi])[index]);
            }
        }

        // -----------------------------------------------------------------
        // Swap-remove (O(1) unordered removal)
        // -----------------------------------------------------------------

        /// @brief Remove element at `index` by swapping with last. O(1).
        void SwapRemove(size_t index) {
            if (index >= size_) return;
            size_t last = size_ - 1;
            if (index != last) {
                template for (constexpr auto m : Detail::kFields<T>) {
                    constexpr size_t fi = FieldIndexOfMember<m>();
                    using FT = typename [:std::meta::type_of(m):];
                    auto* arr = static_cast<FT*>(arrays_[fi]);
                    arr[index] = std::move(arr[last]);
                }
            }
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                std::destroy_at(&static_cast<FT*>(arrays_[fi])[last]);
            }
            --size_;
        }

        // -----------------------------------------------------------------
        // Static queries
        // -----------------------------------------------------------------

        /// @brief Number of SoA fields.
        static constexpr size_t FieldCount = kN;

    private:
        template <FixedString Name>
        static consteval size_t FieldIndex() {
            for (size_t i = 0; i < Detail::kFields<T>.size(); ++i) {
                if (std::meta::identifier_of(Detail::kFields<T>[i]) ==
                    std::string_view(Name))
                    return i;
            }
            throw "SoA::Array: field name not found";
        }

        template <std::meta::info M>
        static consteval size_t FieldIndexOfMember() {
            for (size_t i = 0; i < Detail::kFields<T>.size(); ++i) {
                if (Detail::kFields<T>[i] == M)
                    return i;
            }
            return size_t(-1);
        }

        template <size_t I>
        using FieldTypeAt = typename [:std::meta::type_of(Detail::kFields<T>[I]):];

        size_t GrowCap(size_t required) const {
            size_t cap = capacity_ > 0 ? capacity_ : 8;
            while (cap < required) cap *= 2;
            return cap;
        }

        void ReallocArrays(size_t newCap) {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                constexpr size_t align = Detail::AlignmentFor(m);

                FT* newArr = static_cast<FT*>(Detail::AllocAligned(newCap * sizeof(FT), align));
                FT* oldArr = static_cast<FT*>(arrays_[fi]);

                for (size_t i = 0; i < size_; ++i) {
                    ::new (&newArr[i]) FT(std::move(oldArr[i]));
                    std::destroy_at(&oldArr[i]);
                }

                Detail::FreeAligned(oldArr);
                arrays_[fi] = newArr;
            }
            capacity_ = newCap;
        }

        void DeallocArrays() {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                Detail::FreeAligned(arrays_[fi]);
                arrays_[fi] = nullptr;
            }
            capacity_ = 0;
        }

        void ConstructRange(size_t from, size_t to) {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                auto* arr = static_cast<FT*>(arrays_[fi]);
                for (size_t i = from; i < to; ++i)
                    ::new (&arr[i]) FT{};
            }
        }

        void DestroyRange(size_t from, size_t to) {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                auto* arr = static_cast<FT*>(arrays_[fi]);
                for (size_t i = from; i < to; ++i)
                    std::destroy_at(&arr[i]);
            }
        }

        void CopyElementIn(size_t index, const T& element) {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                ::new (&static_cast<FT*>(arrays_[fi])[index]) FT(element.[:m:]);
            }
        }

        void MoveElementIn(size_t index, T&& element) {
            template for (constexpr auto m : Detail::kFields<T>) {
                constexpr size_t fi = FieldIndexOfMember<m>();
                using FT = typename [:std::meta::type_of(m):];
                ::new (&static_cast<FT*>(arrays_[fi])[index]) FT(std::move(element.[:m:]));
            }
        }
    };

} // namespace Mashiro::SoA
