/**
 * @file Association.h
 * @brief Compile-time keyed heterogeneous associations.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/FixedString.h>

#include <array>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora {

    /**
     * @brief Fixed-shape runtime key-value pair.
     * @details The key and value types are part of the pair type and are stored by value.
     * @tparam K Key type.
     * @tparam V Value type.
     */
    template<typename K, typename V>
    struct Rule {
        K key;
        V value;
    };

    template<typename K, typename V>
    Rule(K, V) -> Rule<K, V>;

    template<auto Key, typename V>
    struct Field;

    /**
     * @brief Compile-time key tag used to construct and access an association field.
     * @details Assigning a value to the tag creates a @ref Field whose key is encoded in its type, enabling compile-
     * time lookup without string hashing.
     * @tparam Key Fixed compile-time key value.
     * @code{.cpp}
     * using Sora::operator""_k;
     * auto field = "id"_k = 3;
     * // field.key == "id", field.value == 3
     * @endcode
     */
    template<auto Key>
    struct KeyTag {
        static constexpr auto key = Key;

        template<typename V>
        [[nodiscard]] constexpr auto operator=(V&& value) const {
            return Field<Key, std::remove_cvref_t<V>>{std::forward<V>(value)};
        }
    };

    /**
     * @brief Construct a compile-time key tag from a string literal of at most 256 characters.
     * @tparam Key String literal encoded as a fixed compile-time value.
     * @return A key tag suitable for field construction and association lookup.
     * @code{.cpp}
     * using Sora::operator""_k;
     * auto key = "name"_k;
     * auto field = key = std::string{"Ada"};
     * // field.value == "Ada"
     * @endcode
     */
    template<FixedString<256> Key>
    [[nodiscard]] consteval KeyTag<Key> operator""_k() noexcept {
        return {};
    }

    /**
     * @brief One compile-time keyed value.
     * @details The key is encoded as a non-type template parameter and the value is owned by the field.
     * @tparam Key Compile-time key value.
     * @tparam V Stored value type.
     */
    template<auto Key, typename V>
    struct Field {
        static constexpr auto key = Key;
        V value;
    };

    namespace Detail {

        template<typename... Fields>
        consteval bool UniqueFieldKeys() {
            if constexpr (sizeof...(Fields) < 2) {
                return true;
            } else {
                constexpr std::array keys{Fields::key.view()...};
                for (size_t i = 0; i < keys.size(); ++i) {
                    for (size_t j = i + 1; j < keys.size(); ++j) {
                        if (keys[i] == keys[j]) {
                            return false;
                        }
                    }
                }
                return true;
            }
        }

    } // namespace Detail

    /**
     * @brief Heterogeneous association with unique compile-time string keys.
     * @details Fields are stored by value in declaration order. Duplicate keys are rejected during constraint
     * checking; lookup is compile-time indexed and performs no runtime string comparison.
     * @tparam Fields Compile-time keyed fields with pairwise-unique keys.
     * @code{.cpp}
     * using Sora::operator""_k;
     * auto input = Sora::Assoc{"id"_k = 3, "name"_k = std::string{"Ada"}};
     * auto output = input["name"_k]; // "Ada"
     * @endcode
     */
    template<typename... Fields>
        requires(Detail::UniqueFieldKeys<Fields...>())
    struct Assoc {
        std::tuple<Fields...> fields;

        /**
         * @brief Construct an association by taking ownership of its fields.
         * @param[in] values Keyed values stored in declaration order.
         */
        constexpr explicit Assoc(Fields... values) noexcept((std::is_nothrow_move_constructible_v<Fields> && ...))
            : fields(std::move(values)...) {}

        /** @brief Return the compile-time tuple index associated with @p Key, or @c size_t(-1) when absent. */
        template<auto Key>
        [[nodiscard]] static consteval size_t IndexOfKey() noexcept {
            if constexpr (sizeof...(Fields) == 0) {
                return static_cast<size_t>(-1);
            } else {
                constexpr std::array matches{(Fields::key == Key)...};
                for (size_t i = 0; i < matches.size(); ++i) {
                    if (matches[i]) {
                        return i;
                    }
                }
                return static_cast<size_t>(-1);
            }
        }

        /** @brief Test at compile time whether @p Key is present. */
        template<auto Key>
        [[nodiscard]] static consteval bool Contains() noexcept {
            return IndexOfKey<Key>() != static_cast<size_t>(-1);
        }

        /**
         * @brief Access the field value associated with @p Key.
         * @details The returned cv-ref category follows the association object. Lookup is compile-time indexed and
         * therefore does not inspect key strings at runtime.
         * @tparam Key Compile-time key value.
         * @return The stored value for @p Key.
         * @code{.cpp}
         * using Sora::operator""_k;
         * auto input = Sora::Assoc{"id"_k = 3};
         * constexpr auto key = "id"_k;
         * auto output = input.Get<key.key>(); // 3
         * @endcode
         */
        template<auto Key>
        [[nodiscard]] constexpr decltype(auto) Get(this auto&& self) noexcept
            requires(Contains<Key>())
        {
            constexpr size_t index = IndexOfKey<Key>();
            return (std::get<index>(std::forward<decltype(self)>(self).fields).value);
        }

        /**
         * @brief Access the field value using a compile-time key tag.
         * @details This is the concise indexing form of @ref Get and preserves the association object's cv-ref
         * category.
         * @tparam Key Compile-time key value.
         * @return The stored value selected by the key tag.
         * @code{.cpp}
         * using Sora::operator""_k;
         * auto input = Sora::Assoc{"id"_k = 3};
         * auto output = input["id"_k]; // 3
         * @endcode
         */
        template<auto Key>
        [[nodiscard]] constexpr decltype(auto) operator[](this auto&& self, KeyTag<Key>) noexcept
            requires(Contains<Key>())
        {
            return std::forward<decltype(self)>(self).template Get<Key>();
        }
    };

    template<typename... Fields>
    Assoc(Fields...) -> Assoc<Fields...>;

} // namespace Sora
