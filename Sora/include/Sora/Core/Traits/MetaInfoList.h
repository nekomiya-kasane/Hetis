/**
 * @file MetaInfoList.h
 * @brief Structural compile-time sequences of C++ reflection entities.
 * @ingroup Core
 *
 * @details `InfoList` is the non-owning sequence representation used by Sora's reflection algorithms. Its elements
 * may reflect types, values, templates, functions, members, or namespaces. Storage must have static lifetime;
 * initializer-list construction and @ref Promote arrange that lifetime through `std::define_static_array`.
 *
 * Structural algorithms return new static sequences and never require their elements to be types. Type-only
 * validation belongs to the `TypeList` projection boundary.
 *
 * @par Example
 * @code{.cpp}
 * constexpr Sora::Meta::InfoList input{^^int, ^^double, ^^int};
 * constexpr auto extended = Sora::Meta::PushBack(input, ^^char);
 * constexpr auto unique = Sora::Meta::Unique(extended);
 * constexpr auto middle = Sora::Meta::Slice(unique, 1, 2);
 *
 * static_assert(Sora::Meta::Equal(middle, Sora::Meta::InfoList{^^double, ^^char}));
 * static_assert(Sora::Meta::IndexOf(input, ^^double) == 1);
 * static_assert(Sora::Meta::Count(input, ^^int) == 2);
 * @endcode
 */
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <meta>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace Sora::Meta {

    /**
     * @brief Structural, annotation-safe view over static reflected entities.
     *
     * @details The public pointer/count representation makes the type structural and therefore usable in P3385
     * annotations and as a non-type template argument. Empty lists may retain a one-past-end pointer into static
     * storage; operations therefore use @p count, not pointer nullness, to detect emptiness and avoid null arithmetic.
     * Copies are constant-time views and do not own storage.
     *
     * @invariant `count == 0 || first != nullptr`.
     * @invariant Every element in `[first, first + count)` has static storage duration.
     * @warning The representation is public to keep the type structural; direct initialization must preserve both
     * invariants. Prefer the constructors and @ref Promote.
     */
    struct InfoList {
        using value_type = std::meta::info;
        using const_iterator = const value_type*; /**< Iterator over static reflected entities. */
        static constexpr std::size_t npos = static_cast<std::size_t>(-1); /**< Missing-index sentinel. */

        const value_type* first{}; /**< First entity in static storage, or null for empty. */
        std::size_t count{};       /**< Number of entities in @p first. */

        /** @brief Construct an empty reflection sequence. */
        consteval InfoList() noexcept = default;

        /**
         * @brief Adopt an existing static array of reflected entities.
         * @tparam N Number of entities in @p items.
         * @param[in] items Static-storage array whose lifetime covers every use of this view.
         */
        template<std::size_t N>
        consteval InfoList(const value_type (&items)[N]) noexcept : first{items}, count{N} {}

        /**
         * @brief Adopt a static pointer/count pair.
         * @param[in] items First entity, or null only when @p length is zero.
         * @param[in] length Number of accessible entities.
         * @pre `length == 0 || items != nullptr`.
         */
        consteval InfoList(const value_type* items, std::size_t length) noexcept : first{items}, count{length} {}

        /**
         * @brief Promote an inline reflection list into static storage.
         * @param[in] items Entities copied into storage created by `std::define_static_array`.
         */
        consteval InfoList(std::initializer_list<value_type> items)
            : first{std::define_static_array(std::vector<value_type>(items.begin(), items.end())).data()},
              count{items.size()} {}

        /** @brief Return the first iterator. */
        [[nodiscard]] consteval const_iterator begin() const noexcept { return first; }

        /** @brief Return the one-past-end iterator without null-pointer arithmetic. */
        [[nodiscard]] consteval const_iterator end() const noexcept { return count == 0 ? first : first + count; }

        /** @brief Return the number of entities. */
        [[nodiscard]] consteval std::size_t size() const noexcept { return count; }

        /** @brief Return whether the sequence is empty. */
        [[nodiscard]] consteval bool empty() const noexcept { return count == 0; }

        /**
         * @brief Return the entity at @p index without bounds checking.
         * @pre `index < size()`.
         */
        [[nodiscard]] consteval const value_type& operator[](std::size_t index) const noexcept { return first[index]; }

        /**
         * @brief Return the entity at @p index with bounds checking.
         * @param[in] index Zero-based entity index.
         * @return Reference into static storage.
         * @throws std::out_of_range During translation when @p index is outside the list.
         */
        [[nodiscard]] consteval const value_type& at(std::size_t index) const {
            if (index >= count) {
                throw std::out_of_range("Sora::Meta::InfoList index out of range");
            }
            return first[index];
        }

        /**
         * @brief Return the first entity.
         * @pre `!empty()`.
         * @throws std::out_of_range During translation when the list is empty.
         */
        [[nodiscard]] consteval const value_type& front() const { return at(0); }

        /**
         * @brief Return the final entity.
         * @pre `!empty()`.
         * @throws std::out_of_range During translation when the list is empty.
         */
        [[nodiscard]] consteval const value_type& back() const { return at(count - 1); }

        /**
         * @brief Return a bounded sub-view without allocating storage.
         * @param[in] offset Zero-based first entity.
         * @param[in] length Number of entities in the view.
         * @return View into the same static storage.
         * @throws std::out_of_range During translation when the requested interval is outside this list.
         */
        [[nodiscard]] consteval InfoList subspan(std::size_t offset, std::size_t length) const {
            if (offset > count || length > count - offset) {
                throw std::out_of_range("Sora::Meta::InfoList subspan out of range");
            }
            return InfoList{count == 0 ? first : first + offset, length};
        }
    };

    /**
     * @brief Promote a sized compile-time reflection range into static storage.
     * @param[in] items Range copied by `std::define_static_array`.
     * @return Static view preserving source order; empty input returns the canonical empty view.
     */
    consteval InfoList Promote(auto&& items)
        requires std::ranges::sized_range<decltype(items)> &&
                 std::same_as<std::remove_cv_t<std::ranges::range_value_t<decltype(items)>>, std::meta::info>
    {
        if (std::ranges::empty(items)) {
            return {};
        }
        auto storage = std::define_static_array(items);
        return InfoList{storage.data(), std::ranges::size(items)};
    }

    /**
     * @brief Concatenate reflection sequences in source order.
     * @tparam Lists One or more `InfoList` types; the zero-pack produces an empty list.
     * @param[in] lists Sequences to concatenate.
     * @return Newly promoted sequence containing every input entity in order.
     * @par Complexity Linear in the total number of input entities.
     */
    template<typename... Lists>
    consteval InfoList Concat(const Lists&... lists)
        requires((std::same_as<std::remove_cvref_t<Lists>, InfoList> && ...))
    {
        std::vector<std::meta::info> result;
        result.reserve((lists.size() + ... + std::size_t{0}));
        const auto append = [&result](const InfoList& list) {
            if (!list.empty()) {
                result.insert(result.end(), list.begin(), list.end());
            }
        };
        (append(lists), ...);
        return result.empty() ? InfoList{} : Promote(result);
    }

    /**
     * @brief Return a sequence with duplicate entities removed, retaining first occurrences.
     * @param[in] list Source sequence.
     * @return Stable unique sequence.
     * @par Complexity Quadratic comparisons; reflection lists are expected to remain small.
     */
    consteval InfoList Unique(const InfoList& list) {
        if (list.empty()) {
            return {};
        }
        std::vector<std::meta::info> result;
        for (const auto entity : list) {
            if (std::ranges::find(result, entity) == result.end()) {
                result.push_back(entity);
            }
        }
        return Promote(result);
    }

    /**
     * @brief Return a sequence in reverse order.
     * @param[in] list Source sequence.
     * @return Newly promoted sequence with reversed order; empty input remains empty.
     * @par Complexity Linear in the sequence length.
     */
    consteval InfoList Reverse(const InfoList& list) {
        if (list.empty()) {
            return {};
        }
        std::vector<std::meta::info> result(list.begin(), list.end());
        std::ranges::reverse(result);
        return Promote(result);
    }

    /**
     * @brief Return an exact bounded slice of a reflection sequence.
     * @param[in] list Source sequence.
     * @param[in] offset Zero-based first entity.
     * @param[in] length Number of entities selected.
     * @return Non-owning view of the selected interval.
     * @throws std::out_of_range During translation when the interval is outside @p list.
     * @par Complexity Constant.
     */
    consteval InfoList Slice(const InfoList& list, std::size_t offset, std::size_t length) {
        return list.subspan(offset, length);
    }

    /**
     * @brief Return the first @p count entities, clamping @p count to the sequence length.
     * @param[in] list Source sequence.
     * @param[in] count Maximum number of entities to retain.
     * @return Non-owning prefix view.
     */
    consteval InfoList Take(const InfoList& list, std::size_t count) {
        return Slice(list, 0, std::min(count, list.size()));
    }

    /**
     * @brief Return all entities after the first @p count entities, clamping @p count to the sequence length.
     * @param[in] list Source sequence.
     * @param[in] count Number of leading entities to discard.
     * @return Non-owning suffix view.
     */
    consteval InfoList Drop(const InfoList& list, std::size_t count) {
        const auto offset = std::min(count, list.size());
        return Slice(list, offset, list.size() - offset);
    }

    /**
     * @brief Return @p list with @p value prepended.
     * @param[in] list Source sequence.
     * @param[in] value Entity inserted at the front.
     * @return Newly promoted sequence.
     */
    consteval InfoList PushFront(const InfoList& list, std::meta::info value) {
        return Concat(InfoList{value}, list);
    }

    /**
     * @brief Return @p list with @p value appended.
     * @param[in] list Source sequence.
     * @param[in] value Entity inserted at the back.
     * @return Newly promoted sequence.
     */
    consteval InfoList PushBack(const InfoList& list, std::meta::info value) {
        return Concat(list, InfoList{value});
    }

    /**
     * @brief Remove the first entity, returning empty when @p list is empty.
     * @param[in] list Source sequence.
     * @return Promoted sequence without its first entity.
     */
    consteval InfoList PopFront(const InfoList& list) {
        return Drop(list, 1);
    }

    /**
     * @brief Remove the final entity, returning empty when @p list is empty.
     * @param[in] list Source sequence.
     * @return Promoted sequence without its final entity.
     */
    consteval InfoList PopBack(const InfoList& list) {
        return Take(list, list.empty() ? 0 : list.size() - 1);
    }

    /**
     * @brief Replace every occurrence of @p oldValue with @p newValue.
     * @param[in] list Source sequence.
     * @param[in] oldValue Entity to replace.
     * @param[in] newValue Replacement entity.
     * @return Newly promoted sequence.
     * @par Complexity Linear in list length.
     */
    consteval InfoList Replace(const InfoList& list, std::meta::info oldValue, std::meta::info newValue) {
        if (list.empty()) {
            return {};
        }
        std::vector<std::meta::info> result(list.begin(), list.end());
        std::ranges::replace(result, oldValue, newValue);
        return Promote(result);
    }

    /**
     * @brief Remove every occurrence of @p value.
     * @param[in] list Source sequence.
     * @param[in] value Entity to remove.
     * @return Newly promoted sequence.
     * @par Complexity Linear in list length.
     */
    consteval InfoList Remove(const InfoList& list, std::meta::info value) {
        if (list.empty()) {
            return {};
        }
        std::vector<std::meta::info> result(list.begin(), list.end());
        std::erase(result, value);
        return Promote(result);
    }

    /**
     * @brief Return whether @p needle occurs in @p list.
     * @param[in] list Sequence to inspect.
     * @param[in] needle Entity to find.
     * @return `true` when at least one equal entity is present.
     * @par Complexity Linear in the inspected prefix.
     */
    consteval bool Contains(const InfoList& list, std::meta::info needle) {
        return std::ranges::find(list, needle) != list.end();
    }

    /**
     * @brief Return the first index of @p needle, or @ref InfoList::npos.
     * @param[in] list Sequence to inspect.
     * @param[in] needle Entity to find.
     * @return Zero-based index, or @ref InfoList::npos when absent.
     */
    consteval std::size_t IndexOf(const InfoList& list, std::meta::info needle) {
        std::size_t index = 0;
        for (const auto entity : list) {
            if (entity == needle) {
                return index;
            }
            ++index;
        }
        return InfoList::npos;
    }

    /**
     * @brief Return the number of occurrences of @p needle.
     * @param[in] list Sequence to inspect.
     * @param[in] needle Entity to count.
     * @return Number of equal entities.
     * @par Complexity Linear in list length.
     */
    consteval std::size_t Count(const InfoList& list, std::meta::info needle) {
        return static_cast<std::size_t>(std::ranges::count(list, needle));
    }

    /**
     * @brief Return whether every entity occurs exactly once.
     * @param[in] list Sequence to inspect.
     * @return `true` when @p list contains no duplicate entity.
     * @par Complexity Quadratic comparisons.
     */
    consteval bool IsUnique(const InfoList& list) {
        return Unique(list).size() == list.size();
    }

    /**
     * @brief Return whether two sequences are equal in size and order.
     * @param[in] left First sequence.
     * @param[in] right Second sequence.
     * @return `true` when both sequences have equal length and element identity.
     * @par Complexity Linear in sequence length.
     */
    consteval bool Equal(const InfoList& left, const InfoList& right) {
        return left.size() == right.size() && (left.empty() || std::ranges::equal(left, right));
    }

} // namespace Sora::Meta
