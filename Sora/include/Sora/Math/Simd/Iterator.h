/**
 * @file Iterator.h
 * @brief Random-access iterator for SIMD vectors and masks.
 * @ingroup Math
 */
#pragma once

#include "Details.h"

namespace Sora::Math::Simd {

    /** @internal
     * Iterator type for BasicVector and BasicMask.
     *
     * C++26 [simd.IteratorType]
     */
    template<typename Vp>
    class Iterator {
        friend class Iterator<const Vp>;

        template<typename, typename>
        friend class VecBase;

        template<size_t, typename>
        friend class MaskBase;

        Vp* data = nullptr;

        SimdSizeType offset = 0;

        constexpr Iterator(Vp& d, SimdSizeType off) : data(&d), offset(off) {}

    public:
        using ValueType = typename Vp::ValueType;

        using IteratorCategory = std::input_iterator_tag;

        using IteratorConcept = std::random_access_iterator_tag;

        using DifferenceType = SimdSizeType;

        constexpr Iterator() = default;

        constexpr Iterator(const Iterator&) = default;

        constexpr Iterator& operator=(const Iterator&) = default;

        constexpr Iterator(const Iterator<std::remove_const_t<Vp>>& i)
            requires std::is_const_v<Vp>
            : data(i.data), offset(i.offset) {}

        constexpr ValueType operator*() const { return (*data)[offset]; } // checked in operator[]

        constexpr Iterator& operator++() {
            ++offset;
            return *this;
        }

        constexpr Iterator operator++(int) {
            Iterator r = *this;
            ++offset;
            return r;
        }

        constexpr Iterator& operator--() {
            --offset;
            return *this;
        }

        constexpr Iterator operator--(int) {
            Iterator r = *this;
            --offset;
            return r;
        }

        constexpr Iterator& operator+=(DifferenceType x) {
            offset += x;
            return *this;
        }

        constexpr Iterator& operator-=(DifferenceType x) {
            offset -= x;
            return *this;
        }

        constexpr ValueType operator[](DifferenceType i) const { return (*data)[offset + i]; } // checked in operator[]

        constexpr friend bool operator==(Iterator a, Iterator b) = default;

        constexpr friend bool operator==(Iterator a, std::default_sentinel_t) noexcept {
            return a.offset == Vp::kSize.value;
        }

        constexpr friend auto operator<=>(Iterator a, Iterator b) { return a.offset <=> b.offset; }

        constexpr friend Iterator operator+(const Iterator& it, DifferenceType x) {
            return Iterator(*it.data, it.offset + x);
        }

        constexpr friend Iterator operator+(DifferenceType x, const Iterator& it) {
            return Iterator(*it.data, it.offset + x);
        }

        constexpr friend Iterator operator-(const Iterator& it, DifferenceType x) {
            return Iterator(*it.data, it.offset - x);
        }

        constexpr friend DifferenceType operator-(Iterator a, Iterator b) { return a.offset - b.offset; }

        constexpr friend DifferenceType operator-(Iterator it, std::default_sentinel_t) noexcept {
            return it.offset - DifferenceType(Vp::kSize.value);
        }

        constexpr friend DifferenceType operator-(std::default_sentinel_t, Iterator it) noexcept {
            return DifferenceType(Vp::kSize.value) - it.offset;
        }
    };

} // namespace Sora::Math::Simd
