/**
 * @file CommandRuntime.h
 * @brief Experimental CAD command runtime: descriptors, contextual key bindings, interaction routing, effects,
 *        compute graphs, and document transactions.
 * @ingroup KernelExperimental
 */
#pragma once

#include <algorithm>
#include <any>
#include <cassert>
#include <compare>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Kernel::Experimental::CommandRuntime {

    // ----------------------------------------------------------------------------------------------------

    struct CommandId {
        std::string value{};

        [[nodiscard]] friend bool operator==(const CommandId&, const CommandId&) noexcept = default;
        [[nodiscard]] friend auto operator<=>(const CommandId&, const CommandId&) noexcept = default;
    };

    struct ScopeId {
        std::string value{};

        [[nodiscard]] friend bool operator==(const ScopeId&, const ScopeId&) noexcept = default;
        [[nodiscard]] friend auto operator<=>(const ScopeId&, const ScopeId&) noexcept = default;
    };

    struct WorkbenchId {
        std::string value{};

        [[nodiscard]] friend bool operator==(const WorkbenchId&, const WorkbenchId&) noexcept = default;
        [[nodiscard]] friend auto operator<=>(const WorkbenchId&, const WorkbenchId&) noexcept = default;
    };

    struct DocumentVersion {
        uint64_t value{};

        [[nodiscard]] friend bool operator==(const DocumentVersion&, const DocumentVersion&) noexcept = default;
        [[nodiscard]] friend auto operator<=>(const DocumentVersion&, const DocumentVersion&) noexcept = default;
    };

    struct TraceLog {
        std::vector<std::string> records{};

        void Push(std::string text) { records.push_back(std::move(text)); }

        [[nodiscard]] std::string Join(std::string_view separator = "\n") const {
            std::ostringstream out;
            for (std::size_t i = 0; i < records.size(); ++i) {
                if (i != 0) {
                    out << separator;
                }
                out << records[i];
            }
            return out.str();
        }
    };

    // ----------------------------------------------------------------------------------------------------

    enum class CommandMode : uint8_t {
        Exclusive,
        Shared,
        Passive,
    };

    enum class ActivationState : uint8_t {
        Created,
        Active,
        Suspended,
        Completed,
        Canceling,
        Canceled,
    };

    enum class ErrorCode : uint8_t {
        None,
        NotFound,
        Unavailable,
        Conflict,
        InvalidInput,
        VersionMismatch,
        CycleDetected,
        Stopped,
        InternalError,
    };

    struct CommandError {
        ErrorCode code{ErrorCode::None};
        std::string message{};

        [[nodiscard]] static CommandError Make(ErrorCode code, std::string message) {
            return CommandError{.code = code, .message = std::move(message)};
        }
    };

    struct Stopped {};

    template<typename T>
    class Completion {
    public:
        using ValueType = T;

        [[nodiscard]] static Completion Value(T value) { return Completion(std::move(value)); }
        [[nodiscard]] static Completion Error(CommandError error) { return Completion(std::move(error)); }
        [[nodiscard]] static Completion Stop() { return Completion(Stopped{}); }

        [[nodiscard]] bool HasValue() const noexcept { return std::holds_alternative<T>(storage_); }
        [[nodiscard]] bool HasError() const noexcept { return std::holds_alternative<CommandError>(storage_); }
        [[nodiscard]] bool IsStopped() const noexcept { return std::holds_alternative<Stopped>(storage_); }

        [[nodiscard]] T& Value() & { return std::get<T>(storage_); }
        [[nodiscard]] const T& Value() const& { return std::get<T>(storage_); }
        [[nodiscard]] T&& Value() && { return std::get<T>(std::move(storage_)); }

        [[nodiscard]] const CommandError& Error() const& { return std::get<CommandError>(storage_); }

    private:
        explicit Completion(T value) : storage_(std::move(value)) {}
        explicit Completion(CommandError error) : storage_(std::move(error)) {}
        explicit Completion(Stopped stopped) : storage_(stopped) {}

        std::variant<T, CommandError, Stopped> storage_;
    };

    template<>
    class Completion<void> {
    public:
        [[nodiscard]] static Completion Value() { return Completion(std::monostate{}); }
        [[nodiscard]] static Completion Error(CommandError error) { return Completion(std::move(error)); }
        [[nodiscard]] static Completion Stop() { return Completion(Stopped{}); }

        [[nodiscard]] bool HasValue() const noexcept { return std::holds_alternative<std::monostate>(storage_); }
        [[nodiscard]] bool HasError() const noexcept { return std::holds_alternative<CommandError>(storage_); }
        [[nodiscard]] bool IsStopped() const noexcept { return std::holds_alternative<Stopped>(storage_); }

        [[nodiscard]] const CommandError& Error() const& { return std::get<CommandError>(storage_); }

    private:
        explicit Completion(std::monostate value) : storage_(value) {}
        explicit Completion(CommandError error) : storage_(std::move(error)) {}
        explicit Completion(Stopped stopped) : storage_(stopped) {}

        std::variant<std::monostate, CommandError, Stopped> storage_;
    };

    template<typename Receiver, typename T>
    concept ReceiverOf = requires(Receiver receiver, T value, CommandError error) {
        receiver.SetValue(std::move(value));
        receiver.SetError(std::move(error));
        receiver.SetStopped();
    };

    template<typename Receiver>
    concept VoidReceiver = requires(Receiver receiver, CommandError error) {
        receiver.SetValue();
        receiver.SetError(std::move(error));
        receiver.SetStopped();
    };

    template<typename T, typename Fn>
    class FunctionSender {
    public:
        using ValueType = T;

        explicit FunctionSender(Fn fn) : fn_(std::move(fn)) {}

        template<typename Receiver>
        class Operation {
        public:
            Operation(Fn fn, Receiver receiver) : fn_(std::move(fn)), receiver_(std::move(receiver)) {}

            void Start() noexcept {
                try {
                    Completion<T> completion = fn_();
                    if (completion.IsStopped()) {
                        receiver_.SetStopped();
                    } else if (completion.HasError()) {
                        receiver_.SetError(completion.Error());
                    } else {
                        if constexpr (std::same_as<T, void>) {
                            receiver_.SetValue();
                        } else {
                            receiver_.SetValue(std::move(completion).Value());
                        }
                    }
                } catch (...) {
                    receiver_.SetError(CommandError::Make(ErrorCode::InternalError, "sender operation threw"));
                }
            }

            [[nodiscard]] auto Take() && {
                return std::move(receiver_).Take();
            }

        private:
            Fn fn_;
            Receiver receiver_;
        };

        template<typename Receiver>
        [[nodiscard]] auto Connect(Receiver receiver) && {
            if constexpr (std::same_as<T, void>) {
                static_assert(VoidReceiver<Receiver>, "Receiver must accept value, error, and stopped completions.");
            } else {
                static_assert(ReceiverOf<Receiver, T>, "Receiver must accept value, error, and stopped completions.");
            }
            return Operation<Receiver>{std::move(fn_), std::move(receiver)};
        }

    private:
        Fn fn_;
    };

    template<typename T, typename Fn>
    [[nodiscard]] auto MakeSender(Fn&& fn) {
        return FunctionSender<T, std::remove_cvref_t<Fn>>(std::forward<Fn>(fn));
    }

    template<typename T>
    class SyncReceiver {
    public:
        void SetValue(T value) { completion_ = Completion<T>::Value(std::move(value)); }
        void SetError(CommandError error) { completion_ = Completion<T>::Error(std::move(error)); }
        void SetStopped() { completion_ = Completion<T>::Stop(); }

        [[nodiscard]] Completion<T> Take() && {
            assert(completion_.has_value());
            return std::move(*completion_);
        }

    private:
        std::optional<Completion<T>> completion_{};
    };

    template<>
    class SyncReceiver<void> {
    public:
        void SetValue() { completion_ = Completion<void>::Value(); }
        void SetError(CommandError error) { completion_ = Completion<void>::Error(std::move(error)); }
        void SetStopped() { completion_ = Completion<void>::Stop(); }

        [[nodiscard]] Completion<void> Take() && {
            assert(completion_.has_value());
            return std::move(*completion_);
        }

    private:
        std::optional<Completion<void>> completion_{};
    };

    template<typename Sender>
    [[nodiscard]] auto SyncWait(Sender&& sender) {
        using T = typename std::remove_cvref_t<Sender>::ValueType;
        SyncReceiver<T> receiver;
        auto operation = std::forward<Sender>(sender).Connect(std::move(receiver));
        operation.Start();
        return std::move(operation).Take();
    }

    class CommandException final : public std::exception {
    public:
        explicit CommandException(CommandError error) : error_(std::move(error)) {}

        [[nodiscard]] const char* what() const noexcept override { return error_.message.c_str(); }
        [[nodiscard]] const CommandError& Error() const noexcept { return error_; }

    private:
        CommandError error_;
    };

    class CommandStoppedException final : public std::exception {
    public:
        [[nodiscard]] const char* what() const noexcept override { return "command stopped"; }
    };

    template<typename Sender>
    concept RuntimeSender = requires {
        typename std::remove_cvref_t<Sender>::ValueType;
    };

    template<RuntimeSender Sender>
    class SenderAwaiter {
    public:
        using T = typename std::remove_cvref_t<Sender>::ValueType;

        explicit SenderAwaiter(Sender sender) : sender_(std::move(sender)) {}

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> continuation) {
            completion_ = SyncWait(std::move(sender_));
            continuation.resume();
        }

        decltype(auto) await_resume() {
            assert(completion_.has_value());
            if (completion_->IsStopped()) {
                throw CommandStoppedException{};
            }
            if (completion_->HasError()) {
                throw CommandException{completion_->Error()};
            }
            if constexpr (std::same_as<T, void>) {
                return;
            } else {
                return std::move(*completion_).Value();
            }
        }

    private:
        Sender sender_;
        std::optional<Completion<T>> completion_{};
    };

    template<RuntimeSender Sender>
    [[nodiscard]] auto operator co_await(Sender&& sender) {
        return SenderAwaiter<std::remove_cvref_t<Sender>>{std::forward<Sender>(sender)};
    }

    template<typename T>
    class CommandTask {
    public:
        struct promise_type {
            std::optional<Completion<T>> completion{};

            [[nodiscard]] auto get_return_object() noexcept {
                return CommandTask(std::coroutine_handle<promise_type>::from_promise(*this));
            }

            [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
            [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }

            void return_value(T value) {
                completion = Completion<T>::Value(std::move(value));
            }

            void unhandled_exception() {
                try {
                    throw;
                } catch (const CommandException& error) {
                    completion = Completion<T>::Error(error.Error());
                } catch (const CommandStoppedException&) {
                    completion = Completion<T>::Stop();
                } catch (...) {
                    completion = Completion<T>::Error(
                        CommandError::Make(ErrorCode::InternalError, "command coroutine threw"));
                }
            }
        };

        explicit CommandTask(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

        CommandTask(CommandTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

        CommandTask& operator=(CommandTask&& other) noexcept {
            if (this != &other) {
                if (handle_) {
                    handle_.destroy();
                }
                handle_ = std::exchange(other.handle_, {});
            }
            return *this;
        }

        CommandTask(const CommandTask&) = delete;
        CommandTask& operator=(const CommandTask&) = delete;

        ~CommandTask() {
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] Completion<T> Run() {
            while (!handle_.done()) {
                handle_.resume();
            }
            assert(handle_.promise().completion.has_value());
            return std::move(*handle_.promise().completion);
        }

    private:
        std::coroutine_handle<promise_type> handle_{};
    };

    // ----------------------------------------------------------------------------------------------------

    struct SelectionSnapshot {
        std::vector<std::string> stable_refs{};
        DocumentVersion version{};

        [[nodiscard]] bool IsImmutable() const noexcept { return true; }
    };

    struct ParameterSnapshot {
        std::map<std::string, double> numbers{};
        std::map<std::string, bool> booleans{};
        std::map<std::string, std::string> texts{};
        DocumentVersion version{};

        [[nodiscard]] bool IsImmutable() const noexcept { return true; }
    };

    struct ComputeInput {
        SelectionSnapshot selection{};
        ParameterSnapshot parameters{};
        DocumentVersion base_version{};
    };

    enum class ParameterValueKind : uint8_t {
        Number,
        Boolean,
        Text,
        Unknown,
    };

    struct ParameterField {
        std::string name{};
        ParameterValueKind kind{ParameterValueKind::Unknown};
        bool required{true};
    };

    struct ParameterSchema {
        std::string name{};
        std::vector<ParameterField> fields{};
        bool generated_from_reflection{};

        [[nodiscard]] bool Empty() const noexcept { return fields.empty(); }
    };

    struct ParameterValidationResult {
        std::vector<std::string> missing_required_fields{};
        std::vector<std::string> unknown_required_fields{};

        [[nodiscard]] bool Valid() const noexcept {
            return missing_required_fields.empty() && unknown_required_fields.empty();
        }
    };

    [[nodiscard]] inline ParameterValidationResult ValidateParameters(const ParameterSchema& schema,
                                                                      const ParameterSnapshot& snapshot) {
        ParameterValidationResult result;
        for (const ParameterField& field : schema.fields) {
            if (!field.required) {
                continue;
            }

            switch (field.kind) {
            case ParameterValueKind::Number:
                if (!snapshot.numbers.contains(field.name)) {
                    result.missing_required_fields.push_back(field.name);
                }
                break;
            case ParameterValueKind::Boolean:
                if (!snapshot.booleans.contains(field.name)) {
                    result.missing_required_fields.push_back(field.name);
                }
                break;
            case ParameterValueKind::Text:
                if (!snapshot.texts.contains(field.name)) {
                    result.missing_required_fields.push_back(field.name);
                }
                break;
            case ParameterValueKind::Unknown:
                result.unknown_required_fields.push_back(field.name);
                break;
            }
        }
        return result;
    }

    template<typename T>
    concept ImmutableSnapshot = requires(const T& value) {
        { value.IsImmutable() } -> std::same_as<bool>;
        { value.version } -> std::same_as<const DocumentVersion&>;
    };

    struct UiAffineTag {};

    template<typename T>
    concept UiAffine = std::derived_from<std::remove_cvref_t<T>, UiAffineTag>;

    template<typename T>
    concept BackgroundSafe =
        ImmutableSnapshot<std::remove_cvref_t<T>> ||
        std::same_as<std::remove_cvref_t<T>, ComputeInput> ||
        std::same_as<std::remove_cvref_t<T>, DocumentVersion> ||
        std::same_as<std::remove_cvref_t<T>, std::string>;

    // ----------------------------------------------------------------------------------------------------

    enum class EventKind : uint8_t {
        Key,
        Pointer,
        Menu,
        Toolbar,
        CommandCompletion,
    };

    struct InputEvent {
        EventKind kind{};
        ScopeId target{};
        std::string payload{};
    };

    enum class EventDisposition : uint8_t {
        Ignored,
        Handled,
    };

    struct InteractionScope {
        ScopeId id{};
        std::optional<ScopeId> parent{};
        std::string label{};
    };

    class InteractionScopeGraph {
    public:
        void Add(InteractionScope scope) {
            scopes_[scope.id.value] = std::move(scope);
        }

        [[nodiscard]] bool Contains(const ScopeId& id) const {
            return scopes_.contains(id.value);
        }

        [[nodiscard]] std::vector<ScopeId> PathToRoot(ScopeId id) const {
            std::vector<ScopeId> path;
            while (scopes_.contains(id.value)) {
                const auto& scope = scopes_.at(id.value);
                path.push_back(scope.id);
                if (!scope.parent) {
                    break;
                }
                id = *scope.parent;
            }
            return path;
        }

    private:
        std::unordered_map<std::string, InteractionScope> scopes_{};
    };

    class InteractionEventRouter {
    public:
        using Handler = std::function<EventDisposition(const InputEvent&)>;

        explicit InteractionEventRouter(TraceLog* trace = nullptr) : trace_(trace) {}

        void AddScope(InteractionScope scope) {
            graph_.Add(std::move(scope));
        }

        void On(ScopeId scope, Handler handler) {
            handlers_[std::move(scope.value)] = std::move(handler);
        }

        void SetActiveFallback(ScopeId scope) {
            active_fallback_ = std::move(scope);
        }

        [[nodiscard]] EventDisposition Dispatch(const InputEvent& event) {
            for (const ScopeId& scope : graph_.PathToRoot(event.target)) {
                if (TryHandle(scope, event, "bubble") == EventDisposition::Handled) {
                    return EventDisposition::Handled;
                }
            }

            if (active_fallback_) {
                return TryHandle(*active_fallback_, event, "fallback");
            }
            return EventDisposition::Ignored;
        }

    private:
        [[nodiscard]] EventDisposition TryHandle(const ScopeId& scope, const InputEvent& event,
                                                 std::string_view phase) {
            const auto it = handlers_.find(scope.value);
            if (it == handlers_.end()) {
                return EventDisposition::Ignored;
            }

            const EventDisposition result = it->second(event);
            if (trace_) {
                trace_->Push("event." + std::string(phase) + ":" + scope.value + ":" +
                             (result == EventDisposition::Handled ? "handled" : "ignored"));
            }
            return result;
        }

        InteractionScopeGraph graph_{};
        std::unordered_map<std::string, Handler> handlers_{};
        std::optional<ScopeId> active_fallback_{};
        TraceLog* trace_{};
    };

    // ----------------------------------------------------------------------------------------------------

    enum class BindingOrigin : uint8_t {
        SystemDefault,
        Workbench,
        UserProfile,
        Project,
        Modal,
    };

    enum class ResolutionStatus : uint8_t {
        Resolved,
        NotFound,
        Conflict,
        TextInputConsumed,
        Suppressed,
    };

    struct KeyChord {
        std::string value{};

        [[nodiscard]] friend bool operator==(const KeyChord&, const KeyChord&) noexcept = default;
    };

    struct KeyBindingContext {
        WorkbenchId workbench{};
        std::string editor_kind{};
        ScopeId focused_scope{};
        bool text_input_active{};
        bool modal{};
    };

    struct KeyBindingRule {
        KeyChord chord{};
        CommandId command{};
        WorkbenchId workbench{};
        std::string editor_kind{};
        ScopeId focused_scope{};
        BindingOrigin origin{BindingOrigin::SystemDefault};
        int precedence{};
        bool modal_only{};
        bool suppressed{};
    };

    struct CommandInvocation {
        CommandId command{};
        std::string source{};
    };

    struct KeyBindingResolution {
        ResolutionStatus status{ResolutionStatus::NotFound};
        std::optional<CommandInvocation> invocation{};
        std::vector<KeyBindingRule> conflicts{};
    };

    class KeyBindingResolver {
    public:
        void Add(KeyBindingRule rule) {
            rules_.push_back(std::move(rule));
        }

        void Suppress(KeyBindingRule rule) {
            rule.suppressed = true;
            rules_.push_back(std::move(rule));
        }

        [[nodiscard]] KeyBindingResolution Resolve(const KeyChord& chord, const KeyBindingContext& context) const {
            if (context.text_input_active) {
                return KeyBindingResolution{.status = ResolutionStatus::TextInputConsumed};
            }

            std::vector<const KeyBindingRule*> matches;
            for (const KeyBindingRule& rule : rules_) {
                if (rule.chord == chord && Matches(rule, context)) {
                    matches.push_back(&rule);
                }
            }
            if (matches.empty()) {
                return KeyBindingResolution{.status = ResolutionStatus::NotFound};
            }

            std::ranges::sort(matches, [&](const KeyBindingRule* lhs, const KeyBindingRule* rhs) {
                return Score(*lhs, context) > Score(*rhs, context);
            });

            const int best = Score(*matches.front(), context);
            std::vector<KeyBindingRule> conflicts;
            for (const KeyBindingRule* match : matches) {
                if (Score(*match, context) == best && !match->suppressed) {
                    conflicts.push_back(*match);
                }
            }
            if (conflicts.empty()) {
                return KeyBindingResolution{.status = ResolutionStatus::Suppressed};
            }
            if (conflicts.size() > 1) {
                return KeyBindingResolution{.status = ResolutionStatus::Conflict, .conflicts = std::move(conflicts)};
            }

            return KeyBindingResolution{
                .status = ResolutionStatus::Resolved,
                .invocation = CommandInvocation{.command = conflicts.front().command, .source = "key:" + chord.value},
            };
        }

    private:
        [[nodiscard]] static bool Matches(const KeyBindingRule& rule, const KeyBindingContext& context) {
            if (!rule.workbench.value.empty() && rule.workbench != context.workbench) {
                return false;
            }
            if (!rule.editor_kind.empty() && rule.editor_kind != context.editor_kind) {
                return false;
            }
            if (!rule.focused_scope.value.empty() && rule.focused_scope != context.focused_scope) {
                return false;
            }
            if (rule.modal_only && !context.modal) {
                return false;
            }
            return true;
        }

        [[nodiscard]] static int Score(const KeyBindingRule& rule, const KeyBindingContext& context) {
            int score = rule.precedence;
            if (!rule.workbench.value.empty() && rule.workbench == context.workbench) {
                score += 100;
            }
            if (!rule.editor_kind.empty() && rule.editor_kind == context.editor_kind) {
                score += 50;
            }
            if (!rule.focused_scope.value.empty() && rule.focused_scope == context.focused_scope) {
                score += 25;
            }
            if (rule.modal_only && context.modal) {
                score += 20;
            }
            score += static_cast<int>(rule.origin) * 10;
            return score;
        }

        std::vector<KeyBindingRule> rules_{};
    };

    // ----------------------------------------------------------------------------------------------------

    struct CapabilityInput {
        WorkbenchId workbench{};
        SelectionSnapshot selection{};
        DocumentVersion document_version{};
        bool write_transaction_open{};
    };

    struct CapabilityResult {
        bool available{true};
        std::string reason{};

        [[nodiscard]] static CapabilityResult Available() { return CapabilityResult{.available = true}; }

        [[nodiscard]] static CapabilityResult Unavailable(std::string reason) {
            return CapabilityResult{.available = false, .reason = std::move(reason)};
        }
    };

    struct DefaultKeyBinding {
        KeyChord chord{};
        WorkbenchId workbench{};
        std::string editor_kind{};
        ScopeId focused_scope{};
        bool modal_only{};
    };

    class CommandActivation;

    using CapabilityQuery = std::function<CapabilityResult(const CapabilityInput&)>;
    using CommandFactory = std::function<std::shared_ptr<CommandActivation>(CommandId, CommandMode, TraceLog*)>;

    struct CommandDescriptor {
        CommandId id{};
        std::string title{};
        std::string icon{};
        std::vector<DefaultKeyBinding> default_key_bindings{};
        CommandMode default_mode{CommandMode::Exclusive};
        CapabilityQuery capability{[](const CapabilityInput&) { return CapabilityResult::Available(); }};
        ParameterSchema parameters{};
        CommandFactory factory{};
    };

    class CommandRegistry {
    public:
        void Register(CommandDescriptor descriptor) {
            descriptors_[descriptor.id.value] = std::move(descriptor);
        }

        [[nodiscard]] const CommandDescriptor* Find(const CommandId& id) const {
            if (const auto it = descriptors_.find(id.value); it != descriptors_.end()) {
                return &it->second;
            }
            return nullptr;
        }

        void ExportDefaultBindings(KeyBindingResolver& resolver) const {
            for (const auto& [_, descriptor] : descriptors_) {
                for (const DefaultKeyBinding& binding : descriptor.default_key_bindings) {
                    resolver.Add(KeyBindingRule{
                        .chord = binding.chord,
                        .command = descriptor.id,
                        .workbench = binding.workbench,
                        .editor_kind = binding.editor_kind,
                        .focused_scope = binding.focused_scope,
                        .origin = BindingOrigin::SystemDefault,
                        .modal_only = binding.modal_only,
                    });
                }
            }
        }

    private:
        std::unordered_map<std::string, CommandDescriptor> descriptors_{};
    };

    // ----------------------------------------------------------------------------------------------------

    class CommandActivation {
    public:
        CommandActivation(CommandId id, CommandMode mode, TraceLog* trace = nullptr)
            : id_(std::move(id)), mode_(mode), trace_(trace) {}

        [[nodiscard]] const CommandId& Id() const noexcept { return id_; }
        [[nodiscard]] CommandMode Mode() const noexcept { return mode_; }
        [[nodiscard]] ActivationState State() const noexcept { return state_; }
        [[nodiscard]] std::stop_token StopToken() const noexcept { return stop_.get_token(); }

        void Activate() {
            state_ = ActivationState::Active;
            Trace("command.activate:" + id_.value);
        }

        void Suspend() {
            state_ = ActivationState::Suspended;
            Trace("command.suspend:" + id_.value);
        }

        void Resume() {
            state_ = ActivationState::Active;
            Trace("command.resume:" + id_.value);
        }

        void Complete() {
            state_ = ActivationState::Completed;
            Trace("command.complete:" + id_.value);
        }

        void Cancel(std::string_view reason = {}) {
            state_ = ActivationState::Canceling;
            stop_.request_stop();
            Trace("command.cancel:" + id_.value + ":" + std::string(reason));
            state_ = ActivationState::Canceled;
        }

    private:
        void Trace(std::string text) {
            if (trace_) {
                trace_->Push(std::move(text));
            }
        }

        CommandId id_{};
        CommandMode mode_{CommandMode::Exclusive};
        ActivationState state_{ActivationState::Created};
        std::stop_source stop_{};
        TraceLog* trace_{};
    };

    class InteractionRouter {
    public:
        explicit InteractionRouter(const CommandRegistry& registry, TraceLog* trace = nullptr)
            : registry_(registry), trace_(trace) {}

        [[nodiscard]] Completion<std::shared_ptr<CommandActivation>> Invoke(const CommandInvocation& invocation,
                                                                            const CapabilityInput& input) {
            const CommandDescriptor* descriptor = registry_.Find(invocation.command);
            if (!descriptor) {
                return Completion<std::shared_ptr<CommandActivation>>::Error(
                    CommandError::Make(ErrorCode::NotFound, "command not found: " + invocation.command.value));
            }

            CapabilityResult capability = descriptor->capability(input);
            if (!capability.available) {
                return Completion<std::shared_ptr<CommandActivation>>::Error(
                    CommandError::Make(ErrorCode::Unavailable, capability.reason));
            }

            auto activation = descriptor->factory
                                  ? descriptor->factory(descriptor->id, descriptor->default_mode, trace_)
                                  : std::make_shared<CommandActivation>(descriptor->id,
                                                                        descriptor->default_mode,
                                                                        trace_);
            ApplyMode(*activation);
            stack_.push_back(activation);
            activation->Activate();
            return Completion<std::shared_ptr<CommandActivation>>::Value(std::move(activation));
        }

        void FinishTop() {
            if (stack_.empty()) {
                return;
            }
            stack_.back()->Complete();
            stack_.pop_back();
            if (!stack_.empty() && stack_.back()->State() == ActivationState::Suspended) {
                stack_.back()->Resume();
            }
        }

        [[nodiscard]] const std::vector<std::shared_ptr<CommandActivation>>& Stack() const noexcept { return stack_; }

    private:
        void ApplyMode(CommandActivation& next) {
            if (next.Mode() == CommandMode::Passive) {
                return;
            }

            if (next.Mode() == CommandMode::Exclusive) {
                for (auto& activation : stack_) {
                    activation->Cancel("exclusive-replacement");
                }
                stack_.clear();
                return;
            }

            if (next.Mode() == CommandMode::Shared && !stack_.empty()) {
                stack_.back()->Suspend();
            }
        }

        const CommandRegistry& registry_;
        TraceLog* trace_{};
        std::vector<std::shared_ptr<CommandActivation>> stack_{};
    };

    // ----------------------------------------------------------------------------------------------------

    struct ComputeValue {
        std::any value{};
        DocumentVersion version{};
    };

    struct ComputeContext {
        ComputeInput input{};
        std::unordered_map<std::string, ComputeValue> values{};

        template<typename T>
        [[nodiscard]] const T& Get(std::string_view node) const {
            return std::any_cast<const T&>(values.at(std::string(node)).value);
        }
    };

    struct ComputeNode {
        std::string id{};
        std::vector<std::string> depends_on{};
        std::function<Completion<std::any>(ComputeContext&)> run{};
    };

    struct ComputeGraphResult {
        std::unordered_map<std::string, ComputeValue> values{};
        DocumentVersion version{};
    };

    class ComputeGraphRuntime {
    public:
        void Add(ComputeNode node) {
            nodes_[node.id] = std::move(node);
        }

        [[nodiscard]] auto Run(ComputeInput input, std::stop_token stop = {}) const {
            return MakeSender<ComputeGraphResult>([this, input = std::move(input), stop]() mutable {
                if (stop.stop_requested()) {
                    return Completion<ComputeGraphResult>::Stop();
                }

                std::vector<std::string> order;
                std::set<std::string> visiting;
                std::set<std::string> visited;
                for (const auto& [id, _] : nodes_) {
                    if (!Visit(id, visiting, visited, order)) {
                        return Completion<ComputeGraphResult>::Error(
                            CommandError::Make(ErrorCode::CycleDetected, "compute graph contains a cycle"));
                    }
                }

                ComputeContext context{.input = input};
                for (const std::string& id : order) {
                    if (stop.stop_requested()) {
                        return Completion<ComputeGraphResult>::Stop();
                    }

                    const ComputeNode& node = nodes_.at(id);
                    Completion<std::any> value = node.run(context);
                    if (value.IsStopped()) {
                        return Completion<ComputeGraphResult>::Stop();
                    }
                    if (value.HasError()) {
                        return Completion<ComputeGraphResult>::Error(value.Error());
                    }
                    context.values[id] = ComputeValue{.value = std::move(value).Value(),
                                                      .version = input.base_version};
                }

                return Completion<ComputeGraphResult>::Value(
                    ComputeGraphResult{.values = std::move(context.values), .version = input.base_version});
            });
        }

        template<UiAffine T>
        [[nodiscard]] auto Run(T&&, std::stop_token = {}) const =
            delete("UI-affine state must be converted to immutable snapshots before background compute.");

    private:
        [[nodiscard]] bool Visit(const std::string& id, std::set<std::string>& visiting, std::set<std::string>& visited,
                                 std::vector<std::string>& order) const {
            if (visited.contains(id)) {
                return true;
            }
            if (visiting.contains(id)) {
                return false;
            }
            const auto it = nodes_.find(id);
            if (it == nodes_.end()) {
                return false;
            }

            visiting.insert(id);
            for (const std::string& dependency : it->second.depends_on) {
                if (!Visit(dependency, visiting, visited, order)) {
                    return false;
                }
            }
            visiting.erase(id);
            visited.insert(id);
            order.push_back(id);
            return true;
        }

        std::unordered_map<std::string, ComputeNode> nodes_{};
    };

    // ----------------------------------------------------------------------------------------------------

    struct StateObjectId {
        std::string value{};

        [[nodiscard]] friend bool operator==(const StateObjectId&, const StateObjectId&) noexcept = default;
        [[nodiscard]] friend auto operator<=>(const StateObjectId&, const StateObjectId&) noexcept = default;
        [[nodiscard]] bool Empty() const noexcept { return value.empty(); }
    };

    struct StateObject {
        StateObjectId id{};
        std::string kind{};
        std::map<std::string, double> parameters{};
    };

    using Feature = StateObject;

    struct DocumentPatch {
        std::vector<Feature> add_features{};
    };

    struct StateObjectUpdate {
        StateObjectId id{};
        std::optional<std::string> kind{};
        std::map<std::string, double> set_parameters{};
        std::vector<std::string> erase_parameters{};

        [[nodiscard]] bool Empty() const noexcept {
            return !kind && set_parameters.empty() && erase_parameters.empty();
        }
    };

    struct ChangeSet {
        std::vector<StateObject> inserts{};
        std::vector<StateObjectUpdate> updates{};
        std::vector<StateObjectId> erases{};

        [[nodiscard]] bool Empty() const noexcept {
            return inserts.empty() && updates.empty() && erases.empty();
        }

        [[nodiscard]] static ChangeSet FromPatch(DocumentPatch patch) {
            return ChangeSet{.inserts = std::move(patch.add_features)};
        }
    };

    struct AppliedChangeSummary {
        std::vector<StateObjectId> inserted{};
        std::vector<StateObjectId> updated{};
        std::vector<StateObjectId> erased{};

        [[nodiscard]] std::size_t OperationCount() const noexcept {
            return inserted.size() + updated.size() + erased.size();
        }
    };

    struct IndexedStateObject {
        std::size_t index{};
        StateObject object{};
    };

    struct AppliedChange {
        AppliedChangeSummary summary{};
        std::vector<StateObject> inserted_after{};
        std::vector<StateObject> updated_before{};
        std::vector<IndexedStateObject> erased_before{};

        [[nodiscard]] std::size_t OperationCount() const noexcept { return summary.OperationCount(); }
    };

    class DocumentModel {
    public:
        [[nodiscard]] DocumentVersion Version() const noexcept { return version_; }
        [[nodiscard]] std::span<const Feature> Features() const noexcept { return std::span<const Feature>{features_}; }
        [[nodiscard]] std::span<const StateObject> Objects() const noexcept {
            return std::span<const StateObject>{features_};
        }

        [[nodiscard]] const StateObject* Find(StateObjectId id) const {
            const auto index = feature_index_.find(id.value);
            if (index == feature_index_.end()) {
                return nullptr;
            }
            return &features_[index->second];
        }

    private:
        friend class DocumentTransactionRuntime;

        [[nodiscard]] bool Contains(const StateObjectId& id) const {
            return !id.Empty() && feature_index_.contains(id.value);
        }

        [[nodiscard]] StateObjectId AllocateId() {
            return StateObjectId{.value = "object:" + std::to_string(next_object_id_++)};
        }

        [[nodiscard]] StateObject Snapshot(const StateObjectId& id) const {
            return features_[feature_index_.at(id.value)];
        }

        [[nodiscard]] IndexedStateObject IndexedSnapshot(const StateObjectId& id) const {
            const std::size_t index = feature_index_.at(id.value);
            return IndexedStateObject{.index = index, .object = features_[index]};
        }

        AppliedChange Apply(ChangeSet change) {
            AppliedChange applied;
            for (const StateObjectId& id : change.erases) {
                applied.erased_before.push_back(IndexedSnapshot(id));
                Erase(id);
                applied.summary.erased.push_back(id);
            }
            for (const StateObjectUpdate& update : change.updates) {
                applied.updated_before.push_back(Snapshot(update.id));
                Update(update);
                applied.summary.updated.push_back(update.id);
            }
            for (StateObject& object : change.inserts) {
                if (object.id.Empty()) {
                    object.id = AllocateId();
                }
                applied.summary.inserted.push_back(object.id);
                applied.inserted_after.push_back(object);
                feature_index_[object.id.value] = features_.size();
                features_.push_back(std::move(object));
            }
            ++version_.value;
            return applied;
        }

        void UndoAppliedChange(std::span<const StateObject> inserted_after,
                               std::span<const StateObject> updated_before,
                               std::span<const IndexedStateObject> erased_before) {
            for (std::size_t i = inserted_after.size(); i > 0; --i) {
                Erase(inserted_after[i - 1].id);
            }
            for (std::size_t i = updated_before.size(); i > 0; --i) {
                Replace(updated_before[i - 1]);
            }
            for (std::size_t i = erased_before.size(); i > 0; --i) {
                RestoreErased(erased_before[i - 1]);
            }
            ++version_.value;
        }

        void Update(const StateObjectUpdate& update) {
            StateObject& object = features_[feature_index_.at(update.id.value)];
            if (update.kind) {
                object.kind = *update.kind;
            }
            for (const auto& [name, value] : update.set_parameters) {
                object.parameters[name] = value;
            }
            for (const std::string& name : update.erase_parameters) {
                object.parameters.erase(name);
            }
        }

        void Erase(const StateObjectId& id) {
            const std::size_t index = feature_index_.at(id.value);
            features_.erase(features_.begin() + static_cast<std::ptrdiff_t>(index));
            RebuildIndex();
        }

        void Replace(StateObject object) {
            features_[feature_index_.at(object.id.value)] = std::move(object);
        }

        void RestoreErased(IndexedStateObject erased) {
            const std::size_t index = std::min(erased.index, features_.size());
            features_.insert(features_.begin() + static_cast<std::ptrdiff_t>(index), std::move(erased.object));
            RebuildIndex();
        }

        void RebuildIndex() {
            feature_index_.clear();
            for (std::size_t i = 0; i < features_.size(); ++i) {
                if (!features_[i].id.Empty()) {
                    feature_index_[features_[i].id.value] = i;
                }
            }
        }

        DocumentVersion version_{};
        std::vector<Feature> features_{};
        std::unordered_map<std::string, std::size_t> feature_index_{};
        uint64_t next_object_id_{1};
    };

    struct TransactionRequest {
        DocumentVersion base_version{};
        std::string label{};
    };

    struct UndoRecord {
        std::size_t id{};
        std::string label{};
        DocumentVersion base_version{};
        DocumentVersion new_version{};
        DocumentVersion undo_from_version{};
        AppliedChangeSummary summary{};
        std::vector<StateObject> inserted_after{};
        std::vector<StateObject> updated_before{};
        std::vector<IndexedStateObject> erased_before{};

        [[nodiscard]] std::size_t OperationCount() const noexcept { return summary.OperationCount(); }
    };

    struct TransactionResult {
        DocumentVersion new_version{};
        std::size_t undo_record{};
        std::size_t applied_operations{};
    };

    class DocumentTransactionRuntime {
    public:
        explicit DocumentTransactionRuntime(DocumentModel& document, TraceLog* trace = nullptr)
            : document_(document), trace_(trace) {}

        [[nodiscard]] std::span<const UndoRecord> UndoRecords() const noexcept {
            return std::span<const UndoRecord>{undo_records_};
        }

        [[nodiscard]] const UndoRecord* FindUndoRecord(std::size_t id) const {
            const auto it = std::ranges::find_if(undo_records_, [id](const UndoRecord& record) {
                return record.id == id;
            });
            return it == undo_records_.end() ? nullptr : std::addressof(*it);
        }

        [[nodiscard]] auto Commit(TransactionRequest request, ChangeSet change) {
            return MakeSender<TransactionResult>([this, request = std::move(request), change = std::move(change)] {
                if (request.base_version != document_.Version()) {
                    return Completion<TransactionResult>::Error(
                        CommandError::Make(ErrorCode::VersionMismatch, "document version changed before commit"));
                }
                if (change.Empty()) {
                    return Completion<TransactionResult>::Error(
                        CommandError::Make(ErrorCode::InvalidInput, "empty change set"));
                }
                if (std::optional<CommandError> conflict = Validate(change)) {
                    return Completion<TransactionResult>::Error(std::move(*conflict));
                }

                AppliedChange applied = document_.Apply(std::move(change));
                const std::size_t undo_record = undo_records_.size() + 1;
                undo_records_.push_back(UndoRecord{.id = undo_record,
                                                   .label = request.label,
                                                   .base_version = request.base_version,
                                                   .new_version = document_.Version(),
                                                   .undo_from_version = document_.Version(),
                                                   .summary = std::move(applied.summary),
                                                   .inserted_after = std::move(applied.inserted_after),
                                                   .updated_before = std::move(applied.updated_before),
                                                   .erased_before = std::move(applied.erased_before)});
                if (trace_) {
                    trace_->Push("transaction.commit:" + request.label);
                }
                return Completion<TransactionResult>::Value(
                    TransactionResult{.new_version = document_.Version(),
                                      .undo_record = undo_record,
                                      .applied_operations = undo_records_.back().OperationCount()});
            });
        }

        [[nodiscard]] auto Commit(TransactionRequest request, DocumentPatch patch) {
            return Commit(std::move(request), ChangeSet::FromPatch(std::move(patch)));
        }

        [[nodiscard]] auto UndoLast() {
            return MakeSender<TransactionResult>([this] {
                if (undo_records_.empty()) {
                    return Completion<TransactionResult>::Error(
                        CommandError::Make(ErrorCode::NotFound, "no undo record"));
                }

                const UndoRecord& last = undo_records_.back();
                if (last.undo_from_version != document_.Version()) {
                    return Completion<TransactionResult>::Error(
                        CommandError::Make(ErrorCode::VersionMismatch, "document version changed before undo"));
                }
                if (std::optional<CommandError> invalid = ValidateUndo(last)) {
                    return Completion<TransactionResult>::Error(std::move(*invalid));
                }

                const std::size_t undo_record = last.id;
                const std::size_t applied_operations = last.OperationCount();
                const std::string label = last.label;
                document_.UndoAppliedChange(last.inserted_after, last.updated_before, last.erased_before);
                undo_records_.pop_back();
                if (!undo_records_.empty()) {
                    undo_records_.back().undo_from_version = document_.Version();
                }
                if (trace_) {
                    trace_->Push("transaction.undo:" + label);
                }
                return Completion<TransactionResult>::Value(TransactionResult{
                    .new_version = document_.Version(),
                    .undo_record = undo_record,
                    .applied_operations = applied_operations,
                });
            });
        }

    private:
        [[nodiscard]] std::optional<CommandError> Validate(const ChangeSet& change) const {
            std::set<std::string> inserted_ids;
            std::set<std::string> erased_ids;
            for (const StateObject& object : change.inserts) {
                if (!object.id.Empty()) {
                    if (document_.Contains(object.id) || inserted_ids.contains(object.id.value)) {
                        return CommandError::Make(ErrorCode::Conflict, "inserted object id already exists");
                    }
                    inserted_ids.insert(object.id.value);
                }
            }
            for (const StateObjectId& id : change.erases) {
                if (id.Empty() || !document_.Contains(id)) {
                    return CommandError::Make(ErrorCode::Conflict, "erased object does not exist");
                }
                if (erased_ids.contains(id.value)) {
                    return CommandError::Make(ErrorCode::Conflict, "object erased more than once");
                }
                erased_ids.insert(id.value);
            }
            for (const StateObjectUpdate& update : change.updates) {
                if (update.id.Empty() || !document_.Contains(update.id)) {
                    return CommandError::Make(ErrorCode::Conflict, "updated object does not exist");
                }
                if (erased_ids.contains(update.id.value)) {
                    return CommandError::Make(ErrorCode::Conflict, "object cannot be updated and erased together");
                }
                if (update.Empty()) {
                    return CommandError::Make(ErrorCode::InvalidInput, "empty object update");
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<CommandError> ValidateUndo(const UndoRecord& record) const {
            for (const StateObject& inserted : record.inserted_after) {
                if (inserted.id.Empty() || !document_.Contains(inserted.id)) {
                    return CommandError::Make(ErrorCode::InternalError, "undo inserted object is missing");
                }
            }
            for (const StateObject& updated : record.updated_before) {
                if (updated.id.Empty() || !document_.Contains(updated.id)) {
                    return CommandError::Make(ErrorCode::InternalError, "undo updated object is missing");
                }
            }
            for (const IndexedStateObject& erased : record.erased_before) {
                if (erased.object.id.Empty() || document_.Contains(erased.object.id)) {
                    return CommandError::Make(ErrorCode::InternalError, "undo erased object already exists");
                }
            }
            return std::nullopt;
        }

        DocumentModel& document_;
        TraceLog* trace_{};
        std::vector<UndoRecord> undo_records_{};
    };

    // ----------------------------------------------------------------------------------------------------

    struct DemoResult {
        TraceLog trace{};
        DocumentModel document{};
        KeyBindingResolution line_resolution{};
        KeyBindingResolution suppressed_line_resolution{};
        KeyBindingResolution measure_resolution{};
        KeyBindingResolution modal_resolution{};
        EventDisposition selection_event{EventDisposition::Ignored};
        EventDisposition fallback_event{EventDisposition::Ignored};
        std::size_t stack_depth_during_measure{};
        std::size_t undo_record{};
        std::size_t undo_record_operations{};
        std::size_t undone_record{};
        std::size_t undo_applied_operations{};
    };

    [[nodiscard]] inline DemoResult RunDemoScenario() {
        DemoResult result;

        CommandRegistry registry;
        registry.Register(CommandDescriptor{
            .id = CommandId{"geometry.create_line"},
            .title = "Create Line",
            .icon = "line",
            .default_key_bindings = {DefaultKeyBinding{.chord = KeyChord{"L"}, .workbench = WorkbenchId{"Sketcher"}}},
            .default_mode = CommandMode::Exclusive,
        });
        registry.Register(CommandDescriptor{
            .id = CommandId{"view.quick_measure"},
            .title = "Quick Measure",
            .icon = "measure",
            .default_key_bindings = {DefaultKeyBinding{.chord = KeyChord{"M"}, .workbench = WorkbenchId{"Sketcher"}}},
            .default_mode = CommandMode::Shared,
        });

        KeyBindingResolver resolver;
        registry.ExportDefaultBindings(resolver);
        resolver.Suppress(KeyBindingRule{
            .chord = KeyChord{"L"},
            .command = CommandId{"geometry.create_line"},
            .workbench = WorkbenchId{"Sketcher"},
            .editor_kind = "Sketch",
            .focused_scope = ScopeId{"viewer"},
            .origin = BindingOrigin::UserProfile,
            .precedence = 100,
        });
        resolver.Add(KeyBindingRule{
            .chord = KeyChord{"P"},
            .command = CommandId{"geometry.create_line"},
            .workbench = WorkbenchId{"Sketcher"},
            .editor_kind = "Sketch",
            .focused_scope = ScopeId{"viewer"},
            .origin = BindingOrigin::UserProfile,
            .precedence = 100,
        });
        resolver.Add(KeyBindingRule{
            .chord = KeyChord{"Enter"},
            .command = CommandId{"geometry.create_line"},
            .origin = BindingOrigin::Modal,
            .precedence = 1000,
            .modal_only = true,
        });

        const KeyBindingContext sketcher{
            .workbench = WorkbenchId{"Sketcher"},
            .editor_kind = "Sketch",
            .focused_scope = ScopeId{"viewer"},
        };
        result.suppressed_line_resolution = resolver.Resolve(KeyChord{"L"}, sketcher);
        result.line_resolution = resolver.Resolve(KeyChord{"P"}, sketcher);
        result.measure_resolution = resolver.Resolve(KeyChord{"M"}, sketcher);

        InteractionRouter router(registry, &result.trace);
        CapabilityInput capability{
            .workbench = WorkbenchId{"Sketcher"},
            .selection = SelectionSnapshot{.stable_refs = {"point:A", "point:B"}, .version = result.document.Version()},
            .document_version = result.document.Version(),
        };
        auto activation = router.Invoke(*result.line_resolution.invocation, capability);
        assert(activation.HasValue());

        auto measure = router.Invoke(*result.measure_resolution.invocation, capability);
        assert(measure.HasValue());
        result.stack_depth_during_measure = router.Stack().size();
        router.FinishTop();

        InteractionEventRouter event_router(&result.trace);
        event_router.AddScope(InteractionScope{.id = ScopeId{"app"}, .label = "Application"});
        event_router.AddScope(InteractionScope{.id = ScopeId{"editor"},
                                               .parent = ScopeId{"app"},
                                               .label = "Sketch Editor"});
        event_router.AddScope(InteractionScope{.id = ScopeId{"viewer"},
                                               .parent = ScopeId{"editor"},
                                               .label = "Viewer"});
        event_router.AddScope(InteractionScope{.id = ScopeId{"line-command"},
                                               .parent = ScopeId{"editor"},
                                               .label = "Line Command"});
        event_router.On(ScopeId{"editor"}, [](const InputEvent& event) {
            return event.payload == "select" ? EventDisposition::Handled : EventDisposition::Ignored;
        });
        event_router.On(ScopeId{"line-command"}, [](const InputEvent& event) {
            return event.payload == "typed-intent" ? EventDisposition::Handled : EventDisposition::Ignored;
        });
        event_router.SetActiveFallback(ScopeId{"line-command"});
        result.selection_event = event_router.Dispatch(InputEvent{.kind = EventKind::Pointer,
                                                                  .target = ScopeId{"viewer"},
                                                                  .payload = "select"});
        result.fallback_event = event_router.Dispatch(InputEvent{.kind = EventKind::Key,
                                                                 .target = ScopeId{"viewer"},
                                                                 .payload = "typed-intent"});

        ComputeGraphRuntime graph;
        graph.Add(ComputeNode{
            .id = "line-preview",
            .run = [](ComputeContext& context) {
                const auto x = context.input.parameters.numbers.at("x");
                const auto y = context.input.parameters.numbers.at("y");
                return Completion<std::any>::Value(std::any(Feature{.kind = "LinePreview",
                                                                    .parameters = {{"x", x}, {"y", y}}}));
            },
        });

        auto graph_result = SyncWait(graph.Run(ComputeInput{
            .selection = SelectionSnapshot{.stable_refs = {"point:A", "point:B"}, .version = result.document.Version()},
            .parameters = ParameterSnapshot{.numbers = {{"x", 1.0}, {"y", 2.0}},
                                            .version = result.document.Version()},
            .base_version = result.document.Version(),
        }));
        assert(graph_result.HasValue());

        DocumentTransactionRuntime transactions(result.document, &result.trace);
        ChangeSet change;
        change.inserts.push_back(std::any_cast<Feature>(graph_result.Value().values.at("line-preview").value));
        change.inserts.back().kind = "Line";

        auto commit = SyncWait(transactions.Commit(TransactionRequest{.base_version = result.document.Version(),
                                                                     .label = "Create Line"},
                                                  std::move(change)));
        assert(commit.HasValue());
        result.undo_record = commit.Value().undo_record;
        result.undo_record_operations = commit.Value().applied_operations;

        auto undo = SyncWait(transactions.UndoLast());
        assert(undo.HasValue());
        result.undone_record = undo.Value().undo_record;
        result.undo_applied_operations = undo.Value().applied_operations;

        result.modal_resolution = resolver.Resolve(KeyChord{"Enter"},
                                                   KeyBindingContext{.workbench = WorkbenchId{"Sketcher"},
                                                                     .editor_kind = "Sketch",
                                                                     .focused_scope = ScopeId{"line"},
                                                                     .modal = true});
        router.FinishTop();
        return result;
    }

} // namespace Sora::Kernel::Experimental::CommandRuntime
