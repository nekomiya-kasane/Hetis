#include "Sora/Core/CLI.h"
#include "Sora/Core/CLI/Descriptions.h"

using namespace Sora;
using namespace Sora::CLI;

struct MyProgram {
    static consteval auto BuildSchema(SchemaBuilder<MyProgram>& builder) noexcept {
        return builder.Name("greet").Policy(Policy::Utf8 | Policy::GnuStyle).Seal();
    }
};

inline constexpr auto program = Compile<MyProgram>();

int main() {
    return 0;
}