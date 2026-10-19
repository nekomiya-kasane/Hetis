#pragma once

/**
 * @file Abstraction.h
 * @brief Defines relation shapes, policies, and abstract relation requirements.
 * @ingroup Core
 */

#include <cstdint>
#include <limits>
#include <concepts>

#include <Sora/Core/Traits/TypeTraits.h>

namespace Sora::Rel {

    /** @brief Describes the structural shape of a relation. */
    enum class RelationShape : uint8_t {
        Tree,    /**< Single parent, acyclic, ordered children. */
        Forest,  /**< Multiple roots; each node has at most one parent. */
        Dag,     /**< Multiple parents, acyclic. */
        Graph,   /**< Cycles may exist. */
        PairSet, /**< Unordered relationship pairs. */
        Custom   /**< Shape semantics are supplied by the relation spec. */
    };

    inline namespace Tags {

        /** @brief Shape tag for tree relations. */
        struct TreeShapeTag {};
        /** @brief Shape tag for forest relations. */
        struct ForestShapeTag {};
        /** @brief Shape tag for directed acyclic graph relations. */
        struct DagShapeTag {};
        /** @brief Shape tag for general graph relations. */
        struct GraphShapeTag {};
        /** @brief Shape tag for unordered pair relations. */
        struct PairSetShapeTag {};

    } // namespace Tags

    /** @brief Describes the purpose of a relation. */
    enum class RelationIntent : uint8_t {
        Ownership,   /**< Represents ownership. */
        Visual,      /**< Represents visual composition or ordering. */
        Interaction, /**< Represents interactive relationships. */
        Command,     /**< Represents command routing. */
        Focus,       /**< Represents focus navigation. */
        Dependency,  /**< Represents dependency relationships. */
        Observation, /**< Represents observation or subscription. */
        Custom       /**< Represents a user-defined purpose. */
    };

    /** @brief Identifies a relation. */
    struct RelationId {
        uint32_t value = 0; /**< Numeric identifier; zero is the default value. */

        /** @brief Compares relation identifiers by value. */
        friend constexpr bool operator==(RelationId, RelationId) = default;
    };

    /** @brief Describes how many parents a node may have. */
    enum class Cardinality : uint8_t { SingleParent, MultiParent };

    /** @brief Describes the ordering policy for children. */
    enum class ChildOrder : uint8_t { Unordered, StableInsertion, ExplicitRank };

    /** @brief Classifies the expected frequency of relation mutations. */
    enum class MutationClass : uint8_t { Static, LowChurn, HighChurn };

    /** @brief Classifies how relation data is persisted. */
    enum class PersistenceClass : uint8_t { Ephemeral, Replayable, Serializable };

    /** @brief Selects how relation changes can be replayed. */
    enum class ReplayMode : uint8_t { None, ObjectIdMutationLog, StructuralSnapshot };

    /** @brief Describes the lifetime scope of a relation. */
    enum class LifetimeClass : uint8_t { StaticProgram, DocumentScoped, ActivationScoped, FrameScoped };

    /** @brief Describes the relation's concurrency model. */
    enum class ConcurrencyClass : uint8_t { SingleThreadConfined, SnapshotReaders, MultiWriterTransactional };

    /** @brief Describes the expected access pattern. */
    enum class AccessPattern : uint8_t { PointLookup, RouteTraversal, BatchTraversal };

    /** @brief Classifies how frequently relation data is accessed. */
    enum class HotnessClass : uint8_t { Cold, Warm, Hot, Extreme };

    /** @brief Classifies the density of relation participation. */
    enum class ParticipationClass : uint8_t { Dense, Sparse, VerySparse };

    /** @brief Describes the preferred data locality. */
    enum class LocalityClass : uint8_t { ContiguousPreferred, StableAddressPreferred, SegmentedPreferred };

    /** @brief Classifies the balance between reads and writes. */
    enum class ReadWriteClass : uint8_t { ReadMostly, Balanced, WriteHeavy };

    /** @brief Classifies whether mutations occur smoothly or in bursts. */
    enum class MutationBurstClass : uint8_t { Smooth, Bursty };

    /** @brief Classifies the expected relation depth. */
    enum class ExpectedDepthClass : uint8_t { Shallow, Medium, Deep };

    /** @brief Describes whether the participating member set is fixed or open. */
    enum class MemberUniverse : uint8_t { ClosedStatic, Open };

    /** @brief Describes whether all members are known or resolved on demand. */
    enum class MembershipGuarantee : uint8_t { Total, OnDemand };

    /** @brief Describes whether participating object addresses remain stable. */
    enum class ObjectAddressStability : uint8_t { Stable, Movable };

    /** @brief Identifies a node kind, with a sentinel for unspecified kinds. */
    struct NodeKindId {
        /** @brief Sentinel value indicating that no node kind was specified. */
        static constexpr uint16_t unspecified_value = UINT16_MAX;

        uint16_t value = unspecified_value; /**< Stored node-kind identifier. */

        /** @brief Compares node-kind identifiers by value. */
        [[nodiscard]] friend constexpr bool operator==(NodeKindId, NodeKindId) noexcept = default;
    };

    /** @brief Checks whether a node-kind identifier is unspecified. */
    [[nodiscard]] constexpr bool unspecified(NodeKindId kind) noexcept {
        return kind.value == NodeKindId::unspecified_value;
    }

    /** @brief Compile-time list of types. */
    template<class... Ts>
    struct type_list {
        /** @brief Number of types in the list. */
        static constexpr size_t size = sizeof...(Ts);
    };

    /** @brief Describes a fixed, fully known set of relation members. */
    template<class AddressPolicy, class... Objects>
    struct closed_total_members {
        using types = type_list<Objects...>; /**< Statically known member types. */
        static constexpr MemberUniverse universe = MemberUniverse::ClosedStatic;
        static constexpr MembershipGuarantee guarantee = MembershipGuarantee::Total;
        static constexpr ObjectAddressStability address_stability = std::same_as<AddressPolicy, stable_address_members>
                                                                        ? ObjectAddressStability::Stable
                                                                        : ObjectAddressStability::Movable;
    };

    /** @brief Describes a fully known static member set that permits dynamic members. */
    template<class AddressPolicy, class... StaticObjects>
    struct open_total_members {
        using types = type_list<StaticObjects...>; /**< Statically known member types. */
        static constexpr MemberUniverse universe = MemberUniverse::Open;
        static constexpr MembershipGuarantee guarantee = MembershipGuarantee::Total;
        static constexpr ObjectAddressStability address_stability = std::same_as<AddressPolicy, stable_address_members>
                                                                        ? ObjectAddressStability::Stable
                                                                        : ObjectAddressStability::Movable;
    };

    /** @brief Describes an open member set whose membership is resolved on demand. */
    struct on_demand_members {
        using types = type_list<>;
        static constexpr MemberUniverse universe = MemberUniverse::Open;
        static constexpr MembershipGuarantee guarantee = MembershipGuarantee::OnDemand;
        static constexpr ObjectAddressStability address_stability = ObjectAddressStability::Movable;
    };

    /** @brief Declares a closed universe of node payload types. */
    template<class... Payloads>
    struct closed_node_universe {
        using static_nodes = type_list<Payloads...>; /**< Statically supported node types. */
        static constexpr bool extensible = false;
    };

    /** @brief Declares a static node set that can be extended through a registry. */
    template<class StaticNodes, class Registry>
    struct extensible_node_universe {
        using static_nodes = StaticNodes; /**< Statically supported node types. */
        using dynamic_registry = Registry; /**< Registry for additional node types. */
        static constexpr bool extensible = true;
    };

    /** @brief Declares a closed set of relation hooks. */
    template<class HookSet>
    struct closed_hook_policy {
        using static_hooks = HookSet; /**< Statically supported hooks. */
        static constexpr bool extensible = false;
    };

    /** @brief Declares a static hook set that can be extended through a registry. */
    template<class HookSet, class Registry>
    struct extensible_hook_policy {
        using static_hooks = HookSet; /**< Statically supported hooks. */
        using dynamic_registry = Registry; /**< Registry for additional hooks. */
        static constexpr bool extensible = true;
    };

    /** @brief Declares a closed set of relation actions. */
    template<class ActionSet>
    struct closed_action_surface {
        using static_actions = ActionSet; /**< Statically supported actions. */
        static constexpr bool extensible = false;
    };

    /** @brief Declares a static action set that can be extended through a registry. */
    template<class ActionSet, class Registry>
    struct extensible_action_surface {
        using static_actions = ActionSet; /**< Statically supported actions. */
        using dynamic_registry = Registry; /**< Registry for additional actions. */
        static constexpr bool extensible = true;
    };

    /** @brief Policy that disallows dynamically registered relation extensions. */
    struct no_extension_policy {
        static constexpr bool allows_dynamic_nodes = false;
        static constexpr bool allows_dynamic_hooks = false;
        static constexpr bool allows_dynamic_actions = false;
    };

    /** @brief Policy that allows dynamic nodes, hooks, and actions. */
    struct dynamic_extension_lane {
        static constexpr bool allows_dynamic_nodes = true;
        static constexpr bool allows_dynamic_hooks = true;
        static constexpr bool allows_dynamic_actions = true;
    };

    /**
     * @brief Checks whether a type provides the abstract relation specification.
     * @tparam Spec Candidate relation specification type.
     *
     * Requires the shape tag and the relation metadata queried by generic
     * relation facilities.
     */
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