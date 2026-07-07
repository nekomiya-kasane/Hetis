#pragma once

/**
 * @file Schema.h
 * @brief Canonical schema declarations for Sora command-line programs.
 * @ingroup Core
 */

#include <span>
#include <cstdint>
#include <string_view>
#include <utility>

#include <Sora/Core/CLI/Descriptions.h>
#include <Sora/Core/Hash.h>

namespace Sora {

    namespace CLI {

        template<typename Root>
        class SchemaBuilder;

        /** @brief Canonical schema slice produced by consteval normalization. */
        struct NormalizedSchema {
            NameId programName = kInvalidNameId;
            Policy policy = Policy::None;
            std::span<CommandDesc const> commands = {};
            std::span<OptionDesc const> options = {};
            std::span<OperandDesc const> operands = {};
        };

        [[nodiscard]] consteval auto QueryNameTable(std::string_view name) noexcept -> NameId {
            const auto hash = Sora::Hashing::Hash(name, Sora::Hashing::Fnv1a32{});
            return hash == kInvalidNameId ? 1u : static_cast<NameId>(hash);
        }

        namespace Concept {

            /** @brief Type that can be used as a command-line program root. */
            template<typename T>
            concept ProgramRoot = requires(SchemaBuilder<T>& builder) {
                T::BuildSchema(builder);
            };

        } // namespace Concept

        template<typename Root>
        class SchemaBuilder {
            NormalizedSchema schema_ = {};

        public:
            consteval auto Name(this auto&& self, std::string_view name) noexcept -> decltype(auto) {
                self.schema_.programName = QueryNameTable(name);
                return std::forward<decltype(self)>(self);
            }

            consteval auto Policy(this auto&& self, Policy value) noexcept -> decltype(auto) {
                self.schema_.policy = value;
                return std::forward<decltype(self)>(self);
            }

            [[nodiscard]] consteval auto Seal(this auto const& self) noexcept -> NormalizedSchema {
                return self.schema_;
            }
        };

    } // namespace CLI

    namespace Concept {

        inline namespace CLI {

            /** @brief Type that can be used as a command-line program root. */
            using Sora::CLI::Concept::ProgramRoot;

        } // namespace CLI

    } // namespace Concept

} // namespace Sora
