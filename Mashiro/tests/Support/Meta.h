/**
 * @file Meta.h
 * @brief Compile-time test metadata utilities — automatic tag derivation, typed assertions, etc.
 *
 * Core facility: `AUTO_TAG` resolves to a compile-time `const char*` of the form
 * `"[Category.TestName]"` derived from `__FILE__` (e.g. `Math/TypesTest.cpp` → `"[Math.Types]"`).
 * This eliminates manual tag maintenance when files are moved across directories.
 *
 * Additional facilities for test authors:
 * - `TEST(desc)` — convenience macro wrapping `TEST_CASE(desc, AUTO_TAG)`.
 * - `MSECTION(desc)` — `SECTION(desc)` (for symmetry and future extension).
 * - `constexpr` string manipulation utilities for building tags at compile time.
 * - `ApproxVec` / `ApproxMat` — component-wise Catch2 Approx matchers.
 * - `Benchmark` helpers (placeholder for future use).
 *
 * All utilities are header-only and designed for C++26 with COCA clang-p2996.
 *
 * @ingroup TestSupport
 */
#pragma once

#include <cstddef>
#include <meta>
#include <string_view>

#include "Mashiro/Core/FixedString.h"

#include <catch2/catch_test_macros.hpp>

// =============================================================================
// §1  Tag derivation from __FILE__ using Core/FixedString
// =============================================================================

namespace Mashiro::Testing {

    /**
     * @brief Derive a Catch2 tag string from a file path at compile time.
     *
     * Convention: `tests/{Category}/{TestName}Test.cpp` → `"[Category.TestName]"`
     *
     * Uses `Mashiro::FixedString` path operations for parsing.
     */
    template<size_t Cap = 128>
    consteval FixedString<Cap> DeriveTag(std::string_view filePath) {
        FixedString<Cap> path(filePath);
        auto category = path.parent_name();
        auto stem_str = path.stem();
        auto name = stem_str.strip_suffix("Test");

        FixedString<Cap> r;
        r.push_back('[');
        r.append(category.view());
        r.push_back('.');
        r.append(name.view());
        r.push_back(']');
        return r;
    }

    /**
     * @brief Derive a tag with an additional sub-tag appended.
     *
     * Example: file `Math/TypesTest.cpp` + sub `"Layout"` → `"[Math.Types][Layout]"`
     */
    template<size_t Cap = 128>
    consteval FixedString<Cap> DeriveTagWith(std::string_view filePath, std::string_view extra) {
        auto base = DeriveTag<Cap>(filePath);
        base.push_back('[');
        base.append(extra);
        base.push_back(']');
        return base;
    }

    /// @brief Compile-time category name of the current test file.
    consteval std::string_view FileCategory(std::string_view path) {
        return FixedString<256>(path).parent_name().view();
    }

    /// @brief Compile-time test name (stem minus "Test" suffix).
    consteval std::string_view FileTestName(std::string_view path) {
        return FixedString<256>(path).stem().strip_suffix("Test").view();
    }

    /// @brief Check if a test file belongs to a given category.
    consteval bool IsCategory(std::string_view path, std::string_view cat) {
        return FixedString<256>(path).parent_name().view() == cat;
    }

} // namespace Mashiro::Testing

// =============================================================================
// Macros — the public API for test authors
// =============================================================================

/**
 * @def AUTO_TAG
 * @brief Evaluates to a `const char*` tag string derived from the current file.
 *
 * Usage: `TEST_CASE("description", AUTO_TAG) { ... }`
 *
 * The tag is `"[Category.TestName]"` where Category is the parent directory name
 * and TestName is the filename stem with trailing "Test" stripped.
 */
#define AUTO_TAG \
    (::Mashiro::StaticStr<::Mashiro::Testing::DeriveTag(__FILE__)>())

/**
 * @def AUTO_TAG_WITH(extra)
 * @brief Like AUTO_TAG but appends an additional `[extra]` sub-tag.
 *
 * Usage: `TEST_CASE("description", AUTO_TAG_WITH("Layout")) { ... }`
 * Result: `"[Math.Types][Layout]"`
 */
#define AUTO_TAG_WITH(extra) \
    (::Mashiro::StaticStr<::Mashiro::Testing::DeriveTagWith(__FILE__, extra)>())

/**
 * @def TEST(desc)
 * @brief Shorthand for `TEST_CASE(desc, AUTO_TAG)`.
 */
#define TEST(...) TEST_CASE(__VA_ARGS__, AUTO_TAG)

/**
 * @def TEST_TAG(desc, extra)
 * @brief Shorthand for `TEST_CASE(desc, AUTO_TAG_WITH(extra))`.
 */
#define TEST_TAG(desc, extra) TEST_CASE(desc, AUTO_TAG_WITH(extra))

/**
 * @def TEST_METHOD(className, desc)
 * @brief Shorthand for `TEST_CASE_METHOD(className, desc, AUTO_TAG)`.
 */
#define TEST_METHOD(className, ...) TEST_CASE_METHOD(className, __VA_ARGS__, AUTO_TAG)
