#pragma once

/**
 * @file Schema.h
 * @brief Canonical schema declarations for Sora command-line programs.
 * @ingroup Core
 */

#include <meta>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <Sora/Core/ADT/FixedCapacityVector.h>
#include <Sora/Core/Hash.h>
#include <Sora/Core/CLI/Descriptions.h>

namespace Sora {

    namespace CLI {

        template<typename Root>
        class SchemaBuilder;

        /** @brief Canonical schema slice produced by consteval normalization. */
        struct NormalizedSchema {
            NameId programName = kInvalidNameId;
            Policy policy = Policy::None;

            FixedCapacityVector<CommandDesc> commands = {};
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
            concept ProgramRoot = requires(SchemaBuilder<T>& builder) { T::BuildSchema(builder); };

        } // namespace Concept

        template<typename Root>
        class SchemaBuilder {
            NormalizedSchema schema_ = {};

        public:
            consteval auto Name(std::string_view name) noexcept -> SchemaBuilder& {
                schema_.programName = QueryNameTable(name);
                return *this;
            }

            consteval auto Policy(Policy value) noexcept -> SchemaBuilder& {
                schema_.policy = value;
                return *this;
            }

            template<typename Cmd>
            consteval auto Command(std::string_view name = std::meta::identifier_of(^^Cmd)) -> SchemaBuilder& {
                CommandDesc desc{
                    .name = QueryNameTable(name),
                    .commandId = static_cast<CommandId>(schema_.commands.size()),
                };

                for (const auto& cmd : schema_.commands) {
                    if (cmd.name == desc.name) {
                        throw "Duplicate command name.";
                    }
                }

                schema_.commands.push_back(desc);
                return *this;
            }

            [[nodiscard]] consteval auto Seal() const noexcept -> NormalizedSchema {
                return schema_;
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
