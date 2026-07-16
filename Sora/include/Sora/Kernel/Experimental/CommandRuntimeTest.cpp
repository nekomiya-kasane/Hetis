/**
 * @file CommandRuntimeTest.cpp
 * @brief Standalone assertions for the experimental CAD command runtime.
 * @ingroup KernelExperimental
 */

#include "CommandRuntime.h"
#include "CommandRuntimeReflection.h"
#include "CommandRuntimeStdexec.h"

#include <any>
#include <cassert>
#include <iostream>
#include <stop_token>
#include <tuple>

namespace Runtime = Sora::Kernel::Experimental::CommandRuntime;

namespace {

    struct FakeWidget : Runtime::UiAffineTag {};

    struct LineParameterObject {
        [[nodiscard]] static Runtime::ParameterSchema CommandParameterSchema() {
            return Runtime::ParameterSchema{
                .name = "LineParameterObject",
                .fields = {Runtime::ParameterField{.name = "x", .kind = Runtime::ParameterValueKind::Number},
                           Runtime::ParameterField{.name = "y", .kind = Runtime::ParameterValueKind::Number},
                           Runtime::ParameterField{.name = "construction",
                                                   .kind = Runtime::ParameterValueKind::Boolean}},
            };
        }
    };

    void TestParameterSchemaValidation() {
        const Runtime::ParameterSchema schema = Runtime::Reflection::SchemaOf<LineParameterObject>();
        assert(schema.name == "LineParameterObject");
        assert(schema.fields.size() == 3);
        assert(!schema.generated_from_reflection);

        const auto missing = Runtime::ValidateParameters(
            schema, Runtime::ParameterSnapshot{.numbers = {{"x", 1.0}}, .version = Runtime::DocumentVersion{}});
        assert(!missing.Valid());
        assert(missing.missing_required_fields.size() == 2);

        const auto valid = Runtime::ValidateParameters(
            schema,
            Runtime::ParameterSnapshot{.numbers = {{"x", 1.0}, {"y", 2.0}},
                                       .booleans = {{"construction", false}},
                                       .version = Runtime::DocumentVersion{}});
        assert(valid.Valid());
    }

    void TestStdexecBridgeStatus() {
        const auto status = Runtime::StdexecBridge::Status();

#if SORA_COMMAND_RUNTIME_HAS_STDEXEC
        assert(status.stdexec_available);
        auto sender = Runtime::StdexecBridge::AsCompletionSender(Runtime::MakeSender<int>([] {
            return Runtime::Completion<int>::Value(7);
        }));
        auto waited = stdexec::sync_wait(std::move(sender));

        assert(waited.has_value());
        Runtime::Completion<int>& completion = std::get<0>(*waited);
        assert(completion.HasValue());
        assert(completion.Value() == 7);
#else
        assert(!status.stdexec_available);
        assert(!status.detached_start_available);
#endif
    }

    void TestContextualKeyBinding() {
        Runtime::KeyBindingResolver resolver;
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"L"},
            .command = Runtime::CommandId{"sketch.line"},
            .workbench = Runtime::WorkbenchId{"Sketcher"},
            .origin = Runtime::BindingOrigin::SystemDefault,
        });
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"L"},
            .command = Runtime::CommandId{"assembly.locate"},
            .workbench = Runtime::WorkbenchId{"Assembly"},
            .origin = Runtime::BindingOrigin::SystemDefault,
        });

        auto sketch = resolver.Resolve(Runtime::KeyChord{"L"},
                                       Runtime::KeyBindingContext{.workbench = Runtime::WorkbenchId{"Sketcher"}});
        auto assembly = resolver.Resolve(Runtime::KeyChord{"L"},
                                         Runtime::KeyBindingContext{.workbench = Runtime::WorkbenchId{"Assembly"}});
        auto text = resolver.Resolve(Runtime::KeyChord{"L"}, Runtime::KeyBindingContext{.text_input_active = true});

        assert(sketch.status == Runtime::ResolutionStatus::Resolved);
        assert(sketch.invocation->command.value == "sketch.line");
        assert(assembly.status == Runtime::ResolutionStatus::Resolved);
        assert(assembly.invocation->command.value == "assembly.locate");
        assert(text.status == Runtime::ResolutionStatus::TextInputConsumed);
    }

    void TestKeyBindingConflict() {
        Runtime::KeyBindingResolver resolver;
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"F"},
            .command = Runtime::CommandId{"feature.fillet"},
            .workbench = Runtime::WorkbenchId{"Part"},
        });
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"F"},
            .command = Runtime::CommandId{"feature.face"},
            .workbench = Runtime::WorkbenchId{"Part"},
        });

        auto resolved = resolver.Resolve(Runtime::KeyChord{"F"},
                                         Runtime::KeyBindingContext{.workbench = Runtime::WorkbenchId{"Part"}});
        assert(resolved.status == Runtime::ResolutionStatus::Conflict);
        assert(resolved.conflicts.size() == 2);
    }

    void TestUserKeyBindingOverride() {
        Runtime::KeyBindingResolver resolver;
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"L"},
            .command = Runtime::CommandId{"sketch.line"},
            .workbench = Runtime::WorkbenchId{"Sketcher"},
            .origin = Runtime::BindingOrigin::SystemDefault,
        });
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"L"},
            .command = Runtime::CommandId{"sketch.polyline"},
            .workbench = Runtime::WorkbenchId{"Sketcher"},
            .origin = Runtime::BindingOrigin::UserProfile,
        });

        auto resolved = resolver.Resolve(Runtime::KeyChord{"L"},
                                         Runtime::KeyBindingContext{.workbench = Runtime::WorkbenchId{"Sketcher"}});

        assert(resolved.status == Runtime::ResolutionStatus::Resolved);
        assert(resolved.invocation->command.value == "sketch.polyline");
    }

    void TestScopedKeyBindingSuppressionAndRebind() {
        Runtime::KeyBindingResolver resolver;
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"L"},
            .command = Runtime::CommandId{"sketch.line"},
            .workbench = Runtime::WorkbenchId{"Sketcher"},
            .origin = Runtime::BindingOrigin::SystemDefault,
        });
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"L"},
            .command = Runtime::CommandId{"sketch.local_line"},
            .workbench = Runtime::WorkbenchId{"Sketcher"},
            .focused_scope = Runtime::ScopeId{"viewer"},
            .origin = Runtime::BindingOrigin::Project,
        });
        resolver.Suppress(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"L"},
            .command = Runtime::CommandId{"sketch.local_line"},
            .workbench = Runtime::WorkbenchId{"Sketcher"},
            .focused_scope = Runtime::ScopeId{"viewer"},
            .origin = Runtime::BindingOrigin::UserProfile,
            .precedence = 100,
        });
        resolver.Add(Runtime::KeyBindingRule{
            .chord = Runtime::KeyChord{"P"},
            .command = Runtime::CommandId{"sketch.local_line"},
            .workbench = Runtime::WorkbenchId{"Sketcher"},
            .focused_scope = Runtime::ScopeId{"viewer"},
            .origin = Runtime::BindingOrigin::UserProfile,
            .precedence = 100,
        });

        const Runtime::KeyBindingContext viewer{.workbench = Runtime::WorkbenchId{"Sketcher"},
                                               .focused_scope = Runtime::ScopeId{"viewer"}};
        const Runtime::KeyBindingContext tree{.workbench = Runtime::WorkbenchId{"Sketcher"},
                                             .focused_scope = Runtime::ScopeId{"tree"}};

        auto suppressed = resolver.Resolve(Runtime::KeyChord{"L"}, viewer);
        auto rebound = resolver.Resolve(Runtime::KeyChord{"P"}, viewer);
        auto fallback = resolver.Resolve(Runtime::KeyChord{"L"}, tree);

        assert(suppressed.status == Runtime::ResolutionStatus::Suppressed);
        assert(rebound.status == Runtime::ResolutionStatus::Resolved);
        assert(rebound.invocation->command.value == "sketch.local_line");
        assert(fallback.status == Runtime::ResolutionStatus::Resolved);
        assert(fallback.invocation->command.value == "sketch.line");
    }

    void TestInteractionEventRouting() {
        Runtime::TraceLog trace;
        Runtime::InteractionEventRouter router(&trace);
        router.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"app"}, .label = "Application"});
        router.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"editor"},
                                                  .parent = Runtime::ScopeId{"app"},
                                                  .label = "Editor"});
        router.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"viewer"},
                                                  .parent = Runtime::ScopeId{"editor"},
                                                  .label = "Viewer"});
        router.AddScope(Runtime::InteractionScope{.id = Runtime::ScopeId{"active-command"},
                                                  .parent = Runtime::ScopeId{"editor"},
                                                  .label = "Active Command"});

        bool editor_handled_select = false;
        bool fallback_handled = false;
        router.On(Runtime::ScopeId{"viewer"}, [](const Runtime::InputEvent&) {
            return Runtime::EventDisposition::Ignored;
        });
        router.On(Runtime::ScopeId{"editor"}, [&](const Runtime::InputEvent& event) {
            const bool handled = event.payload == "select";
            editor_handled_select = editor_handled_select || handled;
            return handled ? Runtime::EventDisposition::Handled : Runtime::EventDisposition::Ignored;
        });
        router.On(Runtime::ScopeId{"active-command"}, [&](const Runtime::InputEvent& event) {
            fallback_handled = event.payload == "typed-intent";
            return fallback_handled ? Runtime::EventDisposition::Handled : Runtime::EventDisposition::Ignored;
        });
        router.SetActiveFallback(Runtime::ScopeId{"active-command"});

        auto selected = router.Dispatch(Runtime::InputEvent{.kind = Runtime::EventKind::Pointer,
                                                            .target = Runtime::ScopeId{"viewer"},
                                                            .payload = "select"});
        auto fallback = router.Dispatch(Runtime::InputEvent{.kind = Runtime::EventKind::Key,
                                                            .target = Runtime::ScopeId{"viewer"},
                                                            .payload = "typed-intent"});

        assert(selected == Runtime::EventDisposition::Handled);
        assert(fallback == Runtime::EventDisposition::Handled);
        assert(editor_handled_select);
        assert(fallback_handled);
        assert(!trace.records.empty());
    }

    void TestRouterModes() {
        Runtime::TraceLog trace;
        Runtime::CommandRegistry registry;
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"edit.extrude"},
            .default_mode = Runtime::CommandMode::Exclusive,
        });
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"view.measure"},
            .default_mode = Runtime::CommandMode::Shared,
        });

        Runtime::InteractionRouter router(registry, &trace);
        Runtime::CapabilityInput capability;

        auto extrude = router.Invoke(Runtime::CommandInvocation{.command = Runtime::CommandId{"edit.extrude"}},
                                     capability);
        assert(extrude.HasValue());
        assert(router.Stack().size() == 1);
        assert(router.Stack().back()->State() == Runtime::ActivationState::Active);

        auto measure = router.Invoke(Runtime::CommandInvocation{.command = Runtime::CommandId{"view.measure"}},
                                     capability);
        assert(measure.HasValue());
        assert(router.Stack().size() == 2);
        assert(router.Stack().front()->State() == Runtime::ActivationState::Suspended);
        assert(router.Stack().back()->State() == Runtime::ActivationState::Active);

        router.FinishTop();
        assert(router.Stack().size() == 1);
        assert(router.Stack().back()->State() == Runtime::ActivationState::Active);
    }

    void TestCommandCapabilityRejection() {
        Runtime::CommandRegistry registry;
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"edit.delete"},
            .capability = [](const Runtime::CapabilityInput& input) {
                return input.selection.stable_refs.empty()
                           ? Runtime::CapabilityResult::Unavailable("selection required")
                           : Runtime::CapabilityResult::Available();
            },
        });

        Runtime::InteractionRouter router(registry);
        auto rejected = router.Invoke(Runtime::CommandInvocation{.command = Runtime::CommandId{"edit.delete"}},
                                      Runtime::CapabilityInput{});

        assert(rejected.HasError());
        assert(rejected.Error().code == Runtime::ErrorCode::Unavailable);
        assert(router.Stack().empty());
    }

    void TestExclusiveCommandCancelsExistingStack() {
        Runtime::CommandRegistry registry;
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"edit.long_operation"},
            .default_mode = Runtime::CommandMode::Shared,
        });
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"edit.replace_all"},
            .default_mode = Runtime::CommandMode::Exclusive,
        });

        Runtime::InteractionRouter router(registry);
        auto existing = router.Invoke(Runtime::CommandInvocation{.command = Runtime::CommandId{"edit.long_operation"}},
                                      Runtime::CapabilityInput{});
        auto existing_activation = existing.Value();

        auto replacement = router.Invoke(Runtime::CommandInvocation{.command = Runtime::CommandId{"edit.replace_all"}},
                                         Runtime::CapabilityInput{});

        assert(replacement.HasValue());
        assert(existing_activation->State() == Runtime::ActivationState::Canceled);
        assert(existing_activation->StopToken().stop_requested());
        assert(router.Stack().size() == 1);
        assert(router.Stack().back()->Id().value == "edit.replace_all");
    }

    void TestPassiveCommandDoesNotSuspendActiveCommand() {
        Runtime::CommandRegistry registry;
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"edit.drag"},
            .default_mode = Runtime::CommandMode::Exclusive,
        });
        registry.Register(Runtime::CommandDescriptor{
            .id = Runtime::CommandId{"view.coordinate_readout"},
            .default_mode = Runtime::CommandMode::Passive,
        });

        Runtime::InteractionRouter router(registry);
        auto drag = router.Invoke(Runtime::CommandInvocation{.command = Runtime::CommandId{"edit.drag"}},
                                  Runtime::CapabilityInput{});
        auto passive = router.Invoke(
            Runtime::CommandInvocation{.command = Runtime::CommandId{"view.coordinate_readout"}},
            Runtime::CapabilityInput{});

        assert(drag.HasValue());
        assert(passive.HasValue());
        assert(router.Stack().size() == 2);
        assert(router.Stack().front()->State() == Runtime::ActivationState::Active);
        assert(router.Stack().back()->State() == Runtime::ActivationState::Active);

        router.FinishTop();
        assert(router.Stack().size() == 1);
        assert(router.Stack().back()->Id().value == "edit.drag");
        assert(router.Stack().back()->State() == Runtime::ActivationState::Active);
    }

    void TestComputeGraphCycleDetection() {
        Runtime::ComputeGraphRuntime graph;
        graph.Add(Runtime::ComputeNode{
            .id = "a",
            .depends_on = {"b"},
            .run = [](Runtime::ComputeContext&) {
                return Runtime::Completion<std::any>::Value(std::any(1));
            },
        });
        graph.Add(Runtime::ComputeNode{
            .id = "b",
            .depends_on = {"a"},
            .run = [](Runtime::ComputeContext&) {
                return Runtime::Completion<std::any>::Value(std::any(2));
            },
        });

        auto result = Runtime::SyncWait(graph.Run(Runtime::ComputeInput{}));

        assert(result.HasError());
        assert(result.Error().code == Runtime::ErrorCode::CycleDetected);
    }

    void TestComputeGraphStopToken() {
        Runtime::ComputeGraphRuntime graph;
        graph.Add(Runtime::ComputeNode{
            .id = "slow",
            .run = [](Runtime::ComputeContext&) {
                return Runtime::Completion<std::any>::Value(std::any(1));
            },
        });

        std::stop_source stop;
        stop.request_stop();
        auto result = Runtime::SyncWait(graph.Run(Runtime::ComputeInput{}, stop.get_token()));

        assert(result.IsStopped());
    }

    void TestComputeGraphAndTransaction() {
        Runtime::DocumentModel document;
        Runtime::TraceLog trace;
        Runtime::ComputeGraphRuntime graph;
        graph.Add(Runtime::ComputeNode{
            .id = "profile",
            .run = [](Runtime::ComputeContext& context) {
                return Runtime::Completion<std::any>::Value(
                    std::any(Runtime::Feature{.kind = "Profile",
                                              .parameters = context.input.parameters.numbers}));
            },
        });
        graph.Add(Runtime::ComputeNode{
            .id = "extrude",
            .depends_on = {"profile"},
            .run = [](Runtime::ComputeContext& context) {
                auto profile = context.Get<Runtime::Feature>("profile");
                Runtime::Feature feature{.kind = "Extrude", .parameters = profile.parameters};
                feature.parameters["depth"] = 10.0;
                return Runtime::Completion<std::any>::Value(std::any(feature));
            },
        });

        auto computed = Runtime::SyncWait(graph.Run(Runtime::ComputeInput{
            .selection = Runtime::SelectionSnapshot{.stable_refs = {"face:1"}, .version = document.Version()},
            .parameters = Runtime::ParameterSnapshot{.numbers = {{"width", 2.0}}, .version = document.Version()},
            .base_version = document.Version(),
        }));
        assert(computed.HasValue());

        Runtime::DocumentPatch patch;
        patch.add_features.push_back(std::any_cast<Runtime::Feature>(computed.Value().values.at("extrude").value));

        Runtime::DocumentTransactionRuntime transactions(document, &trace);
        auto commit = Runtime::SyncWait(transactions.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "Extrude"}, std::move(patch)));
        assert(commit.HasValue());
        assert(document.Version().value == 1);
        assert(document.Features().size() == 1);

        auto stale = Runtime::SyncWait(transactions.Commit(Runtime::TransactionRequest{.base_version = {0},
                                                                                      .label = "Stale"},
                                                           Runtime::DocumentPatch{.add_features = {
                                                               Runtime::Feature{.kind = "Invalid"}}}));
        assert(stale.HasError());
        assert(stale.Error().code == Runtime::ErrorCode::VersionMismatch);
    }

    void TestEmptyTransactionPatchRejected() {
        Runtime::DocumentModel document;
        Runtime::DocumentTransactionRuntime transactions(document);

        auto empty = Runtime::SyncWait(transactions.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "Empty"},
            Runtime::DocumentPatch{}));

        assert(empty.HasError());
        assert(empty.Error().code == Runtime::ErrorCode::InvalidInput);
        assert(document.Version().value == 0);
        assert(document.Features().empty());
    }

    void TestChangeSetUpdateEraseAndConflict() {
        Runtime::DocumentModel document;
        Runtime::DocumentTransactionRuntime transactions(document);

        auto inserted = Runtime::SyncWait(transactions.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "Insert"},
            Runtime::ChangeSet{.inserts = {Runtime::Feature{.kind = "Editable",
                                                            .parameters = {{"width", 1.0}, {"height", 2.0}}}}}));
        assert(inserted.HasValue());
        assert(inserted.Value().applied_operations == 1);
        assert(document.Features().size() == 1);
        const Runtime::StateObjectId id = document.Features()[0].id;
        assert(!id.Empty());
        assert(transactions.UndoRecords().size() == 1);
        assert(transactions.FindUndoRecord(inserted.Value().undo_record)->label == "Insert");
        assert(transactions.FindUndoRecord(inserted.Value().undo_record)->summary.inserted.front() == id);

        auto updated = Runtime::SyncWait(transactions.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "Update"},
            Runtime::ChangeSet{.updates = {Runtime::StateObjectUpdate{
                                   .id = id,
                                   .kind = "Edited",
                                   .set_parameters = {{"width", 3.0}},
                                   .erase_parameters = {"height"}}}}));
        assert(updated.HasValue());
        assert(document.Features()[0].kind == "Edited");
        assert(document.Features()[0].parameters.at("width") == 3.0);
        assert(!document.Features()[0].parameters.contains("height"));
        assert(transactions.UndoRecords().size() == 2);
        assert(transactions.FindUndoRecord(updated.Value().undo_record)->summary.updated.front() == id);

        auto conflict = Runtime::SyncWait(transactions.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "Conflict"},
            Runtime::ChangeSet{.updates = {Runtime::StateObjectUpdate{.id = id, .kind = "Impossible"}},
                               .erases = {id}}));
        assert(conflict.HasError());
        assert(conflict.Error().code == Runtime::ErrorCode::Conflict);
        assert(document.Features().size() == 1);
        assert(transactions.UndoRecords().size() == 2);

        auto erased = Runtime::SyncWait(transactions.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "Erase"},
            Runtime::ChangeSet{.erases = {id}}));
        assert(erased.HasValue());
        assert(erased.Value().applied_operations == 1);
        assert(document.Features().empty());
        assert(transactions.UndoRecords().size() == 3);
        assert(transactions.FindUndoRecord(erased.Value().undo_record)->summary.erased.front() == id);

        auto undo_erase = Runtime::SyncWait(transactions.UndoLast());
        assert(undo_erase.HasValue());
        assert(undo_erase.Value().undo_record == erased.Value().undo_record);
        assert(undo_erase.Value().applied_operations == 1);
        assert(document.Version().value == 4);
        assert(document.Features().size() == 1);
        assert(document.Features()[0].id == id);
        assert(document.Features()[0].kind == "Edited");
        assert(document.Features()[0].parameters.at("width") == 3.0);
        assert(transactions.UndoRecords().size() == 2);
        assert(transactions.FindUndoRecord(erased.Value().undo_record) == nullptr);

        auto undo_update = Runtime::SyncWait(transactions.UndoLast());
        assert(undo_update.HasValue());
        assert(document.Version().value == 5);
        assert(document.Features().size() == 1);
        assert(document.Features()[0].id == id);
        assert(document.Features()[0].kind == "Editable");
        assert(document.Features()[0].parameters.at("width") == 1.0);
        assert(document.Features()[0].parameters.at("height") == 2.0);
        assert(transactions.UndoRecords().size() == 1);

        auto undo_insert = Runtime::SyncWait(transactions.UndoLast());
        assert(undo_insert.HasValue());
        assert(document.Version().value == 6);
        assert(document.Features().empty());
        assert(transactions.UndoRecords().empty());

        auto empty_undo = Runtime::SyncWait(transactions.UndoLast());
        assert(empty_undo.HasError());
        assert(empty_undo.Error().code == Runtime::ErrorCode::NotFound);
    }

    void TestUndoRejectsVersionMismatch() {
        Runtime::DocumentModel document;
        Runtime::DocumentTransactionRuntime first(document);

        auto inserted = Runtime::SyncWait(first.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "First"},
            Runtime::ChangeSet{.inserts = {Runtime::Feature{.kind = "First"}}}));
        assert(inserted.HasValue());
        assert(document.Version().value == 1);

        Runtime::DocumentTransactionRuntime second(document);
        auto external = Runtime::SyncWait(second.Commit(
            Runtime::TransactionRequest{.base_version = document.Version(), .label = "External"},
            Runtime::ChangeSet{.inserts = {Runtime::Feature{.kind = "External"}}}));
        assert(external.HasValue());
        assert(document.Version().value == 2);

        auto stale_undo = Runtime::SyncWait(first.UndoLast());
        assert(stale_undo.HasError());
        assert(stale_undo.Error().code == Runtime::ErrorCode::VersionMismatch);
        assert(document.Features().size() == 2);
        assert(first.UndoRecords().size() == 1);
    }

    Runtime::CommandTask<Runtime::TransactionResult> RunCoroutineExtrude(Runtime::DocumentModel& document,
                                                                         Runtime::ComputeGraphRuntime& graph) {
        auto computed = co_await graph.Run(Runtime::ComputeInput{
            .selection = Runtime::SelectionSnapshot{.stable_refs = {"face:coroutine"}, .version = document.Version()},
            .parameters = Runtime::ParameterSnapshot{.numbers = {{"width", 3.0}}, .version = document.Version()},
            .base_version = document.Version(),
        });

        Runtime::DocumentPatch patch;
        patch.add_features.push_back(std::any_cast<Runtime::Feature>(computed.values.at("extrude").value));

        Runtime::DocumentTransactionRuntime transactions(document);
        co_return co_await transactions.Commit(Runtime::TransactionRequest{.base_version = document.Version(),
                                                                          .label = "Coroutine Extrude"},
                                               std::move(patch));
    }

    void TestCoroutineCommandTask() {
        Runtime::DocumentModel document;
        Runtime::ComputeGraphRuntime graph;
        graph.Add(Runtime::ComputeNode{
            .id = "profile",
            .run = [](Runtime::ComputeContext& context) {
                return Runtime::Completion<std::any>::Value(
                    std::any(Runtime::Feature{.kind = "Profile",
                                              .parameters = context.input.parameters.numbers}));
            },
        });
        graph.Add(Runtime::ComputeNode{
            .id = "extrude",
            .depends_on = {"profile"},
            .run = [](Runtime::ComputeContext& context) {
                auto profile = context.Get<Runtime::Feature>("profile");
                Runtime::Feature feature{.kind = "CoroutineExtrude", .parameters = profile.parameters};
                feature.parameters["depth"] = 20.0;
                return Runtime::Completion<std::any>::Value(std::any(feature));
            },
        });

        auto result = RunCoroutineExtrude(document, graph).Run();
        assert(result.HasValue());
        assert(result.Value().new_version.value == 1);
        assert(document.Features().size() == 1);
        assert(document.Features()[0].kind == "CoroutineExtrude");
    }

    void TestDemoScenario() {
        Runtime::DemoResult result = Runtime::RunDemoScenario();
        assert(result.line_resolution.status == Runtime::ResolutionStatus::Resolved);
        assert(result.suppressed_line_resolution.status == Runtime::ResolutionStatus::Suppressed);
        assert(result.measure_resolution.status == Runtime::ResolutionStatus::Resolved);
        assert(result.modal_resolution.status == Runtime::ResolutionStatus::Resolved);
        assert(result.selection_event == Runtime::EventDisposition::Handled);
        assert(result.fallback_event == Runtime::EventDisposition::Handled);
        assert(result.stack_depth_during_measure == 2);
        assert(result.document.Version().value == 2);
        assert(result.document.Features().empty());
        assert(result.undo_record == 1);
        assert(result.undo_record_operations == 1);
        assert(result.undone_record == 1);
        assert(result.undo_applied_operations == 1);
        assert(!result.trace.records.empty());
    }

    void TestStaticSafetyBoundaries() {
        static_assert(Runtime::UiAffine<FakeWidget>);
        static_assert(Runtime::BackgroundSafe<Runtime::SelectionSnapshot>);
        static_assert(Runtime::BackgroundSafe<Runtime::ParameterSnapshot>);
        static_assert(Runtime::BackgroundSafe<Runtime::ComputeInput>);
        static_assert(!Runtime::BackgroundSafe<FakeWidget>);
    }

} // namespace

int main() {
    TestParameterSchemaValidation();
    TestStdexecBridgeStatus();
    TestContextualKeyBinding();
    TestKeyBindingConflict();
    TestUserKeyBindingOverride();
    TestScopedKeyBindingSuppressionAndRebind();
    TestInteractionEventRouting();
    TestRouterModes();
    TestCommandCapabilityRejection();
    TestExclusiveCommandCancelsExistingStack();
    TestPassiveCommandDoesNotSuspendActiveCommand();
    TestComputeGraphCycleDetection();
    TestComputeGraphStopToken();
    TestComputeGraphAndTransaction();
    TestEmptyTransactionPatchRejected();
    TestChangeSetUpdateEraseAndConflict();
    TestUndoRejectsVersionMismatch();
    TestCoroutineCommandTask();
    TestDemoScenario();
    TestStaticSafetyBoundaries();

    std::cout << "Sora Experimental CommandRuntime tests passed\n";
}
