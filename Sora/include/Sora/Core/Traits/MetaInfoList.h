#pragma once

#include <meta>

namespace Sora::Meta {

    /**
     * @brief Structural, annotation-safe view over a static list of reflected C++ types.
     *
     * P3385 annotations require structural values. `std::span` is not structural, so Core stores a public
     * pointer/length pair. The initializer-list constructor promotes inline reflection lists into static storage,
     * making declarations such as `[[=Sora::$::Implements{^^IPosition, ^^IName}]]` lifetime-safe.
     */
    struct InfoList {
        const std::meta::info* first{}; /**< First reflected type in static storage, or null for empty. */
        std::size_t count{};            /**< Number of reflected types in @ref first. */

        /** @brief Construct an empty type-reflection list. */
        consteval InfoList() noexcept = default;

        /** @brief Adopt an existing static array of type reflections. */
        template<std::size_t N>
        consteval InfoList(const std::meta::info (&items)[N]) noexcept : first{items}, count{N} {}

        /** @brief Promote an inline braced list of type reflections into static storage. */
        consteval InfoList(std::initializer_list<std::meta::info> items)
            : first{std::define_static_array(std::vector<std::meta::info>(items.begin(), items.end())).data()},
              count{items.size()} {}

        /** @brief Return an iterator to the first reflected type. */
        [[nodiscard]] consteval const std::meta::info* begin() const noexcept { return first; }

        /** @brief Return an iterator one past the last reflected type. */
        [[nodiscard]] consteval const std::meta::info* end() const noexcept { return first + count; }

        /** @brief Return the number of reflected types. */
        [[nodiscard]] consteval std::size_t size() const noexcept { return count; }

        /** @brief Return whether the list is empty. */
        [[nodiscard]] consteval bool empty() const noexcept { return count == 0; }
    };

} // namespace Sora::Meta