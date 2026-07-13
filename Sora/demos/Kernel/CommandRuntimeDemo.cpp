#include "Sora/Kernel/Experimental/CommandRuntime.h"
#include "Sora/Kernel/Experimental/CommandRuntimeReflection.h"
#include "Sora/Kernel/Experimental/CommandRuntimeStdexec.h"

#include <any>
#include <cassert>
#include <print>
#include <tuple>

namespace Runtime = Sora::Kernel::Experimental::CommandRuntime;

namespace {

    struct CreateLineParameters {
        [[nodiscard]] static Runtime::ParameterSchema CommandParameterSchema() {
            return Runtime::ParameterSchema{
                .name = "CreateLineParameters",
                .fields = {Runtime::ParameterField{.name = "x", .kind = Runtime::ParameterValueKind::Number},
                           Runtime::ParameterField{.name = "y", .kind = Runtime::ParameterValueKind::Number},
                           Runtime::ParameterField{.name = "construction",
                                                   .kind = Runtime::ParameterValueKind::Boolean}},
            };
        }
    };

    [[nodiscard]] Runtime::CommandRegistry BuildRegistry(const Runtime::ParameterSchema& lineParameters) {
        Runtime::CommandRegistry registry;
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"geometry.create_line"},
            .title = "Create Line",
            .icon = "line",
            .default_key_bindings = {Runtime::DefaultKeyBinding{.chord = Runtime::KeyChord{"L"},
                                                                .workbench = Runtime::WorkbenchId{"Sketcher"}}},
            .default_mode = Runtime::CommandMode::Exclusive,
            .capability = [](const Runtime::CapabilityInput& input) {
                return input.selection.stable_refs.size() >= 2
                           ? Runtime::CapabilityResult::Available()
                           : Runtime::CapabilityResult::Unavailable("two stable point references are required");
            },
            .parameters = lineParameters,
        });
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"view.quick_measure"},
            .title = "Quick Measure",
            .icon = "measure",
            .default_key_bindings = {Runtime::DefaultKeyBinding{.chord = Runtime::KeyChord{"M"},
                                                                .workbench = Runtime::WorkbenchId{"Sketcher"}}},
            .default_mode = Runtime::CommandMode::Shared,
        });
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"view.coordinate_readout"},
            .title = "Coordinate Readout",
            .icon = "cursor",
            .default_mode = Runtime::CommandMode::Passive,
        });
        return registry;
    }

    [[nodiscard]] Runtime::ComputeGraphRuntime BuildLineGraph() {
        Runtime::ComputeGraphRuntime graph;
        graph.Add(Runtime::ComputeNode{
            .id = "profile",
            .run = [](Runtime::ComputeContext& context) {
                const auto x = context.input.parameters.numbers.at("x");
                const auto y = context.input.parameters.numbers.at("y");
                return Runtime::Completion<std::any>::Value(
                    std::any(Runtime::Feature{.kind = "Profile", .parameters = {{"x", x}, {"y", y}}}));
            },
        });
        graph.Add(Runtime::ComputeNode{
            .id = "line",
            .depends_on = {"profile"},
            .run = [](Runtime::ComputeContext& context) {
                Runtime::Feature feature = context.Get<Runtime::Feature>("profile");
                feature.kind = "Line";
                feature.parameters["length"] = 42.0;
                return Runtime::Completion<std::any>::Value(std::any(feature));
            },
        });
        return graph;
    }

    [[nodiscard]] Runtime::Completion<Runtime::ComputeGraphResult> RunPreview(Runtime::ComputeGraphRuntime& graph,
                                                                              Runtime::ComputeInput input) {
#if SORA_COMMAND_RUNTIME_HAS_STDEXEC
        auto sender = Runtime::StdexecBridge::AsCompletionSender(graph.Run(std::move(input)));
        auto waited = stdexec::sync_wait(std::move(sender));
        assert(waited.has_value());
        return std::move(std::get<0>(*waited));
#else
        return Runtime::SyncWait(graph.Run(std::move(input)));
#endif
    }

} // namespace

int main() {
    Runtime::TraceLog trace;
    Runtime::DocumentModel document;

    Runtime::ParameterSchema lineSchema = Runtime::Reflection::SchemaOf<CreateLineParameters>();
    Runtime::ParameterSnapshot parameters{
        .numbers = {{"x", 1.0}, {"y", 2.0}},
        .booleans = {{"construction", false}},
        .version = document.Version(),
    };
    Runtime::ParameterValidationResult validation = Runtime::ValidateParameters(lineSchema, parameters);
    assert(validation.Valid());

    Runtime::CommandRegistry registry = BuildRegistry(lineSchema);
    Runtime::KeyBindingResolver bindings;
    registry.ExportDefaultBindings(bindings);
    bindings.Suppress(Runtime::KeyBindingRule{
        .chord = Runtime::KeyChord{"L"},
        .command = Runtime::CommandId{"geometry.create_line"},
        .workbench = Runtime::WorkbenchId{"Sketcher"},
        .editor_kind = "Sketch",
        .focused_scope = Runtime::ScopeId{"viewer"},
        .origin = Runtime::BindingOrigin::UserProfile,
        .precedence = 100,
    });
    bindings.Add(Runtime::KeyBindingRule{
        .chord = Runtime::KeyChord{"P"},
        .command = Runtime::CommandId{"geometry.create_line"},
        .workbench = Runtime::WorkbenchId{"Sketcher"},
        .editor_kind = "Sketch",
        .focused_scope = Runtime::ScopeId{"viewer"},
        .origin = Runtime::BindingOrigin::UserProfile,
        .precedence = 100,
    });
    bindings.Add(Runtime::KeyBindingRule{
        .chord = Runtime::KeyChord{"Enter"},
        .command = Runtime::CommandId{"geometry.create_line"},
        .origin = Runtime::BindingOrigin::Modal,
        .precedence = 1000,
        .modal_only = true,
    });

    Runtime::KeyBindingContext sketcher{
        .workbench = Runtime::WorkbenchId{"Sketcher"},
        .editor_kind = "Sketch",
        .focused_scope = Runtime::ScopeId{"viewer"},
    };
    Runtime::KeyBindingResolution suppressedLine = bindings.Resolve(Runtime::KeyChord{"L"}, sketcher);
    Runtime::KeyBindingResolution line = bindings.Resolve(Runtime::KeyChord{"P"}, sketcher);
    Runtime::KeyBindingResolution measure = bindings.Resolve(Runtime::KeyChord{"M"}, sketcher);
    assert(suppressedLine.status == Runtime::ResolutionStatus::Suppressed);
    assert(line.status == Runtime::ResolutionStatus::Resolved);
    assert(measure.status == Runtime::ResolutionStatus::Resolved);

    Runtime::CapabilityInput capability{
        .workbench = Runtime::WorkbenchId{"Sketcher"},
        .selection = Runtime::SelectionSnapshot{.stable_refs = {"point:A", "point:B"}, .version = document.Version()},
        .document_version = document.Version(),
    };
    Runtime::InteractionRouter commandRouter(registry, &trace);
    auto lineActivation = commandRouter.Invoke(*line.invocation, capability);
    auto measureActivation = commandRouter.Invoke(*measure.invocation, capability);
    assert(lineActivation.HasValue());
    assert(measureActivation.HasValue());
    assert(commandRouter.Stack().size() == 2);
    assert(commandRouter.Stack().front()->State() == Runtime::ActivationState::Suspended);
    commandRouter.FinishTop();
    assert(commandRouter.Stack().back()->State() == Runtime::ActivationState::Active);

    Runtime::InteractionEventRouter eventRouter(&trace);
    eventRouter.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"app"}, .label = "Application"});
    eventRouter.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"editor"},
                                                  .parent = Runtime::ScopeId{"app"},
                                                  .label = "Sketch Editor"});
    eventRouter.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"viewer"},
                                                  .parent = Runtime::ScopeId{"editor"},
                                                  .label = "Viewer"});
    eventRouter.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"line-command"},
                                                  .parent = Runtime::ScopeId{"editor"},
                                                  .label = "Line Command"});
    eventRouter.On(Runtime::ScopeId{"editor"}, [](const Runtime::InputEvent& event) {
        return event.payload == "select" ? Runtime::EventDisposition::Handled : Runtime::EventDisposition::Ignored;
    });
    eventRouter.On(Runtime::ScopeId{"line-command"}, [](const Runtime::InputEvent& event) {
        return event.payload == "typed-intent" ? Runtime::EventDisposition::Handled
                                               : Runtime::EventDisposition::Ignored;
    });
    eventRouter.SetActiveFallback(Runtime::ScopeId{"line-command"});
    assert(eventRouter.Dispatch(Runtime::InputEvent{.kind = Runtime::EventKind::Pointer,
                                                    .target = Runtime::ScopeId{"viewer"},
                                                    .payload = "select"}) == Runtime::EventDisposition::Handled);
    assert(eventRouter.Dispatch(Runtime::InputEvent{.kind = Runtime::EventKind::Key,
                                                    .target = Runtime::ScopeId{"viewer"},
                                                    .payload = "typed-intent"}) ==
           Runtime::EventDisposition::Handled);

    Runtime::ComputeGraphRuntime graph = BuildLineGraph();
    Runtime::Completion<Runtime::ComputeGraphResult> preview = RunPreview(
        graph, Runtime::ComputeInput{.selection = capability.selection,
                                     .parameters = parameters,
                                     .base_version = document.Version()});
    assert(preview.HasValue());

    Runtime::ChangeSet change;
    change.inserts.push_back(std::any_cast<Runtime::Feature>(preview.Value().values.at("line").value));
    Runtime::DocumentTransactionRuntime transactions(document, &trace);
    Runtime::Completion<Runtime::TransactionResult> commit = Runtime::SyncWait(transactions.Commit(
        Runtime::TransactionRequest{.base_version = document.Version(), .label = "Create Line"}, std::move(change)));
    assert(commit.HasValue());
    const Runtime::UndoRecord* undo = transactions.FindUndoRecord(commit.Value().undo_record);
    assert(undo);
    const std::size_t inserted_count = undo->summary.inserted.size();
    const std::size_t updated_count = undo->summary.updated.size();
    const std::size_t erased_count = undo->summary.erased.size();
    Runtime::Completion<Runtime::TransactionResult> undo_result = Runtime::SyncWait(transactions.UndoLast());
    assert(undo_result.HasValue());
    assert(document.Features().empty());
    assert(transactions.UndoRecords().empty());
    commandRouter.FinishTop();

    Runtime::KeyBindingResolution modal = bindings.Resolve(
        Runtime::KeyChord{"Enter"}, Runtime::KeyBindingContext{.workbench = Runtime::WorkbenchId{"Sketcher"},
                                                               .editor_kind = "Sketch",
                                                               .focused_scope = Runtime::ScopeId{"line-command"},
                                                               .modal = true});
    assert(modal.status == Runtime::ResolutionStatus::Resolved);

    std::println("Command runtime demo passed");
    std::println("  schema: {} ({} fields, reflected={})", lineSchema.name, lineSchema.fields.size(),
                 lineSchema.generated_from_reflection);
    std::println("  stdexec bridge: {}", Runtime::StdexecBridge::Status().stdexec_available ? "available" : "fallback");
    const bool lineShortcutSuppressed = suppressedLine.status == Runtime::ResolutionStatus::Suppressed;
    std::println("  suppressed default line shortcut: {}", lineShortcutSuppressed);
    std::println("  resolved: {}, {}, modal={}", line.invocation->command.value, measure.invocation->command.value,
                 modal.invocation->command.value);
    std::println("  document version: {}, features: {}", document.Version().value, document.Features().size());
    std::println("  committed undo record: {}, undone record: {}", commit.Value().undo_record,
                 undo_result.Value().undo_record);
    std::println("  undo summary: inserted={}, updated={}, erased={}", inserted_count, updated_count, erased_count);
    std::println("  trace:\n{}", trace.Join());
    return 0;
}
