/**
 * @file Service.h
 * @brief Shared ownership for backend service state and executable callback code.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Abi.h>

#include <cstdint>
#include <cassert>
#include <compare>
#include <expected>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Stable identity of one backend service instance. */
    struct BackendServiceId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this identity denotes one service instance. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(BackendServiceId, BackendServiceId) noexcept = default;
        friend constexpr auto operator<=>(BackendServiceId, BackendServiceId) noexcept = default;
    };

    /** @brief Type-erased shared owner keeping callback code mapped and executable. */
    class BackendCodeLease final {
    public:
        BackendCodeLease() noexcept = default;

        /** @brief Retain any concrete module or process-code owner without allocating another control block. */
        template<typename Owner>
        [[nodiscard]] static BackendCodeLease Retain(std::shared_ptr<Owner> owner) noexcept {
            return owner ? BackendCodeLease{std::shared_ptr<const void>{std::move(owner)}} : BackendCodeLease{};
        }

        /** @brief Return whether executable code ownership is present. */
        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }

        /** @brief Return the number of shared code owners for lifecycle diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return owner_.use_count(); }

    private:
        explicit BackendCodeLease(std::shared_ptr<const void> owner) noexcept : owner_(std::move(owner)) {}

        std::shared_ptr<const void> owner_{};
    };

    /** @brief Type-erased shared owner for callback state not owned by a backend service. */
    class BackendCallbackStateLease final {
    public:
        BackendCallbackStateLease() noexcept = default;

        /** @brief Retain one concrete callback state without allocating another control block. */
        template<typename State>
        [[nodiscard]] static BackendCallbackStateLease Retain(std::shared_ptr<State> state) noexcept {
            return state ? BackendCallbackStateLease{std::shared_ptr<void>{std::move(state)}}
                         : BackendCallbackStateLease{};
        }

        /** @brief Return whether callback-state ownership is present. */
        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }

        /** @brief Return the number of shared state owners for lifecycle diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return owner_.use_count(); }

    private:
        explicit BackendCallbackStateLease(std::shared_ptr<void> owner) noexcept : owner_(std::move(owner)) {}

        std::shared_ptr<void> owner_{};
    };

    using BackendServiceDestroy = void (*)(void* state) noexcept;

    /** @brief ABI payload transferring one backend service state to the runtime. */
    struct BackendServicePayload {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendServicePayload);
        BackendServiceId id{};
        void* state = nullptr;
        BackendServiceDestroy destroy = nullptr;
    };

    /** @brief Failure while adopting backend service and code ownership. */
    enum class BackendServiceError : uint8_t {
        InvalidPayload,
        MissingCodeLease,
        OutOfMemory,
    };

    /** @brief Shared lease keeping backend state alive and callback code loaded in the required destruction order. */
    class BackendService final {
        struct Control {
            Control(BackendServicePayload value, BackendCodeLease codeOwner) noexcept
                : payload(value), code(std::move(codeOwner)) {}

            ~Control() noexcept { payload.destroy(payload.state); }

            BackendServicePayload payload;
            BackendCodeLease code;
        };

    public:
        BackendService() noexcept = default;

        /**
         * @brief Adopt backend state while retaining the owner of every callback referenced by that state.
         * @details The service destroy callback runs before the final code lease is released.
         */
        [[nodiscard]] static std::expected<BackendService, BackendServiceError> Adopt(BackendServicePayload payload,
                                                                                      BackendCodeLease code) noexcept {
            const bool valid = ValidBackendAbi(payload.abiVersion, payload.structSize, sizeof(BackendServicePayload)) &&
                               payload.id.Valid() && payload.state != nullptr && payload.destroy != nullptr;
            if (!valid || !code) {
                if (payload.state != nullptr && payload.destroy != nullptr) {
                    payload.destroy(payload.state);
                }
                return std::unexpected(valid ? BackendServiceError::MissingCodeLease
                                             : BackendServiceError::InvalidPayload);
            }
            try {
                return BackendService{std::make_shared<Control>(payload, std::move(code))};
            } catch (const std::bad_alloc&) {
                payload.destroy(payload.state);
                return std::unexpected(BackendServiceError::OutOfMemory);
            }
        }

        /** @brief Return whether this handle owns one backend service. */
        [[nodiscard]] explicit operator bool() const noexcept { return control_ != nullptr; }

        /** @brief Return the stable service identity. @pre This handle is non-empty. */
        [[nodiscard]] BackendServiceId Id() const noexcept {
            assert(control_ != nullptr);
            return control_->payload.id;
        }

        /** @brief Return the opaque root service state. @pre This handle is non-empty. */
        [[nodiscard]] void* State() const noexcept {
            assert(control_ != nullptr);
            return control_->payload.state;
        }

        /** @brief Return the number of shared service owners for lifecycle diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return control_.use_count(); }

    private:
        explicit BackendService(std::shared_ptr<Control> control) noexcept : control_(std::move(control)) {}

        std::shared_ptr<Control> control_{};
    };

    static_assert(std::is_trivially_copyable_v<BackendServiceId>);
    static_assert(std::is_trivially_copyable_v<BackendServicePayload>);
    static_assert(std::is_standard_layout_v<BackendServicePayload>);

} // namespace Sora::Compute
