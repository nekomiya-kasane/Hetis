/**
 * @file CATCommand.h
 * @brief Sora-native CAT command protocol, notification routing, callbacks, record state, and global undo hooks.
 * @ingroup KernelExperimental
 */
#pragma once

#include "Sora/Kernel/Core/EventPortAdaptor.h"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Kernel::Experimental {

    class CATCommand;
    class CATCommandSelector;
    class CATNotification;

    /** @brief CAT-compatible string type backed by the standard library. */
    using CATString = std::string;

    /** @brief CAT-compatible success/failure result representation. */
    using HRESULT = std::int32_t;

    /** @brief Successful CAT-compatible result. */
    inline constexpr HRESULT CAT_S_OK = 0;

    /** @brief Generic CAT-compatible failure result. */
    inline constexpr HRESULT CAT_E_FAIL = static_cast<HRESULT>(0x80004005u);

    /** @brief Command start mode with the original protocol values. */
    enum CATCommandMode : std::uint8_t {
        CATCommandModeShared = 1,    /**< Temporarily displace the active command and keep it resumable. */
        CATCommandModeExclusive = 0, /**< Cancel all commands that precede the requesting command. */
        CATCommandModeUndefined = 3, /**< Do not participate in selector focus arbitration. */
    };

    /** @brief Messages sent from commands to command selectors. */
    enum CATCommandMsg : std::uint8_t {
        CATCommandMsgRequestSharedMode = 0,    /**< Request shared focus. */
        CATCommandMsgRequestExclusiveMode = 1, /**< Request exclusive focus. */
        CATCommandMsgCanceled = 2,             /**< Acknowledge cancellation. */
        CATCommandMsgDesactivated = 3,         /**< Acknowledge deactivation. */
        CATCommandUnknow = 100,                /**< Non-protocol notification. */
    };

    /** @brief Messages sent from a command selector to a command. */
    enum CATCommandSelectorMsg : std::uint8_t {
        CATCommandSelectorMsgActivate = 4,    /**< Give focus to a command. */
        CATCommandSelectorMsgDesactivate = 5, /**< Withdraw focus while preserving resumability. */
        CATCommandSelectorMsgCancel = 6,      /**< Permanently withdraw focus. */
        CATCommandSelectorUnknow = 100,       /**< Non-protocol notification. */
    };


    /** @brief Current command record mode. */
    enum class CATRecordMode : std::uint8_t {
        None,    /**< Ordinary execution. */
        Capture, /**< Record command input. */
        Replay,  /**< Replay previously recorded command input. */
    };

    /** @brief Untyped client data passed back to CAT-compatible callbacks. */
    using CATCommandClientData = void*;

    namespace Detail {

        struct CATCallbackState;
        struct CATNotificationDeliveryState;

    } // namespace Detail

    /**
     * @brief Stable handle for a CAT notification callback.
     *
     * @details A handle owns only callback control metadata. The emitter's @ref Sora::EventPort owns the concrete
     * relation, while the receiver's command state indexes it for CAT-compatible bulk removal operations.
     */
    class CATCallback {
    public:
        /** @brief Construct an empty callback handle. */
        CATCallback() noexcept = default;

        /** @brief Return whether the callback can still be dispatched. */
        [[nodiscard]] bool IsActive() const noexcept;

        /** @brief Disconnect the callback from future notification delivery. */
        void Disconnect() const noexcept;

        /** @brief Return whether this handle names a callback record. */
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        explicit CATCallback(std::shared_ptr<Detail::CATCallbackState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<Detail::CATCallbackState> state_{};

        friend class CATCommand;
    };

    /** @brief Error returned when a marshal read exceeds the available serialized data. */
    struct CATMarshalError {
        size_t offset{};    /**< Cursor at which the read was attempted. */
        size_t requested{}; /**< Number of requested bytes. */
        size_t available{}; /**< Number of bytes available from @ref offset. */
    };

    /** @brief Compact binary buffer used by command capture and replay hooks. */
    class CATMarshal {
    public:
        /** @brief Append a trivially copyable value to this marshal. */
        template<typename T>
            requires std::is_trivially_copyable_v<T>
        void Write(const T& value) {
            auto bytes = std::as_bytes(std::span{std::addressof(value), size_t{1}});
            data_.insert(data_.end(), bytes.begin(), bytes.end());
        }

        /** @brief Read a trivially copyable value and advance the cursor on success. */
        template<typename T>
            requires std::is_trivially_copyable_v<T>
        [[nodiscard]] std::expected<T, CATMarshalError> Read() {
            if (data_.size() - cursor_ < sizeof(T)) {
                return std::unexpected(
                    CATMarshalError{.offset = cursor_, .requested = sizeof(T), .available = data_.size() - cursor_});
            }
            T value{};
            std::memcpy(std::addressof(value), data_.data() + cursor_, sizeof(T));
            cursor_ += sizeof(T);
            return value;
        }

        /** @brief Append a length-prefixed string with a strong exception guarantee. */
        void WriteString(std::string_view value);

        /** @brief Read a length-prefixed string, advancing the cursor only after complete decoding. */
        [[nodiscard]] std::expected<std::string, CATMarshalError> ReadString();

        /** @brief Return all serialized bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return data_; }

        /** @brief Reset the read cursor without changing serialized bytes. */
        void Rewind() noexcept { cursor_ = 0; }

        /** @brief Remove serialized bytes and reset the read cursor. */
        void Clear() noexcept {
            data_.clear();
            cursor_ = 0;
        }

        /** @brief Return the current read cursor. */
        [[nodiscard]] size_t Cursor() const noexcept { return cursor_; }

    private:
        std::vector<std::byte> data_{};
        size_t cursor_{};
    };

    /** @brief Base notification carried by the CAT command send/receive protocol. */
    class [[= Sora::Kernel::$::Implementation]] CATNotification : public BaseUnknown {
    public:
        S_OBJECT

        /** @brief Construct a notification and optionally transfer its initial reference to its next send. */
        explicit CATNotification(bool automaticDelete = false) noexcept : automaticDelete_(automaticDelete) {}

        /** @brief Destroy the notification. */
        ~CATNotification() noexcept override = default;

        /** @brief Return the reflected concrete notification name. */
        [[nodiscard]] std::string_view NotificationName() const noexcept { return GetClassName(); }

        /** @brief Return whether this notification derives from the class identified by @p iid. */
        [[nodiscard]] bool IsAKindOf(Iid iid) const noexcept;

        /** @brief Return whether this notification derives from a class named @p className. */
        [[nodiscard]] bool IsAKindOf(std::string_view className) const noexcept;

        /** @brief Return whether the next public send consumes this notification's initial reference. */
        [[nodiscard]] bool AutomaticDeleteRequested() const noexcept {
            return automaticDelete_.load(std::memory_order_acquire);
        }

        /** @brief Consume and clear the one-shot automatic-delete request. */
        [[nodiscard]] bool ConsumeAutomaticDeleteRequest() noexcept {
            return automaticDelete_.exchange(false, std::memory_order_acq_rel);
        }

    private:
        std::atomic_bool automaticDelete_{};
    };

    /** @brief Standard focus-protocol notification carrying either command-side or selector-side message values. */
    class [[= Kernel::$::Implementation]] CATCommandStandardMsg final : public CATNotification {
    public:
        S_OBJECT

        /** @brief Construct a command-to-selector protocol message. */
        explicit CATCommandStandardMsg(CATCommandMsg type, bool automaticDelete = false) noexcept
            : CATNotification(automaticDelete), _Type(static_cast<std::int32_t>(type)) {}

        /** @brief Construct a selector-to-command protocol message. */
        explicit CATCommandStandardMsg(CATCommandSelectorMsg type, bool automaticDelete = false) noexcept
            : CATNotification(automaticDelete), _Type(static_cast<std::int32_t>(type)) {}

        /** @brief Return whether this value is one of the command-to-selector protocol messages. */
        [[nodiscard]] bool IsCommandMessage() const noexcept;

        /** @brief Return whether this value is one of the selector-to-command protocol messages. */
        [[nodiscard]] bool IsSelectorMessage() const noexcept;

        std::int32_t _Type{}; /**< Original public protocol payload retained for source-level compatibility. */
    };

    /**
     * @brief Event emitted for an ordinary direct delivery after Selector pre-dispatch and before command callbacks.
     */
    struct CATNotificationPublished {
        CATCommand* publisher{};                       /**< Command publishing the notification. */
        CATCommand* target{};                          /**< Direct command-tree recipient. */
        CATNotification* notification{};               /**< Borrowed notification for this dispatch. */
        Detail::CATNotificationDeliveryState* state{}; /**< Borrowed synchronous acceptance state. */
    };

    /** @brief Event emitted when a command enters logical death. */
    struct CATCommandDeleted {
        CATCommand* command{}; /**< Command entering logical death. */
    };

    /**
     * @brief Non-vetoing observation emitted when selector activation is dispatched to a command.
     * @details Observer exceptions are isolated from command-control processing.
     */
    struct CATCommandActivated {
        CATCommand* command{}; /**< Activated command. */
    };

    /**
     * @brief Non-vetoing observation emitted when selector deactivation is dispatched to a command.
     * @details Observer exceptions are isolated from command-control processing.
     */
    struct CATCommandDesactivated {
        CATCommand* command{}; /**< Deactivated command. */
    };

    /**
     * @brief Non-vetoing observation emitted when selector cancellation is dispatched to a command.
     * @details Observer exceptions are isolated from command-control processing.
     */
    struct CATCommandCanceled {
        CATCommand* command{}; /**< Canceled command. */
    };

    /** @brief Prototype for global undo, redo, and data-deallocation functions. */
    using CATGlobalUndoMethod = void (*)(void* data);

    /** @brief Persistent command-global undo/redo action independent of the originating command lifetime. */
    class [[= Kernel::$::Implementation]] CATCommandGlobalUndo : public BaseUnknown {
    public:
        S_OBJECT

        /** @brief Construct an empty global undo object. */
        CATCommandGlobalUndo() noexcept = default;

        /** @brief Construct with before-undo, before-redo, data, and optional data-deallocation functions. */
        CATCommandGlobalUndo(CATGlobalUndoMethod beforeUndo, CATGlobalUndoMethod beforeRedo, void* data,
                             CATGlobalUndoMethod deallocate = nullptr) noexcept;

        /** @brief Construct the obsolete one-method undo form retained by the reference interface. */
        CATCommandGlobalUndo(CATGlobalUndoMethod beforeUndo, void* data) noexcept;

        /** @brief Destroy the object and deallocate its client data exactly once. */
        ~CATCommandGlobalUndo() noexcept override;

        /** @brief Set the function invoked before transactional undo. */
        virtual void SetBeforeUndoMeth(CATGlobalUndoMethod method) noexcept { beforeUndo_ = method; }

        /** @brief Set the function invoked before transactional redo. */
        virtual void SetBeforeRedoMeth(CATGlobalUndoMethod method) noexcept;

        /** @brief Set the function invoked after transactional undo. */
        virtual void SetAfterUndoMeth(CATGlobalUndoMethod method) noexcept { afterUndo_ = method; }

        /** @brief Set the function invoked after transactional redo. */
        virtual void SetAfterRedoMeth(CATGlobalUndoMethod method) noexcept;

        /** @brief Replace the client-data pointer without deallocating the previously stored pointer. */
        virtual void SetData(void* data) noexcept;

        /** @brief Set the client-data deallocation function. */
        virtual void SetDeallocateMeth(CATGlobalUndoMethod method) noexcept { deallocate_ = method; }

        /** @brief Invoke the before-undo function. */
        virtual HRESULT BeforeUndo();

        /** @brief Invoke the before-redo function. */
        virtual HRESULT BeforeRedo();

        /** @brief Invoke the after-undo function. */
        virtual HRESULT AfterUndo();

        /** @brief Invoke the after-redo function. */
        virtual HRESULT AfterRedo();

        /** @brief Execute the complete legacy undo sequence. */
        virtual void ExecuteUndo();

        /** @brief Execute the complete legacy redo sequence. */
        virtual void ExecuteRedo();

        /** @brief Adopt the caller-provided storage reference as the persistent redo object. */
        void SetRedoObject(CATCommandGlobalUndo* redo) noexcept;

        /** @brief Return the associated redo object without transferring ownership. */
        [[nodiscard]] CATCommandGlobalUndo* GetRedoObject() const noexcept { return redo_; }

    private:
        /** @brief Invoke the configured client-data deallocator once and clear the stored pointer. */
        void DeallocateData() noexcept;

        void* data_{};
        CATGlobalUndoMethod beforeUndo_{};
        CATGlobalUndoMethod beforeRedo_{};
        CATGlobalUndoMethod afterUndo_{};
        CATGlobalUndoMethod afterRedo_{};
        CATGlobalUndoMethod deallocate_{};
        CATCommandGlobalUndo* redo_{};
    };

    /** @brief Type-safe callback method accepted by the non-template compatibility overload. */
    using CATCommandMethod = void (CATCommand::*)(CATCommand*, CATNotification*, CATCommandClientData);

    /**
     * @brief Base class for CAT-style command-tree collaboration and command-selector focus arbitration.
     *
     * @details Directed command delivery remains synchronous and deterministic. EventPort supplies explicit callback
     * relations and lifetime leases; the command tree supplies fallback propagation when no callback or override
     * accepts a notification.
     */
    class [[= Sora::Kernel::$::Implementation]] CATCommand : public BaseUnknown {
    public:
        S_OBJECT

        using Emits = EventList<CATNotificationPublished, CATCommandDeleted, CATCommandActivated,
                                CATCommandDesactivated, CATCommandCanceled>;
        using Accepts = EventList<CATNotificationPublished>;

        /** @brief Construct with an optional parent and optional identifier. */
        explicit CATCommand(CATCommand* parent = nullptr, CATString* const identifier = nullptr);

        /** @brief Construct with a parent and selector start mode. */
        CATCommand(CATCommand* parent, CATCommandMode startMode);

        /** @brief Construct with a parent and identifier. */
        CATCommand(CATCommand* parent, const CATString& identifier);

        /** @brief Construct with a parent reference. */
        explicit CATCommand(CATCommand& parent);

        /** @brief Construct a parentless command with an identifier and selector start mode. */
        CATCommand(const CATString& identifier, CATCommandMode startMode);

        /** @brief Disconnect callbacks and release remaining implementation state. */
        ~CATCommand() noexcept override;

        /**
         * @brief Send @p notification synchronously to @p recipient.
         * @details The complete transaction retains both endpoints, so callbacks may release their external owners.
         */
        void SendNotification(CATCommand* recipient, CATNotification* notification);

        /**
         * @brief Queue @p notification and deliver it after the current outermost command dispatch.
         * @pre The storage of @p notification remains alive until delivery; a dynamically allocated automatic-delete
         * notification transfers its initial intrusive reference to this operation.
         */
        void PostNotification(CATCommand* recipient, CATNotification* notification);

        /**
         * @brief Receive from @p sender using Selector-first dispatch or ordinary callback-first command-tree routing.
         * @details The receiver and a non-null @p sender remain alive for the complete synchronous receive transaction.
         */
        void ReceiveNotification(CATCommand* sender, CATNotification* notification);

        /** @brief Request an object from the command currently publishing the received notification. */
        [[nodiscard]] void* SendObject(const char* objectClassNeeded, CATNotification* notification);

        /** @brief Request a focus-state transition from an explicit or nearest command selector. */
        [[nodiscard]] int RequestStatusChange(CATCommandMsg message, CATCommand* commandSelector = nullptr);

        /** @brief Enter logical death at the next safe command boundary. */
        virtual void RequestDelayedDestruction();

        /** @brief Enter logical death, publish deletion, and disconnect every EventPort relation exactly once. */
        virtual HRESULT LogicalDeath();

        /** @brief Return whether this command has entered logical death. */
        [[nodiscard]] bool IsLogicallyDead() const noexcept;

        /**
         * @brief Return a borrowed snapshot of the weakly observed command-tree parent.
         * @warning The caller must provide external lifetime synchronization before dereferencing the result.
         */
        [[nodiscard]] CATCommand* GetFather() const noexcept;

        /**
         * @brief Replace the weakly observed command-tree parent.
         * @throws std::invalid_argument if @p parent is dead or would create a cycle.
         * @throws std::logic_error if this command has entered logical death.
         */
        void SetFather(CATCommand* parent);

        /** @brief Return the command identifier. */
        [[nodiscard]] virtual CATString& GetName() noexcept;

        /** @brief Return the command identifier. */
        [[nodiscard]] virtual const CATString& GetName() const noexcept;

        /** @brief Replace the command identifier. */
        virtual void SetName(const CATString& identifier);

        /** @brief Return the EventPort that owns this command's outgoing callback relations. */
        [[nodiscard]] virtual EventPort* GetCallbackManager();

        /** @brief Return the callback-event identifier for logical deletion. */
        [[nodiscard]] static constexpr EventId Delete() noexcept { return Sora::Traits::EventIdOf<CATCommandDeleted>; }

        /** @brief Return the callback-event identifier for activation. */
        [[nodiscard]] static constexpr EventId Activate() noexcept {
            return Sora::Traits::EventIdOf<CATCommandActivated>;
        }

        /** @brief Return the callback-event identifier for deactivation. */
        [[nodiscard]] static constexpr EventId Desactivate() noexcept {
            return Sora::Traits::EventIdOf<CATCommandDesactivated>;
        }

        /** @brief Return the callback-event identifier for cancellation. */
        [[nodiscard]] static constexpr EventId Cancel() noexcept { return Sora::Traits::EventIdOf<CATCommandCanceled>; }

        /** @brief Return the selector start mode. */
        [[nodiscard]] CATCommandMode GetStartMode() const noexcept;

        /**
         * @brief Analyze a notification not consumed by a registered callback.
         * @return @c true if this command consumed the notification; @c false to continue propagation to its parent.
         */
        [[nodiscard]] virtual bool AnalyseNotification(CATCommand* sender, CATNotification* notification);

        /**
         * @brief Accept selector focus.
         * @return @c true if the transition completed synchronously; otherwise @c false.
         */
        virtual bool Activate(CATCommand* sender, CATNotification* notification);

        /**
         * @brief Release selector focus while remaining resumable.
         * @return @c true to acknowledge immediately; @c false when the command will acknowledge explicitly later.
         */
        virtual bool Desactivate(CATCommand* sender, CATNotification* notification);

        /**
         * @brief Permanently release selector focus.
         * @return @c true to acknowledge immediately; @c false when the command will acknowledge explicitly later.
         */
        virtual bool Cancel(CATCommand* sender, CATNotification* notification);

        /** @brief Return a command-specific object matching @p objectClassNeeded, or null. */
        [[nodiscard]] virtual void* SendCommandSpecificObject(const char* objectClassNeeded,
                                                              CATNotification* notification);

        /** @brief Return the unowned controller pointer. */
        [[nodiscard]] void* GetControler() const noexcept;

        /** @brief Replace the unowned controller pointer. */
        void SetControler(void* controler) noexcept;

        /** @brief Retain the reference API's no-op calling-command compatibility hook. */
        void SetCalling(CATCommand* calling);

        /**
         * @brief Return null, matching the reference implementation's inactive calling-command hook.
         */
        [[nodiscard]] CATCommand* GetCalling() const noexcept;

        /** @brief Replace the interactive prompt text. */
        virtual void SetPrompt(CATString text);

        /** @brief Return the interactive prompt text. */
        [[nodiscard]] virtual CATString GetPrompt() const;

        /** @brief Return nonzero when prompt text is displayable. */
        [[nodiscard]] virtual int GetStatusPrompt() const noexcept;

        /** @brief Hook called by a command factory or application after complete derived construction. */
        virtual void BeginCommand();

        /** @brief Hook called by derived command teardown before derived state disappears. */
        virtual void EndCommand();

        /** @brief Reset command interaction state. */
        virtual void Reset();

        /** @brief Undo command-local effects. */
        virtual void UndoCommand();

        /** @brief Return a persistent global undo object, or null. */
        [[nodiscard]] virtual CATCommandGlobalUndo* GetGlobalUndo();

        /**
         * @brief Capture notification-specific replay data.
         * @return @c true to persist the notification and marshal data; otherwise @c false.
         */
        virtual bool SaveState(CATNotification* notification, CATMarshal& dataToSave);

        /** @brief Restore notification-specific replay data. */
        virtual int RestoreState(CATNotification* notification, CATMarshal& dataToRestore);

        /** @brief Return whether this command is replaying recorded input. */
        [[nodiscard]] bool IsReplay() const noexcept;

        /** @brief Return whether this command is capturing input. */
        [[nodiscard]] bool IsCapture() const noexcept;

        /** @brief Set the process-wide record mode used by every command's replay/capture query. */
        void SetRecordMode(CATRecordMode mode) noexcept;

        /** @brief Return opaque access to the private command protocol state. */
        [[nodiscard]] void* AccessCommandData() noexcept;

    protected:
        /** @brief Add a callback selected by a concrete notification instance's reflected class. */
        CATCallback AddAnalyseNotificationCB(CATCommand* publishingCommand, CATNotification* publishedNotification,
                                             CATCommandMethod method, CATCommandClientData usefulData = nullptr);

        /** @brief Add a callback selected by a notification class name. */
        CATCallback AddAnalyseNotificationCB(CATCommand* publishingCommand, const char* notificationClassName,
                                             CATCommandMethod method, CATCommandClientData usefulData = nullptr);

        /** @brief Add a type-safe callback selected by a concrete notification instance's reflected class. */
        template<typename Receiver>
            requires std::derived_from<Receiver, CATCommand>
        CATCallback AddAnalyseNotificationCB(CATCommand* publishingCommand, CATNotification* publishedNotification,
                                             void (Receiver::*method)(CATCommand*, CATNotification*,
                                                                      CATCommandClientData),
                                             CATCommandClientData usefulData = nullptr) {
            auto* receiver = dynamic_cast<Receiver*>(this);
            if (!receiver || !method) {
                return {};
            }
            return AddAnalyseNotificationCBImpl(
                publishingCommand, publishedNotification, {},
                [receiver, method](CATCommand* sender, CATNotification* notification, CATCommandClientData data) {
                    (receiver->*method)(sender, notification, data);
                },
                usefulData);
        }

        /** @brief Add a type-safe callback selected by a notification class name. */
        template<typename Receiver>
            requires std::derived_from<Receiver, CATCommand>
        CATCallback AddAnalyseNotificationCB(CATCommand* publishingCommand, const char* notificationClassName,
                                             void (Receiver::*method)(CATCommand*, CATNotification*,
                                                                      CATCommandClientData),
                                             CATCommandClientData usefulData = nullptr) {
            auto* receiver = dynamic_cast<Receiver*>(this);
            if (!receiver || !method || !notificationClassName) {
                return {};
            }
            return AddAnalyseNotificationCBImpl(
                publishingCommand, nullptr, notificationClassName,
                [receiver, method](CATCommand* sender, CATNotification* notification, CATCommandClientData data) {
                    (receiver->*method)(sender, notification, data);
                },
                usefulData);
        }

        /** @brief Remove callbacks matching publisher, notification class, and client data. */
        void RemoveAnalyseNotificationCB(CATCommand* publishingCommand, CATNotification* publishedNotification,
                                         CATCommandClientData usefulData);

        /** @brief Remove callbacks matching publisher, class name, and client data. */
        void RemoveAnalyseNotificationCB(CATCommand* publishingCommand, const char* notificationClassName,
                                         CATCommandClientData usefulData);

        /** @brief Remove callbacks matching publisher and notification class. */
        void RemoveAnalyseNotificationCB(CATCommand* publishingCommand, CATNotification* publishedNotification);

        /** @brief Remove callbacks matching publisher and class name. */
        void RemoveAnalyseNotificationCB(CATCommand* publishingCommand, const char* notificationClassName);

        /** @brief Remove the callback named by @p callback. */
        void RemoveAnalyseNotificationCB(const CATCallback& callback);

    private:
        friend class CATCommandSelector;

        /** @brief Command address paired with a strong closure-nucleus lease. */
        struct CommandLease {
            CATCommand* command{};
            ComPtr<BaseUnknown> lifetime{};

            [[nodiscard]] explicit operator bool() const noexcept { return command != nullptr && lifetime; }
        };

        /** @brief Type-erased notification callback retained by a CAT callback record. */
        using NotificationHandler = std::move_only_function<void(CATCommand*, CATNotification*, CATCommandClientData)>;

        /** @brief Opaque mutable command state defined in the implementation file. */
        struct State;

        /** @brief Commit a callback record after its optional EventPort relation has been created. */
        CATCallback AddAnalyseNotificationCBImpl(CATCommand* publishingCommand, CATNotification* publishedNotification,
                                                 std::string_view notificationClassName, NotificationHandler handler,
                                                 CATCommandClientData usefulData);

        /** @brief Dispatch callbacks whose publisher constraint accepts every sender. */
        bool DispatchWildcardCallbacks(CATCommand* sender, CATNotification* notification);

        /** @brief Run EndCommand once and then complete logical death. */
        void CompleteDelayedDestruction();

        /** @brief Dispatch one Selector-to-command control message and its automatic acknowledgement. */
        void DispatchProtocolNotification(CATCommand* sender, CATCommandStandardMsg& message);

        /** @brief Deliver synchronously with endpoint, ownership, and optional Selector-route provenance. */
        void DeliverNotification(CATCommand* recipient, CATNotification* notification, bool consumeInitialReference,
                                 CATCommandSelector* routeOrigin = nullptr);

        /** @brief Remove this command from Selector and callback relations after the delete event. */
        void FinalizeLogicalDeath() noexcept;

        /** @brief Return the nearest physically alive father that has not entered logical death. */
        [[nodiscard]] CommandLease GetAvailableFatherLease() const noexcept;

        /** @brief Promote the direct weak father relation to a strong lease. */
        [[nodiscard]] CommandLease GetFatherLease() const noexcept;

        std::unique_ptr<State> state_;
    };

    /**
     * @brief Allocate a command and invoke @ref CATCommand::BeginCommand after complete derived construction.
     *
     * @tparam Command Concrete CAT command type.
     * @tparam Args Constructor argument types.
     * @param args Constructor arguments.
     * @return Owning Sora component pointer.
     */
    template<typename Command, typename... Args>
        requires std::derived_from<Command, CATCommand> && std::constructible_from<Command, Args...>
    [[nodiscard]] ComPtr<Command> MakeCATCommand(Args&&... args) {
        auto command = MakeComPtr<Command>(std::forward<Args>(args)...);
        command->BeginCommand();
        return command;
    }

} // namespace Sora::Kernel::Experimental
