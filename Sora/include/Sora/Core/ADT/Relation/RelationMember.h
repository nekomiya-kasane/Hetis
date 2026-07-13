#pragma once

#include <type_traits>
#include <concepts>
#include <memory>

namespace Sora::Rel {

    template<class Family>
    class RelationRef;

    template<class Family>
    class RelationFacade;

    template<class Family>
    struct relation_member_slot {
        relation_node_slot_t<Family> slot_{};
    };

    template<class Derived, class... Families>
    class RelationMembers : private relation_member_slot<Families>... {
    public:
        RelationMembers() = default;
        RelationMembers(RelationMembers const&) = delete;
        RelationMembers(RelationMembers&&) = delete;
        RelationMembers& operator=(RelationMembers const&) = delete;
        RelationMembers& operator=(RelationMembers&&) = delete;

        template<class Family>
            requires(std::same_as<Family, Families> || ...)
        [[nodiscard]] relation_node_slot_t<Family>& slot_for_runtime() noexcept {
            return static_cast<relation_member_slot<Family>&>(*this).slot_;
        }

        template<class Family>
            requires((std::same_as<Family, Families> || ...) && !PairSetRelation<Family>)
        [[nodiscard]] RelationRef<Family> rel() noexcept {
            auto& object = static_cast<Derived&>(*this);
            auto relation = object.domain().relations().template get<Family>();
            return RelationRef<Family>{relation.storage(), std::addressof(slot_for_runtime<Family>())};
        }
    };

} // namespace Sora::Rel