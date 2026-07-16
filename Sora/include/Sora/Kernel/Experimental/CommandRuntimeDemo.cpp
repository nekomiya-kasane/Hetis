/**
 * @file CommandRuntimeDemo.cpp
 * @brief Standalone demo for the experimental CAD command runtime.
 * @ingroup KernelExperimental
 */

#include "CommandRuntime.h"
#include "CommandRuntimeReflection.h"
#include "CommandRuntimeStdexec.h"

#include <iostream>

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

} // namespace

int main() {
    using namespace Runtime;

    DemoResult result = RunDemoScenario();
    ParameterSchema schema = Reflection::SchemaOf<CreateLineParameters>();
    ParameterValidationResult validation = ValidateParameters(
        schema, ParameterSnapshot{.numbers = {{"x", 1.0}, {"y", 2.0}},
                                  .booleans = {{"construction", false}},
                                  .version = result.document.Version()});
    const auto reflection = Reflection::Status();
    const auto stdexec = StdexecBridge::Status();

    std::cout << "Sora Experimental CommandRuntime demo\n";
    std::cout << "suppressed default line shortcut: "
              << (result.suppressed_line_resolution.status == ResolutionStatus::Suppressed ? "yes" : "no") << "\n";
    std::cout << "resolved rebound line command: " << result.line_resolution.invocation->command.value << "\n";
    std::cout << "resolved measure command: " << result.measure_resolution.invocation->command.value << "\n";
    std::cout << "resolved modal command: " << result.modal_resolution.invocation->command.value << "\n";
    std::cout << "stack depth during measure: " << result.stack_depth_during_measure << "\n";
    std::cout << "viewer select event: "
              << (result.selection_event == EventDisposition::Handled ? "handled" : "ignored") << "\n";
    std::cout << "active fallback event: "
              << (result.fallback_event == EventDisposition::Handled ? "handled" : "ignored") << "\n";
    std::cout << "parameter schema: " << schema.name << " (" << schema.fields.size() << " fields)\n";
    std::cout << "parameter validation: " << (validation.Valid() ? "valid" : "invalid") << "\n";
    std::cout << "static reflection: " << (reflection.static_reflection_available ? "available" : "manual fallback")
              << "\n";
    std::cout << "stdexec bridge: " << (stdexec.stdexec_available ? "available" : "header unavailable") << "\n";
    std::cout << "document version: " << result.document.Version().value << "\n";
    std::cout << "feature count: " << result.document.Features().size() << "\n";
    std::cout << "undo record: " << result.undo_record << "\n";
    std::cout << "commit operations: " << result.undo_record_operations << "\n";
    std::cout << "undone record: " << result.undone_record << "\n";
    std::cout << "undo operations: " << result.undo_applied_operations << "\n";
    std::cout << "trace:\n" << result.trace.Join() << "\n";
}
