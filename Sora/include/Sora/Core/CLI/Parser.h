#pragma once

/**
 * @file Parser.h
 * @brief Runtime token views and parse-result carriers for sealed Sora CLI programs.
 * @ingroup Core
 */

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <Sora/Core/CLI/Descriptions.h>

namespace Sora::CLI {

    /** @brief Non-owning view of command-line tokens. */
    struct ArgvView {
        std::span<std::string_view const> tokens{}; /**< Already materialised token views. */
        int argc = 0;                               /**< Raw argv count when @ref argv is used. */
        char const* const* argv = nullptr;          /**< Raw argv values, usually excluding executable name. */

        /** @brief Return the number of visible tokens. */
        [[nodiscard]] constexpr std::size_t Size() const noexcept {
            return tokens.empty() ? static_cast<std::size_t>(argc) : tokens.size();
        }

        /** @brief Return token @p index. */
        [[nodiscard]] constexpr std::string_view operator[](std::size_t index) const noexcept {
            if (!tokens.empty()) {
                return tokens[index];
            }
            return argv[index] == nullptr ? std::string_view{} : std::string_view{argv[index]};
        }
    };

    /** @brief Return a non-owning token view over @p argv, skipping the executable name. */
    [[nodiscard]] constexpr ArgvView ArgvFromMain(int argc, char const* const* argv) noexcept {
        if (argc <= 1 || argv == nullptr) {
            return {};
        }
        return ArgvView{.argc = argc - 1, .argv = argv + 1};
    }

    /** @brief Typed parse result carrying root-scope options and the selected command object. */
    template<typename Root, typename CommandVariant>
    struct ParseResult {
        using RootType = Root;
        using CommandVariantType = CommandVariant;

        Root root{};                              /**< Parsed root-scope state. */
        CommandVariant command{};                 /**< Selected command object, or monostate before selection. */
        CommandId commandId = kInvalidCommandId;  /**< Descriptor id of the selected command. */
        CommandId helpCommandId = kInvalidCommandId; /**< Command scope selected by built-in @c -h or @c --help. */

        /** @brief Return true when a concrete subcommand was selected. */
        [[nodiscard]] constexpr bool HasCommand() const noexcept { return commandId != kInvalidCommandId; }

        /** @brief Return true when parsing terminated with a built-in help request. */
        [[nodiscard]] constexpr bool HelpRequested() const noexcept { return helpCommandId != kInvalidCommandId; }

        /** @brief Return the parsed root object. */
        [[nodiscard]] constexpr Root& RootObject() noexcept { return root; }

        /** @brief Return the parsed root object. */
        [[nodiscard]] constexpr const Root& RootObject() const noexcept { return root; }

        /** @brief Return the selected command as @p T. */
        template<typename T>
        [[nodiscard]] constexpr T& CommandObject() {
            return std::get<T>(command);
        }

        /** @brief Return the selected command as @p T. */
        template<typename T>
        [[nodiscard]] constexpr const T& CommandObject() const {
            return std::get<T>(command);
        }
    };

    template<typename Root, typename CommandVariant>
    using ParseExpected = std::expected<ParseResult<Root, CommandVariant>, ParseError>;

} // namespace Sora::CLI
