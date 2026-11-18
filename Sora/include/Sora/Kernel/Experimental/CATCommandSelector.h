/**
 * @file CATCommandSelector.h
 * @brief CAT-compatible command focus selector implemented over Sora command notifications and weak lifetimes.
 * @ingroup KernelExperimental
 */
#pragma once

#include "Sora/Kernel/Experimental/CATCommand.h"

#include <memory>
#include <string>

namespace Sora::Kernel::Experimental {

    /** @brief Selector routing mode with the original protocol values. */
    enum CATCommandSelectorMode : std::uint8_t {
        CATCommandSelectorModeOFF = 0,   /**< Behave as an ordinary command. */
        CATCommandSelectorModeON = 1,    /**< Arbitrate focus and route otherwise unhandled notifications. */
        CATCommandSelectorModeABSORB = 2 /**< Arbitrate focus and absorb notifications that cannot be routed. */
    };

    /** @brief Event emitted after the selector reaches a stable state with no currently focused command. */
    struct CATNoMoreActivatedCommand {
        CATCommandSelector* selector{}; /**< Selector that currently has no activated command. */
    };

    /** @brief Notification sent to a selector's father when an OFF selector activates a child and returns to ON. */
    class [[= Sora::Kernel::$::Implementation]] CATCCSModeOnNotification final : public CATNotification {
    public:
        S_OBJECT
    };

    /**
     * @brief Focus arbiter for a CAT command subtree.
     *
     * @details Shared requests suspend the current focus and preserve it at the top of the resumable stack. Exclusive
     * requests cancel the current focus and every older resumable command before activating the requester. Commands and
     * notifications are observed weakly; selector storage never owns command lifetime.
     */
    class [[= Sora::Kernel::$::Implementation]] CATCommandSelector : public CATCommand {
    public:
        S_OBJECT

        using Emits = EventList<CATNoMoreActivatedCommand>;

        /** @brief Construct a selector; the first instance named exactly @c CATCommandSelector becomes the root. */
        explicit CATCommandSelector(const CATString& identifier = "CATCommandSelector", CATCommand* parent = nullptr);

        /** @brief Remove this selector from global observations and release pending protocol state. */
        ~CATCommandSelector() noexcept override;

        /** @brief Make this selector unavailable, clear pending focus state, and perform command logical death. */
        HRESULT LogicalDeath() override;

        /**
         * @brief Return a borrowed snapshot of the process root selector, or null when none is alive.
         * @warning The caller must provide external lifetime synchronization before dereferencing the result.
         */
        [[nodiscard]] static CATCommandSelector* GetCommandSelectorInstance() noexcept;

        /** @brief Find the nearest selector in @p command's parent chain. */
        [[nodiscard]] static CATCommandSelector* GetCommandSelectorInstance(CATCommand* command,
                                                                            int checkMode = 0) noexcept;

        /**
         * @brief Return a borrowed snapshot of the explicitly configured default selector.
         * @warning The caller must provide external lifetime synchronization before dereferencing the result.
         */
        [[nodiscard]] static CATCommandSelector* GetDefaultCommandSelectorInstance() noexcept;

        /** @brief Set the process default selector; null is rejected for CAT compatibility. */
        static int SetDefaultCommandSelectorInstance(CATCommandSelector* selector);

        /** @brief Change routing mode; the root mode is immutable and OFF first performs exclusive cleanup. */
        virtual int SetSelectorMode(CATCommandSelectorMode newMode);

        /** @brief Return the current selector routing mode. */
        [[nodiscard]] virtual CATCommandSelectorMode GetSelectorMode() const noexcept;

        /**
         * @brief Process a notification as selector protocol input or route it to the focused command.
         * @retval -1 The Selector consumed, absorbed, or routed the notification.
         * @retval 0 Continue ordinary CATCommand reception because this Selector does not handle the notification.
         * @throws std::logic_error if an ordinary notification races another thread's selector dispatch.
         */
        virtual int SelectorReceiveNotification(CATCommand* sender, CATNotification* notification);

        /** @brief Selector-specific object request hook; return null when no matching object is available. */
        [[nodiscard]] virtual void* SelectorSendObject(const char* objectClass, CATNotification* notification);

        /** @brief Return whether empty-focus event publication is currently enabled. */
        [[nodiscard]] int GetSubscribeStatus() const noexcept;

        /** @brief Enable or disable empty-focus event publication. */
        void SetSubscribeStatus(int newValue) noexcept;

        /** @brief Return nonzero exactly when @p command is focused; null tests whether no command is focused. */
        [[nodiscard]] int IsActivated(CATCommand* command) const noexcept;

        /**
         * @brief Return a borrowed snapshot of the currently focused command, or null.
         * @warning The caller must provide external lifetime synchronization before dereferencing the result.
         */
        [[nodiscard]] CATCommand* GetActivatedCommand() const noexcept;

        /** @brief Return the callback-event identifier for a stable selector with no current focus. */
        [[nodiscard]] static constexpr EventId NoMoreActivatedCATCommand() noexcept {
            return Sora::Traits::EventIdOf<CATNoMoreActivatedCommand>;
        }

        /** @brief Write a stable selector-state summary to the diagnostic stream and return zero. */
        int Dump() const;

        /** @brief Return a stable selector-state summary suitable for logs and tests. */
        [[nodiscard]] std::string DescribeState() const;

    private:
        friend class CATCommand;

        /** @brief Opaque focus-machine state defined in the implementation file. */
        struct State;

        /** @brief Acquire the root or nearest eligible Selector with a strong lifetime lease. */
        [[nodiscard]] static CommandLease AcquireCommandSelector(CATCommand* command, int checkMode) noexcept;

        /** @brief Acquire the explicitly configured default Selector with a strong lifetime lease. */
        [[nodiscard]] static CommandLease AcquireDefaultCommandSelector() noexcept;

        /** @brief Return whether this Selector still accepts lookup and protocol operations. */
        [[nodiscard]] bool IsSelectorAvailable() const noexcept;

        /** @brief Remove @p command from the nearest parent Selector and global observations. */
        static void NotifyCommandUnavailable(CATCommand* command, CommandLease parent) noexcept;

        /** @brief Restore an OFF child Selector to ON after it grants focus and notify its father. */
        void CompleteActivationFromOff();

        std::unique_ptr<State> selectorState_;
    };

} // namespace Sora::Kernel::Experimental
