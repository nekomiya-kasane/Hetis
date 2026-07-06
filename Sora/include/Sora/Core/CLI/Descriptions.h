#pragma once

#include <cstdint>

namespace Sora {

    namespace CLI {

        enum class OptionKind : uint8_t {
            Flag,
            Switch = Flag, /**< Boolean flag option. */
            Parameter,     /**< Option with a single value. */
        };

        enum class OptionCardinality : uint8_t {
            None,
            ExactlyOne,
            AtMostOne,
            Any,
            AtLeastOne,
        };

        using FullNameId = uint32_t;

        /** @brief Leaf option description */
        struct OptionDesc {
            OptionKind kind;
            OptionCardinality cardinality;

            uint32_t ownerCommandId;
            uint32_t fieldId;
            uint32_t groupId;
            uint32_t presenceBit;

            char shortName;
            FullNameId fullNameId;

            // TODO: BindFunc
            // TODO: ParseValueFunc
        };

    } // namespace CLI

} // namespace Sora