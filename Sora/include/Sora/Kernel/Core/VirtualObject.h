/**
 * @file VirtualObject.h
 * @brief String-named virtual COM nucleus objects for extension-only closures.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/FixedString.h"
#include "Sora/Core/Hash.h"
#include "Sora/Kernel/Core/BaseObject.h"

#include <algorithm>
#include <memory>
#include <string_view>

namespace Sora::Kernel {

    using VirtualObjectName = Sora::FixedString<128>;

    namespace Detail {

        consteval bool IsVirtualObjectNameValid(VirtualObjectName name) {
            return !name.empty() && std::ranges::all_of(name, [](char c) { return c >= 0x20 && c <= 0x7e; });
        }

    } // namespace Detail

    /** @brief Virtual implementation class identified by a stable string name. */
    template<VirtualObjectName Name>
        requires(Detail::IsVirtualObjectNameValid(Name))
    class [[= Sora::Kernel::$::Implementation]] VirtualObject : public BaseUnknown {
    public:
        using Self = VirtualObject<Name>;

        static constexpr auto kVirtualClassName = Name;

        [[nodiscard]] static Iid GetIidStatic() noexcept { return Traits::IidOf<Self>; }

        [[nodiscard]] Iid GetIid() const noexcept override { return Self::GetIidStatic(); }

        [[nodiscard]] static std::shared_ptr<const MetaClass> GetMetaStatic() noexcept {
            return MetaClass::Query<Self>();
        }

        [[nodiscard]] std::shared_ptr<const MetaClass> GetMeta() const noexcept override {
            return Self::GetMetaStatic();
        }

        [[nodiscard]] static std::string_view GetClassNameStatic() noexcept { return kVirtualClassName.view(); }

        [[nodiscard]] std::string_view GetClassName() const noexcept override { return Self::GetClassNameStatic(); }

        [[nodiscard]] static TypeOfClass GetRoleStatic() noexcept { return TypeOfClass::Implementation; }

        [[nodiscard]] TypeOfClass GetRole() const noexcept override { return Self::GetRoleStatic(); }
    };
    static_assert(Concept::VirtualObjectClass<VirtualObject<"Test">>);

    /** @brief Short spelling for string-named virtual implementation classes. */
    template<VirtualObjectName Name>
    using Virtual = VirtualObject<Name>;

    namespace Traits {

        template<VirtualObjectName Name>
        inline constexpr Iid IidOf<VirtualObject<Name>> = [] consteval {
            if (!Detail::IsVirtualObjectNameValid(Name)) {
                throw std::define_static_string(
                    "Sora::Kernel::VirtualObject name must be non-empty printable ASCII and at most 128 bytes.");
            }

            constexpr auto qualifiedName = Sora::FixedString{"Sora.Kernel.VirtualObject:"} + Name;
            return Iid{Sora::Hashing::Hash(qualifiedName.view(), Sora::Hashing::Fnv1a128{})};
        }();

    } // namespace Traits

} // namespace Sora::Kernel
