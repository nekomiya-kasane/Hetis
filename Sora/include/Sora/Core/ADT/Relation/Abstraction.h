#pragma once

#include <cstdint>
#include <limits>
#include <concepts>

#include <Sora/Core/Traits/TypeTraits.h>

namespace Sora::Rel {

    enum class RelationShape : uint8_t {
        Tree,    // single parent, acyclic, ordered children
        Forest,  // multiple roots, each node still has single parent
        Dag,     // multiple parents, acyclic
        Graph,   // cycles may exist
        PairSet, // unordered relationship pairs
        Custom   // shape semantics provided by Spec::shape_tag
    };

    inline namespace Tags {

        struct TreeShapeTag {};
        struct ForestShapeTag {};
        struct DagShapeTag {};
        struct GraphShapeTag {};
        struct PairSetShapeTag {};

    } // namespace Tags

    enum class RelationIntent : uint8_t {
        Ownership,
        Visual,
        Interaction,
        Command,
        Focus,
        Dependency,
        Observation,
        Custom
    };

    struct RelationId {
        uint32_t value = 0;
        friend constexpr bool operator==(RelationId, RelationId) = default;
    };

    enum class Cardinality : uint8_t { SingleParent, MultiParent };

    enum class ChildOrder : uint8_t { Unordered, StableInsertion, ExplicitRank };

    enum class MutationClass : uint8_t { Static, LowChurn, HighChurn };

    enum class PersistenceClass : uint8_t { Ephemeral, Replayable, Serializable };

    enum class ReplayMode : uint8_t { None, ObjectIdMutationLog, StructuralSnapshot };

    enum class LifetimeClass : uint8_t { StaticProgram, DocumentScoped, ActivationScoped, FrameScoped };

    enum class ConcurrencyClass : uint8_t { SingleThreadConfined, SnapshotReaders, MultiWriterTransactional };

    enum class AccessPattern : uint8_t { PointLookup, RouteTraversal, BatchTraversal };

    enum class HotnessClass : uint8_t { Cold, Warm, Hot, Extreme };

    enum class ParticipationClass : uint8_t { Dense, Sparse, VerySparse };

    enum class LocalityClass : uint8_t { ContiguousPreferred, StableAddressPreferred, SegmentedPreferred };

    enum class ReadWriteClass : uint8_t { ReadMostly, Balanced, WriteHeavy };

    enum class MutationBurstClass : uint8_t { Smooth, Bursty };

    enum class ExpectedDepthClass : uint8_t { Shallow, Medium, Deep };

    enum class MemberUniverse : uint8_t { ClosedStatic, Open };

    enum class MembershipGuarantee : uint8_t { Total, OnDemand };

    enum class ObjectAddressStability : uint8_t { Stable, Movable };

    struct NodeKindId {
        static constexpr uint16_t unspecified_value = UINT16_MAX;

        uint16_t value = unspecified_value;

        [[nodiscard]] friend constexpr bool operator==(NodeKindId, NodeKindId) noexcept = default;
    };

    [[nodiscard]] constexpr bool unspecified(NodeKindId kind) noexcept {
        return kind.value == NodeKindId::unspecified_value;
    }

    template<class... Ts>
    struct type_list {
        static constexpr size_t size = sizeof...(Ts);
    };

    template<class AddressPolicy, class... Objects>
    struct closed_total_members {
        using types = type_list<Objects...>;
        static constexpr MemberUniverse universe = MemberUniverse::ClosedStatic;
        static constexpr MembershipGuarantee guarantee = MembershipGuarantee::Total;
        static constexpr ObjectAddressStability address_stability = std::same_as<AddressPolicy, stable_address_members>
                                                                        ? ObjectAddressStability::Stable
                                                                        : ObjectAddressStability::Movable;
    };

    template<class AddressPolicy, class... StaticObjects>
    struct open_total_members {
        using types = type_list<StaticObjects...>;
        static constexpr MemberUniverse universe = MemberUniverse::Open;
        static constexpr MembershipGuarantee guarantee = MembershipGuarantee::Total;
        static constexpr ObjectAddressStability address_stability = std::same_as<AddressPolicy, stable_address_members>
                                                                        ? ObjectAddressStability::Stable
                                                                        : ObjectAddressStability::Movable;
    };

    struct on_demand_members {
        using types = type_list<>;
        static constexpr MemberUniverse universe = MemberUniverse::Open;
        static constexpr MembershipGuarantee guarantee = MembershipGuarantee::OnDemand;
        static constexpr ObjectAddressStability address_stability = ObjectAddressStability::Movable;
    };

    template<class... Payloads>
    struct closed_node_universe {
        using static_nodes = type_list<Payloads...>;
        static constexpr bool extensible = false;
    };

    template<class StaticNodes, class Registry>
    struct extensible_node_universe {
        using static_nodes = StaticNodes;
        using dynamic_registry = Registry;
        static constexpr bool extensible = true;
    };

    template<class HookSet>
    struct closed_hook_policy {
        using static_hooks = HookSet;
        static constexpr bool extensible = false;
    };

    template<class HookSet, class Registry>
    struct extensible_hook_policy {
        using static_hooks = HookSet;
        using dynamic_registry = Registry;
        static constexpr bool extensible = true;
    };

    template<class ActionSet>
    struct closed_action_surface {
        using static_actions = ActionSet;
        static constexpr bool extensible = false;
    };

    template<class ActionSet, class Registry>
    struct extensible_action_surface {
        using static_actions = ActionSet;
        using dynamic_registry = Registry;
        static constexpr bool extensible = true;
    };

    struct no_extension_policy {
        static constexpr bool allows_dynamic_nodes = false;
        static constexpr bool allows_dynamic_hooks = false;
        static constexpr bool allows_dynamic_actions = false;
    };

    struct dynamic_extension_lane {
        static constexpr bool allows_dynamic_nodes = true;
        static constexpr bool allows_dynamic_hooks = true;
        static constexpr bool allows_dynamic_actions = true;
    };

    template<class Spec>
    concept AbstractRelationSpec = requires {
        typename Spec::ShapeTag;
        { Spec::id } -> std::convertible_to<RelationId>;
        { Spec::shape } -> std::same_as<RelationShape const&>;
        { Spec::intent } -> std::same_as<RelationIntent const&>;
        { Spec::cardinality } -> std::same_as<Cardinality const&>;
        { Spec::child_order } -> std::same_as<ChildOrder const&>;
        { Spec::mutation } -> std::same_as<MutationClass const&>;
        { Spec::persistence } -> std::same_as<PersistenceClass const&>;
        { Spec::replay } -> std::same_as<ReplayMode const&>;
        { Spec::lifetime } -> std::same_as<LifetimeClass const&>;
        { Spec::concurrency } -> std::same_as<ConcurrencyClass const&>;
        { Spec::access_pattern } -> std::same_as<AccessPattern const&>;
        { Spec::hotness } -> std::same_as<HotnessClass const&>;
        { Spec::participation } -> std::same_as<ParticipationClass const&>;
        { Spec::locality } -> std::same_as<LocalityClass const&>;
        { Spec::read_write } -> std::same_as<ReadWriteClass const&>;
        { Spec::mutation_burst } -> std::same_as<MutationBurstClass const&>;
        { Spec::expected_depth } -> std::same_as<ExpectedDepthClass const&>;
    };

} // namespace Sora::Rel