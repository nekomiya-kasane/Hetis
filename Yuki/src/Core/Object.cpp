/**
 * @file Object.cpp
 * @brief Root BaseUnknown metaclass implementation.
 * @ingroup Core
 */
#include <Yuki/Core/Object.h>

#include <cassert>

#include <array>
#include <span>
#include <vector>

namespace Yuki {

    namespace {

        constexpr std::array<ProviderEntry, 0> kNoProvides{};
        constexpr MetaClass kBaseMeta{TypeOfClass::BaseUnknown, IidOf<BaseUnknown>(), "Yuki::BaseUnknown",
                                      std::span<const ProviderEntry>{kNoProvides}};

    } // namespace

    const MetaClass& BaseUnknownMetaClass() noexcept {
        return kBaseMeta;
    }

} // namespace Yuki
