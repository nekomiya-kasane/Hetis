/**
 * @file Meta.h
 * @brief Immutable metaclass facts and dictionary-materialized object-model links.
 * @ingroup Core
 */
#pragma once

#include <span>
#include <string_view>

#include "Yuki/Core/Dispatcher.h"
#include "Yuki/Core/IID.h"
#include "Yuki/Core/Types.h"

namespace Yuki {

    class Dictionary;
    struct ProviderEntry;

    /** @brief Immutable identity and direct provider facts of one object-model class. */
    struct MetaCore {
        TypeOfClass type{TypeOfClass::NothingType}; /**< Declared object-model role. */
        Iid iid{};                                  /**< Class identifier. */
        Iid directBaseIid{};                        /**< Direct object-model base identifier, if any. */
        const MetaClass* directBase{};              /**< Direct object-model base metaclass, when materialized. */
        std::string_view name{};                    /**< Reflected or section-provided diagnostic name. */
        std::span<const ProviderEntry> provides{};  /**< Direct providers contributed by this class. */
    };

    /** @brief Link facts that may reference other object-model classes. */
    struct MetaLinks {
        std::span<const Iid> extendees{}; /**< Components extended by this class. */
    };

    /** @brief Immutable metaclass facade used by the Core runtime and dictionary. */
    class MetaClass {
    public:
        /** @brief Construct an empty metaclass sentinel. */
        constexpr MetaClass() noexcept = default;

        /** @brief Construct a metaclass from core identity, provider, and link facts. */
        constexpr MetaClass(TypeOfClass type, Iid iid, std::string_view name,
                            std::span<const ProviderEntry> provides = {}, Iid directBaseIid = {},
                            const MetaClass* directBase = nullptr,
                            std::span<const Iid> extendees = {}) noexcept
            : core_{type, iid, directBaseIid, directBase, name, provides}, links_{extendees} {}

        /** @brief Return the declared object-model role. */
        [[nodiscard]] constexpr TypeOfClass GetTypeOfClass() const noexcept { return core_.type; }

        /** @brief Return the class IID. */
        [[nodiscard]] constexpr Iid IidValue() const noexcept { return core_.iid; }

        /** @brief Return the direct object-model base IID, or nil. */
        [[nodiscard]] constexpr Iid DirectBaseIid() const noexcept { return core_.directBaseIid; }

        /** @brief Return the direct object-model base metaclass, or null when it has not been materialized. */
        [[nodiscard]] constexpr const MetaClass* DirectBase() const noexcept { return core_.directBase; }

        /** @brief Return whether this metaclass has a direct object-model base. */
        [[nodiscard]] constexpr bool HasDirectBase() const noexcept { return !IsNil(core_.directBaseIid); }

        /** @brief Return the stable diagnostic class name. */
        [[nodiscard]] constexpr std::string_view ClassName() const noexcept { return core_.name; }

        /** @brief Return a one-element direct-base view, or an empty view when there is no base. */
        [[nodiscard]] constexpr std::span<const Iid> Bases() const noexcept {
            return HasDirectBase() ? std::span{&core_.directBaseIid, 1} : std::span<const Iid>{};
        }

        /** @brief Return components extended by this class. */
        [[nodiscard]] constexpr std::span<const Iid> Extendees() const noexcept { return links_.extendees; }

        /** @brief Return direct provider entries contributed by this class. */
        [[nodiscard]] constexpr std::span<const ProviderEntry> Provides() const noexcept { return core_.provides; }

        /** @brief Return immutable core facts. */
        [[nodiscard]] constexpr const MetaCore& Core() const noexcept { return core_; }

        /** @brief Return link facts. */
        [[nodiscard]] constexpr const MetaLinks& Links() const noexcept { return links_; }

        /** @brief Find the highest-priority direct provider for @p iid in this class. */
        [[nodiscard]] const ProviderEntry* FindProvide(Iid iid) const noexcept;

    private:
        friend class Dictionary;

        /** @brief Materialize the direct-base pointer from dictionary state. */
        constexpr void BindDirectBase(const MetaClass* directBase) noexcept { core_.directBase = directBase; }

        MetaCore core_{};
        MetaLinks links_{};
    };

} // namespace Yuki